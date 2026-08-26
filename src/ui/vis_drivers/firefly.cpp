// ui/vis_drivers/firefly.cpp — firefly meadow at dusk (port of
// cliamp/ui/vis_firefly.go).
//
// A low grass silhouette sits at the bottom (pseudo-noise heights, ragged
// edge) and 26 fireflies drift above on slow Lissajous-like curves seeded per
// index. Each firefly blinks: the chance of being "on" this frame depends on
// its per-fly phase plus the high-frequency band energy (which also raises the
// population's brightness), while bass tilts a gentle wind that nudges them
// sideways. Lit flies get a one-dot dim halo; the bright tier paints high-red,
// dim yellow, grass green (cliamp flushStyleRun tag batching).
//
// Pure function of (bands, frame) — no driver state, no RNG. This is a
// renderOnly-style driver: the framework drives analysis + smoothing and
// passes the smoothed bands in; tick() is a no-op and tick_interval follows
// cliamp's defaultDriverTickInterval.
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

// cliamp brailleBit: (row, col) in the 4x2 Braille dot grid -> bit value.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// band_avg returns the mean of bands[lo, hi), guarded against out-of-range
// arguments (cliamp bandAvg).
double band_avg(std::span<const float> bands, int lo, int hi) {
  if (lo < 0) {
    lo = 0;
  }
  if (hi > static_cast<int>(bands.size())) {
    hi = static_cast<int>(bands.size());
  }
  if (hi <= lo) {
    return 0.0;
  }
  double s = 0.0;
  for (int i = lo; i < hi; ++i) {
    s += static_cast<double>(bands[static_cast<std::size_t>(i)]);
  }
  return s / static_cast<double>(hi - lo);
}

// tier_color maps a cliamp style-run tag to a palette slot
// (0=low/green, 1=mid/yellow, 2=high/red).
Color tier_color(int tag) {
  switch (tag) {
    case 2:  return kColorSpecHigh;
    case 1:  return kColorSpecMid;
    default: return kColorSpecLow;
  }
}

// Halo offsets: {dy, dx} for the one-dot ring around a lit firefly
// (cliamp renderFirefly's [4][2]int{{-1,0},{1,0},{0,-1},{0,1}}).
constexpr std::array<std::array<int, 2>, 4> kFireflyHalo = {{
    {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
}};

class FireflyDriver : public VisDriver {
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
    if (dot_rows < 4 || dot_cols < 8) {
      return;  // cliamp: blank frame (strings.Repeat("\n", height-1))
    }

    const int band_count = static_cast<int>(bands.size());
    const double bass = band_avg(bands, 0, band_count / 3);
    const double high = band_avg(bands, 2 * band_count / 3, band_count);

    // Grass silhouette: bottom 1-2 rows, ragged edge.
    std::vector<std::uint8_t> grass(static_cast<std::size_t>(dot_rows) * dot_cols, 0);
    for (int x = 0; x < dot_cols; ++x) {
      // Pseudo-noise heights.
      const double h0 = 2.5 + 1.5 * std::sin(static_cast<double>(x) * 0.41) +
                        1.0 * std::sin(static_cast<double>(x) * 0.17 + 2.3);
      const int h = 1 + static_cast<int>(h0);
      for (int d = 0; d < h; ++d) {
        const int y = dot_rows - 1 - d;
        if (y >= 0) {
          grass[static_cast<std::size_t>(y) * dot_cols + x] = 1;
        }
      }
    }

    // Fireflies.
    constexpr int kNumFlies = 26;
    const double  wind      = bass * 1.5;

    std::vector<std::uint8_t> dim(static_cast<std::size_t>(dot_rows) * dot_cols, 0);
    std::vector<std::uint8_t> bright(static_cast<std::size_t>(dot_rows) * dot_cols, 0);

    const double t = static_cast<double>(frame);
    for (int i = 0; i < kNumFlies; ++i) {
      const std::uint64_t seed = static_cast<std::uint64_t>(i) * 2246822519 + 11;
      // Two slightly incommensurate frequencies for Lissajous-like wandering.
      const double fx = 0.012 + static_cast<double>(seed % 17) / 3500.0;
      const double fy = 0.018 + static_cast<double>((seed >> 4) % 19) / 2900.0;
      const double phx = static_cast<double>(seed % 1000) / 1000.0 * 2.0 *
                         std::numbers::pi_v<double>;
      const double phy = static_cast<double>((seed >> 8) % 1000) / 1000.0 * 2.0 *
                         std::numbers::pi_v<double>;

      const double base_x = static_cast<double>(dot_cols / 2) +
                            std::cos(t * fx + phx) * static_cast<double>(dot_cols - 6) * 0.45;
      const double base_y = static_cast<double>(dot_rows - 4) * 0.5 +
                            std::sin(t * fy + phy) * static_cast<double>(dot_rows - 6) * 0.4;
      const int x = static_cast<int>(base_x + wind * std::sin(t * 0.02 + phx));
      const int y = static_cast<int>(base_y);
      if (x < 0 || x >= dot_cols || y < 0 || y >= dot_rows - 1) {
        continue;
      }
      // Skip if it would land in the grass silhouette.
      if (grass[static_cast<std::size_t>(y) * dot_cols + x]) {
        continue;
      }

      // Blink: chance of being "on" depends on the per-fly phase plus high band.
      const double blink_phase = std::sin(t * 0.18 + static_cast<double>(i) * 1.31) * 0.5;
      const bool   on          = blink_phase + 0.5 + high * 0.4 > 0.55;
      if (!on) {
        // Half-brightness halo so the fly is faintly there.
        dim[static_cast<std::size_t>(y) * dot_cols + x] = 1;
        continue;
      }

      bright[static_cast<std::size_t>(y) * dot_cols + x] = 1;
      // Glow halo (one-dot ring).
      for (const auto& d : kFireflyHalo) {
        const int gx = x + d[1];
        const int gy = y + d[0];
        if (gx >= 0 && gx < dot_cols && gy >= 0 && gy < dot_rows &&
            !grass[static_cast<std::size_t>(gy) * dot_cols + gx]) {
          dim[static_cast<std::size_t>(gy) * dot_cols + gx] = 1;
        }
      }
    }

    // Convert to Braille: bright -> high tier, dim -> mid, grass -> low;
    // per-cell the highest present tier wins (cliamp flushStyleRun runs).
    for (int row = 0; row < height; ++row) {
      for (int col = 0; col < grid.cols(); ++col) {
        std::uint32_t braille = 0x2800;
        int           cell_tag = -1;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const int y = row * 4 + dr;
            const int x = col * 2 + dc;
            const std::size_t idx = static_cast<std::size_t>(y) * dot_cols + x;
            int t = -1;
            if (bright[idx]) {
              t = 2;
            } else if (dim[idx]) {
              t = 1;
            } else if (grass[idx]) {
              t = 0;
            }
            if (t >= 0) {
              braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                     [static_cast<std::size_t>(dc)];
              if (t > cell_tag) {
                cell_tag = t;
              }
            }
          }
        }
        if (cell_tag < 0) {
          cell_tag = 0;  // cliamp: unstyled run defaults to the low tier
        }
        grid.set(row, col, Cell{static_cast<char32_t>(braille), tier_color(cell_tag)});
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // cliamp defaultDriverTickInterval.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_firefly_driver() {
  return std::make_unique<FireflyDriver>();
}

}  // namespace bootamp::ui::vis_drivers
