// ui/vis_drivers/retro.cpp — retro 80s synthwave scene (port of
// cliamp/ui/vis_retro.go renderRetro).
//
// A striped semicircular setting sun above the horizon, a smooth
// audio-reactive wave (cosine-interpolated between bands, with a floor so it
// never vanishes), and a perspective grid floor that scrolls toward the
// viewer. The whole scene is drawn into a 2x-density dot grid and rendered
// through Braille characters for sub-cell resolution. Colors follow the
// priority wave (tier high) > sun (tier mid) > grid (tier low). Render-only
// driver: analysis + smoothing are driven by the framework; tick() is a
// no-op and the cadence is the default driver interval (fast while playing,
// slow otherwise).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
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

// RetroDriver — renderOnly-style: the framework drives analysis + smoothing
// and passes the smoothed bands in.
class RetroDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height     = grid.rows();
    const int char_cols  = grid.cols();
    if (height <= 0 || char_cols <= 0) {
      return;
    }
    const int dot_rows   = height * 4;
    const int dot_cols   = char_cols * 2;
    const int band_count = static_cast<int>(bands.size());

    // Horizon at 40% from top — gives room for wave and sun above.
    const int horizon_dot = std::max(dot_rows * 2 / 5, 2);
    const int floor_rows  = dot_rows - horizon_dot;
    const double center_x = static_cast<double>(dot_cols - 1) / 2.0;

    // Flat dot grid (single allocation): 0=empty, 1=grid, 2=wave, 3=sun
    // (Go grid []byte).
    std::vector<std::uint8_t> dots(static_cast<std::size_t>(dot_rows) * dot_cols,
                                   0);

    // -- SUN: striped semicircle above the horizon.
    const double sun_r = static_cast<double>(horizon_dot) * 0.85;
    for (int dy = 0; dy < horizon_dot; ++dy) {
      const double row_dist = static_cast<double>(horizon_dot - dy);  // above horizon
      if (row_dist > sun_r) {
        continue;
      }
      const double half_w = std::sqrt(sun_r * sun_r - row_dist * row_dist);

      // Bottom half of the sun has horizontal stripe gaps.
      if (row_dist < sun_r * 0.5) {
        const int sw = std::max(1, static_cast<int>(sun_r * 0.15));
        if ((static_cast<int>(row_dist) / sw) % 2 == 1) {
          continue;
        }
      }

      const int left = std::max(0, static_cast<int>(center_x - half_w));
      const int right = std::min(dot_cols - 1, static_cast<int>(center_x + half_w));
      const std::size_t off = static_cast<std::size_t>(dy) * dot_cols;
      for (int dx = left; dx <= right; ++dx) {
        dots[off + static_cast<std::size_t>(dx)] = 3;
      }
    }

    // -- HORIZON LINE.
    {
      const std::size_t off = static_cast<std::size_t>(horizon_dot) * dot_cols;
      for (int dx = 0; dx < dot_cols; ++dx) {
        dots[off + static_cast<std::size_t>(dx)] = 1;
      }
    }

    // -- PERSPECTIVE GRID FLOOR.
    // Vertical lines converging to the vanishing point at (centerX, horizonDot).
    constexpr int kNumVLines = 18;
    for (int i = 0; i <= kNumVLines; ++i) {
      const double bottom_x =
          static_cast<double>(i) * static_cast<double>(dot_cols - 1) /
          static_cast<double>(kNumVLines);
      for (int dy = horizon_dot + 1; dy < dot_rows; ++dy) {
        const double t = static_cast<double>(dy - horizon_dot) /
                         static_cast<double>(std::max(1, floor_rows - 1));
        const double screen_x = center_x + (bottom_x - center_x) * t;
        const int ix          = static_cast<int>(std::round(screen_x));
        if (ix >= 0 && ix < dot_cols) {
          dots[static_cast<std::size_t>(dy) * dot_cols + static_cast<std::size_t>(ix)] = 1;
        }
      }
    }

    // Horizontal lines scrolling toward the viewer (Go math.Mod(frame*0.08, 1)).
    const double scroll = std::fmod(static_cast<double>(frame) * 0.08, 1.0);
    constexpr int kNumHLines = 10;
    for (int i = 0; i < kNumHLines; ++i) {
      double z = (static_cast<double>(i) + scroll) / static_cast<double>(kNumHLines);
      if (z > 1.0) {
        z -= 1.0;
      }
      // Quadratic perspective: dense near the horizon, spread near the viewer.
      const int dy = horizon_dot + 1 +
                     static_cast<int>(z * z *
                                      static_cast<double>(std::max(1, floor_rows - 2)));
      if (dy > horizon_dot && dy < dot_rows) {
        const std::size_t off = static_cast<std::size_t>(dy) * dot_cols;
        for (int dx = 0; dx < dot_cols; ++dx) {
          dots[off + static_cast<std::size_t>(dx)] = 1;
        }
      }
    }

    // -- AUDIO WAVE AT THE HORIZON. Go indexes bands[bandCount-1] for the
    // last column; with no bands the Go code would panic, so the wave is
    // simply skipped (deviation: grid + sun still render).
    if (band_count > 0) {
      std::vector<int> wave_y(static_cast<std::size_t>(dot_cols));
      const double max_wave = static_cast<double>(horizon_dot) * 0.85;
      for (int dx = 0; dx < dot_cols; ++dx) {
        const double band_f =
            static_cast<double>(dx) /
            static_cast<double>(std::max(1, dot_cols - 1)) *
            static_cast<double>(band_count - 1);
        const int bi   = static_cast<int>(band_f);
        const double frac = band_f - static_cast<double>(bi);

        // Cosine interpolation for a smooth curve between bands.
        const double t = (1.0 - std::cos(frac * std::numbers::pi_v<double>)) / 2.0;

        double level;
        if (bi >= band_count - 1) {
          level = static_cast<double>(bands[static_cast<std::size_t>(band_count - 1)]);
        } else {
          level = static_cast<double>(bands[static_cast<std::size_t>(bi)]) * (1.0 - t) +
                  static_cast<double>(bands[static_cast<std::size_t>(bi + 1)]) * t;
        }

        // Small floor so the wave never fully vanishes.
        level = std::max(0.03, level);

        const int wy = horizon_dot - static_cast<int>(level * max_wave);
        wave_y[static_cast<std::size_t>(dx)] =
            std::max(0, std::min(dot_rows - 1, wy));
      }

      // Draw the wave with continuous line connections.
      for (int dx = 0; dx < dot_cols; ++dx) {
        const int y = wave_y[static_cast<std::size_t>(dx)];
        dots[static_cast<std::size_t>(y) * dot_cols + static_cast<std::size_t>(dx)] = 2;
        if (dx > 0) {
          const int lo = std::min(y, wave_y[static_cast<std::size_t>(dx - 1)]);
          const int hi = std::max(y, wave_y[static_cast<std::size_t>(dx - 1)]);
          for (int fy = lo; fy <= hi; ++fy) {
            dots[static_cast<std::size_t>(fy) * dot_cols + static_cast<std::size_t>(dx)] = 2;
          }
        }
      }
    }

    // -- RENDER BRAILLE.
    for (int row = 0; row < height; ++row) {
      const int base = row * 4;
      for (int ch = 0; ch < char_cols; ++ch) {
        std::uint32_t braille = 0x2800;
        const int col_base    = ch * 2;
        bool has_wave         = false;
        bool has_sun          = false;

        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const int dy = base + dr;
            const int dx = col_base + dc;
            if (dy >= dot_rows || dx >= dot_cols) {
              continue;
            }
            switch (dots[static_cast<std::size_t>(dy) * dot_cols + static_cast<std::size_t>(dx)]) {
              case 1:
                braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                      [static_cast<std::size_t>(dc)];
                break;
              case 2:
                braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                      [static_cast<std::size_t>(dc)];
                has_wave = true;
                break;
              case 3:
                braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                      [static_cast<std::size_t>(dc)];
                has_sun = true;
                break;
              default:
                break;
            }
          }
        }

        // Priority: wave (red) > sun (yellow) > grid (green) (Go tags 2/1/0).
        const Color color = has_wave ? kColorSpecHigh
                            : has_sun ? kColorSpecMid
                                      : kColorSpecLow;
        grid.at(row, ch) = Cell{static_cast<char32_t>(braille), color};
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // Go newRenderOnlyDriver: defaultDriverTickInterval — fast while playing
    // without an overlay, slow otherwise.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_retro_driver() {
  return std::make_unique<RetroDriver>();
}

}  // namespace bootamp::ui::vis_drivers
