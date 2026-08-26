// ui/vis_drivers/ascii.cpp — "Ascii" dense shade-block columns
// (port of cliamp/ui/vis_ascii.go).
//
// Thin single-character columns rendered with the Unicode shade blocks
// █ ▓ ▒ ░, using the same dense 1-wide/1-gap layout as ClassicPeak. The band
// levels are linearly resampled to the active column count
// (classicPeakColsForWidth), each row shades the fractional fill within its
// [rowBottom, rowTop] span, and the whole line carries the spectrum color of
// its row-bottom tier (Go specWrap). Render-only driver; cadence is kTickAnim
// while playing (cliamp newFastRenderOnlyDriver(..., TickAnim, ...)) — the
// classic-peak layout animates at the ~30 FPS animation cadence.
#include "ui/vis_drivers/registry.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

// cliamp classicPeakBarWidth = 1 / classicPeakBarGap = 1: the ascii driver
// shares ClassicPeak's dense 1-wide/1-gap layout.
inline constexpr int kClassicPeakBarWidth = 1;
inline constexpr int kClassicPeakBarGap   = 1;

// classicPeakColsForWidth (Go): the number of bars that fit `width` terminal
// cells with the 1-wide/1-gap layout.
int classic_peak_cols_for_width(int width) {
  return std::max(1, (width + kClassicPeakBarGap) /
                         (kClassicPeakBarWidth + kClassicPeakBarGap));
}

// sampleBandLinear (Go): linear interpolation between neighboring bands at a
// fractional position pos (clamped to the ends).
double sample_band_linear(std::span<const float> bands, double pos) {
  switch (bands.size()) {
    case 0:
      return 0.0;
    case 1:
      return static_cast<double>(bands[0]);
  }
  if (pos <= 0) {
    return static_cast<double>(bands[0]);
  }
  const double last = static_cast<double>(bands.size() - 1);
  if (pos >= last) {
    return static_cast<double>(bands[bands.size() - 1]);
  }
  const std::size_t idx  = static_cast<std::size_t>(pos);
  const double      frac = pos - static_cast<double>(idx);
  return static_cast<double>(bands[idx]) * (1.0 - frac) +
         static_cast<double>(bands[idx + 1]) * frac;
}

// resampleBandsLinear (Go): resample `bands` to `total_cols` columns via
// sampleBandLinear.
std::vector<double> resample_bands_linear(std::span<const float> bands,
                                          int total_cols) {
  if (total_cols <= 0 || bands.empty()) {
    return {};
  }
  if (bands.size() == static_cast<std::size_t>(total_cols)) {
    std::vector<double> out;
    out.reserve(bands.size());
    for (const float b : bands) {
      out.push_back(static_cast<double>(b));
    }
    return out;
  }
  std::vector<double> out(static_cast<std::size_t>(total_cols));
  if (total_cols == 1) {
    out[0] = sample_band_linear(bands, static_cast<double>(bands.size() - 1) / 2.0);
    return out;
  }
  const double last = static_cast<double>(bands.size() - 1);
  for (int col = 0; col < total_cols; ++col) {
    const double pos = static_cast<double>(col) / static_cast<double>(total_cols - 1) *
                       last;
    out[static_cast<std::size_t>(col)] = sample_band_linear(bands, pos);
  }
  return out;
}

// shadeBlock maps fractional fill within a row to the Unicode shade characters
// █ ▓ ▒ ░ (Go shadeBlock).
char32_t shade_block(double level, double row_bottom, double row_top) {
  if (level >= row_top) {
    return U'█';
  }
  if (level > row_bottom) {
    const double frac = (level - row_bottom) / (row_top - row_bottom);
    if (frac >= 0.75) {
      return U'▓';
    }
    if (frac >= 0.50) {
      return U'▒';
    }
    if (frac >= 0.25) {
      return U'░';
    }
  }
  return U' ';
}

class AsciiDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(DefaultSpectrumBands): 10 bands, 2048 FFT.
    return {10, 2048};
  }

  void render(std::span<const float> bands, std::uint64_t /*frame*/,
              CellGrid& grid) override {
    const int height = grid.rows();
    const int width  = grid.cols();
    if (height <= 0 || width <= 0) {
      return;
    }
    const int active_cols = classic_peak_cols_for_width(width);
    const std::vector<double> cols = resample_bands_linear(bands, active_cols);
    if (cols.empty()) {
      return;  // the framework always supplies bands; empty = safety
    }

    for (int row = 0; row < height; ++row) {
      const double row_bottom =
          static_cast<double>(height - 1 - row) / static_cast<double>(height);
      const double row_top =
          static_cast<double>(height - row) / static_cast<double>(height);
      // Go specWrap(rowBottom, ...): the whole line takes its row-bottom tier.
      const Color color = spec_color(static_cast<float>(row_bottom));
      for (int i = 0; i < active_cols; ++i) {
        const double level = cols[static_cast<std::size_t>(i)];
        grid.set(row, i * 2, Cell{shade_block(level, row_bottom, row_top), color});
        if (i < active_cols - 1) {
          // 1-wide/1-gap layout; the gap space is inside the colored wrap.
          grid.set(row, i * 2 + 1, Cell{U' ', color});
        }
      }
    }
  }

  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // Go model.tickInterval: band/spectrum modes run at TickFast (50ms =
    // kTickSpectrum) while playing; slow when stopped or under an overlay.
    if (ctx.playing && !ctx.overlay_active) {
      return kTickSpectrum;
    }
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }
};

}  // namespace

std::unique_ptr<VisDriver> make_ascii_driver() {
  return std::make_unique<AsciiDriver>();
}

}  // namespace bootamp::ui::vis_drivers
