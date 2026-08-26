// ui/vis_drivers/classic_peak.cpp — ClassicPeak peak-meter driver (port of
// cliamp/ui/vis_classic_peak.go; contract: vis_drivers/classic_peak.hpp).
//
// Physics port notes (1:1 with Go):
//  * All state math is double, matching Go's float64 fields.
//  * advance() clamps dt to <= 10*tickClassicPeak so long gaps (pause, sleep,
//    stalled frame) step like one frame instead of integrating over a huge
//    interval; missing/zero clocks fall back to exactly 1/60 s.
//  * bands arrive as float32 spans from the framework (smoothed spectrum);
//    Go's classicPeakDriver reads raw float64 v.bands. The C++ contract
//    declares "bands is the smoothed spectrum" for every driver, so the
//    framework passes smoothed bands here too (uniform input across drivers).
//    Values are widened to double on entry.
//  * The Go driver owns its analysis cadence (bandsAt/analysisInterval); in
//    bootamp the framework owns analysis (default_driver_tick), so
//    bands_at_ is kept only as the Go parity clock that the overlay branch
//    resets. analysisInterval is not ported.
#include "ui/vis_drivers/classic_peak.hpp"

#include "ui/tick.hpp"
#include "ui/visualizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

using Clock = std::chrono::steady_clock;

// classicPeakColsForWidth (Go): bar count for a panel width.
int classic_peak_cols_for_width(int width) {
  return std::max(1, (width + kClassicPeakBarGap) /
                         (kClassicPeakBarWidth + kClassicPeakBarGap));
}

// classicPeakRenderWidth (Go): rendered width of `cols` bars incl. gaps.
int classic_peak_render_width(int cols) {
  if (cols <= 0) {
    return 0;
  }
  return (kClassicPeakBarWidth + kClassicPeakBarGap) * cols - kClassicPeakBarGap;
}

// classicPeakGlyph maps a cap level to its (row, glyph) in the 4-row dot
// grid (Go classicPeakGlyph).
std::pair<int, char32_t> classic_peak_glyph(double level, int height) {
  const int dot_rows = std::max(1, height * 4);
  const int dot_y = static_cast<int>(std::round(
      (1.0 - std::min(1.0, level)) * static_cast<double>(dot_rows - 1)));
  const int row   = dot_y / 4;
  const char32_t glyph = kClassicPeakGlyphs[static_cast<std::size_t>(dot_y % 4)];
  return {row, glyph};
}

// classicPeakDetached (Go): the cap is visible only when it sits clearly
// above the bar (tolerance scaled to the dot grid).
bool classic_peak_detached(double level, double peak, int height) {
  const double min_gap = std::max(
      kClassicPeakVisibleEpsilon, 0.5 / static_cast<double>(std::max(1, height * 4)));
  return peak > level + min_gap;
}

// classicPeakStep eases `current` toward `target` with the rise/fall rate
// (Go classicPeakStep).
double classic_peak_step(double current, double target, double dt) {
  const double rate =
      target > current ? kClassicPeakBarRiseRate : kClassicPeakBarFallRate;
  return current + (target - current) * (1.0 - std::exp(-rate * dt));
}

// sampleBandLinear (Go visualizer.go sampleBandLinear).
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
  const int idx   = static_cast<int>(pos);
  const double frac = pos - static_cast<double>(idx);
  return static_cast<double>(bands[static_cast<std::size_t>(idx)]) * (1.0 - frac) +
         static_cast<double>(bands[static_cast<std::size_t>(idx) + 1]) * frac;
}

// resampleBandsLinear (Go visualizer.go resampleBandsLinear).
std::vector<double> resample_bands_linear(std::span<const float> bands,
                                          int total_cols) {
  if (total_cols <= 0 || bands.empty()) {
    return {};
  }
  if (static_cast<int>(bands.size()) == total_cols) {
    std::vector<double> out(bands.begin(), bands.end());
    return out;
  }
  std::vector<double> out(static_cast<std::size_t>(total_cols));
  if (total_cols == 1) {
    out[0] = sample_band_linear(bands,
                                static_cast<double>(bands.size() - 1) / 2.0);
    return out;
  }
  const double last = static_cast<double>(bands.size() - 1);
  for (int col = 0; col < total_cols; ++col) {
    const double pos = static_cast<double>(col) /
                       static_cast<double>(total_cols - 1) * last;
    out[static_cast<std::size_t>(col)] = sample_band_linear(bands, pos);
  }
  return out;
}

// fracBlock — the classic peak meters reuse the shared fractional block
// mapping (Go visualizer.go fracBlock).
char32_t frac_block(double level, double row_bottom, double row_top) {
  static constexpr char32_t kBarBlocks[9] = {
      U' ', U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
  if (level >= row_top) {
    return U'█';
  }
  if (level > row_bottom) {
    const double frac = (level - row_bottom) / (row_top - row_bottom);
    int idx = static_cast<int>(frac * 8.0);
    idx     = std::max(0, std::min(idx, 8));
    return kBarBlocks[static_cast<std::size_t>(idx)];
  }
  return U' ';
}

}  // namespace

VisAnalysisSpec ClassicPeakDriver::analysis_spec() const {
  return {kClassicPeakSpectrumBands, kClassicPeakFFTSize};
}

void ClassicPeakDriver::render(std::span<const float> bands, std::uint64_t,
                               CellGrid& grid) {
  const int height      = grid.rows();
  const int panel_width = grid.cols();
  if (height <= 0 || panel_width <= 0) {
    return;
  }
  rows_ = height;
  cols_ = panel_width;

  const auto [cols, peaks] = render_state(bands);
  const int row_pad =
      std::max(0, panel_width - classic_peak_render_width(static_cast<int>(cols.size())));

  for (int row = 0; row < height; ++row) {
    const double row_bottom =
        static_cast<double>(height - 1 - row) / static_cast<double>(height);
    const double row_top =
        static_cast<double>(height - row) / static_cast<double>(height);
    const Color color = spec_color(static_cast<float>(row_bottom));
    int col           = 0;

    for (int i = 0; i < row_pad; ++i) {
      if (col < panel_width) {
        grid.at(row, col) = Cell{U' ', color};
      }
      ++col;
    }

    for (std::size_t ci = 0; ci < cols.size(); ++ci) {
      const bool cap_visible =
          classic_peak_detached(cols[ci], peaks[ci], height);
      const auto [cap_row, cap_glyph] = classic_peak_glyph(peaks[ci], height);
      char32_t cell = frac_block(cols[ci], row_bottom, row_top);
      if (cap_visible && row == cap_row) {
        cell = cap_glyph;
      }
      for (int w = 0; w < kClassicPeakBarWidth; ++w) {
        if (col < panel_width) {
          grid.at(row, col) = Cell{cell, color};
        }
        ++col;
      }
      if (ci + 1 < cols.size()) {
        for (int g = 0; g < kClassicPeakBarGap; ++g) {
          if (col < panel_width) {
            grid.at(row, col) = Cell{U' ', color};
          }
          ++col;
        }
      }
    }
  }
}

void ClassicPeakDriver::tick(const VisTickContext& ctx, std::uint64_t&,
                             std::span<const float> bands) {
  if (ctx.overlay_active) {
    // Go: reset both clocks and freeze the physics state.
    bands_at_ = Clock::time_point{};
    last_tick_ = Clock::time_point{};
    animating_ = false;
    return;
  }
  if (!ctx.playing) {
    bands_at_ = Clock::time_point{};
  }
  sync(bands);
  if (animating(bands)) {
    advance(ctx.now, bands);
  }
  animating_ = animating(bands);
}

std::chrono::milliseconds ClassicPeakDriver::tick_interval(
    const VisTickContext& ctx) const {
  if (ctx.overlay_active) {
    return kTickSlow;
  }
  if (ctx.playing || animating_) {
    // Go model.tickInterval: band modes run at TickFast (50ms = kTickSpectrum)
    // while playing (classicPeak's own frameInterval only applies at the
    // driver level, which the model overrides while playing).
    return kTickSpectrum;
  }
  return kTickSlow;
}

void ClassicPeakDriver::on_enter() {
  // Go OnEnter: *d = classicPeakDriver{}.
  *this = ClassicPeakDriver{};
}

bool ClassicPeakDriver::animating(std::span<const float> bands) const {
  const std::vector<double> levels = this->levels(bands);
  if (levels.size() != bar_pos_.size() || levels.size() != peak_pos_.size()) {
    return false;
  }
  // Go iterates over peakVel; all four vectors are kept the same length by
  // reset(), so the min() guard is defensive only.
  const std::size_t n =
      std::min(peak_vel_.size(), std::min(bar_pos_.size(), peak_pos_.size()));
  for (std::size_t i = 0; i < n; ++i) {
    if (std::abs(bar_pos_[i] - levels[i]) > kClassicPeakVisibleEpsilon ||
        peak_vel_[i] != 0.0 ||
        peak_pos_[i] > bar_pos_[i] + kClassicPeakVisibleEpsilon) {
      return true;
    }
  }
  return false;
}

bool ClassicPeakDriver::landed(std::size_t i) const {
  return peak_vel_[i] == 0.0 &&
         peak_pos_[i] <= bar_pos_[i] + kClassicPeakVisibleEpsilon;
}

void ClassicPeakDriver::reset(const std::vector<double>& levels,
                              Clock::time_point now) {
  bar_pos_   = levels;
  peak_pos_  = levels;
  peak_vel_.assign(levels.size(), 0.0);
  peak_hold_.assign(levels.size(), 0.0);
  last_tick_ = now;
}

std::vector<double> ClassicPeakDriver::levels(std::span<const float> bands) const {
  // Go reads the global PanelWidth; before the first render bootamp falls
  // back to the styles panel width, then to the last render width.
  const int width = cols_ > 0 ? cols_ : panel_width();
  return resample_bands_linear(bands, classic_peak_cols_for_width(width));
}

void ClassicPeakDriver::sync(std::span<const float> bands) {
  const std::vector<double> levels = this->levels(bands);
  if (levels.size() != bar_pos_.size() || levels.size() != peak_pos_.size()) {
    reset(levels, Clock::time_point{});
    return;
  }
  for (std::size_t i = 0; i < levels.size(); ++i) {
    // Launch only while the cap is landed and the level rose past it.
    if (landed(i) && levels[i] > peak_pos_[i]) {
      const double delta = levels[i] - peak_pos_[i];
      peak_pos_[i] = levels[i];
      peak_vel_[i] = std::min(kClassicPeakLaunchMax,
                              kClassicPeakLaunchBase + kClassicPeakLaunchGain * delta);
      peak_hold_[i] = 0.0;
    }
  }
}

void ClassicPeakDriver::advance(Clock::time_point now,
                                std::span<const float> bands) {
  const std::vector<double> levels = this->levels(bands);
  if (levels.size() != bar_pos_.size() || levels.size() != peak_pos_.size()) {
    reset(levels, now);
    return;
  }

  double dt_seconds = kTickClassicPeakSeconds;
  if (now != Clock::time_point{} && last_tick_ != Clock::time_point{}) {
    dt_seconds = std::chrono::duration<double>(now - last_tick_).count();
  }
  // Clamp dt so long gaps (pause, sleep, stalled frame) step like one frame
  // instead of integrating physics over a huge interval (Go advance).
  if (dt_seconds <= 0.0 || dt_seconds > 10.0 * kTickClassicPeakSeconds) {
    dt_seconds = kTickClassicPeakSeconds;
  }
  last_tick_ = now;

  for (std::size_t i = 0; i < levels.size(); ++i) {
    bar_pos_[i] = classic_peak_step(bar_pos_[i], levels[i], dt_seconds);

    if (peak_hold_[i] > 0.0) {
      peak_hold_[i] = std::max(0.0, peak_hold_[i] - dt_seconds);
      if (peak_hold_[i] > 0.0) {
        continue;
      }
    }

    const double prev_vel = peak_vel_[i];
    peak_pos_[i] += peak_vel_[i] * dt_seconds;
    peak_vel_[i] -= kClassicPeakGravity * dt_seconds;

    if (peak_pos_[i] > kClassicPeakMaxHeight) {
      peak_pos_[i] = kClassicPeakMaxHeight;
    }
    if (prev_vel > 0.0 && peak_vel_[i] <= 0.0 &&
        peak_pos_[i] > bar_pos_[i] + kClassicPeakVisibleEpsilon) {
      // Apex: pause before the fall.
      peak_vel_[i] = 0.0;
      peak_hold_[i] = kClassicPeakApexHold;
      continue;
    }
    if (peak_pos_[i] <= bar_pos_[i]) {
      // Landed on the bar.
      peak_pos_[i] = bar_pos_[i];
      peak_vel_[i] = 0.0;
      peak_hold_[i] = 0.0;
    }
  }
}

std::pair<std::vector<double>, std::vector<double>>
ClassicPeakDriver::render_state(std::span<const float> bands) const {
  const std::vector<double> levels = this->levels(bands);
  if (levels.size() != bar_pos_.size() || levels.size() != peak_pos_.size()) {
    return {levels, levels};
  }
  return {bar_pos_, peak_pos_};
}

std::unique_ptr<VisDriver> make_classic_peak_driver() {
  return std::make_unique<ClassicPeakDriver>();
}

}  // namespace bootamp::ui::vis_drivers
