// ui/vis_drivers/matrix.cpp — Matrix digital rain (port of
// cliamp/ui/vis_matrix.go renderMatrix).
//
// Half-width katakana + digits fall down each terminal column at a fixed
// per-column speed (derived from the column position), leaving a 3-5 char
// trail behind the head. Band energy controls how many columns are active
// (rain density): a scatter-hash gate at frame/20 cadence decides column
// activity, and the character mutates every ~4 frames. Colors follow the
// drop position: bright head (tier high), upper trail (mid), lower trail
// (low). Render-only driver: analysis + smoothing are driven by the
// framework; tick() is a no-op and the cadence is the default driver
// interval (fast while playing, slow otherwise).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bootamp::ui::vis_drivers {

namespace {

// Half-width katakana + digits for the iconic Matrix digital rain (Go
// matrixChars; 41 runes).
constexpr std::array<char32_t, 41> kMatrixChars = {
    U'ｦ', U'ｧ', U'ｨ', U'ｩ', U'ｪ', U'ｫ', U'ｬ', U'ｭ', U'ｮ', U'ｯ',
    U'ｰ', U'ｱ', U'ｲ', U'ｳ', U'ｴ', U'ｵ', U'ｶ', U'ｷ', U'ｸ', U'ｹ', U'ｺ',
    U'ｻ', U'ｼ', U'ｽ', U'ｾ', U'ｿ', U'ﾀ', U'ﾁ', U'ﾂ', U'ﾃ', U'ﾄ',
    U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7', U'8', U'9',
};

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

// MatrixDriver — renderOnly-style: the framework drives analysis + smoothing
// and passes the smoothed bands in.
class MatrixDriver : public VisDriver {
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
      for (int b = 0; b < band_count; ++b) {
        const int bw = vis_band_width(band_count, b, panel_width);
        const double energy =
            static_cast<double>(bands[static_cast<std::size_t>(b)]);
        for (int k = 0; k < bw; ++k) {
          // Per-column seed: fixed for a column, independent of the band.
          const std::uint64_t seed =
              static_cast<std::uint64_t>(col) * 7919 + 104729;

          // Column activity: stable gate, changes every ~20 frames. Higher
          // energy activates more columns (Go scatterHash gate).
          if (scatter_hash(b, 0, col, frame / 20) > energy * 1.5 + 0.1) {
            grid.set(row, col, Cell{U' ', kColorDefault});
            ++col;
            continue;
          }

          // Fixed speed per column (2-4 frames per row step), derived from
          // the column position so each drop falls steadily (Go).
          const std::int64_t speed = 2 + static_cast<std::int64_t>(seed % 3);

          // Trail length: 3-5 characters.
          const std::int64_t trail_len =
              3 + static_cast<std::int64_t>((seed / 7) % 3);

          // Cycle length large enough for a visible gap between drops.
          const std::int64_t cycle_len =
              static_cast<std::int64_t>(height) + trail_len + 4;
          const std::int64_t offset = static_cast<std::int64_t>(
              (seed / 13) % static_cast<std::uint64_t>(cycle_len));
          const std::int64_t pos =
              (static_cast<std::int64_t>(frame) / speed + offset) % cycle_len;

          const std::int64_t dist = pos - static_cast<std::int64_t>(row);
          if (dist < 0 || dist > trail_len) {
            grid.set(row, col, Cell{U' ', kColorDefault});
            ++col;
            continue;
          }

          // Character mutates slowly (~every 4 frames).
          const std::uint64_t char_seed =
              seed ^ (static_cast<std::uint64_t>(row) * 31 + (frame / 4) * 17);
          const char32_t ch =
              kMatrixChars[char_seed % kMatrixChars.size()];

          // Drop-position color: bright head, upper trail, dim lower trail
          // (Go tags 2/1/0).
          const Color color = dist == 0    ? kColorSpecHigh
                              : dist <= 2 ? kColorSpecMid
                                          : kColorSpecLow;
          grid.set(row, col, Cell{ch, color});
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

std::unique_ptr<VisDriver> make_matrix_driver() {
  return std::make_unique<MatrixDriver>();
}

}  // namespace bootamp::ui::vis_drivers
