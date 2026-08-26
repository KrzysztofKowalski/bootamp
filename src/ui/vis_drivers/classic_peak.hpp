// ui/vis_drivers/classic_peak.hpp — ClassicPeak driver class (port of
// cliamp/ui/vis_classic_peak.go classicPeakDriver).
//
// Winamp-style peak meters: bar bodies ease toward the resampled band levels
// (fast rise / slow fall), and detached peak caps launch upward on rises, hang
// briefly at the apex, then fall under gravity until they land on the bars.
//
// Unlike the other drivers this class lives in a header (not file-local in
// the .cpp) because cliamp's vis_classic_peak_test.go tests the physics state
// machine from the same package: barPos/peakPos/peakVel/peakHold and the step
// helpers are the test surface. Members are public, mirroring Go's
// same-package field access. classic_peak.cpp implements the methods and the
// factory declared in registry.hpp.
#pragma once

#include "ui/styles.hpp"
#include "ui/vis_driver.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace bootamp::ui::vis_drivers {

// cliamp vis_classic_peak.go constants.
inline constexpr int kClassicPeakSpectrumBands = 64;
inline constexpr int kClassicPeakFFTSize       = 4096;
// Default animation timestep when elapsed time is missing or non-positive
// (Go tickClassicPeak = time.Second/60).
inline constexpr double kTickClassicPeakSeconds = 1.0 / 60.0;
// Minimum frame rate used when deriving redraw interval from terminal size.
inline constexpr double kClassicPeakMinFPS = 24.0;
// Maximum frame rate used when deriving redraw interval from terminal size.
inline constexpr double kClassicPeakMaxFPS = 60.0;
// Divides the FFT window duration to set spectrum analysis hop size (Go
// classicPeakFFTOverlap; the analysis cadence itself is framework-owned in
// bootamp, so this constant is inert — kept for parity).
inline constexpr double kClassicPeakFFTOverlap = 2.0;
// Minimum spacing between spectrum analyses, regardless of sample rate (Go
// classicPeakSampleFloor; inert, see kClassicPeakFFTOverlap).
inline constexpr double kClassicPeakSampleFloorSeconds = 0.020;
// Minimum upward launch velocity for a newly detached peak cap.
inline constexpr double kClassicPeakLaunchBase = 0.8;
// Extra launch velocity added in proportion to the bar's rise amount.
inline constexpr double kClassicPeakLaunchGain = 1.4;
// Maximum upward launch velocity for the peak cap.
inline constexpr double kClassicPeakLaunchMax = 1.7;
// Downward acceleration applied to the peak cap after launch.
inline constexpr double kClassicPeakGravity = 9.5;
// Time the peak cap pauses at the apex before falling.
inline constexpr double kClassicPeakApexHold = 0.08;
// Rendered width of each spectrum bar in terminal cells.
inline constexpr int kClassicPeakBarWidth = 1;
// Number of spaces inserted between adjacent bars.
inline constexpr int kClassicPeakBarGap = 1;
// Smoothing rate used when bar bodies move upward.
inline constexpr double kClassicPeakBarRiseRate = 34.0;
// Smoothing rate used when bar bodies move downward.
inline constexpr double kClassicPeakBarFallRate = 10.0;
// Highest normalized height a peak cap may reach.
inline constexpr double kClassicPeakMaxHeight = 1.0;
// Small tolerance for treating peak and bar positions as visually equal.
inline constexpr double kClassicPeakVisibleEpsilon = 0.01;

// classicPeakGlyphs (Go): horizontal-scan-line runes, top to bottom.
inline constexpr char32_t kClassicPeakGlyphs[4] = {U'⎺', U'⎻', U'⎼', U'⎽'};

class ClassicPeakDriver : public VisDriver {
public:
  // analysis_spec: 64 bands / 4096 FFT (Go classicPeakSpectrumBands /
  // classicPeakFFTSize).
  VisAnalysisSpec analysis_spec() const override;

  // render draws bars + caps into the grid, centered with rowPad like Go
  // (classicPeakRenderWidth). Records rows_/cols_ from the grid.
  void render(std::span<const float> bands, std::uint64_t frame,
              CellGrid& grid) override;

  // tick mirrors Go classicPeakDriver.Tick minus the framework-owned analysis
  // gate: overlay resets both clocks and freezes; not-playing resets the
  // analysis clock; then sync + (if animating) advance with the bands the
  // framework supplied (smoothed). Paused decay is the framework's job — it
  // keeps passing silent bands and this driver eases barPos/peakPos to them.
  void tick(const VisTickContext& ctx, std::uint64_t& frame,
            std::span<const float> bands) override;

  // tick_interval (Go classicPeakDriver.TickInterval): slow under overlay or
  // at rest; the size-derived frame interval while playing or animating.
  std::chrono::milliseconds tick_interval(const VisTickContext& ctx) const override;

  // on_enter resets the whole driver (Go `*d = classicPeakDriver{}`).
  void on_enter() override;

  // --- state (public: cliamp's vis_classic_peak_test.go pokes these from the
  // same package — the C++ tests do the same) ---
  std::vector<double> bar_pos_;   // eased bar body heights
  std::vector<double> peak_pos_;  // peak cap heights
  std::vector<double> peak_vel_;  // peak cap velocities
  std::vector<double> peak_hold_; // remaining apex hold time (seconds)
  std::chrono::steady_clock::time_point last_tick_;  // clock of last advance
  std::chrono::steady_clock::time_point bands_at_;   // Go bandsAt (analysis
                                                     // clock; inert in bootamp
                                                     // — analysis is framework
                                                     // -owned, kept for the
                                                     // overlay clock reset)
  int rows_ = 0;  // last render height (Go v.Rows)
  int cols_ = 0;  // last render width (Go PanelWidth; levels() falls back to
                  // styles.hpp panel_width() before the first render)
  bool animating_ = false;  // animation state after the last tick() — the
                            // const tick_interval() has no bands access, so it
                            // reads this instead of Go's live animating(v)

  // --- physics helpers (Go methods on classicPeakDriver) ---
  // sync launches landed caps whose target level exceeds the cap position
  // (Go classicPeakDriver.sync); resets on shape change.
  void sync(std::span<const float> bands);
  // advance integrates bars (exponential easing) and caps (gravity) by the
  // clamped dt (Go classicPeakDriver.advance).
  void advance(std::chrono::steady_clock::time_point now,
               std::span<const float> bands);
  // animating reports whether bars are easing, caps are airborne, or a cap
  // still holds a launch (Go classicPeakDriver.animating).
  bool animating(std::span<const float> bands) const;
  // landed reports whether cap i is at rest on its bar (Go landed).
  bool landed(std::size_t i) const;
  // reset snaps all state to `levels` (Go reset).
  void reset(const std::vector<double>& levels,
             std::chrono::steady_clock::time_point now);
  // levels resamples the bands to the active column count for the current
  // width (Go classicPeakDriver.levels).
  std::vector<double> levels(std::span<const float> bands) const;
  // render_state returns (cols, peaks) for rendering, falling back to the
  // resampled levels while the driver is unseeded (Go renderState).
  std::pair<std::vector<double>, std::vector<double>>
  render_state(std::span<const float> bands) const;
};

}  // namespace bootamp::ui::vis_drivers
