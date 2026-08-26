// ui/visualizer.cpp — the visualizer framework: analysis cadence, smoothing,
// frame accounting, driver dispatch (contract: ui/visualizer.hpp).
//
// Port of cliamp/ui/visualizer.go (Visualizer, visModes, defaultDriverTick,
// advanceSmoothing, pausedSettled, syncDriverMode, animationSteps). The Go
// original hand-rolls its FFT and per-spec caches; bootamp delegates the whole
// analyze pipeline to dsp::SpectrumAnalyzer (Hann -> FFTW3f r2c -> |X|^2 ->
// log-rebin -> attack/decay smoothing). The framework keeps everything that
// touches driver state: the per-spec analysis cadence gate (kTickAnalyze), the
// sub-tick classicPeak smoothing, the frame counter, the driver state machine
// (on_enter/on_leave on mode change), and the paused decay/settle logic.
//
// Deviations from Go (see task report):
//  * Lua visualizers are not ported here (Go RegisterLuaVisualizers /
//    luaModeDriver / luaVisNames) — cycle order is the 31 built-ins only.
//  * No elapsed-time animation catch-up (Go animationSteps/frameElapsed):
//    the C++ TickLoop wakes at the driver's own tick_interval, so every tick
//    is exactly one animation step. The contract header declares no frame-
//    timing state, so this is the architecture the loop was built for.
//  * Driver creation is lazy in tick(): the const render()/tick_interval()/
//    uses_raw_samples()/paused_decay_pending() only look up an already-created
//    driver (Go creates on demand via syncDriverMode). The tick loop always
//    ticks before it asks for cadence, so the first tick happens at kTickSlow
//    and the driver's real cadence applies from the second wake.
//  * VisMode::None uses a built-in no-op driver (Go noOpDriver) so the mode
//    table's None entry has no factory.
//  * ctx.analyze is consumed exactly as declared (spec -> band span); the app
//    wiring agent must provide it (pull samples from the audio tap, run them
//    through this visualizer's dsp::SpectrumAnalyzer).
#include "ui/visualizer.hpp"

#include "ui/tick.hpp"
#include "ui/vis_drivers/registry.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::ui {

namespace {

using Clock = std::chrono::steady_clock;

// cliamp pausedDecayEpsilon: band level below which paused spectrum content is
// treated as fully decayed to rest, letting the model drop to the idle tick.
inline constexpr float kPausedDecayEpsilon = 0.01f;

// cliamp maxSmoothDtFrames: cap on dt fed into smoothing easing — long gaps
// (sleep, paused, stalled frame) step like ~1 frame instead of integrating.
inline constexpr int kMaxSmoothDtFrames = 10;

// NormalizeAnalysisSpec (cliamp): band_count < 0 -> 0, fft_size <= 0 ->
// defaultFFTSize (2048). Distinct from dsp::normalize_analysis_spec, which
// operates on the size_t-typed dsp spec.
VisAnalysisSpec normalize_spec(VisAnalysisSpec s) {
  if (s.band_count < 0) {
    s.band_count = 0;
  }
  if (s.fft_size <= 0) {
    s.fft_size = 2048;
  }
  return s;
}

double seconds_of(std::chrono::milliseconds ms) {
  return std::chrono::duration<double>(ms).count();
}

// read_driver returns the cached driver for `mode` without creating it
// (driver creation is lazy inside tick()). Out-of-range modes read as nullptr.
VisDriver* read_driver(const std::unique_ptr<VisDriver> (&drivers)[kVisCount],
                       VisMode mode) {
  const std::size_t m = static_cast<std::size_t>(mode);
  if (m >= kVisCount) {
    return nullptr;
  }
  return drivers[m].get();
}

std::string ascii_lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// advance_smoothing eases `smoothed` toward `bands` with the classicPeak
// fast-attack / slow-decay step (cliamp Visualizer.advanceSmoothing). The
// first call after a spec change (smoothed empty or mis-sized) snaps smoothed
// to the current analysis output so levels appear immediately instead of
// fading in from zero.
void advance_smoothing(std::vector<float>& bands, std::vector<float>& smoothed,
                       Clock::time_point& last_smooth_tick, Clock::time_point now) {
  if (bands.empty()) {
    return;
  }
  const double anim_secs = seconds_of(kTickAnim);
  if (smoothed.size() != bands.size()) {
    smoothed = bands;
    last_smooth_tick = now;
    return;
  }
  double dt = anim_secs;
  if (now != Clock::time_point{} && last_smooth_tick != Clock::time_point{}) {
    dt = std::chrono::duration<double>(now - last_smooth_tick).count();
  }
  if (dt <= 0 || dt > static_cast<double>(kMaxSmoothDtFrames) * anim_secs) {
    dt = anim_secs;
  }
  last_smooth_tick = now;
  for (std::size_t i = 0; i < smoothed.size(); ++i) {
    smoothed[i] = dsp::classic_peak_step(smoothed[i], bands[i], dt);
  }
}

// default_driver_tick (cliamp defaultDriverTick): the analysis-cadence gate
// shared by every driver. Raw-sample modes (band_count == 0) re-analyze on
// every tick (no FFT work); band modes re-analyze at most every kTickAnalyze.
// Smoothing runs on every tick that has band content, even when Analyze is
// unset or skipped, so animation stays smooth across analysis gaps. Under an
// overlay both clocks reset so the first tick after dismissal analyzes
// immediately with a single-frame smoothing step.
void default_driver_tick(const VisTickContext& ctx, const VisAnalysisSpec& spec,
                         std::vector<float>& bands, std::vector<float>& smoothed,
                         Clock::time_point& last_analyze_at,
                         Clock::time_point& last_smooth_tick) {
  if (ctx.overlay_active) {
    last_analyze_at = Clock::time_point{};
    last_smooth_tick = Clock::time_point{};
    return;
  }
  if (ctx.analyze) {
    const bool due = spec.band_count == 0 || last_analyze_at == Clock::time_point{} ||
                     ctx.now == Clock::time_point{} ||
                     (ctx.now - last_analyze_at) >= kTickAnalyze;
    if (due) {
      const std::span<const float> result = ctx.analyze(spec);
      if (spec.band_count > 0) {
        bands.assign(result.begin(), result.end());
      }
      if (ctx.now != Clock::time_point{}) {
        last_analyze_at = ctx.now;
      }
    }
  }
  if (spec.band_count > 0) {
    advance_smoothing(bands, smoothed, last_smooth_tick, ctx.now);
  }
}

// paused_settled (cliamp pausedSettled): a paused visualizer is settled when
// no band content is left to ease down (raw + smoothed below the epsilon),
// the driver has no state left to decay (pause_settled()), and its tick
// interval at rest is no faster than kTickSlow. Stateful drivers signal
// animation through their tick interval; particle drivers implement
// pause_settled().
bool paused_settled(const VisDriver* driver, const VisTickContext& ctx,
                    const std::vector<float>& bands,
                    const std::vector<float>& smoothed) {
  if (!driver) {
    return true;
  }
  const VisAnalysisSpec spec = normalize_spec(driver->analysis_spec());
  if (spec.band_count == 0) {
    if (!driver->pause_settled()) {
      return false;  // raw-sample waveform still held
    }
  } else {
    for (const float b : bands) {
      if (b >= kPausedDecayEpsilon) {
        return false;
      }
    }
    for (const float b : smoothed) {
      if (b >= kPausedDecayEpsilon) {
        return false;
      }
    }
    if (!driver->pause_settled()) {
      return false;
    }
  }
  VisTickContext idle = ctx;
  idle.playing = false;
  return driver->tick_interval(idle) >= kTickSlow;
}

// cliamp noOpDriver: VisMode::None renders nothing, analyzes nothing, and
// always ticks slowly. The zero-value analysis spec {0, 0} normalizes to a
// raw-sample spec, matching Go's UsesRawSamples() == true for None.
class NoOpDriver : public VisDriver {
public:
  VisAnalysisSpec analysis_spec() const override { return {0, 0}; }
  void render(std::span<const float>, std::uint64_t, CellGrid&) override {}
  void tick(const VisTickContext&, std::uint64_t&, std::span<const float>) override {}
  std::chrono::milliseconds tick_interval(const VisTickContext&) const override {
    return kTickSlow;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Mode table
// ---------------------------------------------------------------------------

const std::vector<VisEntry>& all_vis_modes() {
  using namespace vis_drivers;
  // cliamp visModes: the single source of truth for all visualizer modes, in
  // iota order so the array index equals the VisMode value and the cycle key
  // order matches Go exactly. "None" has no factory — the framework creates a
  // built-in no-op driver for it (cliamp newNoOpDriver).
  static const std::array<VisEntry, kVisCount> kModes = {
      VisEntry{"Bars", make_bars_driver},
      VisEntry{"BarsDot", make_bars_dot_driver},
      VisEntry{"Rain", make_rain_driver},
      VisEntry{"BarsOutline", make_bars_outline_driver},
      VisEntry{"Bricks", make_bricks_driver},
      VisEntry{"Columns", make_columns_driver},
      VisEntry{"ClassicPeak", make_classic_peak_driver},
      VisEntry{"Wave", make_wave_driver},
      VisEntry{"Scatter", make_scatter_driver},
      VisEntry{"Flame", make_flame_driver},
      VisEntry{"Retro", make_retro_driver},
      VisEntry{"Pulse", make_pulse_driver},
      VisEntry{"Matrix", make_matrix_driver},
      VisEntry{"Binary", make_binary_driver},
      VisEntry{"Sakura", make_sakura_driver},
      VisEntry{"Firework", make_firework_driver},
      VisEntry{"Bubbles", make_bubbles_driver},
      VisEntry{"Logo", make_logo_driver},
      VisEntry{"Terrain", make_terrain_driver},
      VisEntry{"Scope", make_scope_driver},
      VisEntry{"Heartbeat", make_heartbeat_driver},
      VisEntry{"Butterfly", make_butterfly_driver},
      VisEntry{"Ascii", make_ascii_driver},
      VisEntry{"Firefly", make_firefly_driver},
      VisEntry{"Mosaic", make_mosaic_driver},
      VisEntry{"Sand", make_sand_driver},
      VisEntry{"Geyser", make_geyser_driver},
      VisEntry{"ClassicLED", make_classic_led_driver},
      VisEntry{"Stereo", make_stereo_driver},
      VisEntry{"Mirror", make_mirror_driver},
      VisEntry{"None", nullptr},
  };
  static const std::vector<VisEntry> kModesVec(kModes.begin(), kModes.end());
  return kModesVec;
}

std::pair<VisMode, bool> string_to_vis_mode_exact(std::string_view name) {
  // cliamp StringToVisModeExact: case-insensitive name lookup (the Go side
  // lowercases the name map; the names are ASCII here).
  const std::string lower = ascii_lower(name);
  const auto& modes = all_vis_modes();
  for (std::size_t i = 0; i < modes.size(); ++i) {
    if (ascii_lower(modes[i].name) == lower) {
      return {static_cast<VisMode>(i), true};
    }
  }
  return {VisMode::Count, false};
}

// ---------------------------------------------------------------------------
// Visualizer
// ---------------------------------------------------------------------------

Visualizer::Visualizer(double sample_rate) : analyzer_(sample_rate), sr_(sample_rate) {
  // cliamp NewVisualizer: bands start as an all-zero default-spectrum buffer.
  bands_.assign(bootamp::dsp::kDefaultSpectrumBands, 0.0f);
}

void Visualizer::cycle_mode() {
  // cliamp CycleMode: (Mode + 1) % (VisCount + luaCount); no Lua here.
  mode_ = static_cast<VisMode>((static_cast<int>(mode_) + 1) %
                               static_cast<int>(kVisCount));
}

void Visualizer::set_mode(VisMode mode) {
  // VisMode is a uint8-backed enum (no negatives); out-of-range is >= Count.
  if (static_cast<int>(mode) >= static_cast<int>(kVisCount)) {
    return;  // cliamp SetMode: out-of-range values are ignored
  }
  mode_ = mode;
  request_refresh();
}

std::string Visualizer::mode_name() const {
  const std::size_t m = static_cast<std::size_t>(mode_);
  if (m < kVisCount) {
    return all_vis_modes()[m].name;
  }
  return "Unknown";  // cliamp: out-of-range (Lua) modes
}

std::vector<std::string> Visualizer::all_mode_names() const {
  std::vector<std::string> names;
  names.reserve(kVisCount);
  for (const VisEntry& e : all_vis_modes()) {
    names.push_back(e.name);
  }
  return names;
}

void Visualizer::request_refresh() {
  refresh_pending_ = true;
}

bool Visualizer::consume_refresh() {
  if (!refresh_pending_) {
    return false;
  }
  refresh_pending_ = false;
  return true;
}

void Visualizer::tick(const VisTickContext& ctx) {
  if (rows_ <= 0 || cols_ <= 0) {
    return;  // cliamp: Rows <= 0 || columns() <= 0
  }

  // syncDriverMode (cliamp): create the driver on first use, run on_enter /
  // on_leave on mode change, and clear the cross-mode smoothing state. When
  // the analysis kind flips between raw samples and FFT bands the spectrum
  // history restarts from zero.
  const std::size_t m = static_cast<std::size_t>(mode_);
  VisDriver* driver = nullptr;
  if (m < kVisCount) {
    driver = drivers_[m].get();
    if (!driver) {
      const VisEntry& entry = all_vis_modes()[m];
      if (entry.factory) {
        drivers_[m] = entry.factory();
      } else {
        drivers_[m] = std::make_unique<NoOpDriver>();  // VisMode::None
      }
      driver = drivers_[m].get();
    }
  }
  if (!active_set_) {
    if (driver) {
      driver->on_enter();
    }
    active_mode_ = mode_;
    active_set_ = true;
  } else if (active_mode_ != mode_) {
    const std::size_t pm = static_cast<std::size_t>(active_mode_);
    VisDriver* prev = (pm < kVisCount) ? drivers_[pm].get() : nullptr;
    const VisAnalysisSpec prev_spec =
        normalize_spec(prev ? prev->analysis_spec() : VisAnalysisSpec{});
    const VisAnalysisSpec next_spec =
        normalize_spec(driver ? driver->analysis_spec() : VisAnalysisSpec{});
    if ((prev_spec.band_count == 0) != (next_spec.band_count == 0)) {
      analyzer_.reset_history();  // cliamp resetSpectrumHistory
    }
    smoothed_.clear();
    last_smooth_tick_ = Clock::time_point{};
    if (prev) {
      prev->on_leave();
    }
    if (driver) {
      driver->on_enter();
    }
    active_mode_ = mode_;
  }
  if (!driver) {
    return;
  }
  refresh_pending_ = false;

  const VisAnalysisSpec spec = normalize_spec(driver->analysis_spec());

  if (ctx.paused) {
    // Raw-sample drivers hold their waveform internally; their tick() clears
    // it when ctx.paused is set and pause_settled() reports when it is gone
    // (cliamp clears the framework-owned waveBuf right here). Keep easing the
    // visual down to rest instead of freezing mid-frame; once settled, reset
    // the analyze/smooth clocks so the first tick after resume is fresh
    // (cliamp Suspend).
    if (mode_ != VisMode::None &&
        !paused_settled(driver, ctx, bands_, smoothed_)) {
      default_driver_tick(ctx, spec, bands_, smoothed_, last_analyze_at_,
                          last_smooth_tick_);
      driver->tick(ctx, frame_, smoothed_bands());
    } else if (mode_ != VisMode::None) {
      // Suspend: clocks reset so resuming advances by one frame, not the gap.
      last_analyze_at_ = Clock::time_point{};
      last_smooth_tick_ = Clock::time_point{};
    }
    return;
  }

  if (ctx.overlay_active) {
    // cliamp resetFrameTiming: hidden time never advances the animation
    // frame. The C++ contract has no frame-timing members to reset.
  } else if (mode_ != VisMode::None) {
    // One animation step per tick: the TickLoop wakes at the driver's own
    // tick_interval, so elapsed-time catch-up (Go animationSteps) is not
    // needed — each wake is exactly one frame.
    frame_ += 1;
  }

  if (mode_ != VisMode::None) {
    default_driver_tick(ctx, spec, bands_, smoothed_, last_analyze_at_,
                        last_smooth_tick_);
    driver->tick(ctx, frame_, smoothed_bands());
  }
}

bool Visualizer::render(CellGrid& grid) const {
  if (mode_ == VisMode::None || rows_ <= 0 || cols_ <= 0) {
    return false;
  }
  VisDriver* driver = read_driver(drivers_, mode_);
  if (!driver) {
    return false;  // not ticked yet — creation is lazy inside tick()
  }
  // cliamp Render + fitVisualizerFrame: the grid is cleared (spaces, default
  // color) so drivers that draw sparsely leave blank cells, then filled to
  // the current terminal size.
  grid.resize(rows_, cols_);
  grid.fill(Cell{});
  driver->render(smoothed_bands(), frame_, grid);
  return true;
}

std::chrono::milliseconds Visualizer::tick_interval(const VisTickContext& ctx) const {
  // cliamp TickInterval: no driver -> slow; paused -> slow; otherwise the
  // driver's own cadence.
  const VisDriver* driver = read_driver(drivers_, mode_);
  if (!driver) {
    return kTickSlow;
  }
  if (ctx.paused) {
    // cliamp model.tickInterval: once the paused visualizer's content has
    // fully decayed to rest (no band content left to ease down), drop to the
    // fully-idle cadence (TickIdle); while decay is still pending keep the
    // slow cadence so the bars fall smoothly.
    if (!paused_decay_pending(ctx)) {
      return kTickIdle;
    }
    return kTickSlow;
  }
  return driver->tick_interval(ctx);
}

bool Visualizer::uses_raw_samples() const {
  // cliamp UsesRawSamples: the active driver draws directly from audio
  // samples when its analysis spec has zero bands.
  const VisDriver* driver = read_driver(drivers_, mode_);
  return driver != nullptr && normalize_spec(driver->analysis_spec()).band_count == 0;
}

bool Visualizer::paused_decay_pending(const VisTickContext& ctx) const {
  // cliamp PausedDecayPending: a paused visualizer still needs ticks while
  // its content eases down to rest.
  const VisDriver* driver = read_driver(drivers_, mode_);
  if (!driver) {
    return false;
  }
  return !paused_settled(driver, ctx, bands_, smoothed_);
}

}  // namespace bootamp::ui
