// ui/vis_drivers/bars_dot.cpp — "BarsDot" Braille-dot bars (port of
// cliamp/ui/vis_bars_dot.go renderBarsDot).
//
// Each terminal cell maps to a 4x2 Braille dot grid; dots fill from the bottom
// up proportionally to the band level, giving a stippled texture. One row
// color per spectrum tier: the whole line is one styled run in Go (the tag is
// constant per row because norm == rowBottom), so every cell — braille chars
// and inter-band gaps alike — carries spec_color(rowBottom). Render-only
// driver: analysis + smoothing are framework-owned; cadence follows Go's
// newFastRenderOnlyDriver(TickAnim) (kTickFast while playing).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bootamp::ui::vis_drivers {

namespace {

// brailleBit maps (row, col) in a 4x2 Braille dot grid to its bit value
// (Go brailleBit). Or-ed into U+2800 to form the final codepoint.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// visBandWidth returns the character width of band b across panel_width
// columns (Go visualizer.go visBandWidth).
int vis_band_width(int total_bands, int b, int panel_width) {
  if (total_bands <= 0 || b < 0 || b >= total_bands || panel_width <= 0) {
    return 0;
  }
  const int visible_bands = std::min(total_bands, panel_width);
  if (b >= visible_bands) {
    return 0;
  }
  const int gap_count =
      std::min(visible_bands - 1, std::max(0, panel_width - visible_bands));
  const int band_cols = panel_width - gap_count;
  const int base      = band_cols / visible_bands;
  const int extra     = band_cols % visible_bands;
  return b < extra ? base + 1 : base;
}

// BarsDotDriver — renderBarsDot: per-cell braille pattern, one row color per
// spectrum tier (Go specWrap via flushStyleRun).
class BarsDotDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t,
              CellGrid& grid) override {
    const int height      = grid.rows();
    const int panel_width = grid.cols();
    if (height <= 0 || panel_width <= 0) {
      return;
    }
    const int dot_rows    = height * 4;
    const int band_count  = static_cast<int>(bands.size());
    for (int row = 0; row < height; ++row) {
      // norm == rowBottom: the whole row is one style run in Go, so every
      // cell (dots and gaps) gets the row-bottom tier color.
      const double norm =
          static_cast<double>(height - 1 - row) / static_cast<double>(height);
      const Color color = spec_color(static_cast<float>(norm));
      int col           = 0;
      for (int b = 0; b < band_count; ++b) {
        const int chars = vis_band_width(band_count, b, panel_width);
        for (int c = 0; c < chars; ++c) {
          std::uint32_t braille = 0x2800;
          for (int dr = 0; dr < 4; ++dr) {
            for (int dc = 0; dc < 2; ++dc) {
              const int dot_row = row * 4 + dr;
              // Invert: bars grow from the bottom (Go dotY).
              const double dot_y =
                  static_cast<double>(dot_rows - 1 - dot_row) /
                  static_cast<double>(dot_rows);
              if (dot_y < static_cast<double>(bands[static_cast<std::size_t>(b)])) {
                braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                      [static_cast<std::size_t>(dc)];
              }
            }
          }
          if (col < panel_width) {
            grid.at(row, col) = Cell{static_cast<char32_t>(braille), color};
          }
          ++col;
        }
        if (b < band_count - 1) {
          // Gap character inherits the current style run (Go run.WriteByte).
          if (col < panel_width) {
            grid.at(row, col) = Cell{U' ', color};
          }
          ++col;
        }
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&,
            std::span<const float>) override {}

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

std::unique_ptr<VisDriver> make_bars_dot_driver() {
  return std::make_unique<BarsDotDriver>();
}

}  // namespace bootamp::ui::vis_drivers
