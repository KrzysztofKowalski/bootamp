// ui/vis_drivers/rain.cpp — bar columns with falling rain streaks (port of
// cliamp/ui/vis_rain.go renderRain).
//
// Each band column is a bar whose height follows the band level; inside the
// bar, drops fall at a per-column speed (1-3 frames per row step) with a
// head/body/tail: bright head (┃, tier high), upper body (│, mid), tail
// (:, low). A scatter-hash gate at frame/12 cadence keeps only a fraction of
// columns active, growing with energy. Render-only driver: analysis +
// smoothing are driven by the framework; tick() is a no-op and the cadence
// is the default driver interval (fast while playing, slow otherwise).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bootamp::ui::vis_drivers {

namespace {

// vis_band_width returns the character width for band b in a panel of
// panel_width cells (Go visBandWidth). At narrow widths only the leading
// visible bands receive columns.
int vis_band_width(int total_bands, int b, int panel_width) {
  if (total_bands <= 0 || b < 0 || b >= total_bands || panel_width <= 0) {
    return 0;
  }
  const int visible_bands = std::min(total_bands, panel_width);
  if (b >= visible_bands) {
    return 0;
  }
  const int gap_count =
      std::min(visible_bands - 1, std::max(0, panel_width - visible_bands));
  const int band_cols = panel_width - gap_count;
  const int base      = band_cols / visible_bands;
  const int extra     = band_cols % visible_bands;
  return b < extra ? base + 1 : base;
}

// scatter_hash returns a pseudo-random value in [0, 1) for a given position
// and frame (Go scatterHash). Computed in double like the Go original so the
// column-activity gate matches bit-for-bit.
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

// RainDriver — renderOnly-style: the framework drives analysis + smoothing
// and passes the smoothed bands in.
class RainDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override {
    const int height      = grid.rows();
    const int panel_width = grid.cols();
    if (height <= 0 || panel_width <= 0) {
      return;
    }
    const int band_count = static_cast<int>(bands.size());
    if (band_count <= 0) {
      return;  // Go renders empty rows; the grid is already space-filled
    }

    for (int row = 0; row < height; ++row) {
      int col = 0;
      const double row_norm =
          static_cast<double>(height - 1 - row) / static_cast<double>(height);

      for (int b = 0; b < band_count; ++b) {
        const int bw = vis_band_width(band_count, b, panel_width);
        const double level =
            static_cast<double>(bands[static_cast<std::size_t>(b)]);

        for (int k = 0; k < bw; ++k) {
          if (row_norm >= level) {
            // Above the bar — empty.
            grid.set(row, col, Cell{U' ', kColorDefault});
            ++col;
            continue;
          }

          // Inside the bar — check if this column is active.
          const std::uint64_t seed =
              static_cast<std::uint64_t>(col) * 7919 + 104729;

          // Column activation: gate changes slowly, energy controls density
          // (Go scatterHash gate at frame/12).
          if (scatter_hash(b, 0, col, frame / 12) > level * 1.6 + 0.1) {
            grid.set(row, col, Cell{U' ', kColorDefault});
            ++col;
            continue;
          }

          // Per-column fall speed: 1-3 frames per row step.
          const std::int64_t speed = 1 + static_cast<std::int64_t>(seed % 3);

          // Drop length: 2-4 characters.
          const std::int64_t drop_len =
              2 + static_cast<std::int64_t>((seed / 7) % 3);

          // Cycle through visible height with gap before repeating.
          const std::int64_t cycle_len =
              static_cast<std::int64_t>(height) + drop_len + 3;
          const std::int64_t offset = static_cast<std::int64_t>(
              (seed / 13) % static_cast<std::uint64_t>(cycle_len));
          const std::int64_t pos =
              (static_cast<std::int64_t>(frame) / speed + offset) % cycle_len;

          const std::int64_t dist = pos - static_cast<std::int64_t>(row);
          if (dist >= 0 && dist < drop_len) {
            // Head gets ┃, body gets thinner chars (Go).
            const char32_t ch =
                dist == 0    ? U'┃'
                : dist == 1  ? U'│'
                             : U':';
            // Color by drop position: bright head, mid body, dim tail (Go
            // tags 2/1/0).
            const Color color = dist == 0    ? kColorSpecHigh
                                : dist == 1  ? kColorSpecMid
                                             : kColorSpecLow;
            grid.set(row, col, Cell{ch, color});
          } else {
            // Empty space between drops.
            grid.set(row, col, Cell{U' ', kColorDefault});
          }
          ++col;
        }
        if (b < band_count - 1) {  // inter-band gap column (Go)
          grid.set(row, col, Cell{U' ', kColorDefault});
          ++col;
        }
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

std::unique_ptr<VisDriver> make_rain_driver() {
  return std::make_unique<RainDriver>();
}

}  // namespace bootamp::ui::vis_drivers
