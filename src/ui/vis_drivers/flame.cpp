// ui/vis_drivers/flame.cpp — doom-fire heat propagation (port of
// cliamp/ui/vis_flame.go).
//
// A heat field is fed at the bottom row from the spectrum (per-column linear
// band sampling plus a small LCG sparkle); each tick every cell inherits its
// neighbour-below's heat with a small lateral wind jitter and a random decay,
// so the result is a continuous lapping flame instead of independent columns.
// Bass thickens the heat source; quiet passages settle into a low flickering
// bed of embers. The renderer maps heat to tiers: hot cores yellow (mid
// spectrum tier), bodies red (high tier), and stochastically stipples wispy
// low-heat tips via scatterHash.
//
// The RNG is the same 64-bit LCG as the Go driver (same constants, same call
// sequence), so the heat evolution is bit-exact for the same input stream.
// Stateful: tick() advances the heat field, render() reads it. The VisDriver
// contract gives tick() no grid, so the panel dimensions are cached from the
// last render() call; the first tick before any render is a no-op.
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

// cliamp brailleBit: (row, col) in the 4x2 Braille dot grid -> bit value.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// scatterHash returns a pseudo-random value in [0, 1) for a given dot position
// and frame (cliamp scatterHash). Computed in double like the Go original.
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

// sample_band_linear linearly interpolates between adjacent bands at a
// fractional band position (cliamp sampleBandLinear).
double sample_band_linear(std::span<const float> bands, double pos) {
  if (bands.empty()) {
    return 0.0;
  }
  if (bands.size() == 1) {
    return static_cast<double>(bands[0]);
  }
  if (pos <= 0) {
    return static_cast<double>(bands[0]);
  }
  const double last = static_cast<double>(bands.size() - 1);
  if (pos >= last) {
    return static_cast<double>(bands[bands.size() - 1]);
  }
  const int idx = static_cast<int>(pos);
  const double frac = pos - static_cast<double>(idx);
  return static_cast<double>(bands[static_cast<std::size_t>(idx)]) * (1.0 - frac) +
         static_cast<double>(bands[static_cast<std::size_t>(idx) + 1]) * frac;
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

class FlameDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void tick(const VisTickContext& ctx, std::uint64_t& frame,
            std::span<const float> bands) override {
    // cliamp flameDriver.Tick: the framework's defaultDriverTick already ran;
    // under an overlay the heat field stays frozen.
    if (ctx.overlay_active) {
      return;
    }
    const int dot_rows = rows_ * 4;
    const int dot_cols = cols_ * 2;
    if (dot_rows < 4 || dot_cols < 4) {
      return;
    }
    ensure(dot_rows, dot_cols);
    frame_ = frame;

    const int band_count = static_cast<int>(bands.size());

    // Source (bottom) row: per-column heat seeded from a smooth spectrum
    // sample plus a small per-column sparkle so the base shimmers even on
    // quiet input.
    if (band_count > 0) {
      const double last = static_cast<double>(band_count - 1);
      for (int x = 0; x < dot_cols; ++x) {
        const double pos =
            static_cast<double>(x) / static_cast<double>(std::max(1, dot_cols - 1)) * last;
        const double src = sample_band_linear(bands, pos);

        step_rng();
        const double sparkle = static_cast<double>((rng_ >> 33) % 100) / 100.0 * 0.18;

        // Always keep the base mildly lit so a bed of embers is visible.
        double base = 0.30 + 0.70 * src + sparkle;
        if (base > 1.05) {
          base = 1.05;
        }
        heat_[static_cast<std::size_t>(x)] = base;
      }
    } else {
      for (int x = 0; x < dot_cols; ++x) {
        step_rng();
        heat_[static_cast<std::size_t>(x)] =
            0.30 + static_cast<double>((rng_ >> 33) % 100) / 100.0 * 0.20;
      }
    }

    // Propagate heat upward. Process top->down so we always read row y-1
    // before any later iteration overwrites it. Each cell inherits from a
    // horizontally jittered neighbour below (the "wind") and loses a
    // randomised amount of heat — that randomness gives the flame its wispy
    // texture.
    for (int y = dot_rows - 1; y >= 1; --y) {
      // Heat decays faster toward the top so flames taper.
      const double height_frac =
          static_cast<double>(y) / static_cast<double>(std::max(1, dot_rows - 1));
      const double decay_base = 0.010 + 0.028 * height_frac;
      for (int x = 0; x < dot_cols; ++x) {
        step_rng();
        std::uint64_t r = rng_ >> 33;
        const int     offset = static_cast<int>(r % 3) - 1;  // -1, 0, +1
        r >>= 2;
        const double decay_jitter = static_cast<double>(r % 100) / 100.0 * 0.018;
        int source_x = x + offset;
        if (source_x < 0) {
          source_x = 0;
        } else if (source_x >= dot_cols) {
          source_x = dot_cols - 1;
        }
        double next = heat_[static_cast<std::size_t>(y - 1) * dot_cols + source_x] -
                      decay_base - decay_jitter;
        if (next < 0) {
          next = 0;
        }
        heat_[static_cast<std::size_t>(y) * dot_cols + x] = next;
      }
    }
  }

  void render(std::span<const float>, std::uint64_t, CellGrid& grid) override {
    const int height    = grid.rows();
    const int dot_rows  = height * 4;
    const int dot_cols  = grid.cols() * 2;
    // Cache the panel size for tick() (cliamp reads v.Rows / PanelWidth live).
    rows_ = height;
    cols_ = grid.cols();
    if (dot_rows < 4 || dot_cols < 4) {
      return;  // cliamp: blank frame
    }
    if (dot_rows_ != dot_rows || dot_cols_ != dot_cols ||
        static_cast<int>(heat_.size()) != dot_rows * dot_cols) {
      ensure(dot_rows, dot_cols);
    }

    for (int row = 0; row < height; ++row) {
      for (int col = 0; col < grid.cols(); ++col) {
        std::uint32_t braille = 0x2800;
        int           cell_tag = -1;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const int y = row * 4 + dr;
            const int x = col * 2 + dc;
            // Panel y=0 is top; the heat buffer y=0 is bottom (the source).
            const int  heat_y = dot_rows - 1 - y;
            const double h = heat_[static_cast<std::size_t>(heat_y) * dot_cols + x];

            // Wispy tips: at low heat, only stochastically light the dot so
            // the upper edge has a soft, broken silhouette.
            if (h < 0.10) {
              continue;
            }
            if (h < 0.25 && scatter_hash(0, y, x, frame_) > h * 4) {
              continue;
            }

            // Tier mapping:
            //   hottest -> yellow (mid spectrum tier)
            //   body    -> red (high tier)
            //   tips    -> red, stippled above
            const int t = (h >= 0.55) ? 1 : 2;
            braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                   [static_cast<std::size_t>(dc)];
            if (t > cell_tag) {
              cell_tag = t;
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

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // cliamp defaultDriverTickInterval.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }

  void on_enter() override {
    // cliamp flameDriver.OnEnter: the heat bed restarts from zero (the buffer
    // itself is kept allocated and re-ensured by the next tick).
    std::fill(heat_.begin(), heat_.end(), 0.0);
  }

  void on_leave() override {}

private:
  void ensure(int rows, int cols) {
    if (rows == dot_rows_ && cols == dot_cols_ &&
        static_cast<int>(heat_.size()) == rows * cols) {
      return;
    }
    heat_.assign(static_cast<std::size_t>(rows) * cols, 0.0);
    dot_rows_ = rows;
    dot_cols_ = cols;
  }

  // step_rng advances the cliamp flameDriver LCG one step. The Go driver
  // inlines this advance at each consumption point; the call sequence (and
  // thus the generated values) is identical.
  void step_rng() {
    rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
  }

  std::vector<double> heat_;
  int                 dot_rows_ = 0;
  int                 dot_cols_ = 0;
  int                 rows_     = 0;  // cached from the last render()
  int                 cols_     = 0;
  std::uint64_t       rng_      = 0xF1A3C0DE0BADCAFEULL;  // cliamp seed
  std::uint64_t       frame_    = 0;  // cliamp d.frame, set from the tick frame
};

}  // namespace

std::unique_ptr<VisDriver> make_flame_driver() {
  return std::make_unique<FlameDriver>();
}

}  // namespace bootamp::ui::vis_drivers
