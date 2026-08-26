// ui/vis_drivers/bars.cpp — "Bars" smooth spectrum bars (port of
// cliamp/ui/vis_bars.go renderBars).
//
// Per-row geometry: each band gets visBandWidth columns (the panel width is
// distributed across the visible bands, with one gap column between bands);
// each cell holds the fractional Unicode block for the band level within the
// row's [rowBottom, rowTop) span (Go fracBlock / barBlocks). The whole row
// carries the spectrum color of its row-bottom tier (Go specWrap). This is a
// render-only driver: analysis + smoothing are framework-owned (tick() is a
// no-op) and the cadence follows Go's newFastRenderOnlyDriver(TickAnim):
// fast while playing, slow when stopped or under an overlay. The C++ mapping
// of Go's TickAnim (16 ms) is kTickFast (16 ms, "~60 FPS").
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

// cliamp barBlocks: 9 fractional block levels (space .. full block).
constexpr std::array<char32_t, 9> kBarBlocks = {
    U' ', U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};

// fracBlock returns the fractional Unicode block for a level within the row
// span [rowBottom, rowTop] (Go visualizer.go fracBlock). Computed in double
// like the Go original so the golden output matches.
char32_t frac_block(double level, double row_bottom, double row_top) {
  if (level >= row_top) {
    return U'█';
  }
  if (level > row_bottom) {
    const double frac =
        (level - row_bottom) / (row_top - row_bottom);
    int idx = static_cast<int>(frac *
                               static_cast<double>(kBarBlocks.size() - 1));
    idx = std::max(0, std::min(idx,
                               static_cast<int>(kBarBlocks.size()) - 1));
    return kBarBlocks[static_cast<std::size_t>(idx)];
  }
  return U' ';
}

// visBandWidth returns the character width of band b across panel_width
// columns (Go visualizer.go visBandWidth). At narrow widths only the leading
// visible bands receive columns.
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

// BarsDriver — renderBars: fractional Unicode blocks per band, one row color
// per spectrum tier (Go specWrap).
class BarsDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t,
              CellGrid& grid) override {
    const int height       = grid.rows();
    const int panel_width  = grid.cols();
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
        const int bw = vis_band_width(band_count, i, panel_width);
        const char32_t block =
            frac_block(static_cast<double>(bands[static_cast<std::size_t>(i)]),
                       row_bottom, row_top);
        for (int w = 0; w < bw; ++w) {
          if (col < panel_width) {
            grid.at(row, col) = Cell{block, color};
          }
          ++col;
        }
        if (i < band_count - 1) {
          // Inter-band gap inherits the row color (Go writes it inside the
          // specWrap'd body). Out-of-bounds gaps are dropped, like Go's
          // fitVisualizerFrame clipping.
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

std::unique_ptr<VisDriver> make_bars_driver() {
  return std::make_unique<BarsDriver>();
}

}  // namespace bootamp::ui::vis_drivers
