// ui/vis_drivers/pulse.cpp — "Pulse" braille pulsating circle
// (port of cliamp/ui/vis_pulse.go).
//
// A pulsating ellipse drawn in a 4×2-dot braille grid that fills the display
// width. The radius at each angle blends per-band frequency energy with the
// overall level so the whole shape surges on every beat while still deforming
// per frequency; a clean shockwave ring radiates outward on transients. The
// interior is solid-filled with an anti-aliased edge and a green→yellow→red
// radial color gradient. Per-dot polar coordinates (distance + angle) are
// cached per panel size (Go pulseCoords) so the hot render loop reads flat
// arrays instead of calling ~3360 sqrt/atan2 per frame. Render-only driver:
// the framework drives analysis + smoothing; cadence is the default
// (kTickFast while playing, kTickSlow otherwise).
#include "ui/vis_drivers/registry.hpp"

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
// and frame (Go scatterHash, shared with the logo driver). Computed in double
// like the Go original so the dot gate matches bit-for-bit; dots persist for a
// few frames (frame/3 staggering) to create a twinkling effect.
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

// pulseDotIndex flattens the per-dot (row, col, dr, dc) index into the coord
// cache arrays (Go pulseDotIndex).
std::size_t pulse_dot_index(int row, int col, int dr, int dc, int width) {
  return static_cast<std::size_t>(((row * width + col) * 4 + dr) * 2 + dc);
}

// PulseCoords caches per-dot distance-from-center and angle values for the
// current panel dimensions (Go pulseCoords). Recomputed lazily on resize so
// the hot render loop skips the sqrt/atan2 work per frame.
struct PulseCoords {
  int                 width  = 0;
  int                 height = 0;
  double              max_r  = 0.0;
  std::vector<double> dist;
  std::vector<double> angle;
};

class PulseDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height = grid.rows();
    const int width  = grid.cols();
    if (height <= 0 || width <= 0 || bands.empty()) {
      return;  // the framework always supplies 10 bands; empty = safety
    }
    const PulseCoords& coords      = pulse_coords(width, height);
    const std::size_t  band_count  = bands.size();
    const double       max_r       = coords.max_r;

    double total_energy = 0.0;
    for (const float e : bands) {
      total_energy += static_cast<double>(e);
    }
    const double avg_energy = total_energy / static_cast<double>(band_count);

    // Shockwave: expanding ring that fades as it grows.
    const double shock_phase = std::fmod(static_cast<double>(frame) * 0.10, 1.0);
    const double shock_r     = max_r * (0.3 + 0.7 * shock_phase);
    const double shock_strength =
        avg_energy * avg_energy * (1.0 - shock_phase * shock_phase);

    // Gentle breathing keeps the shape alive during silence.
    const double breath = std::sin(static_cast<double>(frame) * 0.05) * 0.02;

    // Per-frame rotation offset — added uniformly to every cached angle.
    const double rot_offset =
        static_cast<double>(frame) * (0.015 + avg_energy * 0.04);
    const double two_pi     = 2.0 * std::numbers::pi;
    const double band_scale = static_cast<double>(band_count) / two_pi;

    for (int row = 0; row < height; ++row) {
      for (int col = 0; col < width; ++col) {
        std::uint32_t braille  = 0x2800;
        double        max_norm = 0.0;

        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const std::size_t idx  = pulse_dot_index(row, col, dr, dc, width);
            const double      dist = coords.dist[idx];

            double rot_angle = coords.angle[idx] + rot_offset;
            rot_angle -= std::floor(rot_angle / two_pi) * two_pi;

            // Cosine-interpolated band mapping.
            const double      band_pos = rot_angle * band_scale;
            const std::size_t band_idx =
                static_cast<std::size_t>(static_cast<int>(band_pos) %
                                         static_cast<int>(band_count));
            const std::size_t next_band = (band_idx + 1) % band_count;
            const double      frac = band_pos - std::floor(band_pos);
            const double      t    = (1.0 - std::cos(frac * std::numbers::pi)) / 2.0;
            const double      energy =
                static_cast<double>(bands[band_idx]) * (1.0 - t) +
                static_cast<double>(bands[next_band]) * t;

            // Blend per-band with overall so the whole shape beats.
            const double blended = energy * 0.6 + avg_energy * 0.4;
            const double punch   = blended * blended;
            const double r       = max_r * (0.08 + breath + 0.92 * punch);

            // --- Solid fill ---
            if (r > 0.5 && dist <= r) {
              const double norm = dist / r;
              if (norm > max_norm) {
                max_norm = norm;
              }
              braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                     [static_cast<std::size_t>(dc)];
            } else if (r > 0.5 && dist < r + 1.5) {
              // Anti-aliased edge.
              const double edge_fade = 1.0 - (dist - r) / 1.5;
              if (scatter_hash(static_cast<int>(band_idx), row * 4 + dr,
                               col * 2 + dc, frame) < edge_fade * 0.7) {
                braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                       [static_cast<std::size_t>(dc)];
                if (max_norm < 0.9) {
                  max_norm = 0.9;
                }
              }
            }

            // --- Shockwave ring ---
            if (shock_strength > 0.05) {
              const double shock_dist  = std::abs(dist - shock_r);
              const double shock_thick = 0.6 + shock_strength * 1.5;
              if (shock_dist < shock_thick) {
                const double fade = 1.0 - shock_dist / shock_thick;
                if (fade > 0.4) {
                  braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                         [static_cast<std::size_t>(dc)];
                  if (max_norm < 0.65) {
                    max_norm = 0.65;
                  }
                }
              }
            }
          }
        }

        // Radial color gradient: green core → yellow → red edge (Go specTag).
        grid.set(row, col, Cell{static_cast<char32_t>(braille),
                                spec_color(static_cast<float>(max_norm))});
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // cliamp renderOnlyDriver -> defaultDriverTickInterval: overlays and
    // stopped playback tick slowly; actively playing ticks fast.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }

private:
  // pulseCoords (Go): lazily recompute the per-dot polar cache on resize.
  const PulseCoords& pulse_coords(int width, int height) {
    if (coords_.width == width && coords_.height == height) {
      return coords_;
    }
    const int    dot_rows = height * 4;
    const int    dot_cols = width * 2;
    const double center_x = static_cast<double>(dot_cols) / 2.0;
    const double center_y = static_cast<double>(dot_rows) / 2.0;
    const double x_scale  = center_y / center_x;

    coords_.width  = width;
    coords_.height = height;
    coords_.max_r  = center_y - 1.0;
    coords_.dist.assign(static_cast<std::size_t>(height) * width * 8, 0.0);
    coords_.angle.assign(static_cast<std::size_t>(height) * width * 8, 0.0);
    for (int row = 0; row < height; ++row) {
      for (int col = 0; col < width; ++col) {
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const double      dx =
                (static_cast<double>(col * 2 + dc) - center_x) * x_scale;
            const double      dy = static_cast<double>(row * 4 + dr) - center_y;
            const std::size_t idx = pulse_dot_index(row, col, dr, dc, width);
            coords_.dist[idx]     = std::sqrt(dx * dx + dy * dy);
            double a              = std::atan2(dy, dx);
            if (a < 0) {
              a += 2.0 * std::numbers::pi;
            }
            coords_.angle[idx] = a;
          }
        }
      }
    }
    return coords_;
  }

  PulseCoords coords_;
};

}  // namespace

std::unique_ptr<VisDriver> make_pulse_driver() {
  return std::make_unique<PulseDriver>();
}

}  // namespace bootamp::ui::vis_drivers
