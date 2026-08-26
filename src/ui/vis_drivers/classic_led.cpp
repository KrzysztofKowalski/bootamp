// ui/vis_drivers/classic_led.cpp — Winamp 2.9 LED matrix with falling peak caps
// (port of cliamp/ui/vis_classic_led.go).
//
// Each spectrum slot renders as a 2-cell wide LED bar of "▄" glyphs; the peak
// cap "▀" holds at the apex for classicLEDPeakHold seconds, then falls at a
// constant rate (quantized into LED rows by render). Body levels ease toward
// the analysis output with a fast attack / medium decay. Frame cadence is
// classicLED's own 30 FPS (time.Second/30), not the shared TickAnim.
//
// Framework contract notes (see also vis_driver.hpp):
//  * The C++ framework runs the analysis cadence gate + smoothing centrally
//    (default_driver_tick) and hands the driver the smoothed bands, so the
//    driver's own analysis bookkeeping (Go bandsAt / lastAnalyzeAt) is not
//    ported — tick() eases body/peak toward the bands it is given. The Go
//    original reads v.bands (raw); here the target is the smoothed span.
//  * analysis_spec() must report the band count before any render, when the
//    framework's grid size is unknown, so it uses ui::panel_width() (Go's
//    PanelWidth at analysis time); render() uses grid.cols() (Go sets
//    PanelWidth = v.columns() before Render). With the default padding these
//    agree (74).
#include "ui/vis_driver.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <chrono>
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

// cliamp vis_classic_led.go constants.
inline constexpr int    kLedFFTSize  = 2048;  // classicLEDFFTSize
inline constexpr int    kLedBarWidth = 2;     // classicLEDBarWidth
inline constexpr int    kLedBarGap   = 1;     // classicLEDBarGap
inline constexpr int    kLedFPS      = 30;    // classicLEDFPS
inline constexpr double kLedRiseRate = 60.0;  // classicLEDRiseRate
inline constexpr double kLedFallRate = 16.0;  // classicLEDFallRate
inline constexpr double kLedPeakHold = 0.45;  // classicLEDPeakHold
inline constexpr double kLedPeakFall = 0.55;  // classicLEDPeakFall
inline constexpr double kLedEpsilon  = 1e-3;  // classicLEDEpsilon

// classicLEDBarCount: how many bars fit in `width` cells (min 1).
int led_bar_count(int width) {
  return std::max(1, (width + kLedBarGap) / (kLedBarWidth + kLedBarGap));
}

// classicLEDRenderWidth: width in cells occupied by `bars` bars.
int led_render_width(int bars) {
  if (bars <= 0) {
    return 0;
  }
  return bars * (kLedBarWidth + kLedBarGap) - kLedBarGap;
}

// sample_band_linear (cliamp sampleBandLinear): linear interpolation at
// fractional band position `pos`; double math like the Go original.
double sample_band_linear(std::span<const float> bands, double pos) {
  switch (bands.size()) {
    case 0:
      return 0.0;
    case 1:
      return static_cast<double>(bands[0]);
    default:
      break;
  }
  if (pos <= 0) {
    return static_cast<double>(bands[0]);
  }
  const double last = static_cast<double>(bands.size() - 1);
  if (pos >= last) {
    return static_cast<double>(bands[bands.size() - 1]);
  }
  const std::size_t idx = static_cast<std::size_t>(pos);
  const double      frac = pos - static_cast<double>(idx);
  return static_cast<double>(bands[idx]) * (1.0 - frac) +
         static_cast<double>(bands[idx + 1]) * frac;
}

// resample_bands_linear (cliamp resampleBandsLinear): resample the band span
// to exactly `total_cols` positions. Empty input yields empty output (Go nil).
std::vector<double> resample_bands_linear(std::span<const float> bands,
                                          int total_cols) {
  if (total_cols <= 0 || bands.empty()) {
    return {};
  }
  if (static_cast<int>(bands.size()) == total_cols) {
    return std::vector<double>(bands.begin(), bands.end());
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

// ClassicLEDDriver (cliamp classicLEDDriver): stateful LED matrix.
class ClassicLEDDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // Go AnalysisSpec: classicLEDBarCount(PanelWidth), classicLEDFFTSize.
    // The C++ framework queries this before any render, when the grid size is
    // unknown, so the count comes from ui::panel_width() (default 74 -> 25).
    return {led_bar_count(panel_width()), kLedFFTSize};
  }

  void on_enter() override {
    // Go OnEnter: reset the whole driver (`*d = classicLEDDriver{}`).
    body_.clear();
    peak_.clear();
    hold_.clear();
    last_bands_.clear();
    last_tick_ = Clock::time_point{};
  }

  void render(std::span<const float> bands, std::uint64_t,
              CellGrid& grid) override {
    const int height = grid.rows();
    const int width  = grid.cols();
    if (height <= 0 || width <= 0) {
      return;
    }
    const int bars = led_bar_count(width);
    const auto [body, peak] = render_state(bands, bars);

    const int    row_pad  = std::max(0, width - led_render_width(bars));
    const double height_f = static_cast<double>(height);

    for (int row = 0; row < height; ++row) {
      // Go specWrap(rowBottom, ...) colors the whole line by its row-bottom
      // spectrum tier.
      const float row_bottom =
          static_cast<float>(height - 1 - row) / static_cast<float>(height);
      const Color color = spec_color(row_bottom);
      const int   rfb   = height - 1 - row;  // row position from the bottom

      int c = 0;
      for (int p = 0; p < row_pad; ++p) {
        grid.set(row, c++, Cell{U' ', color});
      }
      for (int b = 0; b < bars; ++b) {
        const int lit = static_cast<int>(std::floor(body[b] * height_f + 1e-6));
        // Quantize the peak to a row index. A peak only renders when it sits
        // strictly above the bar body; otherwise it merges with the LED.
        int peak_seg = static_cast<int>(std::floor(peak[b] * height_f + 1e-6));
        if (peak_seg >= height) {
          peak_seg = height - 1;
        }
        const bool show_peak = peak[b] > body[b] + 0.5 / height_f &&
                               peak_seg >= lit;

        char32_t glyph = U' ';
        if (rfb < lit) {
          glyph = U'▄';
        } else if (show_peak && rfb == peak_seg) {
          glyph = U'▀';
        }
        for (int k = 0; k < kLedBarWidth; ++k) {
          grid.set(row, c++, Cell{glyph, color});
        }
        if (b < bars - 1) {
          grid.set(row, c++, Cell{U' ', color});
        }
      }
    }
  }

  void tick(const VisTickContext& ctx, std::uint64_t&,
            std::span<const float> bands) override {
    if (ctx.overlay_active) {
      // Go resets both wall clocks so the first tick after dismissal advances
      // with a single-frame step.
      last_tick_ = Clock::time_point{};
      return;
    }
    // Keep the latest target bands for tick_interval()'s animating() check
    // (Go reads v.bands there; tick_interval is const and gets no bands).
    last_bands_.assign(bands.begin(), bands.end());
    advance(ctx.now);
  }

  std::chrono::milliseconds
  tick_interval(const VisTickContext& ctx) const override {
    if (ctx.overlay_active) {
      return kTickSlow;
    }
    if (ctx.playing || animating()) {
      return frame_interval();
    }
    return kTickSlow;
  }

private:
  // frameInterval: time.Second / classicLEDFPS = 33.33ms. C++ milliseconds
  // truncate to 33ms (== kTickAnim, the ~30 FPS animation tier).
  static std::chrono::milliseconds frame_interval() {
    return std::chrono::milliseconds(1000 / kLedFPS);
  }

  // levels (Go levels()): the analysis bands resampled to the bar count.
  std::vector<double> levels() const {
    return resample_bands_linear(last_bands_, led_bar_count(panel_width()));
  }

  // animating (Go animating()): any body content, visible peak, or drift from
  // the current levels keeps the driver on the fast cadence.
  bool animating() const {
    if (body_.empty()) {
      return false;
    }
    const std::vector<double> lv = levels();
    if (lv.size() != body_.size()) {
      return false;
    }
    for (std::size_t i = 0; i < body_.size(); ++i) {
      if (body_[i] > kLedEpsilon || peak_[i] > body_[i] + kLedEpsilon ||
          std::abs(body_[i] - lv[i]) > kLedEpsilon) {
        return true;
      }
    }
    return false;
  }

  // advance (Go advance()): ease body toward the levels and run the peak
  // hold/fall state machine. dt is clamped so long gaps step like one frame.
  void advance(Clock::time_point now) {
    const std::vector<double> lv = levels();
    if (body_.size() != lv.size() || peak_.size() != lv.size() ||
        hold_.size() != lv.size()) {
      body_ = lv;
      peak_ = lv;
      hold_.assign(lv.size(), 0.0);
      last_tick_ = now;
      return;
    }

    double dt_s = 1.0 / static_cast<double>(kLedFPS);  // frameInterval
    if (now != Clock::time_point{} && last_tick_ != Clock::time_point{}) {
      dt_s = std::chrono::duration<double>(now - last_tick_).count();
    }
    // Clamp dt so long gaps (sleep, overlay dismiss) step like one frame
    // rather than integrating peak decay over a huge interval.
    if (dt_s <= 0 || dt_s > 10.0 / static_cast<double>(kLedFPS)) {
      dt_s = 1.0 / static_cast<double>(kLedFPS);
    }
    last_tick_ = now;

    for (std::size_t i = 0; i < lv.size(); ++i) {
      const double target = lv[i];
      const double rate =
          target > body_[i] ? kLedRiseRate : kLedFallRate;
      body_[i] += (target - body_[i]) * (1.0 - std::exp(-rate * dt_s));

      if (body_[i] >= peak_[i]) {
        peak_[i] = body_[i];
        hold_[i] = kLedPeakHold;
      } else if (hold_[i] > 0) {
        hold_[i] = std::max(0.0, hold_[i] - dt_s);
      } else {
        peak_[i] = std::max(body_[i], peak_[i] - kLedPeakFall * dt_s);
      }
    }
  }

  // render_state (Go renderState()): the body/peak pair to draw — the eased
  // state when it matches the current bar count, else levels computed from
  // the bands (all-zero when the count still mismatches).
  std::pair<std::vector<double>, std::vector<double>>
  render_state(std::span<const float> bands, int bars) const {
    if (body_.size() == static_cast<std::size_t>(bars) &&
        peak_.size() == static_cast<std::size_t>(bars)) {
      return {body_, peak_};
    }
    std::vector<double> lv = resample_bands_linear(bands, bars);
    if (lv.size() != static_cast<std::size_t>(bars)) {
      lv.assign(static_cast<std::size_t>(bars), 0.0);
    }
    return {lv, lv};
  }

  std::vector<double> body_;   // eased LED level per bar
  std::vector<double> peak_;   // peak cap level per bar
  std::vector<double> hold_;   // remaining peak hold time (seconds)
  std::vector<float>  last_bands_;  // bands from the most recent tick
  Clock::time_point   last_tick_;
};

}  // namespace

std::unique_ptr<VisDriver> make_classic_led_driver() {
  return std::make_unique<ClassicLEDDriver>();
}

}  // namespace bootamp::ui::vis_drivers
