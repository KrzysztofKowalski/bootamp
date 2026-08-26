// ui/vis_drivers/wave.cpp — Braille-character oscilloscope waveform
// (port of cliamp/ui/vis_wave.go).
//
// Each Braille character covers a 2×4 dot grid, giving smooth sub-cell
// resolution: the mono waveform is downsampled to one y-position per horizontal
// dot column, consecutive points are connected vertically, and the resulting
// dot runs are packed into Braille glyphs. Row-bottom tiers pick the spectrum
// color per line, like Go's specWrap.
//
// Raw-sample driver: the Go original is a renderOnly driver whose
// defaultDriverTick calls ctx.Analyze(spec) every tick (band_count == 0 has no
// FFT gate), which fills the framework-owned waveBuf. The C++ framework keeps
// analysis for band drivers and hands raw-sample drivers the
// VisTickContext::waveform_samples_into callback, so this driver pulls its own
// sample window in tick(). Per the vis_driver.hpp contract it clears the
// window when ctx.paused is set and reports pause_settled() from it.
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

// Max waveform window offered to the tap on each tick (Go sampleBuf is sized
// to defaultFFTSize = 2048 via EnsureSampleBuf).
inline constexpr std::size_t kWaveBufMax = 2048;

// WaveDriver (cliamp renderWave, registered with a raw-sample spec and TickWave).
class WaveDriver : public VisDriver {
public:
  WaveDriver() : samples_(kWaveBufMax) {}

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

  void render(std::span<const float>, std::uint64_t, CellGrid& grid) override {
    const int height     = grid.rows();
    const int char_cols  = grid.cols();
    if (height <= 0 || char_cols <= 0) {
      return;
    }
    const int dot_rows = height * 4;
    const int dot_cols = char_cols * 2;

    // Downsample audio to one y-position per horizontal dot column.
    std::vector<int> ypos(static_cast<std::size_t>(dot_cols));
    for (int x = 0; x < dot_cols; ++x) {
      double sample = 0.0;
      if (n_ > 0) {
        std::size_t idx =
            static_cast<std::size_t>(x) * n_ / static_cast<std::size_t>(dot_cols);
        if (idx >= n_) {
          idx = n_ - 1;
        }
        sample = static_cast<double>(samples_[idx]);
      }
      // Map sample [-1, 1] to dot row [0, dotRows-1]; center is dotRows/2.
      const int y = static_cast<int>((1.0 - sample) *
                                     static_cast<double>(dot_rows - 1) / 2.0);
      ypos[static_cast<std::size_t>(x)] =
          std::max(0, std::min(dot_rows - 1, y));
    }

    for (int row = 0; row < height; ++row) {
      // Go specWrap: the whole line takes the spectrum color of its
      // row-bottom tier.
      const float row_bottom =
          static_cast<float>(height - 1 - row) / static_cast<float>(height);
      const Color color = spec_color(row_bottom);
      const int   dot_row_start = row * 4;

      for (int ch = 0; ch < char_cols; ++ch) {
        std::uint32_t braille = 0x2800;
        const int     dot_col_start = ch * 2;
        for (int dc = 0; dc < 2; ++dc) {
          const int x = dot_col_start + dc;
          const int y = ypos[static_cast<std::size_t>(x)];

          // Connect to the previous point so the waveform is continuous.
          int prev_y = y;
          if (x > 0) {
            prev_y = ypos[static_cast<std::size_t>(x - 1)];
          }
          const int y_min = std::min(y, prev_y);
          const int y_max = std::max(y, prev_y);

          for (int dr = 0; dr < 4; ++dr) {
            const int dot_y = dot_row_start + dr;
            if (dot_y >= y_min && dot_y <= y_max) {
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

std::unique_ptr<VisDriver> make_wave_driver() {
  return std::make_unique<WaveDriver>();
}

}  // namespace bootamp::ui::vis_drivers
