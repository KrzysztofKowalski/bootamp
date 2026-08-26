// ui/styles.cpp — shared UI style/palette state (port of cliamp/ui/styles.go).
//
// Go keeps mutable package vars (PaddingH, paddingV, PanelWidth) plus the pure
// contrastingTextColor helper. The lipgloss color vars and FrameStyle have no
// C++ equivalent here: colors are palette slots resolved by the FTXUI blit
// (default ANSI mapping in ui/styles.hpp, default_ansi()), and ApplyThemeColors
// belongs to the theme module (post-MVP) which will override at the blit layer.
#include "ui/styles.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace bootamp::ui {

namespace {

// Go styles.go: var PaddingH = 3 / var paddingV = 1 / var PanelWidth = 80 - 2*3.
int g_padding_h = 3;
int g_padding_v = 1;
int g_panel_width = kPanelWidthBase - 2 * g_padding_h;

}  // namespace

int padding_h() noexcept {
  return g_padding_h;
}

int vertical_padding() noexcept {
  return g_padding_v;
}

int panel_width() noexcept {
  return g_panel_width;
}

// Go SetPadding(h, v): updates the frame padding and re-derives PanelWidth.
// The Go FrameStyle re-pad is not ported here — frame composition lives in the
// FTXUI blit layer.
void set_padding(int h, int v) {
  g_padding_h = h;
  g_padding_v = v;
  g_panel_width = kPanelWidthBase - 2 * h;
}

// 1:1 port of Go contrastingTextColor: strconv.ParseUint(strings.TrimPrefix(hex,
// "#"), 16, 24) — any parse failure (non-hex char, empty, or more than 6
// digits, which exceeds bitSize 24) falls back to "#ffffff". Then sRGB linear
// luminance; luminance > 0.179 ⇒ black gives more contrast.
std::string contrasting_text_color(std::string_view hex) {
  std::string_view digits = hex;
  if (!digits.empty() && digits[0] == '#') {
    digits.remove_prefix(1);
  }
  std::uint64_t value = 0;
  if (digits.empty() || digits.size() > 6) {
    return "#ffffff";
  }
  for (char ch : digits) {
    std::uint64_t nibble;
    if (ch >= '0' && ch <= '9') {
      nibble = static_cast<std::uint64_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      nibble = static_cast<std::uint64_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      nibble = static_cast<std::uint64_t>(ch - 'A' + 10);
    } else {
      return "#ffffff";
    }
    value = value * 16 + nibble;
  }

  const auto linear = [](std::uint64_t channel) -> double {
    const double component = static_cast<double>(channel) / 255.0;
    if (component <= 0.04045) {
      return component / 12.92;
    }
    return std::pow((component + 0.055) / 1.055, 2.4);
  };
  const double luminance = 0.2126 * linear(value >> 16) +
                           0.7152 * linear((value >> 8) & 0xFF) +
                           0.0722 * linear(value & 0xFF);
  return luminance > 0.179 ? "#000000" : "#ffffff";
}

}  // namespace bootamp::ui
