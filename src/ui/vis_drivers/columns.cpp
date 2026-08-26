// ui/vis_drivers/columns.cpp — "Columns" dense interpolated columns (port of
// cliamp/ui/vis_columns.go renderColumns).
//
// Each band spans visBandWidth single-character-wide columns; levels are
// linearly interpolated between neighboring bands (Go interpolateBandColumns)
// so adjacent columns vary slightly for a dense, organic look. Each column
// renders the fractional Unicode block for its interpolated level within the
// row's [rowBottom, rowTop) span (Go fracBlock); whole rows carry the
// spectrum color of the row-bottom tier (Go specWrap). Render-only driver:
// analysis + smoothing are framework-owned; cadence follows Go's
// newFastRenderOnlyDriver(TickAnim) (kTickFast while playing).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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

// interpolateBandColumns builds per-column levels by interpolating between
// neighboring bands (Go interpolateBandColumns).
std::vector<double> interpolate_band_columns(std::span<const float> bands,
                                             const std::vector<int>& band_cols) {
  int total_cols = 0;
  for (const int width : band_cols) {
    total_cols += width;
  }
  std::vector<double> cols(static_cast<std::size_t>(total_cols));
  int offset = 0;
  for (std::size_t b = 0; b < bands.size(); ++b) {
    const int width = band_cols[b];
    if (width <= 0) {
      continue;
    }
    const double level = static_cast<double>(bands[b]);
    double next_level  = level;
    if (b + 1 < bands.size()) {
      next_level = static_cast<double>(bands[b + 1]);
    }
    for (int c = 0; c < width; ++c) {
      const double t = static_cast<double>(c) / static_cast<double>(width);
      cols[static_cast<std::size_t>(offset + c)] = level * (1.0 - t) + next_level * t;
    }
    offset += width;
  }
  return cols;
}

// ColumnsDriver — renderColumns: per-column interpolated levels, one row
// color per spectrum tier (Go specWrap).
class ColumnsDriver : public VisDriver {
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

    // Per-band column counts; cols below is a flat level per display column.
    std::vector<int> band_cols(static_cast<std::size_t>(band_count));
    for (int b = 0; b < band_count; ++b) {
      band_cols[static_cast<std::size_t>(b)] = vis_band_width(band_count, b, panel_width);
    }
    const std::vector<double> cols = interpolate_band_columns(bands, band_cols);

    for (int row = 0; row < height; ++row) {
      const double row_bottom =
          static_cast<double>(height - 1 - row) / static_cast<double>(height);
      const double row_top =
          static_cast<double>(height - row) / static_cast<double>(height);
      const Color color = spec_color(static_cast<float>(row_bottom));
      int offset        = 0;
      int col           = 0;
      for (int b = 0; b < band_count; ++b) {
        for (int c = 0; c < band_cols[static_cast<std::size_t>(b)]; ++c) {
          const char32_t block =
              frac_block(cols[static_cast<std::size_t>(offset + c)],
                         row_bottom, row_top);
          if (col < panel_width) {
            grid.at(row, col) = Cell{block, color};
          }
          ++col;
        }
        offset += band_cols[static_cast<std::size_t>(b)];
        if (b < band_count - 1) {
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

std::unique_ptr<VisDriver> make_columns_driver() {
  return std::make_unique<ColumnsDriver>();
}

}  // namespace bootamp::ui::vis_drivers
