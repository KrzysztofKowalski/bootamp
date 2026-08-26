// ui/vis_drivers/logo.cpp — "CLIAMP" pixel-art logo (port of cliamp/ui/vis_logo.go).
//
// The 5×7 pixel glyphs for C/L/I/A/M/P are stamped into a 2x-dot-density
// braille grid. Individual dots appear/disappear with the associated frequency
// band's energy — loud passages fill the text solid, silence dissolves it into
// scattered pixels (the shared scatterHash gate). A gentle sine wave and an
// energy bounce keep the letters alive. Row-bottom tiers pick the spectrum
// color per line, like Go's specWrap. This is a renderOnly-style driver: the
// framework drives analysis + smoothing and passes the smoothed bands in.
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bootamp::ui::vis_drivers {

namespace {

// cliamp vis_logo.go layout constants.
constexpr int kLogoLetterW    = 5;  // pixel columns per glyph
constexpr int kLogoLetterH    = 7;  // pixel rows per glyph
constexpr int kLogoNumLetters = 6;
constexpr int kLogoGap        = 2;  // pixel gap between letters
// Total pixel width: 6*5 + 5*2 = 40.
constexpr int kLogoTotalW = kLogoNumLetters * kLogoLetterW + (kLogoNumLetters - 1) * kLogoGap;

// logoGlyphs holds 5×7 pixel bitmaps for each letter in "CLIAMP". Each row is
// 5 bits wide; bit 4 (0x10) is the leftmost pixel (Go vis_logo.go).
constexpr std::array<std::array<std::uint8_t, kLogoLetterH>, kLogoNumLetters> kLogoGlyphs = {{
    {{0x0E, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0E}},  // C
    {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},  // L
    {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},  // I
    {{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},  // A
    {{0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}},  // M
    {{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},  // P
}};

// brailleBit maps (row, col) in the 4×2 Braille dot grid to its bit value
// (Go brailleBit). Or-ed into U+2800 to form the final codepoint.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// scatterHash returns a pseudo-random value in [0, 1) for a given dot position
// and frame (Go scatterHash). Computed in double like the Go original so the
// dot gate matches bit-for-bit; dots persist for a few frames (frame/3
// staggering) to create a twinkling effect.
double scatter_hash(int band, int row, int col, std::uint64_t frame) {
  const std::uint64_t f = (frame + static_cast<std::uint64_t>(row * 3 + col)) / 3;
  std::uint64_t h = static_cast<std::uint64_t>(band) * 7919 +
                    static_cast<std::uint64_t>(row) * 6271 +
                    static_cast<std::uint64_t>(col) * 3037 + f * 104729;
  h ^= h >> 16;
  h *= 0x45d9f3b37197344bULL;
  h ^= h >> 16;
  return static_cast<double>(h % 10000) / 10000.0;
}

// LogoDriver — renderOnly-style: analysis + smoothing are driven by the
// framework; tick() is a no-op and tick_interval follows cliamp's
// defaultDriverTickInterval (fast while playing, slow otherwise).
class LogoDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height      = grid.rows();
    const int panel_width = grid.cols();
    if (height <= 0 || panel_width <= 0) {
      return;
    }
    const int dot_rows = height * 4;
    const int dot_cols = panel_width * 2;

    std::vector<bool> dots(static_cast<std::size_t>(dot_rows) * dot_cols);

    // Scale letters to fill the panel (75% of height for bounce headroom).
    int scale_x = dot_cols / kLogoTotalW;
    int scale_y = (dot_rows * 3 / 4) / kLogoLetterH;
    scale_x     = std::max(scale_x, 1);
    scale_y     = std::max(scale_y, 1);

    const int rendered_w    = kLogoTotalW * scale_x;
    const int rendered_h    = kLogoLetterH * scale_y;
    const int offset_x      = (dot_cols - rendered_w) / 2;
    const int base_offset_y = (dot_rows - rendered_h) / 2;

    // Map 6 letters across the 10 frequency bands (Go letterBand).
    static constexpr std::array<int, kLogoNumLetters> kLetterBand = {0, 2, 4, 5, 7, 9};
    const auto band_at = [&bands](int idx) -> double {
      if (idx < 0 || static_cast<std::size_t>(idx) >= bands.size()) {
        return 0.0;
      }
      return static_cast<double>(bands[static_cast<std::size_t>(idx)]);
    };

    for (int li = 0; li < kLogoNumLetters; ++li) {
      const double energy = band_at(kLetterBand[static_cast<std::size_t>(li)]);

      // Gentle traveling wave for life during silence + subtle bounce.
      const double wave =
          std::sin(static_cast<double>(frame) * 0.06 + static_cast<double>(li) * 0.9) * 1.5;
      const int bounce =
          static_cast<int>(energy * static_cast<double>(base_offset_y) * 0.3 + wave);

      const int letter_x = offset_x + li * (kLogoLetterW + kLogoGap) * scale_x;
      const int letter_y = base_offset_y - bounce;

      for (int py = 0; py < kLogoLetterH; ++py) {
        const std::uint8_t row = kLogoGlyphs[static_cast<std::size_t>(li)]
                                            [static_cast<std::size_t>(py)];
        for (int px = 0; px < kLogoLetterW; ++px) {
          if ((row & (1u << (kLogoLetterW - 1 - px))) == 0) {
            continue;
          }
          // Stamp each glyph pixel as a scaled block of dots. Each dot's
          // visibility is gated by energy — loud fills the text solid,
          // silence dissolves it to scattered pixels (Go fill).
          const double fill = energy * energy * 0.75 + 0.15;
          for (int sy = 0; sy < scale_y; ++sy) {
            for (int sx = 0; sx < scale_x; ++sx) {
              const int dx = letter_x + px * scale_x + sx;
              const int dy = letter_y + py * scale_y + sy;
              if (dx < 0 || dx >= dot_cols || dy < 0 || dy >= dot_rows) {
                continue;
              }
              if (scatter_hash(li, py * scale_y + sy, px * scale_x + sx, frame) > fill) {
                continue;
              }
              dots[static_cast<std::size_t>(dy) * dot_cols + dx] = true;
            }
          }
        }
      }
    }

    // Convert the dot grid to Braille characters; each terminal line carries
    // the spectrum color of its row-bottom tier (Go specWrap).
    for (int row = 0; row < height; ++row) {
      const float row_bottom =
          static_cast<float>(height - 1 - row) / static_cast<float>(height);
      const Color color = spec_color(row_bottom);
      for (int ch = 0; ch < panel_width; ++ch) {
        std::uint32_t braille = 0x2800;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            if (dots[static_cast<std::size_t>(row * 4 + dr) * dot_cols + ch * 2 + dc]) {
              braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                     [static_cast<std::size_t>(dc)];
            }
          }
        }
        grid.set(row, ch, Cell{static_cast<char32_t>(braille), color});
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // Go model.tickInterval: band/spectrum modes run at TickFast (50ms =
    // kTickSpectrum) while playing; slow when stopped or under an overlay.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickSpectrum;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_logo_driver() {
  return std::make_unique<LogoDriver>();
}

}  // namespace bootamp::ui::vis_drivers
