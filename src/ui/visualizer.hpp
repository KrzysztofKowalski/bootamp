// ui/visualizer.hpp — the visualizer framework: analyze + render dispatch.
//
// Port of cliamp/ui/visualizer.go. Per VisAnalysisSpec: Hann → FFTW3f r2c →
// |X|² → log-rebin → attack/decay smoothing (the dsp::SpectrumAnalyzer). render()
// dispatches to the active driver, which fills a CellGrid. The framework owns
// the per-spec cache, the smoothed bands, the frame counter, and the driver
// state machine (on_enter/on_leave on mode change). Drivers live in
// ui/vis_drivers/. The CellGrid is the golden-test surface.
#pragma once

#include "dsp/spectrum.hpp"
#include "ui/cell.hpp"
#include "ui/vis_driver.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bootamp::ui {

inline constexpr int kDefaultVisRows = 5;

// Visualizer owns the analyzer, smoothed bands, frame counter, and active
// driver. One instance per app. The UI tick loop calls tick() (drives analysis
// cadence + smoothing) and render() (fills the CellGrid for blit).
class Visualizer {
public:
  explicit Visualizer(double sample_rate);

  // cycle_mode advances to the next visualizer mode (cliamp CycleMode).
  void cycle_mode();
  // set_mode switches to `mode` if in range (cliamp SetMode).
  void set_mode(VisMode mode);
  // mode_name returns the active mode's display name.
  std::string mode_name() const;
  // all_mode_names returns cycle-order names (built-ins).
  std::vector<std::string> all_mode_names() const;

  // tick drives analysis + smoothing + frame accounting. ctx supplies the
  // analyze callback + playback state. Port of cliamp Visualizer.Tick.
  void tick(const VisTickContext& ctx);

  // render fills `grid` with the active driver's output for the current frame.
  // Returns false if the mode is None or the grid is empty.
  bool render(CellGrid& grid) const;

  // bands / smoothed_bands access the latest analysis output.
  std::span<const float> bands() const          { return bands_; }
  std::span<const float> smoothed_bands() const { return smoothed_; }

  // frame counter + sample rate.
  std::uint64_t frame() const { return frame_; }
  double        sample_rate() const { return sr_; }

  // columns()/rows() are the render target size, set by the UI from the
  // terminal dimensions before render().
  void set_size(int cols, int rows) { cols_ = cols; rows_ = rows; }
  int  cols() const { return cols_; }
  int  rows() const { return rows_; }

  // request_refresh / consume_refresh flag a redraw (cliamp RequestRefresh).
  void request_refresh();
  bool consume_refresh();

  // uses_raw_samples reports whether the active driver draws from raw samples.
  bool uses_raw_samples() const;

  // tick_interval asks the active driver for the current animation cadence.
  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const;

  // paused_decay_pending reports whether a paused visualizer still needs ticks.
  bool paused_decay_pending(const VisTickContext& ctx) const;

private:
  dsp::SpectrumAnalyzer          analyzer_;
  double                          sr_;
  VisMode                         mode_           = VisMode::Bars;
  VisMode                         active_mode_    = VisMode::Bars;
  bool                            active_set_     = false;
  bool                            refresh_pending_ = true;
  int                             cols_           = 0;
  int                             rows_           = kDefaultVisRows;
  std::uint64_t                   frame_          = 0;
  std::vector<float>              bands_;
  std::vector<float>              smoothed_;
  std::chrono::steady_clock::time_point last_smooth_tick_;
  std::chrono::steady_clock::time_point last_analyze_at_;
  std::unique_ptr<VisDriver>       drivers_[kVisCount];
};

}  // namespace bootamp::ui