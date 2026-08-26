// ui/styles.hpp — shared UI style/palette helpers.
//
// Port of cliamp/ui/styles.go. The Go version caches lipgloss styles + raw ANSI
// prefix/suffix for the 3-tier spectrum colors (specLow/Mid/High) so per-frame
// rendering avoids a fresh lipgloss.Render allocation. bootamp's CellGrid uses
// Color palette slots instead, so this header exposes the palette mapping +
// the spectrum-tier thresholds the drivers share.
#pragma once

#include "ui/cell.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace bootamp::ui {

// Spectrum color tiers (cliamp specTag thresholds). A band level ≥0.6 ⇒ high,
// ≥0.3 ⇒ mid, else low. Drivers pick the tier from the row-bottom level.
inline constexpr float kSpecTierHigh = 0.6f;
inline constexpr float kSpecTierMid  = 0.3f;

// spec_tag returns 0/1/2 for the spectrum color tier at `norm` level.
inline int spec_tag(float norm) {
  if (norm >= kSpecTierHigh) return 2;
  if (norm >= kSpecTierMid)  return 1;
  return 0;
}

// Palette slots for the visualizer. Drivers write these; the FTXUI blit resolves
// them to ftxui::Color via the active theme.
inline constexpr Color kColorSpecLow  = 1;
inline constexpr Color kColorSpecMid  = 2;
inline constexpr Color kColorSpecHigh = 3;
inline constexpr Color kColorAccent    = 4;
inline constexpr Color kColorMuted     = 5;

// spec_color returns the palette slot for a given row-bottom level.
inline Color spec_color(float row_bottom) {
  switch (spec_tag(row_bottom)) {
    case 2:  return kColorSpecHigh;
    case 1:  return kColorSpecMid;
    default: return kColorSpecLow;
  }
}

// Remaining semantic slots (cliamp/styles.go palette). kColorBackground is the
// default slot 0 (Go ColorBackground = nil, i.e. transparent).
inline constexpr Color kColorTitle   = 6;  // Go ColorTitle   (ANSI 10)
inline constexpr Color kColorText    = 7;  // Go ColorText    (ANSI 15)
inline constexpr Color kColorDim     = 8;  // Go ColorDim     (ANSI 7)
inline constexpr Color kColorPlaying = 9;  // Go ColorPlaying (ANSI 10)
inline constexpr Color kColorSeekBar = 10; // Go ColorSeekBar (ANSI 11)
inline constexpr Color kColorVolume  = 11; // Go ColorVolume  (ANSI 2)
inline constexpr Color kColorError   = 12; // Go ColorError   (ANSI 9)
inline constexpr Color kColorWarning = 13; // Go ColorWarning (ANSI 11)
inline constexpr Color kColorKeyBG   = 14; // Go ColorKeyBG   (ANSI 8)
inline constexpr Color kColorKeyFG   = 15; // Go ColorKeyFG   (ANSI 15)

// default_ansi returns the default ANSI color index (0-15) for a palette slot —
// the cliamp/styles.go lipgloss.ANSIColor defaults. The FTXUI blit resolves
// slots with these when the theme is default; a themed override replaces them
// at the blit layer. kColorMuted maps to Go ColorDim (ANSI 7, light gray).
inline std::uint8_t default_ansi(Color slot) {
  switch (slot) {
    case kColorSpecLow:  return 10;  // bright green
    case kColorSpecMid:  return 11;  // bright yellow
    case kColorSpecHigh: return 9;   // bright red
    case kColorAccent:   return 11;  // bright yellow
    case kColorMuted:    return 7;   // light gray (Go ColorDim)
    case kColorTitle:    return 10;  // bright green
    case kColorText:     return 15;  // bright white
    case kColorDim:      return 7;   // light gray
    case kColorPlaying:  return 10;  // bright green
    case kColorSeekBar:  return 11;  // bright yellow
    case kColorVolume:   return 2;   // green
    case kColorError:    return 9;   // bright red
    case kColorWarning:  return 11;  // bright yellow
    case kColorKeyBG:    return 8;   // bright black
    case kColorKeyFG:    return 15;  // bright white
    default:             return 0;   // default foreground (incl. background)
  }
}

// Frame padding state (cliamp/styles.go PaddingH / paddingV / PanelWidth).
inline constexpr int kPanelWidthBase = 80;  // Go PanelWidth = 80 - 2*PaddingH

// set_padding updates the frame padding and re-derives panel_width
// (Go SetPadding; the FrameStyle re-pad happens in the FTXUI blit layer).
void set_padding(int h, int v);
int  padding_h() noexcept;          // Go PaddingH (default 3)
int  vertical_padding() noexcept;   // Go VerticalPadding() (default 1)
int  panel_width() noexcept;        // Go PanelWidth (default 80 - 2*3 = 74)

// contrasting_text_color returns "#000000" or "#ffffff" — the color with better
// contrast on top of the given #rrggbb accent (1:1 port of Go
// contrastingTextColor: sRGB relative luminance, crossover 0.179). Inputs that
// are not 1-6 hex digits (after one optional leading '#') fall back to white.
std::string contrasting_text_color(std::string_view hex);

}  // namespace bootamp::ui