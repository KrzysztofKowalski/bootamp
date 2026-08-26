// tests/ui/test_url.cpp — UrlModel tests (URL input prompt).
//
// Ports the Go URL input behavior (ui/model/keys.go: the "u" key opens at
// keys.go:776-779, handleURLInputKey handles esc/enter) onto the plain-C++
// model: submit fires on_submit with the trimmed query and closes, an empty
// submit stays open, cancel fires on_cancel and closes.
#include "ui/screens/url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace bootamp::ui::screens;

TEST_CASE("url submit fires on_submit with the query and closes", "[screens][url]") {
  UrlModel m;
  std::string submitted;
  m.set_actions(UrlActions{.on_submit = [&](std::string_view q) {
                             submitted = std::string(q);
                           }});
  m.open();
  REQUIRE(m.active());
  m.query() = "https://example.com/radio";
  m.submit();
  REQUIRE(submitted == "https://example.com/radio");
  REQUIRE(!m.active());
}

TEST_CASE("url submit trims whitespace", "[screens][url]") {
  // Go handleURLInputKey: input := strings.TrimSpace(m.urlInput).
  UrlModel m;
  std::string submitted;
  m.set_actions(UrlActions{.on_submit = [&](std::string_view q) {
                             submitted = std::string(q);
                           }});
  m.open();
  m.query() = "  https://example.com/track  ";
  m.submit();
  REQUIRE(submitted == "https://example.com/track");
  REQUIRE(!m.active());
}

TEST_CASE("url empty submit stays open with no callback", "[screens][url]") {
  // Go: empty input re-arms the prompt with "Enter a stream, track, or
  // playlist URL." (urlInputting=true).
  UrlModel m;
  int submitted = 0;
  m.set_actions(UrlActions{.on_submit = [&](std::string_view) { ++submitted; }});
  m.open();
  m.query() = "   ";
  m.submit();
  REQUIRE(submitted == 0);
  REQUIRE(m.active());
}

TEST_CASE("url cancel fires on_cancel and closes", "[screens][url]") {
  // Go esc: urlInputting=false (bootamp also notifies the host via on_cancel).
  UrlModel m;
  int cancelled = 0;
  m.set_actions(UrlActions{.on_cancel = [&] { ++cancelled; }});
  m.open();
  REQUIRE(m.active());
  m.cancel();
  REQUIRE(cancelled == 1);
  REQUIRE(!m.active());
}

TEST_CASE("url open resets the query", "[screens][url]") {
  // Go "u": urlInputting=true, urlInput="" (fresh buffer per open).
  UrlModel m;
  m.open();
  m.query() = "leftover";
  m.cancel();
  m.open();
  REQUIRE(m.query().empty());
  REQUIRE(m.active());
}
