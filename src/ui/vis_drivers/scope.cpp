// ui/vis_drivers/scope.cpp — Lissajous-style XY oscilloscope in Braille dots
// (port of cliamp/ui/vis_scope.go).
//
// Since the audio tap is mono, a phase-delayed copy of the signal is used as
// the Y axis. The delay slowly oscillates over time (sin of the frame counter),
// producing continuously evolving Lissajous figures — circles for pure tones,
// complex knots for music. ~512 XY pairs are plotted with linear interpolation
// between consecutive points, then packed into 4×2 Braille blocks. Row-bottom
// tiers pick the spectrum color per line, like Go's specWrap.
//
// Raw-sample driver: same plumbing as the wave driver — tick() pulls its own
// sample window via VisTickContext::waveform_samples_into, clears it when
// ctx.paused is set, and reports pause_settled() from it.
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

// Max waveform window offered to the tap on each tick (Go sampleBuf sized to
// defaultFFTSize = 2048).
inline constexpr std::size_t kScopeBufMax = 2048;

// ScopeDriver (cliamp renderScope, registered with a raw-sample spec and
// TickWave).
class ScopeDriver : public VisDriver {
public:
  ScopeDriver() : samples_(kScopeBufMax) {}

  VisAnalysisSpec analysis_spec() const override {
    // Go spectrumAnalysisSpec(0): raw-sample mode, default FFT size.
    return {0, 2048};
  }

  void tick(const VisTickContext& ctx, std::uint64_t&,
            std::span<const float>) override {
    if (ctx.paused) {
      // Contract: raw-sample drivers hold their waveform internally and must
      // clear it on a paused tick so pause_settled() can report decay.
      n_ = 0;
      return;
    }
    if (ctx.overlay_active) {
      return;
    }
    // Go defaultDriverTick: raw-sample modes refresh their waveform on every
    // tick (no FFT work, so no kTickAnalyze gate).
    if (ctx.waveform_samples_into) {
      n_ = ctx.waveform_samples_into(std::span<float>(samples_));
    }
  }

  bool pause_settled() const override { return n_ == 0; }

  std::chrono::milliseconds
  tick_interval(const VisTickContext& ctx) const override {
    // Go renderOnlyDriver with tickDuration = TickWave: playing (no overlay)
    // runs at TickWave; otherwise the default cadence.
    if (ctx.overlay_active) {
      return kTickSlow;
    }
    if (ctx.playing) {
      return kTickWave;
    }
    return kTickSlow;
  }

  void render(std::span<const float>, std::uint64_t frame,
              CellGrid& grid) override {
    const int height = grid.rows();
    const int width  = grid.cols();
    if (height <= 0 || width <= 0) {
      return;
    }
    const int dot_rows = height * 4;
    const int dot_cols = width * 2;
    const int n        = static_cast<int>(n_);

    std::vector<std::uint8_t> dots(static_cast<std::size_t>(dot_rows) * dot_cols,
                                   0);

    if (n > 1) {
      // Phase delay slowly oscillates for evolving Lissajous patterns.
      const int base_delay = n / 4;
      const int wobble = static_cast<int>(
          std::sin(static_cast<double>(frame) * 0.02) *
          static_cast<double>(n / 8));
      const int delay = std::max(1, std::min(n - 1, base_delay + wobble));

      // Plot ~512 XY pairs for a dense, smooth figure.
      const int plot_points = std::min(n - delay, 512);
      const int step        = std::max(1, (n - delay) / plot_points);

      int  prev_dot_x = 0;
      int  prev_dot_y = 0;
      bool first      = true;

      for (int i = 0; i + delay < n; i += step) {
        const double x = static_cast<double>(samples_[static_cast<std::size_t>(i)]);
        const double y = static_cast<double>(
            samples_[static_cast<std::size_t>(i + delay)]);

        // Map [-1, 1] to dot coordinates.
        int dot_x = static_cast<int>((x + 1.0) * 0.5 * static_cast<double>(dot_cols - 1));
        int dot_y = static_cast<int>((1.0 - y) * 0.5 * static_cast<double>(dot_rows - 1));
        dot_x     = std::max(0, std::min(dot_cols - 1, dot_x));
        dot_y     = std::max(0, std::min(dot_rows - 1, dot_y));

        dots[static_cast<std::size_t>(dot_y) * dot_cols + dot_x] = 1;

        // Interpolate between consecutive points for smoother curves.
        if (!first) {
          const int dx = dot_x - prev_dot_x;
          const int dy = dot_y - prev_dot_y;
          const int adx = dx < 0 ? -dx : dx;
          const int ady = dy < 0 ? -dy : dy;
          const int steps = std::max(adx, ady);
          if (steps > 0 && steps < 30) {
            for (int s = 1; s < steps; ++s) {
              const int mx = prev_dot_x + dx * s / steps;
              const int my = prev_dot_y + dy * s / steps;
              if (mx >= 0 && mx < dot_cols && my >= 0 && my < dot_rows) {
                dots[static_cast<std::size_t>(my) * dot_cols + mx] = 1;
              }
            }
          }
        }

        prev_dot_x = dot_x;
        prev_dot_y = dot_y;
        first      = false;
      }
    }

    // Convert the dot grid to Braille characters.
    for (int row = 0; row < height; ++row) {
      // Go specWrap: the whole line takes the spectrum color of its
      // row-bottom tier.
      const float row_bottom =
          static_cast<float>(height - 1 - row) / static_cast<float>(height);
      const Color color = spec_color(row_bottom);

      for (int ch = 0; ch < width; ++ch) {
        std::uint32_t braille = 0x2800;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            if (dots[static_cast<std::size_t>(row * 4 + dr) * dot_cols + ch * 2 + dc]) {
              braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                    [static_cast<std::size_t>(dc)];
            }
          }
        }
        grid.set(row, ch, Cell{static_cast<char32_t>(braille), color});
      }
    }
  }

private:
  std::vector<float> samples_;  // latest mono waveform from the tap
  std::size_t        n_ = 0;    // valid sample count
};

}  // namespace

std::unique_ptr<VisDriver> make_scope_driver() {
  return std::make_unique<ScopeDriver>();
}

}  // namespace bootamp::ui::vis_drivers
