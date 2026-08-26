// ui/vis_drivers/sand.cpp — falling-sand cellular automaton (port of
// cliamp/ui/vis_sand.go).
//
// A dot-grid of grains (int8 tiers: 1=green, 2=yellow, 3=red) is poured from
// the top by the spectrum bands (low bands emit red hot-bass grains, mid
// yellow, high green). Each frame grains fall straight down or slide
// diagonally onto piles; bass transients shake the bed (transient bump +
// sustained rumble), and once the bed passes ~30% capacity a strong bass kick
// converts every grain into a ballistic particle for a multi-frame explosion
// before the simulation restarts from an empty bed.
//
// The RNG is a 64-bit LCG (same constants and call sequence as Go:
// rand01 = (rng*6364136223846793005+1442695040888963407)>>33 %1000 /1000), so
// the grain sequence is bit-exact vs the Go driver for the same input stream.
// The driver is stateful: tick() advances the simulation, render() re-reads
// the grid. The VisDriver contract gives tick() no grid, so the panel
// dimensions are cached from the last render() call (cliamp reads v.Rows /
// PanelWidth live each tick; the cache is refreshed on every render, and the
// first tick before any render is a no-op because the cached size is 0).
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

// sandParticle is one grain in mid-flight during the explosion sequence
// (cliamp sandParticle): sub-dot position + velocity + tier.
struct SandParticle {
  double       x = 0.0, y = 0.0, vx = 0.0, vy = 0.0;
  std::int8_t  tier = 0;
};

// tier_color maps a cliamp style-run tag to a palette slot
// (0=low/green, 1=mid/yellow, 2=high/red).
Color tier_color(int tag) {
  switch (tag) {
    case 2:  return kColorSpecHigh;
    case 1:  return kColorSpecMid;
    default: return kColorSpecLow;
  }
}

class SandDriver : public VisDriver {
public:
  SandDriver() = default;

  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void tick(const VisTickContext& ctx, std::uint64_t& frame,
            std::span<const float> bands) override {
    // cliamp sandDriver.Tick: the framework's defaultDriverTick already ran
    // (and under an overlay it reset its clocks); the driver keeps its own
    // state frozen while the overlay is up.
    if (ctx.overlay_active) {
      return;
    }
    const int dot_rows = rows_ * 4;
    const int dot_cols = cols_ * 2;
    if (dot_rows < 4 || dot_cols < 4) {
      return;
    }
    ensure(dot_rows, dot_cols);

    const double bass       = band_avg(bands, 0, std::max(1, static_cast<int>(bands.size()) / 3));
    const int    band_count = static_cast<int>(bands.size());

    // EXPLOSION PHASE: while particles are still in flight the normal sand
    // simulation is suspended entirely; the grid is re-derived from particle
    // positions each tick so the renderer is unchanged.
    if (explosion_ttl_ > 0 || !particles_.empty()) {
      tick_explosion();
      prev_bass_ = bass;
      return;
    }

    // Spawn grains: each band emits at a column proportional to its index,
    // with a small spread so neighbouring grains don't stack into a tower.
    if (band_count > 0) {
      for (int b = 0; b < band_count; ++b) {
        const double level = static_cast<double>(bands[static_cast<std::size_t>(b)]);
        if (level < 0.10) {
          continue;
        }
        // Probability of emitting this frame scales with the band level.
        if (rand01() > level * 0.85) {
          continue;
        }
        const int centre = (b * 2 + 1) * dot_cols / (2 * band_count);
        int       spread = dot_cols / (band_count * 2);
        if (spread < 1) {
          spread = 1;
        }
        int x = centre + static_cast<int>(rand01() * static_cast<double>(2 * spread)) - spread;
        if (x < 0) {
          x = 0;
        }
        if (x >= dot_cols) {
          x = dot_cols - 1;
        }
        // Tier mapping: low bands -> red (hot bass), mid -> yellow, high -> green.
        std::int8_t tier = 1;
        if (b < band_count / 3) {
          tier = 3;  // red
        } else if (b < 2 * band_count / 3) {
          tier = 2;  // yellow
        }
        if (grid_[static_cast<std::size_t>(x)] == 0) {
          grid_[static_cast<std::size_t>(x)] = tier;
        }
      }
    }

    // Bass-driven bumps. Three regimes layered together:
    //   0. EXPLOSION — when the bed has accumulated past ~40% capacity, the
    //      next bass kick blows everything sky-high and the simulation
    //      restarts from an empty bed.
    //   1. TRANSIENT BUMP — rising edge of bass, fires once per kick: violent
    //      vertical lift across the whole bed.
    //   2. SUSTAINED RUMBLE — when bass stays high, every frame jitters
    //      grains so the bed never settles still during a heavy passage.
    const double delta = bass - prev_bass_;
    prev_bass_         = bass;

    // 0. Explosion check: fires before the normal bump branches so the grid
    // is cleared *instead* of being merely shaken when overfilled.
    if (delta > 0.06 && bass > 0.15) {
      int fill = 0;
      for (const std::int8_t g : grid_) {
        if (g != 0) {
          ++fill;
        }
      }
      if (static_cast<double>(fill) / static_cast<double>(grid_.size()) > 0.30) {
        start_explosion();
        return;
      }
    }

    // 1. Transient bump.
    if (delta > 0.06 && bass > 0.15) {
      double strength = delta * 3.5 + bass * 0.8;
      if (strength > 1.4) {
        strength = 1.4;
      }
      // Process top-down so a lifted grain isn't visited again this frame.
      for (int y = 0; y < dot_rows; ++y) {
        const double depth_frac =
            static_cast<double>(y) / static_cast<double>(std::max(1, dot_rows - 1));
        // Probability close to 1 near the bottom on a strong kick.
        const double lift_prob = std::min(strength * (0.30 + 0.70 * depth_frac), 0.95);
        // Lift height scales with strength AND depth.
        const int lift_max = 2 + static_cast<int>(strength * 7.0 * (0.4 + 0.6 * depth_frac));
        // Lateral spread also scales — sand sprays out, not just up.
        const int jitter_range = 1 + static_cast<int>(strength * 5.0);
        for (int x = 0; x < dot_cols; ++x) {
          const std::int8_t g = grid_[static_cast<std::size_t>(y) * dot_cols + x];
          if (g == 0) {
            continue;
          }
          if (rand01() > lift_prob) {
            continue;
          }
          const int lift = 1 + static_cast<int>(rand01() * static_cast<double>(lift_max));
          const int jitter =
              static_cast<int>(rand01() * static_cast<double>(2 * jitter_range + 1)) -
              jitter_range;
          int ny = y - lift;
          if (ny < 0) {
            ny = 0;
          }
          int nx = x + jitter;
          if (nx < 0) {
            nx = 0;
          }
          if (nx >= dot_cols) {
            nx = dot_cols - 1;
          }
          if (grid_[static_cast<std::size_t>(ny) * dot_cols + nx] == 0) {
            grid_[static_cast<std::size_t>(ny) * dot_cols + nx] = g;
            grid_[static_cast<std::size_t>(y) * dot_cols + x]   = 0;
          }
        }
      }
    }

    // 2. Sustained rumble — applies whenever bass is high, regardless of the
    // transient. Smaller per-grain motion but applied every frame.
    if (bass > 0.30) {
      double rumble = (bass - 0.30) * 1.8;
      if (rumble > 0.6) {
        rumble = 0.6;
      }
      // Only churn the bottom half — that's what's coupled to the speaker.
      const int min_y = dot_rows / 2;
      for (int y = min_y; y < dot_rows; ++y) {
        const double depth_frac =
            static_cast<double>(y - min_y) /
            static_cast<double>(std::max(1, dot_rows - 1 - min_y));
        const double prob = rumble * (0.15 + 0.55 * depth_frac);
        for (int x = 0; x < dot_cols; ++x) {
          const std::int8_t g = grid_[static_cast<std::size_t>(y) * dot_cols + x];
          if (g == 0) {
            continue;
          }
          if (rand01() > prob) {
            continue;
          }
          const int lift = 1 + static_cast<int>(rand01() * 2.0);  // 1..2
          const int jitter = static_cast<int>(rand01() * 5) - 2;  // -2..+2
          int ny = y - lift;
          if (ny < 0) {
            ny = 0;
          }
          int nx = x + jitter;
          if (nx < 0) {
            nx = 0;
          }
          if (nx >= dot_cols) {
            nx = dot_cols - 1;
          }
          if (grid_[static_cast<std::size_t>(ny) * dot_cols + nx] == 0) {
            grid_[static_cast<std::size_t>(ny) * dot_cols + nx] = g;
            grid_[static_cast<std::size_t>(y) * dot_cols + x]   = 0;
          }
        }
      }
    }

    // Falling pass: bottom-up so a grain we just moved into y+1 isn't moved
    // twice this frame. Grains at the bottom row leave the grid.
    for (int y = dot_rows - 2; y >= 0; --y) {
      // Alternate the horizontal scan direction each frame so piles don't
      // lean permanently to one side.
      const bool left_first = (frame % 2) == 0;
      int        start_x, end_x, step_x;
      if (left_first) {
        start_x = 0;
        end_x   = dot_cols;
        step_x  = 1;
      } else {
        start_x = dot_cols - 1;
        end_x   = -1;
        step_x  = -1;
      }
      for (int x = start_x; x != end_x; x += step_x) {
        const std::int8_t g = grid_[static_cast<std::size_t>(y) * dot_cols + x];
        if (g == 0) {
          continue;
        }
        // Try straight down.
        if (grid_[static_cast<std::size_t>(y + 1) * dot_cols + x] == 0) {
          grid_[static_cast<std::size_t>(y + 1) * dot_cols + x] = g;
          grid_[static_cast<std::size_t>(y) * dot_cols + x]     = 0;
          continue;
        }
        // Diagonal: pick left or right first based on parity for symmetry.
        int diag1 = -1, diag2 = 1;
        if (rand01() < 0.5) {
          diag1 = 1;
          diag2 = -1;
        }
        for (const int dx : {diag1, diag2}) {
          const int nx = x + dx;
          if (nx < 0 || nx >= dot_cols) {
            continue;
          }
          if (grid_[static_cast<std::size_t>(y + 1) * dot_cols + nx] == 0) {
            grid_[static_cast<std::size_t>(y + 1) * dot_cols + nx] = g;
            grid_[static_cast<std::size_t>(y) * dot_cols + x]      = 0;
            break;
          }
        }
      }
    }

    // Floor: grains in the very bottom row drift off-screen at a slow rate so
    // the grid doesn't fill up over time.
    for (int x = 0; x < dot_cols; ++x) {
      if (grid_[static_cast<std::size_t>(dot_rows - 1) * dot_cols + x] != 0 &&
          rand01() < 0.04) {
        grid_[static_cast<std::size_t>(dot_rows - 1) * dot_cols + x] = 0;
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
        static_cast<int>(grid_.size()) != dot_rows * dot_cols) {
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
            const std::int8_t g = grid_[static_cast<std::size_t>(y) * dot_cols + x];
            if (g == 0) {
              continue;
            }
            // Tier: 1=green(0), 2=yellow(1), 3=red(2).
            const int t = static_cast<int>(g) - 1;
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
    // Go model.tickInterval: band/spectrum modes run at TickFast (50ms =
    // kTickSpectrum) while playing; slow when stopped or under an overlay.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickSpectrum;
  }

  bool pause_settled() const override {
    // cliamp sandDriver.pauseSettled: not settled while grains are in flight.
    return explosion_ttl_ == 0 && particles_.empty();
  }

  void on_enter() override {
    // cliamp sandDriver.OnEnter: full state reset (rng intentionally kept).
    grid_.clear();
    dot_rows_ = 0;
    dot_cols_ = 0;
    prev_bass_ = 0.0;
    particles_.clear();
    explosion_ttl_ = 0;
  }

  void on_leave() override {}

private:
  void ensure(int rows, int cols) {
    if (rows == dot_rows_ && cols == dot_cols_ &&
        static_cast<int>(grid_.size()) == rows * cols) {
      return;
    }
    grid_.assign(static_cast<std::size_t>(rows) * cols, 0);
    dot_rows_ = rows;
    dot_cols_ = cols;
  }

  // rand01 returns a deterministic pseudo-random float in [0,1) (cliamp
  // sandDriver.rand01 — same LCG constants, same call sequence).
  double rand01() {
    rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>((rng_ >> 33) % 1000) / 1000.0;
  }

  // start_explosion converts every grain on the grid into a ballistic
  // particle with a random outward velocity, then enters the multi-frame
  // explosion phase (cliamp sandDriver.startExplosion). Bottom grains carry
  // slightly more upward energy.
  void start_explosion() {
    particles_.clear();
    for (int y = 0; y < dot_rows_; ++y) {
      const double depth_frac =
          static_cast<double>(y) / static_cast<double>(std::max(1, dot_rows_ - 1));
      for (int x = 0; x < dot_cols_; ++x) {
        const std::int8_t g = grid_[static_cast<std::size_t>(y) * dot_cols_ + x];
        if (g == 0) {
          continue;
        }
        grid_[static_cast<std::size_t>(y) * dot_cols_ + x] = 0;
        // Vertical: -3..-9 dot/frame upward, biased so bottom grains fly
        // fastest. Lateral: ±4 dot/frame for a wide spray.
        const double vy = -(2.0 + rand01() * 5.0 + depth_frac * 2.0);
        const double vx = (rand01() - 0.5) * 8.0;
        particles_.push_back(
            SandParticle{static_cast<double>(x), static_cast<double>(y), vx, vy, g});
      }
    }
    // Generous TTL — particles will mostly fall off earlier; the natural end
    // is when the particles list empties. TTL is the safety cap.
    explosion_ttl_ = 80;
  }

  // tick_explosion advances all in-flight particles one frame: gravity pulls
  // them down, drag slows lateral motion, and any particle that leaves the
  // panel through any edge is removed (cliamp sandDriver.tickExplosion). The
  // grid is fully rebuilt from the surviving particles.
  void tick_explosion() {
    constexpr double kGravity = 0.50;
    constexpr double kDrag    = 0.985;
    std::fill(grid_.begin(), grid_.end(), 0);

    std::size_t w = 0;
    for (std::size_t i = 0; i < particles_.size(); ++i) {
      SandParticle p = particles_[i];
      p.vy += kGravity;
      p.vx *= kDrag;
      p.x += p.vx;
      p.y += p.vy;
      const int ix = static_cast<int>(p.x);
      const int iy = static_cast<int>(p.y);
      if (iy < 0 || iy >= dot_rows_ || ix < 0 || ix >= dot_cols_) {
        // Off panel — particle is gone.
        continue;
      }
      grid_[static_cast<std::size_t>(iy) * dot_cols_ + ix] = p.tier;
      particles_[w++] = p;
    }
    particles_.resize(w);

    if (explosion_ttl_ > 0) {
      --explosion_ttl_;
    }
    if (particles_.empty()) {
      explosion_ttl_ = 0;
    }
  }

  std::vector<std::int8_t> grid_;  // 0 empty; 1 green; 2 yellow; 3 red
  int                      dot_rows_ = 0;
  int                      dot_cols_ = 0;
  int                      rows_     = 0;  // cached from the last render()
  int                      cols_     = 0;
  std::uint64_t            rng_      = 0x5A4D5A4D5A4DULL;  // cliamp seed
  double                   prev_bass_ = 0.0;
  std::vector<SandParticle> particles_;
  int                       explosion_ttl_ = 0;
};

}  // namespace

std::unique_ptr<VisDriver> make_sand_driver() {
  return std::make_unique<SandDriver>();
}

}  // namespace bootamp::ui::vis_drivers
