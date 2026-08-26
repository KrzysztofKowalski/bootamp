// ui/fit.cpp — frame fitting (port of cliamp/ui/fit.go).
//
// Go FitRect clips text to a terminal rectangle without splitting ANSI escapes
// or wide characters, and intentionally does not pad rows. bootamp splits that
// into two paths:
//   * fit_grid  — the CellGrid blit path: src is clipped into dst's top-left
//     rectangle (no padding — dst cells outside the overlap keep their
//     previous content, matching Go's compact no-trailing-whitespace layout).
//   * clip_text — single-line status truncation with Go ansi.Truncate
//     semantics: wide runes count 2 and are never split, combining marks count
//     0, and ANSI CSI/OSC escapes pass through verbatim without counting.
#include "ui/fit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bootamp::ui {

namespace {

// Display width of one codepoint (subset of wcwidth covering everything the
// cliamp visualizers emit): 2 for East Asian wide/fullwidth, 0 for combining
// marks / zero-width format chars / control chars, else 1.
int width_of(char32_t cp) {
  if (cp == 0) {
    return 0;
  }
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
    return 0;  // C0/C1 controls
  }
  // Combining marks and zero-width format characters.
  if ((cp >= 0x0300 && cp <= 0x036F) ||   // Combining Diacritical Marks
      (cp >= 0x1AB0 && cp <= 0x1AFF) ||   // Combining Diacritical Marks Extended
      (cp >= 0x1DC0 && cp <= 0x1DFF) ||   // Combining Diacritical Marks Supplement
      (cp >= 0x20D0 && cp <= 0x20FF) ||   // Combining Marks for Symbols
      (cp >= 0xFE00 && cp <= 0xFE0F) ||   // Variation Selectors
      (cp >= 0xFE20 && cp <= 0xFE2F) ||   // Combining Half Marks
      cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x200E ||
      cp == 0x200F || cp == 0x2060 || cp == 0xFEFF) {
    return 0;
  }
  // East Asian Wide / Fullwidth.
  if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
      (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK Radicals .. CJK Symbols & Punctuation
      (cp >= 0x3041 && cp <= 0x33FF) ||   // Hiragana .. CJK Compatibility
      (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Extension A
      (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
      (cp >= 0xA000 && cp <= 0xA4CF) ||   // Yi Syllables
      (cp >= 0xA960 && cp <= 0xA97F) ||   // Hangul Jamo Extended-A
      (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul Syllables
      (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
      (cp >= 0xFE30 && cp <= 0xFE4F) ||   // CJK Compatibility Forms
      (cp >= 0xFF00 && cp <= 0xFF60) ||   // Fullwidth Forms
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // Fullwidth Signs
      (cp >= 0x20000 && cp <= 0x2FFFD) || // CJK Extensions B-F
      (cp >= 0x30000 && cp <= 0x3FFFD)) {
    return 2;
  }
  return 1;
}

// Decode the UTF-8 codepoint starting at s[i]; advances i past it. Invalid
// sequences decode as U+FFFD and advance one byte.
char32_t decode_utf8(std::string_view s, std::size_t& i) {
  const auto b0 = static_cast<std::uint8_t>(s[i]);
  if (b0 < 0x80) {
    ++i;
    return b0;
  }
  int      len;
  char32_t cp;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
  } else {
    ++i;
    return 0xFFFD;
  }
  if (i + static_cast<std::size_t>(len) > s.size()) {
    ++i;
    return 0xFFFD;
  }
  for (int k = 1; k < len; ++k) {
    const auto b = static_cast<std::uint8_t>(s[i + static_cast<std::size_t>(k)]);
    if ((b & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  i += static_cast<std::size_t>(len);
  if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
      (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    return 0xFFFD;  // overlong / out of range / surrogate
  }
  return cp;
}

// Copy one ANSI escape sequence verbatim starting at s[i] ('\x1b') and advance
// i past it. Handles CSI (ESC [ ... final byte 0x40-0x7E), OSC (ESC ] ...
// BEL or ST) and single-byte escapes (ESC + one char). A sequence truncated by
// end-of-string is copied as-is (edge case; Go drops unterminated sequences —
// see deviations note in test_cell.cpp if this ever matters).
void copy_ansi(std::string_view s, std::size_t& i, std::string& out) {
  const std::size_t n = s.size();
  out.push_back('\x1b');
  ++i;
  if (i >= n) {
    return;
  }
  const auto b = static_cast<std::uint8_t>(s[i]);
  if (b == '[') {  // CSI
    out.push_back('[');
    ++i;
    while (i < n) {
      const auto c = static_cast<std::uint8_t>(s[i]);
      out.push_back(static_cast<char>(c));
      ++i;
      if (c >= 0x40 && c <= 0x7E) {
        break;  // final byte
      }
    }
  } else if (b == ']') {  // OSC
    out.push_back(']');
    ++i;
    while (i < n) {
      const auto c = static_cast<std::uint8_t>(s[i]);
      out.push_back(static_cast<char>(c));
      ++i;
      if (c == '\x07') {
        break;  // BEL terminator
      }
      if (c == '\x1b' && i < n && s[i] == '\\') {  // ST terminator
        out.push_back('\\');
        ++i;
        break;
      }
    }
  } else {  // ESC + single char (e.g. ESC 7 save cursor)
    out.push_back(static_cast<char>(b));
    ++i;
  }
}

}  // namespace

void fit_grid(const CellGrid& src, CellGrid& dst) {
  const int rows = std::min(src.rows(), dst.rows());
  const int cols = std::min(src.cols(), dst.cols());
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      dst.at(r, c) = src.at(r, c);
    }
  }
  // No padding: dst cells outside the clipped rectangle keep their previous
  // content (Go FitRect does not pad rows either).
}

std::string clip_text(std::string_view text, int width) {
  if (width <= 0 || text.empty()) {
    return "";
  }
  std::string out;
  out.reserve(text.size());
  int         used = 0;
  bool        full = false;
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    if (static_cast<std::uint8_t>(text[i]) == 0x1B) {
      copy_ansi(text, i, out);  // escapes never count toward width
      continue;
    }
    const std::size_t start = i;
    const char32_t    cp = decode_utf8(text, i);
    if (full) {
      continue;  // drop content past the limit; keep scanning for escapes
    }
    const int w = width_of(cp);
    if (w == 0) {
      out.append(text.substr(start, i - start));  // combining mark attaches
      continue;
    }
    if (used + w > width) {
      full = true;  // wide rune that does not fit is dropped, not split
      continue;
    }
    out.append(text.substr(start, i - start));
    used += w;
  }
  return out;
}

int utf_width(char32_t cp) noexcept {
  return width_of(cp);
}

}  // namespace bootamp::ui
