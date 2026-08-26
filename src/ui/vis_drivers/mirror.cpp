// ui/vis_drivers/mirror.cpp — Braille spectrum bars mirrored about a
// horizontal axis (port of cliamp/ui/vis_mirror.go).
//
// One vertical bar per spectrum slot grows symmetrically around a persistent
// horizontal axis. Braille subcells preserve the taper and narrow gaps in a
// small terminal panel. Each bar's amplitude combines the band environment
// level and a per-bar wobble; bar tips tier to the high color. The whole
// output is tier-colored per cell (the shared brailleGrid rasteriser in Go
// uses max dot tier per Braille cell; empty cells render as empty Braille in
// the low tier), unlike the row-tier coloring of the wave/scope drivers.
//
// Render-only driver: the Go original is a fast renderOnly driver with
// spectrumAnalysisSpec(DefaultSpectrumBands) and TickAnim; the framework
// drives analysis + smoothing and passes the smoothed bands, so tick() is a
// no-op.
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

// mirrorSpanPercent (Go): fraction of the dot width covered by the bars.
inline constexpr int kMirrorSpanPercent = 84;

// brailleBit maps (row, col) in the 4×2 Braille dot grid to its bit value
// (Go brailleBit). Or-ed into U+2800 to form the final codepoint.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// BrailleGrid is a 4×2 dot-per-cell rasteriser (cliamp brailleGrid, the
// subset mirror uses: ensure/set with max-tier merging). ensure() clears the
// previous frame, so the grid is purely per-render state; kept file-local —
// other drivers that share it in Go (geyser, sand, ...) replicate it.
class BrailleGrid {
public:
  void ensure(int rows, int cols) {
    if (rows == dot_rows_ && cols == dot_cols_ &&
        cells_.size() == static_cast<std::size_t>(rows) * cols) {
      std::fill(cells_.begin(), cells_.end(), 0);
      return;
    }
    cells_.assign(static_cast<std::size_t>(rows) * cols, 0);
    dot_rows_ = rows;
    dot_cols_ = cols;
  }

  void set(int x, int y, std::int8_t tier) {
    if (x < 0 || x >= dot_cols_ || y < 0 || y >= dot_rows_) {
      return;  // out of range — drop
    }
    std::int8_t& cell =
        cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(dot_cols_) + x];
    if (tier > cell) {
      cell = tier;
    }
  }

  std::int8_t at(int x, int y) const {
    return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(dot_cols_) + x];
  }

private:
  std::vector<std::int8_t> cells_;
  int                      dot_rows_ = 0;
  int                      dot_cols_ = 0;
};

// MirrorDriver (cliamp renderMirror).
class MirrorDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // Go spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {
    // Render-only: the framework runs analysis + smoothing (Go
    // defaultDriverTick).
  }

  std::chrono::milliseconds
  tick_interval(const VisTickContext& ctx) const override {
    // Go model.tickInterval: band/spectrum modes run at TickFast (50ms =
    // kTickSpectrum) while playing; slow when stopped or under an overlay.
    if (ctx.overlay_active) {
      return kTickSlow;
    }
    if (ctx.playing) {
      return kTickSpectrum;
    }
    return kTickSlow;
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height = grid.rows();
    const int width  = grid.cols();
    if (height <= 0 || width <= 0) {
      return;
    }
    const int dot_rows = height * 4;
    const int dot_cols = width * 2;
    // Go: span = max(2, dotCols*84/100); span = min(dotCols, span - span%2).
    int span = std::max(2, dot_cols * kMirrorSpanPercent / 100);
    span     = std::min(dot_cols, span - span % 2);
    const int bar_count = std::max(1, span / 2);
    const int x0        = (dot_cols - span) / 2;
    const int axis_y    = dot_rows / 2;
    const int max_radius = std::min(axis_y, dot_rows - 1 - axis_y);

    grid_.ensure(dot_rows, dot_cols);
    for (int x = x0; x < x0 + span; ++x) {
      grid_.set(x, axis_y, 1);  // the persistent horizontal axis
    }

    double env = 0.0;
    for (const float level : bands) {
      env += std::max(0.0, std::min(1.0, static_cast<double>(level)));
    }
    if (!bands.empty()) {
      env /= static_cast<double>(bands.size());
    }

    // Go: t := float64(v.Frame()) * TickAnim.Seconds(). The C++ contract's
    // ~30 FPS kTickAnim replaces Go's 16ms TickAnim, so the wobble phase
    // advances faster per frame (see report).
    const double t =
        static_cast<double>(frame) * std::chrono::duration<double>(kTickAnim).count();
    const double half_bars = static_cast<double>(bar_count - 1) / 2.0;
    for (int i = 0; i < bar_count; ++i) {
      double distance = 0.0;
      if (half_bars > 0) {
        distance = std::abs(static_cast<double>(i) - half_bars) / half_bars;
      }
      const double wobble = 0.4 + 0.6 * std::abs(
          std::sin(t * 4.6 + static_cast<double>(i) * 0.42) *
          std::sin(t * 1.9 - static_cast<double>(i) * 0.13));
      const double amplitude =
          static_cast<double>(dot_rows) * 0.80 * (1.0 - distance * 0.55) *
          (0.3 + 0.7 * env) * (0.35 + 0.65 * wobble);
      const int radius = std::min(max_radius,
                                  std::max(1, static_cast<int>(std::round(amplitude))));
      const int x      = x0 + i * 2 + 1;

      for (int y = axis_y - radius; y <= axis_y + radius; ++y) {
        std::int8_t tier = 2;
        int         distance_to_axis = y - axis_y;
        if (distance_to_axis < 0) {
          distance_to_axis = -distance_to_axis;
        }
        if (static_cast<double>(distance_to_axis) / static_cast<double>(radius) >=
            0.75) {
          tier = 3;
        }
        grid_.set(x, y, tier);
      }
    }

    // Compose one Braille glyph per character cell, colored by the cell's
    // max dot tier (Go brailleGrid.render; empty cells render empty Braille
    // in the low tier).
    for (int row = 0; row < height; ++row) {
      for (int ch = 0; ch < width; ++ch) {
        std::uint32_t braille = 0x2800;
        int           cell_tag = -1;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const std::int8_t tier = grid_.at(ch * 2 + dc, row * 4 + dr);
            if (tier == 0) {
              continue;
            }
            braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                  [static_cast<std::size_t>(dc)];
            cell_tag = std::max(cell_tag, static_cast<int>(tier) - 1);
          }
        }
        if (cell_tag < 0) {
          cell_tag = 0;
        }
        Color color = kColorSpecLow;
        if (cell_tag == 2) {
          color = kColorSpecHigh;
        } else if (cell_tag == 1) {
          color = kColorSpecMid;
        }
        grid.set(row, ch, Cell{static_cast<char32_t>(braille), color});
      }
    }
  }

private:
  BrailleGrid grid_;
};

}  // namespace

std::unique_ptr<VisDriver> make_mirror_driver() {
  return std::make_unique<MirrorDriver>();
}

}  // namespace bootamp::ui::vis_drivers
