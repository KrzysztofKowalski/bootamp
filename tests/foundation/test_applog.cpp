// tests/test_applog.cpp — Catch2 tests for foundation/applog.
//
// Port of cliamp/applog/applog_test.go, focused on the dual-sink ring: ring
// wraps at max_entries (4) keeping the most recent, and order is preserved
// (oldest-first). Also covers drain/clear semantics, parse_level, and the
// file-only vs ring-only vs dual-sink routing.
//
// Golden-file comparisons are not used here (none exist for applog); analytic
// invariants from the Go tests are verified directly.
//
// Compile-only (syntax check): the non-template applog functions are declared
// in the header and defined in applog.cpp; -fsyntax-only resolves the calls
// against the declarations without linking.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "foundation/applog.hpp"

namespace applog = bootamp::foundation::applog;
namespace fs = std::filesystem;

namespace {
// reset clears package-level state so tests don't leak into each other.
// Mirrors Go's reset(t). The returned CloseFn (from a fresh init to /dev/null)
// restores the logger to the discard sink; drain() clears the ring.
void reset() {
  // Drain any leftover ring entries from a prior test.
  (void)applog::drain();
  // Re-init to a throwaway file in the system temp dir, then immediately
  // close, so the logger is reset to the discard sink. If init fails we
  // simply proceed — the ring is the state under test and drain() handled it.
  auto tmp = fs::temp_directory_path() / "bootamp_applog_reset.log";
  if (auto r = applog::init(tmp, applog::Level::info)) {
    (*r)();  // close -> discard sink
  }
  (void)applog::drain();
}

// file_contains reads `path` and returns whether `needle` appears in it.
auto file_contains(const fs::path& path, std::string_view needle) -> bool {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str().find(std::string(needle)) != std::string::npos;
}
}  // namespace

TEST_CASE("applog: ring wraps at 4 (max_entries)", "[applog][ring]") {
  reset();
  for (int i = 0; i < 6; ++i) {
    applog::user_warn("msg-{}", i);
  }
  auto got = applog::ring_entries();
  REQUIRE(got.size() == applog::max_entries);  // 4
  // Most recent 4 of "msg-0".."msg-5" are msg-2..msg-5, oldest-first.
  CHECK(got[0].text == "msg-2");
  CHECK(got[1].text == "msg-3");
  CHECK(got[2].text == "msg-4");
  CHECK(got[3].text == "msg-5");
}

TEST_CASE("applog: ring preserves insertion order", "[applog][ring]") {
  reset();
  applog::user_warn("hello {}", "world");
  applog::user_warn("n={}", 42);
  auto got = applog::ring_entries();
  REQUIRE(got.size() == 2);
  CHECK(got[0].text == "hello world");
  CHECK(got[1].text == "n=42");
  // At timestamp is set (non-zero).
  CHECK(got[0].at != std::chrono::system_clock::time_point{});
  CHECK(got[1].at != std::chrono::system_clock::time_point{});
  // Monotonic-ish: second entry not before the first.
  CHECK_FALSE(got[1].at < got[0].at);
}

TEST_CASE("applog: drain returns and clears the buffer", "[applog][ring]") {
  reset();
  applog::user_warn("first");
  auto first = applog::drain();
  REQUIRE(first.size() == 1);
  CHECK(first[0].text == "first");

  auto second = applog::drain();
  CHECK(second.empty());  // Go returns nil; C++ returns empty vector
}

TEST_CASE("applog: drain on empty buffer returns empty", "[applog][ring]") {
  reset();
  auto got = applog::drain();
  CHECK(got.empty());
}

TEST_CASE("applog: status pushes ring only (no file write)", "[applog][routing]") {
  reset();
  auto path = fs::temp_directory_path() / "bootamp_applog_status.log";
  fs::remove(path);
  auto close = applog::init(path, applog::Level::debug);
  REQUIRE(close.has_value());

  applog::status("nothing-on-disk");

  auto ring = applog::ring_entries();
  REQUIRE(ring.size() == 1);
  CHECK(ring[0].text == "nothing-on-disk");
  CHECK_FALSE(file_contains(path, "nothing-on-disk"));
  close.value()();
}

TEST_CASE("applog: debug/info/warn/error skip the ring", "[applog][routing]") {
  reset();
  auto path = fs::temp_directory_path() / "bootamp_applog_diag.log";
  fs::remove(path);
  auto close = applog::init(path, applog::Level::debug);
  REQUIRE(close.has_value());

  applog::debug("dbg-msg");
  applog::info("info-msg");
  applog::warn("warn-msg");
  applog::error("err-msg");

  CHECK(applog::ring_entries().empty());  // diagnostic logs must not push to ring
  close.value()();
  for (auto want : {"dbg-msg", "info-msg", "warn-msg", "err-msg"}) {
    CHECK(file_contains(path, want));
  }
}

TEST_CASE("applog: user_warn/user_error hit both sinks", "[applog][routing]") {
  reset();
  auto path = fs::temp_directory_path() / "bootamp_applog_user.log";
  fs::remove(path);
  auto close = applog::init(path, applog::Level::debug);
  REQUIRE(close.has_value());

  applog::user_warn("careful: {}", "oops");
  applog::user_error("boom: {}", 7);

  auto ring = applog::ring_entries();
  REQUIRE(ring.size() == 2);
  CHECK(ring[0].text == "careful: oops");
  CHECK(ring[1].text == "boom: 7");
  close.value()();
  CHECK(file_contains(path, "careful: oops"));
  CHECK(file_contains(path, "boom: 7"));
}

TEST_CASE("applog: level filtering suppresses below threshold", "[applog][filter]") {
  reset();
  auto path = fs::temp_directory_path() / "bootamp_applog_filter.log";
  fs::remove(path);
  auto close = applog::init(path, applog::Level::warn);
  REQUIRE(close.has_value());

  applog::debug("hidden-debug");
  applog::info("hidden-info");
  applog::warn("visible-warn");

  close.value()();
  CHECK_FALSE(file_contains(path, "hidden-debug"));
  CHECK_FALSE(file_contains(path, "hidden-info"));
  CHECK(file_contains(path, "visible-warn"));
}

TEST_CASE("applog: parse_level accepts aliases and rejects junk", "[applog][level]") {
  CHECK(applog::parse_level("").has_value());
  CHECK(applog::parse_level("").value() == applog::Level::info);
  CHECK(applog::parse_level("info").value() == applog::Level::info);
  CHECK(applog::parse_level("INFO").value() == applog::Level::info);
  CHECK(applog::parse_level(" debug ").value() == applog::Level::debug);
  CHECK(applog::parse_level("warn").value() == applog::Level::warn);
  CHECK(applog::parse_level("warning").value() == applog::Level::warn);
  CHECK(applog::parse_level("error").value() == applog::Level::error);
  CHECK_FALSE(applog::parse_level("trace").has_value());
  CHECK_FALSE(applog::parse_level("verbose").has_value());
}

TEST_CASE("applog: level_name round-trips", "[applog][level]") {
  CHECK(applog::level_name(applog::Level::debug) == "debug");
  CHECK(applog::level_name(applog::Level::info) == "info");
  CHECK(applog::level_name(applog::Level::warn) == "warn");
  CHECK(applog::level_name(applog::Level::error) == "error");
}

TEST_CASE("applog: init creates missing parent directories", "[applog][init]") {
  reset();
  auto nested = fs::temp_directory_path() / "bootamp_applog_a" / "b" / "c" / "app.log";
  fs::remove_all(fs::temp_directory_path() / "bootamp_applog_a");
  auto close = applog::init(nested, applog::Level::info);
  REQUIRE(close.has_value());
  CHECK(fs::exists(nested));
  close.value()();
}

TEST_CASE("applog: ring is thread-safe under concurrent writers", "[applog][ring]") {
  reset();
  const int goroutines = 20;
  const int per_goroutine = 50;
  std::vector<std::thread> threads;
  threads.reserve(goroutines);
  for (int i = 0; i < goroutines; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < per_goroutine; ++j) {
        applog::user_warn("g-log");
      }
    });
  }
  for (auto& t : threads) t.join();

  auto got = applog::ring_entries();
  CHECK(got.size() <= applog::max_entries);
  for (const auto& e : got) {
    CHECK(e.text.rfind("g-log", 0) == 0);
  }
}

TEST_CASE("applog: ring_entries does not clear (status line can re-poll)", "[applog][ring]") {
  reset();
  applog::user_warn("persist");
  auto a = applog::ring_entries();
  auto b = applog::ring_entries();
  REQUIRE(a.size() == 1);
  REQUIRE(b.size() == 1);
  CHECK(a[0].text == "persist");
  CHECK(b[0].text == "persist");
  (void)applog::drain();
}

TEST_CASE("applog: push_ring strips trailing newlines and drops empties", "[applog][ring]") {
  reset();
  applog::push_ring("with-newline\n");
  applog::push_ring("\n");
  applog::push_ring("");
  auto got = applog::ring_entries();
  REQUIRE(got.size() == 1);
  CHECK(got[0].text == "with-newline");
  (void)applog::drain();
}