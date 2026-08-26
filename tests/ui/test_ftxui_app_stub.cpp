// tests/ui/test_ftxui_app_stub.cpp — tests for the no-FTXUI stub + the plain
// helpers of ui/ftxui_app.cpp.
//
// The test_ui target links bootamp_ui, which propagates BOOTAMP_HAS_FTXUI
// (1 when FTXUI is installed). The stub-specific test below is therefore
// guarded: it only runs when BOOTAMP_HAS_FTXUI is 0 (the #else stub branch).
// The rest of the suite is build-independent:
//   * the FtxuiApp interface types (KeyCallback/StatusProvider) are usable;
//   * the pure helpers of the blit/key path: braille_bitmap (the DrawPixel
//     decode for braille-dot cells, cliamp brailleBit order), utf8_of, and
//     canonical_key_name (raw FTXUI event input -> canonical key names).
// The FTXUI-backed implementation itself (FtxuiAppImpl in ftxui_app.cpp) is
// compiled only with BOOTAMP_HAS_FTXUI=1.
#include "ui/ftxui_app.hpp"
#include "ui/ftxui_app_impl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>

using namespace bootamp::ui;

#if !BOOTAMP_HAS_FTXUI
TEST_CASE("make_ftxui_app stub returns nullptr without FTXUI", "[ui][ftxui]") {
  Visualizer vis(44100);
  std::unique_ptr<FtxuiApp> app =
      make_ftxui_app(vis, FtxuiApp::KeyCallback{}, FtxuiApp::StatusProvider{});
  CHECK(app == nullptr);
}
#endif  // !BOOTAMP_HAS_FTXUI

TEST_CASE("FtxuiApp interface types are callable", "[ui][ftxui]") {
  std::string seen;
  FtxuiApp::KeyCallback on_key = [&seen](std::string_view key) {
    seen.assign(key);
  };
  FtxuiApp::StatusProvider status = [] { return std::string("playing 1:23/4:56"); };

  on_key("space");
  CHECK(seen == "space");
  on_key("shift+left");
  CHECK(seen == "shift+left");
  CHECK(status() == "playing 1:23/4:56");

  // Default-constructed callbacks are empty (the app tolerates them).
  FtxuiApp::KeyCallback none;
  CHECK(!none);
}

TEST_CASE("braille_bitmap decodes cliamp brailleBit layout", "[ui][ftxui]") {
  // out[2*r+c]: dot row r (0..3, 3 = bottom dots 7/8), dot column c (0..1).
  // cliamp brailleBit: bit(r,c) = 1<<(3c+r) for r<3, 1<<(6+c) for r==3.
  CHECK(braille_bitmap(U'⠀') == std::array<bool, 8>{});
  CHECK(braille_bitmap(U'⠁') ==
        (std::array<bool, 8>{true, false, false, false, false, false, false, false}));  // dot1
  CHECK(braille_bitmap(U'⠈') ==
        (std::array<bool, 8>{false, true, false, false, false, false, false, false}));  // dot4
  CHECK(braille_bitmap(U'⠂') ==
        (std::array<bool, 8>{false, false, true, false, false, false, false, false}));  // dot2
  CHECK(braille_bitmap(U'⠐') ==
        (std::array<bool, 8>{false, false, false, true, false, false, false, false}));  // dot5
  CHECK(braille_bitmap(U'⠄') ==
        (std::array<bool, 8>{false, false, false, false, true, false, false, false}));  // dot3
  CHECK(braille_bitmap(U'⠠') ==
        (std::array<bool, 8>{false, false, false, false, false, true, false, false}));  // dot6
  CHECK(braille_bitmap(U'⡀') ==
        (std::array<bool, 8>{false, false, false, false, false, false, true, false}));  // dot7
  CHECK(braille_bitmap(U'⢀') ==
        (std::array<bool, 8>{false, false, false, false, false, false, false, true}));  // dot8
  // Composite: U+283F = dots 1-6 (all upper dots), U+28C0 = dots 7+8.
  CHECK(braille_bitmap(U'⠿') ==
        (std::array<bool, 8>{true, true, true, true, true, true, false, false}));
  CHECK(braille_bitmap(U'⣀') ==
        (std::array<bool, 8>{false, false, false, false, false, false, true, true}));
  // Non-braille runes decode to nothing.
  CHECK(braille_bitmap(U' ') == std::array<bool, 8>{});
  CHECK(braille_bitmap(U'\U0001F600') == std::array<bool, 8>{});
  CHECK(braille_bitmap(U'⟿') == std::array<bool, 8>{});
  CHECK(braille_bitmap(U'⤀') == std::array<bool, 8>{});
}

TEST_CASE("utf8_of encodes codepoints", "[ui][ftxui]") {
  CHECK(utf8_of(U'A') == "A");
  CHECK(utf8_of(U' ') == " ");
  CHECK(utf8_of(U'\0') == std::string(1, '\0'));
  CHECK(utf8_of(U'é') == "\xC3\xA9");                 // é
  CHECK(utf8_of(U'⠀') == "\xE2\xA0\x80");             // braille blank
  CHECK(utf8_of(U'⢀') == "\xE2\xA2\x80");             // braille dot 8
  CHECK(utf8_of(U'\U0001F600') == "\xF0\x9F\x98\x80");     // 😀
}

TEST_CASE("canonical_key_name maps character events", "[ui][ftxui]") {
  CHECK(canonical_key_name(true, " ") == "space");
  CHECK(canonical_key_name(true, "a") == "a");
  CHECK(canonical_key_name(true, "n") == "n");
  CHECK(canonical_key_name(true, "V") == "V");
  CHECK(canonical_key_name(true, "?") == "?");
  CHECK(canonical_key_name(true, "/") == "/");
  CHECK(canonical_key_name(true, "0") == "0");
  // Unclaimed character input: control bytes, wide/combining runes, empty.
  CHECK(canonical_key_name(true, "\x01") == "");
  CHECK(canonical_key_name(true, "\x7F") == "");
  CHECK(canonical_key_name(true, "\xC3\xA9") == "");  // é (2 bytes)
  CHECK(canonical_key_name(true, "") == "");
}

TEST_CASE("canonical_key_name maps special (escape) events", "[ui][ftxui]") {
  // Arrows (FTXUI Event statics, verified against 7.0.3 event.cpp).
  CHECK(canonical_key_name(false, "\x1B[A") == "up");
  CHECK(canonical_key_name(false, "\x1B[B") == "down");
  CHECK(canonical_key_name(false, "\x1B[C") == "right");
  CHECK(canonical_key_name(false, "\x1B[D") == "left");
  // Ctrl+arrows.
  CHECK(canonical_key_name(false, "\x1B[1;5A") == "ctrl+up");
  CHECK(canonical_key_name(false, "\x1B[1;5B") == "ctrl+down");
  CHECK(canonical_key_name(false, "\x1B[1;5C") == "ctrl+right");
  CHECK(canonical_key_name(false, "\x1B[1;5D") == "ctrl+left");
  // Shift+arrows (no FTXUI statics; xterm CSI 1;2<letter>).
  CHECK(canonical_key_name(false, "\x1B[1;2A") == "shift+up");
  CHECK(canonical_key_name(false, "\x1B[1;2B") == "shift+down");
  CHECK(canonical_key_name(false, "\x1B[1;2C") == "shift+right");
  CHECK(canonical_key_name(false, "\x1B[1;2D") == "shift+left");
  // Navigation + editing.
  CHECK(canonical_key_name(false, "\x1B[Z") == "shift+tab");
  CHECK(canonical_key_name(false, "\x1B[H") == "home");
  CHECK(canonical_key_name(false, "\x1B[F") == "end");
  CHECK(canonical_key_name(false, "\x1B[5~") == "pageup");
  CHECK(canonical_key_name(false, "\x1B[6~") == "pagedown");
  CHECK(canonical_key_name(false, "\x1B[3~") == "delete");
  CHECK(canonical_key_name(false, "\x1B[2~") == "insert");
  CHECK(canonical_key_name(false, "\x1B") == "esc");
  // Enter arrives normalized to \n (input parser maps \r -> \n); both match.
  CHECK(canonical_key_name(false, "\n") == "enter");
  CHECK(canonical_key_name(false, "\r") == "enter");
  CHECK(canonical_key_name(false, "\t") == "tab");
  CHECK(canonical_key_name(false, "\x7F") == "backspace");
  // Ctrl+letter control bytes 0x01..0x1A.
  CHECK(canonical_key_name(false, "\x01") == "ctrl+a");
  CHECK(canonical_key_name(false, "\x03") == "ctrl+c");
  CHECK(canonical_key_name(false, "\x0B") == "ctrl+k");
  CHECK(canonical_key_name(false, "\x1A") == "ctrl+z");
  // Unclaimed special input: function keys, the Custom event byte, unknown.
  CHECK(canonical_key_name(false, "\x1BOP") == "");   // F1
  CHECK(canonical_key_name(false, "\x1B[15~") == ""); // F5
  CHECK(canonical_key_name(false, "\x00") == "");     // Event::Custom
  CHECK(canonical_key_name(false, "") == "");
  CHECK(canonical_key_name(false, "\x1B[1;2E") == "");  // unknown modifier
  CHECK(canonical_key_name(false, "\x1BOA") == "");     // ss3 up (alt/shift)
}
