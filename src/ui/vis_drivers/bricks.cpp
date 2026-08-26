// ui/vis_drivers/bricks.cpp — solid block columns with visible gaps (port of
// cliamp/ui/vis_bricks.go renderBricks).
//
// Each band column is a vertical stack of half-height blocks (▄) — one brick
// per terminal row — separated by blank rows, so the total height matches the
// bars visualizer. A brick is drawn only when the band level exceeds the row's
// threshold; the whole line carries the spectrum color of its row-bottom tier
// (Go specWrap). Fast render-only driver: analysis + smoothing are driven by
// the framework; tick() is a no-op and the cadence is kTickAnim while playing
// (Go newFastRenderOnlyDriver with TickAnim).
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

// vis_band_width returns the character width for band b in a panel of
// panel_width cells (Go visBandWidth). At narrow widths only the leading
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

// BricksDriver — renderOnly-style: the framework drives analysis + smoothing
// and passes the smoothed bands in.
class BricksDriver : public VisDriver {
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
    if (band_count <= 0) {
      return;  // Go renders empty rows; the grid is already space-filled
    }

    for (int row = 0; row < height; ++row) {
      // Go renderBricks: rowThreshold = (height-1-row)/height; the whole line
      // gets the spectrum color of that tier (Go specWrap).
      const double row_threshold =
          static_cast<double>(height - 1 - row) / static_cast<double>(height);
      const Color color = spec_color(static_cast<float>(row_threshold));

      int col = 0;
      for (int b = 0; b < band_count; ++b) {
        const int bw =
            vis_band_width(band_count, b, panel_width);
        const double level = static_cast<double>(bands[static_cast<std::size_t>(b)]);
        for (int k = 0; k < bw; ++k) {
          grid.set(row, col,
              Cell{level > row_threshold ? U'▄' : U' ', color});
          ++col;
        }
        if (b < band_count - 1) {  // inter-band gap (Go writes a space)
          grid.set(row, col, Cell{U' ', color});
          ++col;
        }
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // Go newFastRenderOnlyDriver(TickAnim): TickAnim while playing without an
    // overlay, otherwise the slow cadence.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickAnim;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_bricks_driver() {
  return std::make_unique<BricksDriver>();
}

}  // namespace bootamp::ui::vis_drivers
