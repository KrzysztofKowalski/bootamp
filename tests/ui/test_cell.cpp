// tests/ui/test_cell.cpp — CellGrid + fit + styles tests.
//
// Ports cliamp/ui/fit_test.go (FitRect table) and cliamp/ui/styles_test.go
// (contrastingTextColor table) onto the C++ API, plus CellGrid resize/fill/
// bounds/dump cases that have no Go twin (the Go side rendered with lipgloss;
// here the grid is the golden surface).
//
// Go mapping notes:
//   FitRect(text, w, h)          → clip_text(text, w) for a single line;
//                                  fit_grid(src, dst) for the row clamp.
//   lipgloss.Width(got) <= w     → display_width(got) <= w property check.
#include "ui/cell.hpp"
#include "ui/fit.hpp"
#include "ui/styles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace bootamp::ui;

namespace {

// Decode one UTF-8 codepoint at s[i], advancing i. Test-only; inputs come from
// our own literals or clip_text output, so overlong/truncated forms do not
// occur.
char32_t decode(std::string_view s, std::size_t& i) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    ++i;
    return static_cast<char32_t>(b0);
  }
  int      len;
  char32_t cp;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
  } else {
    len = 4;
    cp = b0 & 0x07;
  }
  for (int k = 1; k < len; ++k) {
    cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
  }
  i += static_cast<std::size_t>(len);
  return cp;
}

// Display width of a string, skipping CSI escapes — the lipgloss.Width analog
// used for the "result fits the limit" property check.
int display_width(std::string_view s) {
  int         w = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    if (static_cast<unsigned char>(s[i]) == 0x1B) {
      ++i;
      if (i < s.size() && s[i] == '[') {
        ++i;
        while (i < s.size() &&
               (static_cast<unsigned char>(s[i]) < 0x40 ||
                static_cast<unsigned char>(s[i]) > 0x7E)) {
          ++i;
        }
        if (i < s.size()) {
          ++i;  // final byte
        }
      }
      continue;
    }
    w += utf_width(decode(s, i));
  }
  return w;
}

}  // namespace

// --- CellGrid ----------------------------------------------------------------

TEST_CASE("CellGrid resize keeps contents where possible", "[ui][cell]") {
  CellGrid g(2, 3);
  for (int r = 0; r < g.rows(); ++r) {
    for (int c = 0; c < g.cols(); ++c) {
      g.at(r, c).rune = static_cast<char32_t>('a' + r * 3 + c);
    }
  }
  CHECK(dump_grid(g) == "abc\ndef");

  // Grow: overlap kept, new cells default (space, default color).
  g.resize(3, 4);
  REQUIRE(g.rows() == 3);
  REQUIRE(g.cols() == 4);
  CHECK(g.at(2, 3).rune == U' ');
  CHECK(g.at(2, 3).color == kColorDefault);
  CHECK(dump_grid(g) == "abc \ndef \n    ");

  // Same size: content untouched.
  g.resize(3, 4);
  CHECK(dump_grid(g) == "abc \ndef \n    ");

  // Shrink: top-left kept.
  g.resize(1, 2);
  CHECK(g.rows() == 1);
  CHECK(g.cols() == 2);
  CHECK(dump_grid(g) == "ab");
}

TEST_CASE("CellGrid fill and cell defaults", "[ui][cell]") {
  CellGrid g(2, 2);
  CHECK(g.at(0, 0).rune == U' ');
  CHECK(g.at(0, 0).color == kColorDefault);

  g.fill(Cell{U'#', kColorAccent});
  for (int r = 0; r < g.rows(); ++r) {
    for (int c = 0; c < g.cols(); ++c) {
      CHECK(g.at(r, c).rune == U'#');
      CHECK(g.at(r, c).color == kColorAccent);
    }
  }
  CHECK(dump_grid(g) == "##\n##");
}

TEST_CASE("CellGrid set is bounds-checked", "[ui][cell]") {
  CellGrid g(2, 3);
  g.set(0, 0, Cell{U'x', kColorError});
  CHECK(g.at(0, 0).rune == U'x');

  // Out-of-range writes are dropped: negative and past-the-end on both axes.
  g.set(-1, 0, Cell{U'y', kColorError});
  g.set(0, -1, Cell{U'y', kColorError});
  g.set(2, 0, Cell{U'y', kColorError});
  g.set(0, 3, Cell{U'y', kColorError});
  CHECK(g.at(0, 0).rune == U'x');
  CHECK(dump_grid(g) == "x  \n   ");
}

TEST_CASE("CellGrid zero and negative sizes", "[ui][cell]") {
  CellGrid g;
  CHECK(g.rows() == 0);
  CHECK(g.cols() == 0);
  CHECK(dump_grid(g).empty());

  g.resize(0, 5);
  CHECK(g.rows() == 0);
  CHECK(g.cols() == 5);
  CHECK(dump_grid(g).empty());

  g.resize(3, 4);
  CHECK(dump_grid(g) == "    \n    \n    ");

  g.resize(-2, -1);  // clamps to zero
  CHECK(g.rows() == 0);
  CHECK(g.cols() == 0);
}

TEST_CASE("dump_grid emits UTF-8, omits color, is deterministic", "[ui][cell]") {
  CellGrid g(1, 2);
  g.at(0, 0).rune = U'音';  // U+97F3, 3 UTF-8 bytes
  g.at(0, 1).rune = U'a';
  CHECK(dump_grid(g) == "\xE9\x9F\xB3" "a");

  // Identical runes, different colors: dump identical (color asserted
  // separately via cells()).
  CellGrid g2(1, 2);
  g2.at(0, 0) = Cell{U'音', kColorError};
  g2.at(0, 1) = Cell{U'a', kColorKeyFG};
  CHECK(dump_grid(g2) == dump_grid(g));
  CHECK(g2.cells()[0].color == kColorError);
  CHECK(g2.cells()[1].color == kColorKeyFG);
}

// --- fit (port of fit_test.go) -----------------------------------------------

TEST_CASE("clip_text ports FitRect cases", "[ui][fit]") {
  const std::string kAni = "\xE9\x9F\xB3";  // 音

  SECTION("non-positive") {
    // Go FitRect("text", 0, 1) == ""
    CHECK(clip_text("text", 0) == "");
    CHECK(clip_text("text", -3) == "");
    CHECK(clip_text("", 10) == "");
  }

  SECTION("wide runes count 2 and are never split") {
    // Go FitRect("ab音c", 4, 1) == "ab音"
    CHECK(clip_text("ab" + kAni + "c", 4) == "ab" + kAni);
    // 音 would overflow width 3: dropped whole, not split
    CHECK(clip_text("ab" + kAni, 3) == "ab");
  }

  SECTION("ANSI escapes pass through uncounted") {
    // Go FitRect("\x1b[31mabcdef\x1b[0m", 3, 1) == "\x1b[31mabc\x1b[0m"
    CHECK(clip_text("\x1b[31mabcdef\x1b[0m", 3) == "\x1b[31mabc\x1b[0m");
    // Trailing escape after the content limit is still preserved.
    CHECK(clip_text("\x1b[31mabcXYZ\x1b[0m", 3) == "\x1b[31mabc\x1b[0m");
    // Style switch mid-string stays intact.
    CHECK(clip_text("\x1b[31mab\x1b[32mcdef\x1b[0m", 3) == "\x1b[31mab\x1b[32mc\x1b[0m");
  }

  SECTION("result display width never exceeds the limit") {
    const std::string texts[] = {"ab" + kAni + "c", "\x1b[31mabcdef\x1b[0m", "plain text"};
    for (const auto& t : texts) {
      for (int w = 1; w <= 10; ++w) {
        const auto got = clip_text(t, w);
        CHECK(display_width(got) <= w);
      }
    }
  }
}

TEST_CASE("fit_grid clips rows like FitRect height", "[ui][fit]") {
  // Go FitRect("one\ntwo\nthree", 10, 2) == "one\ntwo"
  CellGrid src(3, 10);
  const char* const rows[] = {"one", "two", "three"};
  for (int r = 0; r < 3; ++r) {
    for (std::size_t c = 0; rows[r][c] != '\0'; ++c) {
      src.at(r, static_cast<int>(c)).rune = static_cast<char32_t>(rows[r][c]);
    }
  }
  CellGrid dst(2, 10);
  fit_grid(src, dst);
  CHECK(dump_grid(dst) == "one" + std::string(7, ' ') + "\ntwo" + std::string(7, ' '));
}

TEST_CASE("fit_grid does not pad and leaves dst outside the overlap", "[ui][fit]") {
  CellGrid src(1, 2);
  src.at(0, 0).rune = U'x';
  src.at(0, 1).rune = U'y';
  CellGrid dst(2, 4);
  dst.fill(Cell{U'Z'});
  fit_grid(src, dst);
  CHECK(dump_grid(dst) == "xyZZ\nZZZZ");
}

TEST_CASE("fit_grid with a zero-size dst is a no-op", "[ui][fit]") {
  CellGrid src(2, 2);
  src.fill(Cell{U'x'});
  CellGrid dst(0, 0);
  fit_grid(src, dst);
  CHECK(dump_grid(dst).empty());
}

TEST_CASE("utf_width classifies codepoints", "[ui][fit]") {
  CHECK(utf_width(U'a') == 1);
  CHECK(utf_width(U'\x7F') == 0);        // DEL control
  CHECK(utf_width(U'音') == 2);          // U+97F3 CJK ideograph
  CHECK(utf_width(U'　') == 2);          // U+3000 ideographic space
  CHECK(utf_width(U'Ａ') == 2);          // U+FF21 fullwidth form
  CHECK(utf_width(U'́') == 0);      // combining acute
  CHECK(utf_width(U'​') == 0);      // zero-width space
  CHECK(utf_width(U'\n') == 0);          // control
}

// --- styles (port of styles_test.go) -----------------------------------------

TEST_CASE("contrasting_text_color ports styles_test.go", "[ui][styles]") {
  SECTION("light accent") {
    CHECK(contrasting_text_color("#f7df50") == "#000000");
  }
  SECTION("dark accent") {
    CHECK(contrasting_text_color("#3e4a5e") == "#ffffff");
  }
  SECTION("invalid accent") {
    CHECK(contrasting_text_color("blue") == "#ffffff");
  }
  SECTION("empty string") {
    CHECK(contrasting_text_color("") == "#ffffff");
  }
  SECTION("double hash (second '#' is not a hex digit)") {
    CHECK(contrasting_text_color("##ff0000") == "#ffffff");
  }
  SECTION("no leading hash") {
    // Pure red: linear luminance 0.2126 > 0.179 → black.
    CHECK(contrasting_text_color("ff0000") == "#000000");
  }
  SECTION("uppercase hex") {
    CHECK(contrasting_text_color("#F7DF50") == "#000000");
  }
  SECTION("more than 6 digits overflows ParseUint bitSize 24") {
    CHECK(contrasting_text_color("#1000000") == "#ffffff");
  }
}

TEST_CASE("spec_tag and spec_color thresholds", "[ui][styles]") {
  CHECK(spec_tag(0.0f) == 0);
  CHECK(spec_tag(0.29f) == 0);
  CHECK(spec_tag(0.3f) == 1);   // kSpecTierMid inclusive
  CHECK(spec_tag(0.59f) == 1);
  CHECK(spec_tag(0.6f) == 2);   // kSpecTierHigh inclusive
  CHECK(spec_tag(1.0f) == 2);

  CHECK(spec_color(0.8f) == kColorSpecHigh);
  CHECK(spec_color(0.4f) == kColorSpecMid);
  CHECK(spec_color(0.1f) == kColorSpecLow);
}

TEST_CASE("default_ansi matches cliamp ANSI defaults", "[ui][styles]") {
  CHECK(default_ansi(kColorSpecLow) == 10);   // bright green
  CHECK(default_ansi(kColorSpecMid) == 11);   // bright yellow
  CHECK(default_ansi(kColorSpecHigh) == 9);   // bright red
  CHECK(default_ansi(kColorAccent) == 11);    // bright yellow
  CHECK(default_ansi(kColorMuted) == 7);      // Go ColorDim
  CHECK(default_ansi(kColorTitle) == 10);
  CHECK(default_ansi(kColorText) == 15);
  CHECK(default_ansi(kColorDim) == 7);
  CHECK(default_ansi(kColorPlaying) == 10);
  CHECK(default_ansi(kColorSeekBar) == 11);
  CHECK(default_ansi(kColorVolume) == 2);
  CHECK(default_ansi(kColorError) == 9);
  CHECK(default_ansi(kColorWarning) == 11);
  CHECK(default_ansi(kColorKeyBG) == 8);
  CHECK(default_ansi(kColorKeyFG) == 15);
  CHECK(default_ansi(kColorDefault) == 0);
}

TEST_CASE("padding state ports styles.go", "[ui][styles]") {
  CHECK(padding_h() == 3);
  CHECK(vertical_padding() == 1);
  CHECK(panel_width() == 74);  // 80 - 2*3

  set_padding(4, 2);
  CHECK(padding_h() == 4);
  CHECK(vertical_padding() == 2);
  CHECK(panel_width() == 72);  // 80 - 2*4

  set_padding(3, 1);
  CHECK(panel_width() == 74);
}
