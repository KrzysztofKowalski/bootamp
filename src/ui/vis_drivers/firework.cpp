// ui/vis_drivers/firework.cpp — firework bursts (port of cliamp/ui/vis_firework.go).
//
// Each burst launches from the bottom with a rising trail, then explodes into a
// sphere of particles that drift downward with gravity and fade. Audio energy
// drives the number of simultaneous bursts and the size of each explosion.
// This is a renderOnly-style driver: the framework drives analysis + smoothing
// and passes the smoothed bands in; tick() is a no-op.
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

class FireworkDriver : public VisDriver {
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
    // cliamp renderFirework: degenerate sizes render blank lines.
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

    // Number of simultaneous firework bursts: 5 quiet, up to 14 loud.
    const int num_bursts = 5 + static_cast<int>(avg_energy * 9.0);
    const std::uint64_t cycle_len  = 48;
    const std::uint64_t launch_len = 10;

    for (int i = 0; i < num_bursts; ++i) {
      // Seed changes each cycle so bursts appear in new positions.
      const std::uint64_t cycle = (frame + static_cast<std::uint64_t>(i) * 7) / cycle_len;
      const std::uint64_t seed  = cycle * 104729 + static_cast<std::uint64_t>(i) * 7919;

      // Stagger starts so bursts don't all fire simultaneously.
      const std::uint64_t offset =
          static_cast<std::uint64_t>(i) * cycle_len / static_cast<std::uint64_t>(num_bursts) +
          (seed / 3) % 5;
      const std::uint64_t local_frame = (frame + offset) % cycle_len;

      // Burst center — spread across the panel, upper portion.
      const int cx = static_cast<int>((seed * 6271) % static_cast<std::uint64_t>(dot_cols));
      const int cy =
          static_cast<int>((seed * 4391) % static_cast<std::uint64_t>(dot_rows / 2)) +
          dot_rows / 8;

      // Associated band for energy-driven sizing.
      const int band_idx = static_cast<int>(seed % static_cast<std::uint64_t>(bands.size()));
      const double energy = static_cast<double>(bands[static_cast<std::size_t>(band_idx)]);

      if (local_frame < launch_len) {
        // Rising trail from bottom to burst center.
        const double progress = static_cast<double>(local_frame) / static_cast<double>(launch_len);
        const int trail_y =
            dot_rows - 1 - static_cast<int>(static_cast<double>(dot_rows - 1 - cy) * progress);
        // Short trail of a few dots.
        for (int dy = 0; dy < 4; ++dy) {
          const int ty = trail_y + dy;
          if (ty >= 0 && ty < dot_rows && cx >= 0 && cx < dot_cols) {
            dots[static_cast<std::size_t>(ty) * dot_cols + cx] = true;
          }
        }
      } else {
        // Burst expansion and fade.
        const double burst_t =
            static_cast<double>(local_frame - launch_len) / static_cast<double>(cycle_len - launch_len);

        const double max_radius = 3.0 + energy * 8.0;
        // Fast expansion, then slow drift.
        const double radius = max_radius * std::min(burst_t * 3.0, 1.0);
        // Gravity pulls particles down over time.
        const double gravity = burst_t * burst_t * 5.0;
        // Particles fade out over time.
        const double fade = std::max(0.0, 1.0 - burst_t * 1.3);

        const int num_particles = 18 + static_cast<int>(energy * 18.0);
        for (int p = 0; p < num_particles; ++p) {
          const double angle = static_cast<double>(p) / static_cast<double>(num_particles) *
                               2.0 * std::numbers::pi_v<double>;
          const std::uint64_t p_seed = seed + static_cast<std::uint64_t>(p) * 2909;
          const double speed = 0.6 + static_cast<double>(p_seed % 400) / 1000.0;

          const int px = cx + static_cast<int>(std::cos(angle) * radius * speed);
          const int py = cy + static_cast<int>(std::sin(angle) * radius * speed + gravity);

          // Stochastic fade — more particles disappear as time passes.
          if (scatter_hash(band_idx, p, static_cast<int>(seed % 100), frame) > fade) {
            continue;
          }

          if (px >= 0 && px < dot_cols && py >= 0 && py < dot_rows) {
            dots[static_cast<std::size_t>(py) * dot_cols + px] = true;
          }
        }
      }
    }

    // Convert dot grid to Braille characters with row-based spectrum color
    // (Go specWrap: top rows bright, bottom dimmer — fireworks in a night sky).
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

std::unique_ptr<VisDriver> make_firework_driver() {
  return std::make_unique<FireworkDriver>();
}

}  // namespace bootamp::ui::vis_drivers
