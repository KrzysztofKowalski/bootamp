// tests/ui/test_gieres_overrides.cpp — gieres.ini parser + endpoint
// fallback tests.
//
// The ini parser runs on in-memory texts (no filesystem); the fallback runs
// through gieres_with_fallback with counting fake hooks (no network). The
// real resolution order (gieres.ini > config > default) lives in
// config::gieres_base_url_for, which reads the user's config dir and is left
// to a manual test.
#include "ui/screens/gieres.hpp"

#include "config/gieres_overrides.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

using bootamp::config::parse_gieres_ini_base_url;
using bootamp::ui::GieresDay;
using bootamp::ui::GieresListing;
using bootamp::ui::GieresSegment;
using namespace bootamp::ui::screens;

namespace {

// CountingHooks makes one endpoint's hook set: every fetch counts a call and
// returns the canned error ("" = success). Only the shapes are exercised —
// the by_date/segments/now_playing hooks return default-constructed results.
struct CountingHooks {
  std::string err;  // canned error; "" = success
  int         org_calls = 0;
  int         by_calls  = 0;
  int         seg_calls = 0;
  int         np_calls  = 0;

  GieresFallbackHooks make(std::string base) {
    return GieresFallbackHooks{
        .base = std::move(base),
        .orgonity =
            [this](int /*page*/, std::string_view /*q*/,
                            std::string_view /*sort*/) mutable
            -> std::expected<GieresListing, std::string> {
              ++org_calls;
              if (!err.empty()) {
                return std::unexpected(err);
              }
              return GieresListing{};
            },
        .by_date =
            [this](std::string_view /*date*/) mutable
            -> std::expected<GieresDay, std::string> {
              ++by_calls;
              if (!err.empty()) {
                return std::unexpected(err);
              }
              return GieresDay{};
            },
        .segments =
            [this](std::string_view /*start*/,
                            std::string_view /*end*/) mutable
            -> std::expected<std::vector<GieresSegment>, std::string> {
              ++seg_calls;
              if (!err.empty()) {
                return std::unexpected(err);
              }
              return std::vector<GieresSegment>{};
            },
        .now_playing =
            [this]() mutable -> std::expected<std::string, std::string> {
              ++np_calls;
              if (!err.empty()) {
                return std::unexpected(err);
              }
              return std::string{"Radio Radio ..::.."};
            },
    };
  }
};

const std::string kLan = "http://192.168.1.154:13080";
const std::string kPub = "https://gieres.cytr.us";

}  // namespace

// ---------------------------------------------------------------------------
// gieres.ini parser
// ---------------------------------------------------------------------------

TEST_CASE("gieres ini parser extracts base_url", "[gieres][config]") {
  REQUIRE(parse_gieres_ini_base_url("") == "");
  REQUIRE(parse_gieres_ini_base_url("# comment\n; also a comment\n") == "");
  REQUIRE(parse_gieres_ini_base_url("other = 1\n\nbase_url = http://a\n") ==
          "http://a");
  REQUIRE(parse_gieres_ini_base_url("  base_url   =   https://gieres.cytr.us  ") ==
          "https://gieres.cytr.us");
  REQUIRE(parse_gieres_ini_base_url("base_url = \"https://gieres.cytr.us\"\n") ==
          "https://gieres.cytr.us");
  REQUIRE(parse_gieres_ini_base_url("base_url = 'http://x'\n") == "http://x");
  // An empty value is skipped; a later usable key wins.
  REQUIRE(parse_gieres_ini_base_url("base_url=\nbase_url = http://b\n") ==
          "http://b");
  // The first usable value wins over later duplicates.
  REQUIRE(parse_gieres_ini_base_url("base_url = http://first\n"
                                    "base_url = http://second\n") ==
          "http://first");
  REQUIRE(parse_gieres_ini_base_url("garbage line\nbase_url = http://x\n") ==
          "http://x");
}

// ---------------------------------------------------------------------------
// Transport-error predicate
// ---------------------------------------------------------------------------

TEST_CASE("transport errors gate the gieres fallback", "[gieres][config]") {
  REQUIRE(gieres_is_transport_error("dial 192.168.1.154:13080: connection refused"));
  REQUIRE(gieres_is_transport_error(
      "dial gieres.cytr.us:443: getaddrinfo: Name or service not known"));
  REQUIRE(gieres_is_transport_error("connect timeout"));
  REQUIRE(gieres_is_transport_error("tls handshake gieres.cytr.us:443: eof"));
  // The endpoint answered — no retry.
  REQUIRE_FALSE(gieres_is_transport_error("gieres: /orgonity.json HTTP 404"));
  REQUIRE_FALSE(gieres_is_transport_error("http status 404 Not Found"));
  REQUIRE_FALSE(
      gieres_is_transport_error("gieres: /echelon/segments body is not a JSON object"));
}

// ---------------------------------------------------------------------------
// Sticky fallback
// ---------------------------------------------------------------------------

TEST_CASE("gieres fallback retries the public endpoint and sticks",
          "[gieres][model]") {
  auto active = std::make_shared<GieresActiveBase>();
  CountingHooks lan, pub;
  lan.err = "dial 192.168.1.154:13080: connection refused";
  auto hooks = gieres_with_fallback(lan.make(kLan), pub.make(kPub), active);

  auto r = hooks.orgonity(1, "", "date");
  REQUIRE(r.has_value());
  REQUIRE(lan.org_calls == 1);
  REQUIRE(pub.org_calls == 1);
  REQUIRE(active->get() == kPub);

  // Sticky: the second fetch goes straight to the public endpoint.
  r = hooks.orgonity(2, "", "date");
  REQUIRE(r.has_value());
  REQUIRE(lan.org_calls == 1);  // the LAN was not retried
  REQUIRE(pub.org_calls == 2);
}

TEST_CASE("gieres http and parse errors do not trigger the fallback",
          "[gieres][model]") {
  auto active = std::make_shared<GieresActiveBase>();
  CountingHooks lan, pub;
  lan.err = "gieres: /orgonity.json HTTP 404";
  auto hooks = gieres_with_fallback(lan.make(kLan), pub.make(kPub), active);

  auto r = hooks.orgonity(1, "", "date");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() == lan.err);
  REQUIRE(lan.org_calls == 1);
  REQUIRE(pub.org_calls == 0);  // the endpoint answered — no retry
  REQUIRE(active->get().empty());
}

TEST_CASE("gieres fallback recovers to the primary when it comes back",
          "[gieres][model]") {
  auto active = std::make_shared<GieresActiveBase>();
  CountingHooks lan, pub;
  lan.err = "dial 192.168.1.154:13080: connection refused";
  auto hooks = gieres_with_fallback(lan.make(kLan), pub.make(kPub), active);

  REQUIRE(hooks.orgonity(1, "", "date").has_value());
  REQUIRE(active->get() == kPub);

  // The LAN came back: the sticky public attempt fails on transport, the
  // primary retry succeeds, and the primary is authoritative again.
  lan.err.clear();
  pub.err = "connect timeout";
  REQUIRE(hooks.orgonity(2, "", "date").has_value());
  REQUIRE(lan.org_calls == 2);  // 1 fail + 1 recovery retry
  REQUIRE(pub.org_calls == 2);
  REQUIRE(active->get().empty());
}

TEST_CASE("gieres both endpoints down surfaces the primary error",
          "[gieres][model]") {
  auto active = std::make_shared<GieresActiveBase>();
  CountingHooks lan, pub;
  lan.err = "dial 192.168.1.154:13080: connection refused";
  pub.err = "dial gieres.cytr.us:443: connect timeout";
  auto hooks = gieres_with_fallback(lan.make(kLan), pub.make(kPub), active);

  auto r = hooks.orgonity(1, "", "date");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() == lan.err);
  REQUIRE(active->get().empty());
}