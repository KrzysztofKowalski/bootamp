// tests/ui/test_jump.cpp — JumpModel tests (jump-to-time prompt).
//
// Ports the Go parseJumpTarget table (ui/model/jump_test.go) plus the enter
// behavior of handleJumpKey (keys.go) onto the plain-C++ model: a valid
// "ss"/"mm:ss"/"hh:mm:ss" query fires on_jump with the total seconds and
// closes; an invalid query stays open and clears the buffer (bootamp; Go
// preserves the input and shows the parse error).
#include "ui/screens/jump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

using namespace bootamp::ui::screens;

TEST_CASE("jump mm:ss parses to seconds", "[screens][jump]") {
  // Go jump_test.go "minutes seconds": "58:05" → 58m5s; here "1:30" → 90.0.
  JumpModel m;
  double jumped = -1.0;
  m.set_actions(JumpActions{.on_jump = [&](double s) { jumped = s; }});
  m.open();
  m.query() = "1:30";
  m.submit();
  REQUIRE(jumped == 90.0);
  REQUIRE(!m.active());
}

TEST_CASE("jump bare seconds parses", "[screens][jump]") {
  // Go jump_test.go "seconds only": "10" → 10s.
  JumpModel m;
  double jumped = -1.0;
  m.set_actions(JumpActions{.on_jump = [&](double s) { jumped = s; }});
  m.open();
  m.query() = "90";
  m.submit();
  REQUIRE(jumped == 90.0);
  REQUIRE(!m.active());
}

TEST_CASE("jump hh:mm:ss parses to seconds", "[screens][jump]") {
  // Go jump_test.go "hours minutes seconds": "1:02:03" → 1h2m3s; here
  // "1:30:00" → 5400.0.
  JumpModel m;
  double jumped = -1.0;
  m.set_actions(JumpActions{.on_jump = [&](double s) { jumped = s; }});
  m.open();
  m.query() = "1:30:00";
  m.submit();
  REQUIRE(jumped == 5400.0);
  REQUIRE(!m.active());
}

TEST_CASE("jump clock variants match Go table", "[screens][jump]") {
  // Go jump_test.go success rows: "58:05"→3485, "58:6"→3486, "58:"→3480,
  // "  12:3  "→723, "2:3:4"→7394, "1::03"→3603, "1:02:"→3720.
  const auto expect = [](std::string query, double want) {
    JumpModel m;
    double jumped = -1.0;
    m.set_actions(JumpActions{.on_jump = [&](double s) { jumped = s; }});
    m.open();
    m.query() = std::move(query);
    m.submit();
    REQUIRE(jumped == want);
    REQUIRE(!m.active());
  };
  expect("58:05", 3485.0);
  expect("58:6", 3486.0);
  expect("58:", 3480.0);
  expect("  12:3  ", 723.0);
  expect("2:3:4", 7394.0);
  expect("1::03", 3603.0);
  expect("1:02:", 3720.0);
}

TEST_CASE("jump garbage stays open and clears the query", "[screens][jump]") {
  // Go jump_test.go failure rows ("not number"); bootamp clears the query
  // for a retry (Go preserves it and surfaces the error in the status line).
  JumpModel m;
  int jumped = 0;
  m.set_actions(JumpActions{.on_jump = [&](double) { ++jumped; }});
  m.open();
  m.query() = "garbage";
  m.submit();
  REQUIRE(jumped == 0);
  REQUIRE(m.active());
  REQUIRE(m.query().empty());
}

TEST_CASE("jump invalid clock fields are rejected", "[screens][jump]") {
  // Go jump_test.go failure rows: "1:60:00" (minutes > 59), "1:00:60"
  // (seconds > 59), "10:60", "99:99", "1:2:3:4" (too many colons), "10:123"
  // (too many second digits), "" (empty), "abc".
  const std::string invalid[] = {"1:60:00", "1:00:60", "10:60", "99:99",
                                 "1:2:3:4", "10:123", "",       "abc"};
  for (const std::string& q : invalid) {
    JumpModel m;
    int jumped = 0;
    m.set_actions(JumpActions{.on_jump = [&](double) { ++jumped; }});
    m.open();
    m.query() = q;
    m.submit();
    INFO("query: " << q);
    REQUIRE(jumped == 0);
    REQUIRE(m.active());
    REQUIRE(m.query().empty());
  }
}

TEST_CASE("jump cancel fires on_cancel and closes", "[screens][jump]") {
  // Go esc: closeJumpMode (jumping=false).
  JumpModel m;
  int cancelled = 0;
  m.set_actions(JumpActions{.on_cancel = [&] { ++cancelled; }});
  m.open();
  REQUIRE(m.active());
  m.query() = "1:30";
  m.cancel();
  REQUIRE(cancelled == 1);
  REQUIRE(!m.active());
}
