// ui/vis_drivers/scatter.cpp — twinkling particle field (port of
// cliamp/ui/vis_scatter.go).
//
// Dot density per band is proportional to the squared energy level, with a
// gravity bias that makes particles denser near the bottom. This is a
// renderOnly-style driver: the framework drives analysis + smoothing and
// passes the smoothed bands in; tick() is a no-op.
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
// dot gate matches bit-for-bit.
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

// visBandWidth returns the character width for band b (Go visBandWidth). At
// narrow widths only the leading visible bands receive columns; final frame
// fitting clips the legacy inter-band gaps emitted by older renderers.
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

class ScatterDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height      = grid.rows();
    const int panel_width = grid.cols();
    if (height <= 0 || panel_width <= 0 || bands.empty()) {
      return;
    }
    const int dot_rows   = height * 4;
    const int band_count = static_cast<int>(bands.size());
    if (dot_rows < 4) {
      return;  // guard: dot_rows-1 must stay positive (framework never does this)
    }

    for (int row = 0; row < height; ++row) {
      const float row_bottom =
          static_cast<float>(height - 1 - row) / static_cast<float>(height);
      const Color color = spec_color(row_bottom);
      int col = 0;
      for (int b = 0; b < band_count; ++b) {
        const int chars_per_band = vis_band_width(band_count, b, panel_width);
        for (int c = 0; c < chars_per_band; ++c) {
          std::uint32_t braille = 0x2800;

          for (int dr = 0; dr < 4; ++dr) {
            for (int dc = 0; dc < 2; ++dc) {
              const int dot_row = row * 4 + dr;
              const int dot_col = c * 2 + dc;

              const double h = scatter_hash(b, dot_row, dot_col, frame);

              // Gravity bias: more particles settle near the bottom.
              const double height_factor =
                  0.5 + 0.5 * static_cast<double>(dot_row) / static_cast<double>(dot_rows - 1);
              const double level = static_cast<double>(bands[static_cast<std::size_t>(b)]);
              const double threshold = level * level * height_factor;

              if (h < threshold) {
                braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                       [static_cast<std::size_t>(dc)];
              }
            }
          }

          grid.set(row, col, Cell{static_cast<char32_t>(braille), color});
          ++col;
        }
        if (b < band_count - 1) {
          // Legacy inter-band gap; clipped by the fit when the panel is
          // narrower than the full band layout (grid.set drops out-of-range).
          grid.set(row, col, Cell{U' ', color});
          ++col;
        }
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // cliamp defaultDriverTickInterval: overlays and stopped playback tick
    // slowly; actively playing ticks fast.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_scatter_driver() {
  return std::make_unique<ScatterDriver>();
}

}  // namespace bootamp::ui::vis_drivers
