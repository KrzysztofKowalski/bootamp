// ui/vis_drivers/sakura.cpp — falling cherry blossom petals (port of
// cliamp/ui/vis_sakura.go).
//
// Each petal is a small Braille silhouette (9 fixed shapes, 2-6 dots) that
// drifts downward at a per-petal fall speed with a gentle sinusoidal lateral
// sway. The number of petals on screen scales with the average band energy:
// 12 at silence, up to 28 when loud. Petal placement is a pure function of
// the petal index (seed) and the frame counter, so the whole render is
// deterministic — no driver state, no RNG. This is a renderOnly-style driver:
// the framework drives analysis + smoothing and passes the smoothed bands in;
// tick() is a no-op and tick_interval follows cliamp's defaultDriverTickInterval.
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

// cliamp brailleBit: (row, col) in the 4x2 Braille dot grid -> bit value.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// One petal silhouette: up to kSakuraMaxDots {row, col} offsets from the
// petal center, then how many of those dots are used. cliamp sakuraShapes.
constexpr int kSakuraMaxDots = 6;
constexpr int kSakuraShapeCount = 9;
struct SakuraShape {
  std::array<std::pair<int, int>, kSakuraMaxDots> dots;
  int                                             count;
};
constexpr std::array<SakuraShape, kSakuraShapeCount> kSakuraShapes = {{
    // Large — 6 dots, wide teardrop (close petals, slow fall).
    {{{{0, 1}, {1, 0}, {1, 1}, {1, 2}, {2, 0}, {2, 1}}}, 6},
    {{{{0, 1}, {1, 0}, {1, 1}, {1, 2}, {2, 1}, {2, 2}}}, 6},
    {{{{0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {2, 1}}}, 6},
    // Medium — 4 dots.
    {{{{0, 1}, {1, 0}, {1, 1}, {2, 0}, {0, 0}, {0, 0}}}, 4},
    {{{{0, 0}, {1, 0}, {1, 1}, {2, 1}, {0, 0}, {0, 0}}}, 4},
    {{{{0, 0}, {0, 1}, {1, 1}, {2, 1}, {0, 0}, {0, 0}}}, 4},
    // Small — 2-3 dots, distant (fast fall).
    {{{{0, 0}, {1, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}}, 2},
    {{{{0, 1}, {1, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}}, 2},
    {{{{0, 0}, {0, 1}, {1, 0}, {0, 0}, {0, 0}, {0, 0}}}, 3},
}};

// SakuraDriver — cliamp renderSakura, a pure function of (bands, frame).
class SakuraDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height    = grid.rows();
    const int dot_rows  = height * 4;
    const int dot_cols  = grid.cols() * 2;
    if (dot_rows < 4 || dot_cols < 4) {
      return;  // cliamp: blank frame (strings.Repeat("\n", height-1))
    }

    std::vector<std::uint8_t> dots(static_cast<std::size_t>(dot_rows) * dot_cols, 0);

    double total_energy = 0.0;
    for (const float e : bands) {
      total_energy += static_cast<double>(e);
    }
    // cliamp: totalEnergy / float64(len(bands)). The framework always passes
    // a non-empty band array (default 10 bands); the empty guard keeps the
    // NaN of a Go 0/0 from entering an int conversion.
    const double avg_energy =
        bands.empty() ? 0.0 : total_energy / static_cast<double>(bands.size());

    // 12 petals at silence, up to 28 when loud.
    const int num_petals = 12 + static_cast<int>(avg_energy * 16.0);

    for (int p = 0; p < num_petals; ++p) {
      const std::uint64_t seed = static_cast<std::uint64_t>(p) * 104729 + 7919;

      // Shape — first 3 are large, next 3 medium, last 3 small.
      const int shape_idx =
          static_cast<int>((seed * 4391) % static_cast<std::uint64_t>(kSakuraShapeCount));
      const SakuraShape& shape = kSakuraShapes[static_cast<std::size_t>(shape_idx)];

      // Large shapes fall slower (close), small ones faster (distant).
      const int fall_speed = (shape_idx >= 6) ? 2 : 1;

      // X: spread across the entire panel width.
      const int base_x = static_cast<int>(seed % static_cast<std::uint64_t>(dot_cols));

      // Y: slow scroll with off-screen buffer for smooth entry/exit.
      const int wrap_h = dot_rows + 10;
      const int base_y =
          static_cast<int>((seed * 3037) % static_cast<std::uint64_t>(wrap_h));
      // cliamp int(v.frame): Go's int is 64-bit; int64_t wraps the same way.
      const std::int64_t y =
          (static_cast<std::int64_t>(base_y) + static_cast<std::int64_t>(frame) * fall_speed / 8) %
              wrap_h -
          5;

      // Gentle lateral sway — each petal has its own phase.
      const double sway_phase = static_cast<double>(seed % 1000) / 1000.0 * 2.0 *
                                std::numbers::pi_v<double>;
      const double sway = std::sin(static_cast<double>(frame) * 0.015 + sway_phase) * 3.0;
      const int    x    = base_x + static_cast<int>(sway);

      // Stamp petal onto the dot grid.
      for (int d = 0; d < shape.count; ++d) {
        const int dr = static_cast<int>(y) + shape.dots[static_cast<std::size_t>(d)].first;
        const int dc = x + shape.dots[static_cast<std::size_t>(d)].second;
        if (dr >= 0 && dr < dot_rows && dc >= 0 && dc < dot_cols) {
          dots[static_cast<std::size_t>(dr) * dot_cols + dc] = 1;
        }
      }
    }

    // Convert the dot grid to Braille characters; each terminal line carries
    // the spectrum color of its row-bottom tier (cliamp specWrap).
    for (int row = 0; row < height; ++row) {
      const float row_bottom =
          static_cast<float>(height - 1 - row) / static_cast<float>(height);
      const Color color = spec_color(row_bottom);
      for (int ch = 0; ch < grid.cols(); ++ch) {
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

std::unique_ptr<VisDriver> make_sakura_driver() {
  return std::make_unique<SakuraDriver>();
}

}  // namespace bootamp::ui::vis_drivers
