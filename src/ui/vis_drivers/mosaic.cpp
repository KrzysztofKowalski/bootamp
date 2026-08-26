// ui/vis_drivers/mosaic.cpp — static heatmap of small tiles (port of
// cliamp/ui/vis_mosaic.go mosaicDriver).
//
// The grid never scrolls: each tile sits in a fixed (row, column) position and
// lights up or fades in place. Each cell is wired at startup to one frequency
// band and a personal ignition threshold (drawn from [0.04, 0.78] with a
// deterministic per-driver RNG, reseeded per grid generation so every visit
// reshuffles), so loud passages light up many tiles at once while quiet
// passages light only the most-sensitive ones — a speckled, gradually
// saturating pattern. Tiles render as discrete brightness tiers (space,
// ░▒▓█, hot █ yellow, overdrive █ red) and decay by 0.88 each tick.
//
// The driver keeps a cached grid size learned from render(); tick() runs the
// ignite/decay pass over the cached cells (the C++ contract's tick() carries
// no grid dimensions, so the first tick before any render is a no-op — the
// framework's first render still generates the cells deterministically).
#include "ui/vis_driver.hpp"

#include "dsp/spectrum.hpp"
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

// Tile layout constants (Go mosaicCellW / mosaicCellGap / mosaicDecay).
constexpr int    kMosaicCellW  = 2;   // characters per tile
constexpr int    kMosaicCellGap = 1;  // characters between tiles
constexpr double kMosaicDecay  = 0.88;

// Per-driver RNG (Go mosaicDriver.rng: the same PCG-style LCG as Go uses,
// seeded with 0xC1AB1A1015D5 and reseeded at each grid generation).
constexpr std::uint64_t kMosaicRngSeed = 0xC1AB1A1015D5ULL;
constexpr std::uint64_t kMosaicRngMul  = 6364136223846793005ULL;
constexpr std::uint64_t kMosaicRngInc  = 1442695040888963407ULL;

// mosaicLevel is one of the discrete brightness tiers a tile can show
// (Go mosaicLevels). Empty cells render as plain spaces so unlit positions
// disappear into the background. tier: 0 = green, 1 = yellow, 2 = red,
// -1 = no color.
struct MosaicLevel {
  char32_t glyph;
  int      tier;
};

constexpr std::array<MosaicLevel, 7> kMosaicLevels = {{
    {U' ', -1},
    {U'░', 0},
    {U'▒', 0},
    {U'▓', 0},
    {U'█', 0},
    {U'█', 1},  // hot
    {U'█', 2},  // overdrive
}};

MosaicLevel mosaic_level_for(double intensity) {
  if (intensity >= 0.85) {
    return kMosaicLevels[6];
  }
  if (intensity >= 0.65) {
    return kMosaicLevels[5];
  }
  if (intensity >= 0.45) {
    return kMosaicLevels[4];
  }
  if (intensity >= 0.28) {
    return kMosaicLevels[3];
  }
  if (intensity >= 0.15) {
    return kMosaicLevels[2];
  }
  if (intensity >= 0.05) {
    return kMosaicLevels[1];
  }
  return kMosaicLevels[0];
}

// mosaic_tile_count returns how many tiles fit horizontally in panel_width
// (Go mosaicTileCount). The last tile needs no trailing gap.
int mosaic_tile_count(int panel_width) {
  constexpr int step = kMosaicCellW + kMosaicCellGap;
  if (panel_width < kMosaicCellW) {
    return 0;
  }
  return (panel_width + kMosaicCellGap) / step;
}

// mosaic_tier_color maps a Go style-run tier to a palette slot.
Color mosaic_tier_color(int tier) {
  switch (tier) {
    case 2:  return kColorSpecHigh;
    case 1:  return kColorSpecMid;
    case 0:  return kColorSpecLow;
    default: return kColorDefault;
  }
}

struct MosaicCellState {
  int    band_idx   = 0;     // which spectrum band this cell listens to
  double threshold  = 0.0;   // band level required to ignite this cell
  double value      = 0.0;   // current displayed intensity, decays each tick
};

}  // namespace

class MosaicDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t,
              CellGrid& grid) override {
    const int rows  = grid.rows();
    const int tiles = mosaic_tile_count(grid.cols());
    if (rows <= 0 || tiles <= 0) {
      return;  // Go renders empty lines
    }
    ensure_grid(rows, tiles, static_cast<int>(bands.size()));

    for (int row = 0; row < rows; ++row) {
      int col = 0;
      for (int t = 0; t < tiles; ++t) {
        const MosaicCellState& cell =
            cells_[static_cast<std::size_t>(row * tiles + t)];
        const MosaicLevel level = mosaic_level_for(cell.value);
        const Color color = mosaic_tier_color(level.tier);
        for (int c = 0; c < kMosaicCellW; ++c) {
          grid.at(row, col) = Cell{level.glyph, color};
          ++col;
        }
        if (t < tiles - 1) {
          for (int g = 0; g < kMosaicCellGap; ++g) {
            grid.at(row, col) = Cell{U' ', kColorDefault};
            ++col;
          }
        }
      }
    }
  }

  void tick(const VisTickContext& ctx, std::uint64_t&, std::span<const float> bands) override {
    if (ctx.overlay_active) {
      return;  // Go: overlay ticks skip the ignite/decay pass
    }
    // The C++ contract's tick() carries no grid dimensions; the size is
    // learned from the last render(). Before the first render there is
    // nothing to decay (Go guards rows/tiles <= 0 the same way).
    if (rows_ <= 0 || tiles_ <= 0) {
      return;
    }
    ensure_grid(rows_, tiles_, static_cast<int>(bands.size()));
    if (bands.empty()) {
      // Still decay so cells don't stick lit during silence (Go).
      for (MosaicCellState& c : cells_) {
        c.value *= kMosaicDecay;
      }
      return;
    }

    // For each cell: if its assigned band exceeds the cell's threshold,
    // ignite (set value to the band level — clamped to 1.05 so spikes can
    // briefly promote into the yellow/red tiers). Otherwise decay in place.
    for (MosaicCellState& c : cells_) {
      const double level = static_cast<double>(
          bands[static_cast<std::size_t>(c.band_idx)]);
      if (level > c.threshold) {
        double ignited = level;
        if (ignited > 1.05) {
          ignited = 1.05;
        }
        if (ignited > c.value) {
          c.value = ignited;
        }
      }
      c.value *= kMosaicDecay;
      if (c.value < 0.001) {
        c.value = 0.0;
      }
    }
  }

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // Go defaultDriverTickInterval: fast while playing without an overlay,
    // slow otherwise.
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }

  void on_enter() override {
    // Force the grid to be regenerated on the next render/tick so each visit
    // reshuffles thresholds and band assignments — keeps the visualizer fresh
    // (Go OnEnter).
    cells_.clear();
    rows_  = 0;
    tiles_ = 0;
  }

private:
  void ensure_grid(int rows, int tiles, int band_count) {
    if (rows == rows_ && tiles == tiles_ &&
        static_cast<int>(cells_.size()) == rows * tiles) {
      return;
    }
    rows_  = rows;
    tiles_ = tiles;
    cells_.assign(static_cast<std::size_t>(rows * tiles), MosaicCellState{});
    if (band_count <= 0) {
      band_count = static_cast<int>(bootamp::dsp::kDefaultSpectrumBands);
    }

    // Each cell picks a band biased toward its row (top -> treble, bottom ->
    // bass) plus a small jitter so neighbors don't share the same band, and a
    // per-cell threshold drawn from [0.04, 0.78] so the lit-cell density rises
    // naturally with loudness (Go ensureGrid, same LCG constants + seed).
    rng_ = kMosaicRngSeed;
    for (int r = 0; r < rows; ++r) {
      int base_band = band_count / 2;
      if (rows > 1) {
        base_band = (rows - 1 - r) * (band_count - 1) / (rows - 1);
      }
      for (int c = 0; c < tiles; ++c) {
        rng_ = rng_ * kMosaicRngMul + kMosaicRngInc;
        const int jitter = static_cast<int>((rng_ >> 33) % 5) - 2;  // -2..+2
        int band         = base_band + jitter;
        band             = std::clamp(band, 0, band_count - 1);
        rng_             = rng_ * kMosaicRngMul + kMosaicRngInc;
        const double th  = 0.04 + static_cast<double>((rng_ >> 33) % 1000) / 1000.0 * 0.74;
        cells_[static_cast<std::size_t>(r * tiles + c)] =
            MosaicCellState{band, th, 0.0};
      }
    }
  }

  int                   rows_  = 0;
  int                   tiles_ = 0;
  std::vector<MosaicCellState> cells_;
  std::uint64_t         rng_   = kMosaicRngSeed;
};

std::unique_ptr<VisDriver> make_mosaic_driver() {
  return std::make_unique<MosaicDriver>();
}

}  // namespace bootamp::ui::vis_drivers
