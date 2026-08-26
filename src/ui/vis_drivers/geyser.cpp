// ui/vis_drivers/geyser.cpp — bass-driven particle fountain (port of
// cliamp/ui/vis_geyser.go).
//
// Sustained loudness keeps a steady column of mist, bass transients launch
// strong vertical jets, and every particle then arcs back down under gravity
// with a touch of lateral spray. Particles inherit a tier from the band that
// produced them (bass red / mid yellow / treble green), so dense bass passages
// paint the column red and treble embellishments add green sparkles to the
// canopy. Stateful driver: tick() advances the particle physics with the
// smoothed bands the framework passes in; render() rasterizes the stored dot
// grid. The driver's RNG is the Go rng64 LCG seeded at 0xFEED5EED.
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

// rng64 advances a 64-bit LCG and returns a [0,1) double (Go rng64). Unsigned
// arithmetic wraps, matching Go's uint64 semantics.
double rng64(std::uint64_t& state) {
  state = state * 6364136223846793005ULL + 1442695040888963407ULL;
  return static_cast<double>((state >> 33) % 1000) / 1000.0;
}

// bandAvg returns the mean of bands[lo:hi], guarded against out-of-range
// arguments (Go bandAvg in vis_firefly.go, shared by subband consumers).
double band_avg(std::span<const float> b, int lo, int hi) {
  if (lo < 0) {
    lo = 0;
  }
  if (hi > static_cast<int>(b.size())) {
    hi = static_cast<int>(b.size());
  }
  if (hi <= lo) {
    return 0.0;
  }
  double s = 0.0;
  for (int i = lo; i < hi; ++i) {
    s += static_cast<double>(b[static_cast<std::size_t>(i)]);
  }
  return s / static_cast<double>(hi - lo);
}

// BrailleGrid is a 4×2 dot-per-cell rasteriser (Go brailleGrid in
// vis_braillegrid.go): each dot stores a tier (1..3 = low/mid/high colour,
// 0 = empty) and the renderer composes one Braille glyph per character cell.
class BrailleGrid {
public:
  void ensure(int rows, int cols) {
    if (rows == dot_rows_ && cols == dot_cols_ &&
        cells_.size() == static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols)) {
      clear();
      return;
    }
    cells_.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), 0);
    dot_rows_ = rows;
    dot_cols_ = cols;
  }

  void clear() { std::fill(cells_.begin(), cells_.end(), 0); }

  void set(int x, int y, std::int8_t tier) {
    if (x < 0 || x >= dot_cols_ || y < 0 || y >= dot_rows_) {
      return;
    }
    std::int8_t& cell = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(dot_cols_) + x];
    if (tier > cell) {
      cell = tier;
    }
  }

  int dot_rows() const { return dot_rows_; }
  int dot_cols() const { return dot_cols_; }

  // at returns the tier at (x, y); the caller has already bounds-checked the
  // rasterization region against dot_rows_/dot_cols_.
  std::int8_t at(int x, int y) const {
    return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(dot_cols_) + x];
  }

private:
  std::vector<std::int8_t> cells_;
  int                      dot_rows_ = 0;
  int                      dot_cols_ = 0;
};

struct GeyserParticle {
  double     x, y;
  double     vx, vy;
  std::int8_t tier;
  int        life;
};

class GeyserDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void tick(const VisTickContext& ctx, std::uint64_t&, std::span<const float> bands) override {
    // cliamp geyserDriver.Tick: the analysis cadence + smoothing part is
    // handled by the framework (defaultDriverTick); this is the physics half.
    if (ctx.overlay_active) {
      return;
    }
    // The render target size is cached from the last render() call — the C++
    // driver has no size access inside tick (deviates from Go, which reads
    // v.Rows/PanelWidth directly; the first tick after a resize is a no-op).
    if (!size_valid_) {
      return;
    }
    const int dot_rows = target_rows_ * 4;
    const int dot_cols = target_cols_ * 2;
    // cliamp: degenerate sizes return early.
    if (dot_rows < 4 || dot_cols < 4) {
      return;
    }
    grid_.ensure(dot_rows, dot_cols);

    if (bands.empty()) {
      return;
    }
    const int n = static_cast<int>(bands.size());
    const double bass  = band_avg(bands, 0, std::max(1, n / 3));
    const double mid   = band_avg(bands, n / 3, 2 * n / 3);
    const double high  = band_avg(bands, 2 * n / 3, n);
    const double delta = bass - prev_bass_;
    prev_bass_ = bass;

    const int jet_x      = dot_cols / 2;
    const int jet_spread = std::max(2, dot_cols / 16);

    // Steady drizzle: spawn rate scales with overall loudness. Bass weights
    // most heavily so a heavy bassline alone keeps the column flowing.
    const double steady = bass * 0.85 + mid * 0.25 + high * 0.08;
    for (int i = 0; i < static_cast<int>(steady * 6.0); ++i) {
      spawn(jet_x, dot_rows - 1, jet_spread, 1.5 + steady * 4.5, bass, mid);
    }

    // Transient kick: shoot a thick burst. Triggers on smaller deltas now so
    // even gentler kick drums register.
    if (delta > 0.06 && bass > 0.15) {
      const int burst = 40 + static_cast<int>(delta * 180.0);
      for (int i = 0; i < burst; ++i) {
        spawn(jet_x, dot_rows - 1, jet_spread * 2, 4.5 + delta * 10.0 + bass * 4.0, bass, mid);
      }
    }

    // Advance particles (Go in-place compaction of d.particles).
    constexpr double kGravity = 0.30;
    constexpr double kDrag    = 0.992;
    std::vector<GeyserParticle> live;
    live.reserve(particles_.size());
    for (const GeyserParticle& p : particles_) {
      GeyserParticle q = p;
      q.vy += kGravity;
      q.vx *= kDrag;
      q.x += q.vx;
      q.y += q.vy;
      ++q.life;
      int ix = static_cast<int>(q.x);
      int iy = static_cast<int>(q.y);
      if (iy >= dot_rows || ix < 0 || ix >= dot_cols || q.life > 200) {
        continue;
      }
      if (iy < 0) {
        iy = 0;
      }
      grid_.set(ix, iy, q.tier);
      live.push_back(q);
    }
    particles_ = std::move(live);
  }

  void render(std::span<const float>, std::uint64_t, CellGrid& grid) override {
    const int height      = grid.rows();
    const int panel_width = grid.cols();
    if (height <= 0 || panel_width <= 0) {
      return;
    }
    // Cache the render target so the next tick() sizes the dot grid.
    target_rows_ = height;
    target_cols_ = panel_width;
    size_valid_  = true;

    // cliamp brailleGrid.render: a stored grid too small for the target
    // renders blank lines (the framework pre-fills the grid with spaces).
    if (grid_.dot_rows() < height * 4 || grid_.dot_cols() < panel_width * 2) {
      return;
    }
    for (int row = 0; row < height; ++row) {
      for (int ch = 0; ch < panel_width; ++ch) {
        std::uint32_t braille = 0x2800;
        int cell_tag = -1;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const int y = row * 4 + dr;
            const int x = ch * 2 + dc;
            const std::int8_t t = grid_.at(x, y);
            if (t == 0) {
              continue;
            }
            braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                   [static_cast<std::size_t>(dc)];
            if (static_cast<int>(t) - 1 > cell_tag) {
              cell_tag = static_cast<int>(t) - 1;
            }
          }
        }
        if (cell_tag < 0) {
          cell_tag = 0;
        }
        // Tier 1/2/3 -> spec low/mid/high (Go flushStyleRun tags 0/1/2).
        Color color = kColorSpecLow;
        if (cell_tag == 1) {
          color = kColorSpecMid;
        } else if (cell_tag == 2) {
          color = kColorSpecHigh;
        }
        grid.set(row, ch, Cell{static_cast<char32_t>(braille), color});
      }
    }
  }

  void on_enter() override {
    // cliamp geyserDriver.OnEnter: reset the dot grid, particles, and the
    // previous-bass integrator. The render-size cache is kept — the panel
    // size is a property of the frame, not the driver (Go reads v.Rows).
    grid_       = BrailleGrid{};
    particles_.clear();
    prev_bass_ = 0.0;
  }

  bool pause_settled() const override {
    // cliamp geyserDriver.pauseSettled: settled when no particles remain to
    // fall out of the panel.
    return particles_.empty();
  }

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // Go model.tickInterval: band/spectrum modes run at TickFast (50ms =
    // kTickSpectrum) while playing; slow when stopped or under an overlay.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickSpectrum;
  }

private:
  void spawn(int x, int y, int spread, double vy, double bass, double mid) {
    const int jx = x + static_cast<int>(rng64(rng_) * static_cast<double>(2 * spread + 1)) -
                   spread;
    const double vy_jitter = vy * (0.6 + rng64(rng_) * 0.5);
    const double vx_jitter = (rng64(rng_) - 0.5) * (1.0 + vy * 0.4);
    const double r         = rng64(rng_);
    // Tier from the band that produced the particle (Go spawn switch; the
    // `high` band is only referenced in the default case — a no-op).
    std::int8_t tier = 1;
    if (r < bass) {
      tier = 3;
    } else if (r < bass + mid) {
      tier = 2;
    }
    particles_.push_back(
        GeyserParticle{static_cast<double>(jx), static_cast<double>(y), vx_jitter, -vy_jitter,
                       tier, 0});
  }

  BrailleGrid        grid_;
  std::vector<GeyserParticle> particles_;
  std::uint64_t      rng_        = 0xFEED5EED;  // cliamp newGeyserDriver seed
  double             prev_bass_  = 0.0;
  int                target_rows_ = 0;
  int                target_cols_ = 0;
  bool               size_valid_  = false;
};

}  // namespace

std::unique_ptr<VisDriver> make_geyser_driver() {
  return std::make_unique<GeyserDriver>();
}

}  // namespace bootamp::ui::vis_drivers
