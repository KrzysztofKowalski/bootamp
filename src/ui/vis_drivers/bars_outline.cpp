// ui/vis_drivers/bars_outline.cpp — "BarsOutline" line-graph bars (port of
// cliamp/ui/vis_bars_outline.go renderBarsOutline).
//
// Only the top edge of each bar is drawn, as a horizontal '─' run, with empty
// space above and below — a minimal line-graph style. A band's level crosses
// a row when rowBottom < level < rowTop; that row gets the outline for the
// band. Whole rows carry the spectrum color of the row-bottom tier (Go
// specWrap). Render-only driver: analysis + smoothing are framework-owned;
// cadence follows Go's newFastRenderOnlyDriver(TickAnim) (kTickFast while
// playing).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bootamp::ui::vis_drivers {

namespace {

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

// BarsOutlineDriver — renderBarsOutline: '─' only on the row containing each
// band's peak, one row color per spectrum tier (Go specWrap).
class BarsOutlineDriver : public VisDriver {
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
    const int band_count = static_cast<int>(bands.size());
    for (int row = 0; row < height; ++row) {
      const double row_bottom =
          static_cast<double>(height - 1 - row) / static_cast<double>(height);
      const double row_top =
          static_cast<double>(height - row) / static_cast<double>(height);
      const Color color = spec_color(static_cast<float>(row_bottom));
      int col           = 0;
      for (int i = 0; i < band_count; ++i) {
        const double level = static_cast<double>(bands[static_cast<std::size_t>(i)]);
        const int bw       = vis_band_width(band_count, i, panel_width);
        char32_t cell      = U' ';
        if (level >= row_top) {
          // Fully below the peak — empty inside.
          cell = U' ';
        } else if (level > row_bottom) {
          // This row contains the peak — draw the outline.
          cell = U'─';
        } else {
          // Above the peak — empty.
          cell = U' ';
        }
        for (int w = 0; w < bw; ++w) {
          if (col < panel_width) {
            grid.at(row, col) = Cell{cell, color};
          }
          ++col;
        }
        if (i < band_count - 1) {
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
    // cliamp newFastRenderOnlyDriver(TickAnim): fast while playing, slow when
    // stopped or under an overlay (Go renderOnlyDriver.TickInterval).
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_bars_outline_driver() {
  return std::make_unique<BarsOutlineDriver>();
}

}  // namespace bootamp::ui::vis_drivers
