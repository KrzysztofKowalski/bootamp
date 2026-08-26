// ui/vis_driver.hpp — visualizer driver interface.
//
// Port of cliamp's visModeDriver interface. Each of the 33 modes is a driver
// that takes the smoothed bands (or raw samples) + frame counter + RNG and
// writes runes/colors into a CellGrid. analysis_spec() reports whether the
// driver wants FFT bands (band_count>0) or raw samples (0). tick_interval()
// selects the animation cadence; on_enter/on_leave manage driver state.
#pragma once

#include "ui/cell.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace bootamp::ui {

// VisTickContext mirrors cliamp VisTickContext. The driver uses `now` for
// timing, `playing`/`paused`/`overlay_active` for cadence, and `analyze` /
// `stereo_samples_into` callbacks to pull data from the Tap ring.
struct VisTickContext {
  std::chrono::steady_clock::time_point now;
  bool                                  playing       = false;
  bool                                  paused        = false;
  bool                                  overlay_active = false;
  // focused mirrors the app's DECSET 1004 focus state (default true: players
  // and headless contexts don't require UI state); the tick loop idles while
  // false so an invisible window stops re-rendering every frame.
  bool                                  focused        = true;
  std::function<std::span<const float>(const struct VisAnalysisSpec&)> analyze;
  std::function<std::size_t(std::span<float>)>  waveform_samples_into;  // mono
  std::function<std::size_t(std::span<std::array<float,2>>)> stereo_samples_into;
};

// VisMode enumerates the 33 visualizer modes (cliamp VisMode). The integer
// values match cliamp's iota order so cycle order is identical.
enum class VisMode : std::uint8_t {
  Bars, BarsDot, Rain, BarsOutline, Bricks, Columns, ClassicPeak, Wave,
  Scatter, Flame, Retro, Pulse, Matrix, Binary, Sakura, Firework, Bubbles,
  Logo, Terrain, Scope, Heartbeat, Butterfly, Ascii, Firefly, Mosaic, Sand,
  Geyser, ClassicLED, Stereo, Mirror, None,
  Count  // sentinel
};
inline constexpr std::size_t kVisCount = static_cast<std::size_t>(VisMode::Count);

// VisAnalysisSpec (cliamp VisAnalysisSpec): band_count==0 ⇒ raw-sample mode.
struct VisAnalysisSpec {
  int band_count = 10;
  int fft_size   = 2048;
  bool operator==(const VisAnalysisSpec&) const = default;
};

// VisEntry pairs a display name with a driver factory (cliamp visEntry).
struct VisEntry {
  std::string                                name;
  std::function<std::unique_ptr<class VisDriver>()> factory;
};

// VisDriver is the abstract visualizer mode driver (cliamp visModeDriver).
class VisDriver {
public:
  virtual ~VisDriver() = default;
  virtual VisAnalysisSpec analysis_spec() const = 0;
  // render fills `grid` for the current frame. bands is the smoothed spectrum
  // (empty for raw-sample modes); frame is the animation counter.
  virtual void render(std::span<const float> bands, std::uint64_t frame,
                      CellGrid& grid) = 0;
  virtual void tick(const VisTickContext& ctx, std::uint64_t& frame,
                     std::span<const float> bands) = 0;
  virtual std::chrono::milliseconds
  tick_interval(const VisTickContext& ctx) const = 0;
  virtual void on_enter() {}
  virtual void on_leave() {}

  // pause_settled reports whether the driver holds no content left to ease
  // down while paused (cliamp visPauseSettler, default = settled). Band-driven
  // drivers keep the default — the framework decides their settling from the
  // band levels + tick interval. Raw-sample drivers hold their waveform
  // internally: they must clear it inside tick() when ctx.paused is set and
  // override pause_settled() to report whether waveform content remains, so
  // the framework keeps ticking until the waveform is gone (cliamp clears the
  // framework-owned waveBuf on the first paused tick).
  virtual bool pause_settled() const { return true; }
};

// all_vis_modes returns the 33-entry name→mode table in cycle order (cliamp
// visModes). Used by the visualizer framework and the `v` cycle key.
const std::vector<VisEntry>& all_vis_modes();

// string_to_vis_mode_exact converts a name to VisMode (cliamp StringToVisModeExact).
std::pair<VisMode, bool> string_to_vis_mode_exact(std::string_view name);

}  // namespace bootamp::ui