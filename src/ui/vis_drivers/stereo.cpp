// ui/vis_drivers/stereo.cpp — stereo L/R horizontal LED peak meters
// (port of cliamp/ui/vis_stereo.go).
//
// Each channel renders as a horizontal meter: RMS level ("▮") and peak ("■")
// in a dB-normalized scale (stereoFloorDB = -48 dB -> 0, unity -> 1). The two
// channels stack vertically; odd heights leave one blank middle row. Levels
// ease toward the target with a fast attack / slow decay and the peak holds
// for stereoPeakHold then falls at a constant rate. While not playing the
// targets zero out so paused meters decay to rest; the framework keeps ticking
// until tick_interval() drops to kTickSlow (Go signals settling through the
// interval, not visPauseSettler — pause_settled() stays default).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

using Clock = std::chrono::steady_clock;

// cliamp vis_stereo.go constants.
inline constexpr int    kStereoWindow        = 2048;  // stereoSampleWindow
inline constexpr double kStereoFloorDB       = -48.0;  // stereoFloorDB
inline constexpr double kStereoRiseRate      = 36.0;   // stereoRiseRate
inline constexpr double kStereoFallRate      = 10.0;   // stereoFallRate
inline constexpr double kStereoPeakHold      = 0.45;   // stereoPeakHold (450ms, in seconds)
inline constexpr double kStereoPeakFallRate  = 0.65;   // stereoPeakFallRate
inline constexpr double kStereoEpsilon       = 1e-3;   // stereoEpsilon
// cliamp maxSmoothDtFrames: cap on dt fed into smoothing easing.
inline constexpr int kMaxSmoothDtFrames = 10;

// stereo_db_level (Go stereoDBLevel): dB of the amplitude normalized into
// [0,1] with stereoFloorDB as the zero point.
double stereo_db_level(double amplitude) {
  if (amplitude <= 0) {
    return 0;
  }
  const double db = 20.0 * std::log10(amplitude);
  return std::max(0.0, std::min(1.0, (db - kStereoFloorDB) / -kStereoFloorDB));
}

// stereo_metrics (Go stereoMetrics): per-channel RMS level + peak, both dB-
// normalized. Returns zeroes for an empty window.
std::pair<std::array<double, 2>, std::array<double, 2>>
stereo_metrics(std::span<const std::array<float, 2>> samples) {
  std::array<double, 2> sum_squares{};
  std::array<double, 2> peak{};
  if (samples.empty()) {
    return {sum_squares, peak};
  }
  for (const auto& sample : samples) {
    for (int channel = 0; channel < 2; ++channel) {
      const double value =
          static_cast<double>(sample[static_cast<std::size_t>(channel)]);
      sum_squares[static_cast<std::size_t>(channel)] += value * value;
      peak[static_cast<std::size_t>(channel)] =
          std::max(peak[static_cast<std::size_t>(channel)], std::abs(value));
    }
  }
  std::array<double, 2> level{};
  for (int channel = 0; channel < 2; ++channel) {
    const double rms = std::sqrt(sum_squares[static_cast<std::size_t>(channel)] /
                                 static_cast<double>(samples.size()));
    level[static_cast<std::size_t>(channel)] = stereo_db_level(rms);
    peak[static_cast<std::size_t>(channel)] =
        stereo_db_level(peak[static_cast<std::size_t>(channel)]);
  }
  return {level, peak};
}

// StereoDriver (cliamp stereoDriver): stateful stereo meters.
class StereoDriver : public VisDriver {
public:
  StereoDriver() : samples_(static_cast<std::size_t>(kStereoWindow)) {}

  VisAnalysisSpec analysis_spec() const override {
    // Go spectrumAnalysisSpec(0): raw-sample mode, default FFT size.
    return {0, 2048};
  }

  void on_enter() override {
    // Go OnEnter keeps the sample buffer across the reset.
    samples_.resize(static_cast<std::size_t>(kStereoWindow));
    level_       = {};
    peak_        = {};
    hold_        = {};
    target_level_ = {};
    target_peak_  = {};
    last_tick_    = Clock::time_point{};
    samples_at_   = Clock::time_point{};
  }

  void render(std::span<const float>, std::uint64_t, CellGrid& grid) override {
    const int height = grid.rows();
    if (height <= 0) {
      return;  // Go: height <= 0 renders ""
    }
    const int width = grid.cols();

    if (height == 1) {
      render_meter(grid, 0, "L ", level_[0], peak_[0], width);
      return;
    }

    const int thickness = height / 2;
    int       row       = 0;

    for (int i = 0; i < thickness; ++i) {
      std::string_view label = "  ";
      if (i == thickness / 2) {
        label = "L ";
      }
      render_meter(grid, row + i, label, level_[0], peak_[0], width);
    }
    row += thickness;
    if (height % 2 != 0) {
      ++row;  // one blank row between the channels (grid stays pre-cleared)
    }
    for (int i = 0; i < thickness; ++i) {
      std::string_view label = "  ";
      if (i == thickness / 2) {
        label = "R ";
      }
      render_meter(grid, row + i, label, level_[1], peak_[1], width);
    }
  }

  void tick(const VisTickContext& ctx, std::uint64_t&,
            std::span<const float>) override {
    if (ctx.overlay_active) {
      // Go resets both clocks so the first tick after dismissal samples
      // immediately and smoothing dt resets to a single-frame step.
      last_tick_  = Clock::time_point{};
      samples_at_ = Clock::time_point{};
      return;
    }
    if (ctx.playing) {
      sample(ctx);
    } else {
      // Not playing (incl. paused): targets zero out so the meters decay to
      // rest instead of freezing mid-frame.
      target_level_ = {};
      target_peak_  = {};
      samples_at_   = Clock::time_point{};
    }
    advance(ctx.now);
  }

  std::chrono::milliseconds
  tick_interval(const VisTickContext& ctx) const override {
    if (ctx.overlay_active) {
      return kTickSlow;
    }
    if (ctx.playing || animating()) {
      // Go TickAnim (16ms, ~60 FPS animation) — the C++ fast tier.
      return kTickFast;
    }
    return kTickSlow;
  }

private:
  // sample (Go sample()): pull a stereo window from the tap at most every
  // kTickAnalyze and derive the target level/peak.
  void sample(const VisTickContext& ctx) {
    if (!ctx.stereo_samples_into) {
      target_level_ = {};
      target_peak_  = {};
      return;
    }
    if (samples_at_ != Clock::time_point{} && ctx.now != Clock::time_point{} &&
        (ctx.now - samples_at_) < kTickAnalyze) {
      return;
    }
    const std::size_t n =
        ctx.stereo_samples_into(std::span<std::array<float, 2>>(samples_));
    const auto [level, peak] =
        stereo_metrics(std::span<const std::array<float, 2>>(samples_).first(n));
    target_level_ = level;
    target_peak_  = peak;
    if (ctx.now != Clock::time_point{}) {
      samples_at_ = ctx.now;
    }
  }

  // advance (Go advance()): ease level toward the target, run the peak
  // hold/fall state machine. dt is clamped so long gaps step like one frame.
  void advance(Clock::time_point now) {
    double dt_s = std::chrono::duration<double>(kTickAnim).count();
    if (now != Clock::time_point{} && last_tick_ != Clock::time_point{}) {
      dt_s = std::chrono::duration<double>(now - last_tick_).count();
    }
    if (dt_s <= 0 || dt_s > static_cast<double>(kMaxSmoothDtFrames) *
                                 std::chrono::duration<double>(kTickAnim).count()) {
      dt_s = std::chrono::duration<double>(kTickAnim).count();
    }
    last_tick_ = now;

    for (int channel = 0; channel < 2; ++channel) {
      const std::size_t ch = static_cast<std::size_t>(channel);
      const double rate =
          target_level_[ch] > level_[ch] ? kStereoRiseRate : kStereoFallRate;
      level_[ch] += (target_level_[ch] - level_[ch]) *
                    (1.0 - std::exp(-rate * dt_s));

      if (target_peak_[ch] > peak_[ch]) {
        peak_[ch] = target_peak_[ch];
        hold_[ch] = kStereoPeakHold;
      } else if (hold_[ch] > 0) {
        hold_[ch] = std::max(0.0, hold_[ch] - dt_s);
      } else {
        peak_[ch] = std::max(level_[ch], peak_[ch] - kStereoPeakFallRate * dt_s);
      }
    }
  }

  // animating (Go animating()): any meter content or drift from the targets
  // keeps the driver on the animation cadence.
  bool animating() const {
    for (int channel = 0; channel < 2; ++channel) {
      const std::size_t ch = static_cast<std::size_t>(channel);
      if (level_[ch] > kStereoEpsilon || peak_[ch] > kStereoEpsilon ||
          std::abs(level_[ch] - target_level_[ch]) > kStereoEpsilon) {
        return true;
      }
    }
    return false;
  }

  // render_meter (Go renderStereoMeter): one horizontal meter row. Cells
  // before the level are "·", lit cells "▮", the peak cell "■"; consecutive
  // same-tier cells share the spectrum color (Go flushStyleRun), unstyled
  // runs (label + unlit cells) stay default.
  static void render_meter(CellGrid& grid, int row, std::string_view label,
                           double level, double peak, int width) {
    if (width <= 0) {
      return;  // Go: width <= 0 renders ""
    }
    if (width <= static_cast<int>(label.size())) {
      for (int c = 0; c < width; ++c) {
        grid.set(row, c, Cell{static_cast<char32_t>(
                                 static_cast<unsigned char>(label[static_cast<std::size_t>(c)])),
                              kColorDefault});
      }
      return;
    }

    const int cells = width - static_cast<int>(label.size());
    const int lit   = std::min(cells, static_cast<int>(std::round(level * cells)));
    int       peak_cell = -1;
    if (peak > 0) {
      peak_cell = std::min(cells - 1,
                           std::max(0, static_cast<int>(std::round(peak * cells)) - 1));
    }

    int c = 0;
    for (const char ch : label) {
      grid.set(row, c++, Cell{static_cast<char32_t>(static_cast<unsigned char>(ch)),
                              kColorDefault});
    }
    for (int cell = 0; cell < cells; ++cell) {
      char32_t glyph = U'·';
      int      tag   = -1;
      if (cell < lit) {
        glyph = U'▮';
        tag = spec_tag(static_cast<float>(cell) /
                       static_cast<float>(std::max(1, cells - 1)));
      }
      if (cell == peak_cell) {
        glyph = U'■';
        tag = spec_tag(static_cast<float>(cell) /
                       static_cast<float>(std::max(1, cells - 1)));
      }
      Color color = kColorDefault;  // tag -1: unstyled run
      if (tag == 2) {
        color = kColorSpecHigh;
      } else if (tag == 1) {
        color = kColorSpecMid;
      } else if (tag == 0) {
        color = kColorSpecLow;
      }
      grid.set(row, c++, Cell{glyph, color});
    }
  }

  std::vector<std::array<float, 2>> samples_;  // stereo tap window
  std::array<double, 2> level_ {};             // eased meter level per channel
  std::array<double, 2> peak_ {};              // peak cap per channel
  std::array<double, 2> hold_ {};              // remaining peak hold (seconds)
  std::array<double, 2> target_level_ {};
  std::array<double, 2> target_peak_ {};
  Clock::time_point     last_tick_;
  Clock::time_point     samples_at_;
};

}  // namespace

std::unique_ptr<VisDriver> make_stereo_driver() {
  return std::make_unique<StereoDriver>();
}

}  // namespace bootamp::ui::vis_drivers
