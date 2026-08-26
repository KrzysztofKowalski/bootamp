// ui/vis_drivers/butterfly.cpp — mirrored Rorschach/butterfly pattern
// (port of cliamp/ui/vis_butterfly.go).
//
// The spectrum bands are mirrored horizontally from the center, and organic
// variation is added via sine wobble and scatterHash to create ink-blot-like
// shapes that pulse with the music. This is a renderOnly-style driver: the
// framework drives analysis + smoothing and passes the smoothed bands in;
// tick() is a no-op.
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

class ButterflyDriver : public VisDriver {
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
    const int dot_cols   = panel_width * 2;
    const int center_x   = dot_cols / 2;
    const int band_count = static_cast<int>(bands.size());

    std::vector<bool> dots(static_cast<std::size_t>(dot_rows) * dot_cols);

    for (int dy = 0; dy < dot_rows; ++dy) {
      // Map vertical position to a band index (Go linear interpolation).
      const double band_f =
          static_cast<double>(dy) / static_cast<double>(std::max(1, dot_rows - 1)) *
          static_cast<double>(band_count - 1);
      const int bi = static_cast<int>(band_f);
      const double frac = band_f - static_cast<double>(bi);
      double energy;
      if (bi >= band_count - 1) {
        energy = static_cast<double>(bands[static_cast<std::size_t>(band_count - 1)]);
      } else {
        energy = static_cast<double>(bands[static_cast<std::size_t>(bi)]) * (1.0 - frac) +
                 static_cast<double>(bands[static_cast<std::size_t>(bi + 1)]) * frac;
      }

      // Wing width: how far from center the pattern extends.
      const double t = static_cast<double>(frame) * 0.08 + static_cast<double>(dy) * 0.3;
      const double wobble = std::sin(t) * 0.15;
      const int wing_width =
          static_cast<int>(static_cast<double>(center_x) * (energy + wobble) * 0.9);

      for (int dx = 0; dx < wing_width; ++dx) {
        // Distance from center normalized to wing width.
        const double norm =
            static_cast<double>(dx) / static_cast<double>(std::max(1, wing_width));

        // Organic edge: denser near center, sparser at edges.
        double threshold = (1.0 - norm * norm) * energy;
        // Add frame-based flicker at the edges.
        if (norm > 0.6) {
          threshold *= 0.5 + 0.5 * std::sin(static_cast<double>(frame) * 0.1 +
                                            static_cast<double>(dy) * 0.5 +
                                            static_cast<double>(dx) * 0.3);
        }

        if (scatter_hash(bi, dy, dx, frame / 3) < threshold) {
          // Right wing.
          const int rx = center_x + dx;
          if (rx < dot_cols) {
            dots[static_cast<std::size_t>(dy) * dot_cols + rx] = true;
          }
          // Left wing (mirror).
          const int lx = center_x - 1 - dx;
          if (lx >= 0) {
            dots[static_cast<std::size_t>(dy) * dot_cols + lx] = true;
          }
        }
      }

      // Central spine — always drawn.
      if (energy > 0.05) {
        dots[static_cast<std::size_t>(dy) * dot_cols + center_x] = true;
        if (center_x > 0) {
          dots[static_cast<std::size_t>(dy) * dot_cols + center_x - 1] = true;
        }
      }
    }

    // Render braille with row-based coloring. Note the row gradient is
    // inverted vs the other renderers: Go butterfly colors by row/(rows-1),
    // so the TOP rows are dim and the BOTTOM rows bright.
    for (int row = 0; row < height; ++row) {
      const float norm =
          static_cast<float>(row) / static_cast<float>(std::max(1, height - 1));
      const Color color = spec_color(norm);
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
    // cliamp defaultDriverTickInterval: overlays and stopped playback tick
    // slowly; actively playing ticks fast.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_butterfly_driver() {
  return std::make_unique<ButterflyDriver>();
}

}  // namespace bootamp::ui::vis_drivers
