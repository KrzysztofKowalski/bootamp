// ui/vis_drivers/bubbles.cpp — rising air bubbles (port of cliamp/ui/vis_bubbles.go).
//
// Each bubble is a hollow ring with a tiny specular highlight that drifts
// upward and sways laterally. Audio energy modulates the sway and highlight
// intensity; the bubble count is fixed so bubbles never pop in or out of
// existence mid-air. Bubbles fade stochastically as they approach the surface
// so they appear to "pop". This is a renderOnly-style driver: the framework
// drives analysis + smoothing and passes the smoothed bands in; tick() is a
// no-op.
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

class BubblesDriver : public VisDriver {
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
    const int dot_rows = height * 4;
    const int dot_cols = panel_width * 2;
    // cliamp renderBubbles: degenerate sizes render blank lines.
    if (dot_rows < 4 || dot_cols < 4) {
      return;
    }

    std::vector<bool> dots(static_cast<std::size_t>(dot_rows) * dot_cols);

    double total_energy = 0.0;
    for (const float e : bands) {
      total_energy += static_cast<double>(e);
    }
    const double avg_energy =
        total_energy / static_cast<double>(bands.size());

    // Fixed count — changing this per frame makes bubbles spawn/vanish mid-air.
    constexpr int num_bubbles = 18;

    for (int i = 0; i < num_bubbles; ++i) {
      const std::uint64_t seed = static_cast<std::uint64_t>(i) * 104729 + 7919;

      // Stable per-bubble radius (1.5 to 4.0 dots). Must not depend on
      // per-frame audio, otherwise trajectory parameters derived from it
      // (speedDiv, wrapH, baseY) jitter every frame and the bubble flashes
      // around the screen instead of rising smoothly.
      const double radius = 1.5 + static_cast<double>(seed % 100) / 100.0 * 2.5;

      // Bigger bubbles rise slower (buoyancy feels floaty).
      const int speed_div = 3 + static_cast<int>(radius);

      // Continuous upward scroll with off-screen buffer for smooth entry/exit.
      const int wrap_h = dot_rows + static_cast<int>(radius * 2.0) + 8;
      const int base_y = static_cast<int>((seed * 3037) % static_cast<std::uint64_t>(wrap_h));
      const std::int64_t frame_i64 = static_cast<std::int64_t>(frame);  // Go int(uint64)
      const int y = wrap_h - 1 - static_cast<int>((static_cast<std::int64_t>(base_y) +
                                                   frame_i64 / speed_div) %
                                                  static_cast<std::int64_t>(wrap_h)) -
                    static_cast<int>(radius) - 2;

      // Horizontal position with gentle sinusoidal sway. Amplitude scales
      // with overall energy — quiet passages drift calmly, loud passages
      // wobble a bit more. This only shifts x, so it can't destabilize the
      // trajectory.
      const int base_x = static_cast<int>(seed % static_cast<std::uint64_t>(dot_cols));
      const double sway_phase =
          static_cast<double>(seed % 1000) / 1000.0 * 2.0 * std::numbers::pi_v<double>;
      const double sway_amp = 1.5 + avg_energy * 2.5;
      const double sway = std::sin(static_cast<double>(frame) * 0.03 + sway_phase) * sway_amp;
      const int x = base_x + static_cast<int>(sway);

      // Pop fade — the last few rows thin the ring stochastically.
      const int pop_zone = static_cast<int>(radius) + 3;
      double pop_fade = 1.0;
      if (y < pop_zone) {
        pop_fade = std::max(0.0, static_cast<double>(y) / static_cast<double>(pop_zone));
      }

      // Draw hollow ring.
      const double r_inner = radius - 0.9;
      const int bbox = static_cast<int>(radius) + 1;
      for (int dy = -bbox; dy <= bbox; ++dy) {
        for (int dx = -bbox; dx <= bbox; ++dx) {
          const double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));
          if (dist > radius || dist < r_inner) {
            continue;
          }
          // Stable per-bubble pop pattern (no frame dependency) so the ring
          // doesn't strobe as it fades near the top.
          if (pop_fade < 1.0 && scatter_hash(i, dy, dx, 0) > pop_fade) {
            continue;
          }
          const int gy = y + dy;
          const int gx = x + dx;
          if (gy >= 0 && gy < dot_rows && gx >= 0 && gx < dot_cols) {
            dots[static_cast<std::size_t>(gy) * dot_cols + gx] = true;
          }
        }
      }

      // Specular highlight — small bright cluster in the upper-left quadrant.
      if (radius >= 2.0 && pop_fade > 0.5) {
        const int hx = x - static_cast<int>(radius * 0.45);
        const int hy = y - static_cast<int>(radius * 0.45);
        // Go [][2]int{{0, 0}, {0, 1}, {1, 0}}.
        constexpr std::array<std::array<int, 2>, 3> kHighlight = {{{0, 0}, {0, 1}, {1, 0}}};
        for (const auto& d : kHighlight) {
          const int gy = hy + d[0];
          const int gx = hx + d[1];
          if (gy >= 0 && gy < dot_rows && gx >= 0 && gx < dot_cols) {
            dots[static_cast<std::size_t>(gy) * dot_cols + gx] = true;
          }
        }
      }
    }

    // Convert dot grid to Braille characters with row-based spectrum color
    // (Go specWrap: top rows warm — light through surface, bottom rows cool —
    // depth).
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
    // cliamp defaultDriverTickInterval: overlays and stopped playback tick
    // slowly; actively playing ticks fast.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_bubbles_driver() {
  return std::make_unique<BubblesDriver>();
}

}  // namespace bootamp::ui::vis_drivers
