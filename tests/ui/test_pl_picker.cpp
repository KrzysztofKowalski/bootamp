// tests/ui/test_pl_picker.cpp — PlPickerModel tests (playlist picker).
//
// Follows cliamp's theme-picker interaction (keys.go handleThemeKey up/down
// wrap, overlays.go themePickerSelect/themePickerCancel) applied to a generic
// host-supplied name list: move wraps/clamps, submit picks names[cursor] and
// closes, empty names fire nothing, cancel fires on_cancel.
#include "ui/screens/pl_picker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace bootamp::ui::screens;

TEST_CASE("pl picker move wraps around the ends", "[screens][pl_picker]") {
  // Go theme picker up/down: 0 → last, last → 0.
  PlPickerModel m;
  m.open({"a", "b", "c"});
  REQUIRE(m.active());
  REQUIRE(m.cursor() == 0);

  m.move(-1);  // wrap to last
  REQUIRE(m.cursor() == 2);
  m.move(1);  // wrap to first
  REQUIRE(m.cursor() == 0);
  m.move(1);  // plain down
  REQUIRE(m.cursor() == 1);
}

TEST_CASE("pl picker move clamps on an empty list", "[screens][pl_picker]") {
  PlPickerModel m;
  m.open({});
  REQUIRE(m.count() == 0);
  m.move(1);
  REQUIRE(m.cursor() == 0);
  m.move(-1);
  REQUIRE(m.cursor() == 0);
}

TEST_CASE("pl picker submit picks names[cursor] and closes", "[screens][pl_picker]") {
  // Go themePickerSelect: apply the current entry + visible=false.
  PlPickerModel m;
  std::string picked;
  m.set_actions(PlPickerActions{.on_pick = [&](std::string_view name) {
                                  picked = std::string(name);
                                }});
  m.open({"alpha", "beta", "gamma"});
  m.move(1);
  m.move(1);  // cursor 2
  m.submit();
  REQUIRE(picked == "gamma");
  REQUIRE(!m.active());
}

TEST_CASE("pl picker empty names submit fires no callback", "[screens][pl_picker]") {
  PlPickerModel m;
  int picked = 0;
  m.set_actions(PlPickerActions{.on_pick = [&](std::string_view) { ++picked; }});
  m.open({});
  m.submit();
  REQUIRE(picked == 0);
  REQUIRE(m.active());  // stays open (nothing to pick)
}

TEST_CASE("pl picker cancel fires on_cancel and closes", "[screens][pl_picker]") {
  // Go esc: themePickerCancel.
  PlPickerModel m;
  int cancelled = 0;
  m.set_actions(PlPickerActions{.on_cancel = [&] { ++cancelled; }});
  m.open({"a", "b"});
  REQUIRE(m.active());
  m.cancel();
  REQUIRE(cancelled == 1);
  REQUIRE(!m.active());
}

TEST_CASE("pl picker reopen replaces the names and resets the cursor",
          "[screens][pl_picker]") {
  PlPickerModel m;
  m.open({"a", "b", "c"});
  m.move(2);  // cursor 2
  m.open({"x", "y"});  // reopen (Go: picker re-opens with a fresh list)
  REQUIRE(m.count() == 2);
  REQUIRE(m.cursor() == 0);
  REQUIRE(m.names()[0] == "x");
  REQUIRE(m.names()[1] == "y");
}
