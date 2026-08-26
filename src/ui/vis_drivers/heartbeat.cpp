// ui/vis_drivers/heartbeat.cpp — "Heartbeat" ECG pulse-monitor trace
// (port of cliamp/ui/vis_heartbeat.go).
//
// A scrolling ECG/pulse-monitor trace drawn in a 4×2-dot braille grid. Raw
// audio is shaped like an ECG trace (sample*|sample| sharpens peaks, flattens
// noise); the trace scrolls left each frame for the classic hospital-monitor
// look. A faint dashed baseline runs at center (on for 6 dots, off for 4);
// trace dots are red, baseline dots are green. Raw-sample mode (0 bands): the
// driver pulls mono samples in tick() via ctx.waveform_samples_into and holds
// them internally (Go's framework-owned waveBuf). On pause the buffer is
// cleared inside tick() and pause_settled() reports when the waveform is gone,
// so the framework keeps ticking until the trace is cleared. Cadence: kTickWave
// while playing (cliamp newFastRenderOnlyDriver(..., TickWave, ...)), the
// default cadence otherwise.
#include "ui/vis_drivers/registry.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bootamp::ui::vis_drivers {

namespace {

// cliamp defaultFFTSize: the Go side stores the analyzed sample block in a
// 2048-entry waveBuf; mirror that capacity here.
inline constexpr std::size_t kHeartbeatBufSize = 2048;

// brailleBit maps (row, col) in the 4×2 Braille dot grid to its bit value
// (Go brailleBit). Or-ed into U+2800 to form the final codepoint.
constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

class HeartbeatDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override {
    // cliamp spectrumAnalysisSpec(0): raw-sample mode, 2048 FFT size.
    return {0, 2048};
  }

  void render(std::span<const float> /*bands*/, std::uint64_t /*frame*/,
              CellGrid& grid) override {
    const int height = grid.rows();
    const int width  = grid.cols();
    if (height <= 0 || width <= 0) {
      return;
    }
    const int dot_rows = height * 4;
    const int dot_cols = width * 2;
    const std::size_t n = wave_.size();

    // Build a y-position for each dot column from the raw audio (Go
    // renderHeartbeat: y = int(centerY - shaped*amplitude) with the sample
    // magnitude-squared to shape the waveform like an ECG trace).
    std::vector<int> ypos(static_cast<std::size_t>(dot_cols));
    const double center_y  = static_cast<double>(dot_rows) / 2.0;
    const double amplitude = static_cast<double>(dot_rows) * 0.45;
    for (int x = 0; x < dot_cols; ++x) {
      double sample = 0.0;
      if (n > 0) {
        std::size_t idx =
            static_cast<std::size_t>(x) * n / static_cast<std::size_t>(dot_cols);
        if (idx >= n) {
          idx = n - 1;
        }
        sample = static_cast<double>(wave_[idx]);
      }
      const double shaped = sample * std::abs(sample);  // keep sign, square magnitude
      const double yf     = center_y - shaped * amplitude;
      ypos[static_cast<std::size_t>(x)] = static_cast<int>(
          std::clamp(yf, 0.0, static_cast<double>(dot_rows - 1)));
    }

    std::vector<bool> grid_dots(static_cast<std::size_t>(dot_rows) * dot_cols,
                                false);

    // Draw the ECG trace with continuous line connections (Go: the vertical
    // run between consecutive y positions is filled at the newer column).
    for (int x = 0; x < dot_cols; ++x) {
      const int y = ypos[static_cast<std::size_t>(x)];
      grid_dots[static_cast<std::size_t>(y) * dot_cols + x] = true;
      if (x > 0) {
        const int lo = std::min(y, ypos[static_cast<std::size_t>(x - 1)]);
        const int hi = std::max(y, ypos[static_cast<std::size_t>(x - 1)]);
        for (int fy = lo; fy <= hi; ++fy) {
          grid_dots[static_cast<std::size_t>(fy) * dot_cols + x] = true;
        }
      }
    }

    // Draw a faint baseline at center (Go: dashed, on for 6 dots, off for 4).
    const int base_y = dot_rows / 2;
    for (int x = 0; x < dot_cols; ++x) {
      if (!grid_dots[static_cast<std::size_t>(base_y) * dot_cols + x] &&
          (x / 6) % 2 == 0) {
        grid_dots[static_cast<std::size_t>(base_y) * dot_cols + x] = true;
      }
    }

    // Render braille characters (Go: tag 0 = green baseline, tag 2 = red trace).
    for (int row = 0; row < height; ++row) {
      for (int ch = 0; ch < width; ++ch) {
        std::uint32_t braille  = 0x2800;
        bool          has_trace = false;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const int dy = row * 4 + dr;
            const int dx = ch * 2 + dc;
            if (grid_dots[static_cast<std::size_t>(dy) * dot_cols + dx]) {
              braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                     [static_cast<std::size_t>(dc)];
              // Part of the trace (not the baseline) if off-center.
              if (dy != base_y) {
                has_trace = true;
              }
            }
          }
        }
        const Color color = has_trace ? kColorSpecHigh : kColorSpecLow;
        grid.set(row, ch, Cell{static_cast<char32_t>(braille), color});
      }
    }
  }

  void tick(const VisTickContext& ctx, std::uint64_t&, std::span<const float>) override {
    if (ctx.paused) {
      // cliamp Visualizer.Tick clears the framework-owned waveBuf on the first
      // paused tick; here the driver owns the waveform and clears it itself.
      wave_.clear();
      return;
    }
    if (ctx.waveform_samples_into) {
      // Raw-sample modes have no FFT work — refresh the waveform every tick.
      wave_.resize(kHeartbeatBufSize);
      const std::size_t n = ctx.waveform_samples_into(std::span<float>(wave_));
      wave_.resize(n);
    }
  }

  bool pause_settled() const override {
    // Raw-sample driver: settled once the held waveform is gone.
    return wave_.empty();
  }

  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override {
    // cliamp renderOnlyDriver.TickInterval with tickDuration = TickWave:
    // TickWave while playing and no overlay; the default cadence otherwise.
    if (ctx.playing && !ctx.overlay_active) {
      return kTickWave;
    }
    if (ctx.overlay_active || !ctx.playing) {
      return kTickSlow;
    }
    return kTickFast;
  }

private:
  std::vector<float> wave_;
};

}  // namespace

std::unique_ptr<VisDriver> make_heartbeat_driver() {
  return std::make_unique<HeartbeatDriver>();
}

}  // namespace bootamp::ui::vis_drivers
