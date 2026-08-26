// tests/ui/test_visualizer.cpp — visualizer framework tests.
//
// Port of cliamp/ui/visualizer_driver_test.go cases that exercise the
// framework (mode cycling, the visModes table, refresh flag, tick cadence,
// paused decay/suspend, mode-switch smoothing, tick_interval, raw-sample
// reporting) plus driver-level tests for the Logo driver (vis_logo.go). The
// drivers are created through the real factories (all_vis_modes); Logo is the
// deterministic band driver used for the framework flow tests. The analyzer
// itself (Hann/FFT) is dsp-side and covered by the dsp tests; here ctx.analyze
// is a scripted band source.
#include "ui/visualizer.hpp"

#include "ui/styles.hpp"
#include "ui/tick.hpp"
#include "ui/vis_drivers/registry.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace bootamp::ui;
using bootamp::dsp::kDefaultSpectrumBands;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::string_view, 31> kExpectedModeNames = {
    "Bars",      "BarsDot",   "Rain",       "BarsOutline", "Bricks",    "Columns",
    "ClassicPeak", "Wave",    "Scatter",    "Flame",       "Retro",     "Pulse",
    "Matrix",    "Binary",    "Sakura",     "Firework",    "Bubbles",   "Logo",
    "Terrain",   "Scope",     "Heartbeat",  "Butterfly",   "Ascii",     "Firefly",
    "Mosaic",    "Sand",      "Geyser",     "ClassicLED",  "Stereo",    "Mirror",
    "None",
};

// A scripted ctx.analyze source: returns `bands` (optionally resized to the
// requested band count), decaying every value by 0.8 per call once `decay` is
// set — the Go Analyze(nil) silence gate behavior.
struct BandSource {
  std::vector<float> bands;
  bool               decay = false;
  int                calls = 0;

  std::span<const float> operator()(const VisAnalysisSpec& spec) {
    ++calls;
    if (spec.band_count > 0 && static_cast<int>(bands.size()) != spec.band_count) {
      bands.assign(static_cast<std::size_t>(spec.band_count), bands.empty() ? 0.0f : bands[0]);
    }
    if (decay) {
      for (float& b : bands) {
        b *= 0.8f;
      }
    }
    return bands;
  }
};

VisTickContext tick_ctx(Clock::time_point now, BandSource& source, bool paused = false,
                        bool playing = true, bool overlay = false) {
  VisTickContext ctx;
  ctx.now            = now;
  ctx.playing        = playing;
  ctx.paused         = paused;
  ctx.overlay_active = overlay;
  ctx.analyze        = [&source](const VisAnalysisSpec& spec) { return source(spec); };
  return ctx;
}

std::size_t dot_cell_count(const CellGrid& grid) {
  std::size_t n = 0;
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      if (grid.at(r, c).rune != U'⠀') {  // any braille dot set
        ++n;
      }
    }
  }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mode table + cycling
// ---------------------------------------------------------------------------

TEST_CASE("vis mode table follows cliamp visModes cycle order") {
  static_assert(kVisCount == 31, "Go VisCount: 31 built-in modes (None included)");

  const auto& modes = all_vis_modes();
  REQUIRE(modes.size() == kExpectedModeNames.size());
  for (std::size_t i = 0; i < modes.size(); ++i) {
    INFO("mode[" << i << "]");
    REQUIRE(modes[i].name == kExpectedModeNames[i]);
    // Go has a factory for every mode except None (newNoOpDriver).
    REQUIRE(static_cast<bool>(modes[i].factory) == (i != kExpectedModeNames.size() - 1));
  }
}

TEST_CASE("all_mode_names returns the built-ins in cycle order") {
  Visualizer v(44100);
  const auto names = v.all_mode_names();
  REQUIRE(names.size() == kExpectedModeNames.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    REQUIRE(names[i] == kExpectedModeNames[i]);
  }
}

TEST_CASE("cycle_mode walks every mode and wraps") {
  Visualizer v(44100);
  REQUIRE(v.mode_name() == "Bars");  // cliamp default
  for (std::size_t i = 1; i < kExpectedModeNames.size(); ++i) {
    v.cycle_mode();
    REQUIRE(v.mode_name() == kExpectedModeNames[i]);
  }
  v.cycle_mode();  // None -> wraps to Bars
  REQUIRE(v.mode_name() == "Bars");
}

TEST_CASE("set_mode switches modes and ignores out-of-range values") {
  Visualizer v(44100);
  v.set_mode(VisMode::Logo);
  REQUIRE(v.mode_name() == "Logo");
  REQUIRE(v.consume_refresh());  // SetMode requests a refresh (cliamp)

  v.set_mode(VisMode::Count);  // out of range — ignored
  REQUIRE(v.mode_name() == "Logo");
  v.set_mode(static_cast<VisMode>(999));  // wildly out of range — ignored
  REQUIRE(v.mode_name() == "Logo");
}

TEST_CASE("string_to_vis_mode_exact is case-insensitive") {
  // Every built-in name round-trips (cliamp StringToVisModeExact).
  for (std::size_t i = 0; i < kExpectedModeNames.size(); ++i) {
    const auto [mode, ok] = string_to_vis_mode_exact(kExpectedModeNames[i]);
    REQUIRE(ok);
    REQUIRE(static_cast<std::size_t>(mode) == i);
  }
  const auto [lower, ok_lower] = string_to_vis_mode_exact("bars");
  REQUIRE(ok_lower);
  REQUIRE(lower == VisMode::Bars);
  const auto [upper, ok_upper] = string_to_vis_mode_exact("CLASSICPEAK");
  REQUIRE(ok_upper);
  REQUIRE(upper == VisMode::ClassicPeak);
  const auto [none, ok_none] = string_to_vis_mode_exact("none");
  REQUIRE(ok_none);
  REQUIRE(none == VisMode::None);
  const auto [bad, ok_bad] = string_to_vis_mode_exact("no-such-mode");
  REQUIRE_FALSE(ok_bad);
  REQUIRE(bad == VisMode::Count);
}

TEST_CASE("refresh flag is consumed and re-requested") {
  Visualizer v(44100);
  REQUIRE(v.consume_refresh());  // starts pending (cliamp NewVisualizer)
  REQUIRE_FALSE(v.consume_refresh());
  v.request_refresh();
  REQUIRE(v.consume_refresh());
  REQUIRE_FALSE(v.consume_refresh());
}

// ---------------------------------------------------------------------------
// tick: analysis cadence, smoothing, frame accounting
// ---------------------------------------------------------------------------

TEST_CASE("tick without a render size is a no-op") {
  Visualizer v(44100);  // cols_ == 0 — never sized
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  v.tick(tick_ctx(Clock::time_point(std::chrono::milliseconds(1000)), src));
  REQUIRE(src.calls == 0);
  REQUIRE(v.frame() == 0);
  REQUIRE(v.tick_interval(tick_ctx(Clock::now(), src)) == kTickSlow);
  CellGrid grid;
  REQUIRE_FALSE(v.render(grid));
}

TEST_CASE("tick analyzes at the kTickAnalyze cadence and stores bands") {
  Visualizer v(44100);
  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  const Clock::time_point t0(std::chrono::milliseconds(1000));

  v.tick(tick_ctx(t0, src));  // first tick: no prior analyze timestamp -> due
  REQUIRE(src.calls == 1);
  REQUIRE(v.frame() == 1);
  REQUIRE(v.bands().size() == kDefaultSpectrumBands);
  for (float b : v.bands()) {
    REQUIRE(b == Catch::Approx(0.4f));
  }

  v.tick(tick_ctx(t0 + std::chrono::milliseconds(16), src));  // within window
  REQUIRE(src.calls == 1);  // cadence gate holds
  REQUIRE(v.frame() == 2);

  v.tick(tick_ctx(t0 + kTickAnalyze + std::chrono::milliseconds(1), src));  // past window
  REQUIRE(src.calls == 2);
  REQUIRE(v.frame() == 3);
}

TEST_CASE("smoothed bands snap on first tick then ease toward bands") {
  Visualizer v(44100);
  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  const Clock::time_point t0(std::chrono::milliseconds(1000));

  v.tick(tick_ctx(t0, src));
  REQUIRE(v.smoothed_bands().size() == kDefaultSpectrumBands);
  for (float s : v.smoothed_bands()) {
    REQUIRE(s == Catch::Approx(0.4f));  // snap, not eased
  }

  // Raise the target past the analyze window: smoothing must ease upward.
  src.bands.assign(kDefaultSpectrumBands, 1.0f);
  v.tick(tick_ctx(t0 + kTickAnalyze + std::chrono::milliseconds(1), src));
  REQUIRE(src.calls == 2);
  REQUIRE(v.bands()[0] == Catch::Approx(1.0f));
  const float eased = v.smoothed_bands()[0];
  REQUIRE(eased > 0.4f);
  REQUIRE(eased < 1.0f);
  REQUIRE(eased != Catch::Approx(1.0f));  // eased, not snapped
}

TEST_CASE("tick without an analyze callback still smooths and advances") {
  Visualizer v(44100);
  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  VisTickContext ctx;
  ctx.now     = Clock::time_point(std::chrono::milliseconds(1000));
  ctx.playing = true;  // no analyze callback (cliamp defaultDriverTick)
  v.tick(ctx);
  REQUIRE(v.frame() == 1);
  REQUIRE(v.smoothed_bands().size() == kDefaultSpectrumBands);
}

TEST_CASE("frame does not advance while paused or under an overlay") {
  Visualizer v(44100);
  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  const Clock::time_point t0(std::chrono::milliseconds(1000));

  v.tick(tick_ctx(t0, src));
  REQUIRE(v.frame() == 1);
  const int calls_after_playing = src.calls;

  v.tick(tick_ctx(t0 + kTickSlow, src, /*paused=*/true));
  REQUIRE(v.frame() == 1);
  REQUIRE(src.calls > calls_after_playing);  // paused decay still analyzes

  v.tick(tick_ctx(t0 + 2 * kTickSlow, src, /*paused=*/false, /*playing=*/true,
                  /*overlay=*/true));
  REQUIRE(v.frame() == 1);  // hidden time never advances the frame
  REQUIRE(src.calls == calls_after_playing + 1);  // overlay skips analysis
}

TEST_CASE("paused decay eases to rest then suspends") {
  Visualizer v(44100);
  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.8f);

  // Seed the bands + smoothing with a playing tick (Go test sets v.bands
  // directly; the framework has no setter, so drive it through analysis).
  const Clock::time_point t0(std::chrono::milliseconds(1000));
  v.tick(tick_ctx(t0, src));
  REQUIRE(v.bands()[0] == Catch::Approx(0.8f));

  // Decay toward silence while paused (Go Analyze(nil) decays x0.8).
  src.decay = true;
  std::vector<float> prev(v.smoothed_bands().begin(), v.smoothed_bands().end());
  bool settled = false;
  for (int i = 0; i < 240 && !settled; ++i) {
    v.tick(tick_ctx(t0 + (i + 1) * kTickSlow, src, /*paused=*/true));
    const auto cur = v.smoothed_bands();
    for (std::size_t b = 0; b < cur.size(); ++b) {
      if (cur[b] > prev[b] + 1e-6f) {
        FAIL("paused tick " << i << " band " << b << " rose " << prev[b] << " -> "
                            << cur[b] << ", want monotonic decay");
      }
    }
    prev.assign(cur.begin(), cur.end());
    if (!v.paused_decay_pending(tick_ctx(t0 + (i + 2) * kTickSlow, src, /*paused=*/true))) {
      settled = true;
    }
  }
  REQUIRE(settled);  // the visualizer must settle to rest (cliamp test cap: 240)
  for (float b : v.bands()) {
    REQUIRE(b < 0.01f);
  }
  for (float b : v.smoothed_bands()) {
    REQUIRE(b < 0.01f);
  }
  REQUIRE(v.frame() == 1);  // paused ticks never advance the frame

  // Once settled, further paused ticks suspend and leave everything alone.
  const int calls_before = src.calls;
  const auto bands_before = std::vector<float>(v.bands().begin(), v.bands().end());
  const auto smoothed_before = std::vector<float>(v.smoothed_bands().begin(), v.smoothed_bands().end());
  v.tick(tick_ctx(t0 + std::chrono::seconds(10), src, /*paused=*/true));
  REQUIRE(src.calls == calls_before);
  REQUIRE(v.frame() == 1);
  for (std::size_t i = 0; i < v.bands().size(); ++i) {
    REQUIRE(v.bands()[i] == bands_before[i]);
    REQUIRE(v.smoothed_bands()[i] == smoothed_before[i]);
  }
}

TEST_CASE("paused_decay_pending reports band content above the epsilon") {
  Visualizer v(44100);
  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.8f);
  const Clock::time_point t0(std::chrono::milliseconds(1000));
  v.tick(tick_ctx(t0, src));

  // Paused with 0.8 bands: still needs decay ticks.
  REQUIRE(v.paused_decay_pending(tick_ctx(t0, src, /*paused=*/true)));
  // Go's pausedSettled ignores the Paused flag — the report is content-based
  // (band levels + rest cadence), so it stays true for a playing ctx too.
  REQUIRE(v.paused_decay_pending(tick_ctx(t0, src, /*paused=*/false)));

  // Decay to rest: the pending report flips to false.
  src.decay = true;
  for (int i = 0; i < 240; ++i) {
    v.tick(tick_ctx(t0 + (i + 1) * kTickSlow, src, /*paused=*/true));
    if (!v.paused_decay_pending(tick_ctx(t0 + (i + 2) * kTickSlow, src, /*paused=*/true))) {
      break;
    }
  }
  REQUIRE_FALSE(v.paused_decay_pending(tick_ctx(t0, src, /*paused=*/true)));
}

// ---------------------------------------------------------------------------
// tick_interval / uses_raw_samples
// ---------------------------------------------------------------------------

TEST_CASE("tick_interval follows driver cadence, paused and no-driver fallbacks") {
  Visualizer v(44100);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  const Clock::time_point t0(std::chrono::milliseconds(1000));

  // No driver created yet: slow (the first wake happens at kTickSlow).
  REQUIRE(v.tick_interval(tick_ctx(t0, src)) == kTickSlow);

  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  v.tick(tick_ctx(t0, src));  // creates the Logo driver

  REQUIRE(v.tick_interval(tick_ctx(t0, src)) == kTickFast);             // playing
  REQUIRE(v.tick_interval(tick_ctx(t0, src, /*paused=*/true)) == kTickSlow);
  REQUIRE(v.tick_interval(tick_ctx(t0, src, /*paused=*/false, /*playing=*/true,
                                   /*overlay=*/true)) == kTickSlow);
  REQUIRE(v.tick_interval(tick_ctx(t0, src, /*paused=*/false, /*playing=*/false)) == kTickSlow);

  v.set_mode(VisMode::None);
  v.tick(tick_ctx(t0, src));  // no-op driver: always slow
  REQUIRE(v.tick_interval(tick_ctx(t0, src)) == kTickSlow);
}

TEST_CASE("uses_raw_samples reports band-driven vs raw-sample modes") {
  Visualizer v(44100);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  REQUIRE_FALSE(v.uses_raw_samples());  // no driver yet

  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);  // 10-band FFT spec
  v.tick(tick_ctx(Clock::time_point(std::chrono::milliseconds(1000)), src));
  REQUIRE_FALSE(v.uses_raw_samples());

  // VisMode::None uses the no-op driver whose zero-value spec normalizes to a
  // raw-sample spec (cliamp: UsesRawSamples is true for None).
  v.set_mode(VisMode::None);
  v.tick(tick_ctx(Clock::time_point(std::chrono::milliseconds(2000)), src));
  REQUIRE(v.uses_raw_samples());
}

// ---------------------------------------------------------------------------
// render dispatch
// ---------------------------------------------------------------------------

TEST_CASE("render dispatches to the active driver and fills the grid") {
  Visualizer v(44100);
  v.set_size(40, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.9f);
  const Clock::time_point t0(std::chrono::milliseconds(1000));
  v.tick(tick_ctx(t0, src));

  CellGrid grid;
  REQUIRE(v.render(grid));
  REQUIRE(grid.rows() == 5);
  REQUIRE(grid.cols() == 40);
  // The logo writes a braille glyph into every cell.
  for (int r = 0; r < grid.rows(); ++r) {
    for (int c = 0; c < grid.cols(); ++c) {
      const char32_t rune = grid.at(r, c).rune;
      REQUIRE(rune >= U'⠀');
      REQUIRE(rune <= U'⣿');
    }
  }
  // Per-line spectrum tiers (Go specWrap): top row high, bottom row low.
  REQUIRE(grid.at(0, 0).color == kColorSpecHigh);
  REQUIRE(grid.at(4, 0).color == kColorSpecLow);
}

TEST_CASE("render returns false for None mode, no driver, or empty size") {
  Visualizer v(44100);
  CellGrid grid;
  REQUIRE_FALSE(v.render(grid));  // no driver yet

  v.set_size(0, 5);  // degenerate size
  REQUIRE_FALSE(v.render(grid));

  v.set_size(40, 5);
  v.set_mode(VisMode::None);
  REQUIRE_FALSE(v.render(grid));  // hidden mode

  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  v.tick(tick_ctx(Clock::time_point(std::chrono::milliseconds(1000)), src));
  REQUIRE(v.render(grid));
  v.set_mode(VisMode::None);
  REQUIRE_FALSE(v.render(grid));
}

TEST_CASE("mode switch clears the smoothed bands") {
  Visualizer v(44100);
  v.set_size(80, 5);
  v.set_mode(VisMode::Logo);
  BandSource src;
  src.bands = std::vector<float>(kDefaultSpectrumBands, 0.4f);
  const Clock::time_point t0(std::chrono::milliseconds(1000));

  v.tick(tick_ctx(t0, src));  // snap: smoothed == bands
  src.bands.assign(kDefaultSpectrumBands, 1.0f);
  v.tick(tick_ctx(t0 + kTickAnalyze + std::chrono::milliseconds(1), src));
  // Eased toward 1.0 — smoothed no longer equals the raw bands.
  bool eased_differs = false;
  for (std::size_t i = 0; i < v.bands().size(); ++i) {
    if (v.smoothed_bands()[i] != v.bands()[i]) {
      eased_differs = true;
    }
  }
  REQUIRE(eased_differs);

  // Switch modes: the framework clears smoothed_ (cliamp syncDriverMode), so
  // the next tick snaps to the fresh analysis output.
  v.set_mode(VisMode::Bars);
  src.bands.assign(kDefaultSpectrumBands, 0.4f);
  v.tick(tick_ctx(t0 + 2 * kTickAnalyze + std::chrono::milliseconds(2), src));
  for (std::size_t i = 0; i < v.bands().size(); ++i) {
    REQUIRE(v.smoothed_bands()[i] == v.bands()[i]);
  }
}

// ---------------------------------------------------------------------------
// Logo driver
// ---------------------------------------------------------------------------

TEST_CASE("logo driver declares the default 10-band spec") {
  auto driver = vis_drivers::make_logo_driver();
  REQUIRE(driver != nullptr);
  const auto spec = driver->analysis_spec();
  REQUIRE(spec.band_count == 10);
  REQUIRE(spec.fft_size == 2048);

  BandSource src;
  VisTickContext ctx;
  ctx.playing = true;
  REQUIRE(driver->tick_interval(ctx) == kTickFast);
  ctx.playing = false;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  ctx.playing = true;
  ctx.overlay_active = true;
  REQUIRE(driver->tick_interval(ctx) == kTickSlow);
  REQUIRE(driver->pause_settled());  // band-driven: framework decides settling
}

TEST_CASE("logo renders dense dots for loud bands and sparse for silence") {
  auto driver = vis_drivers::make_logo_driver();

  CellGrid loud(5, 40), quiet(5, 40);
  std::vector<float> hi(kDefaultSpectrumBands, 0.9f);
  std::vector<float> lo(kDefaultSpectrumBands, 0.0f);
  driver->render(hi, 0, loud);
  driver->render(lo, 0, quiet);

  const std::size_t loud_dots  = dot_cell_count(loud);
  const std::size_t quiet_dots = dot_cell_count(quiet);
  REQUIRE(loud_dots > 0);
  REQUIRE(quiet_dots > 0);
  REQUIRE(loud_dots > quiet_dots * 2);  // loud fills the text solid
}

TEST_CASE("logo rendering is deterministic for a given frame") {
  auto driver = vis_drivers::make_logo_driver();
  CellGrid a(5, 40), b(5, 40);
  std::vector<float> bands(kDefaultSpectrumBands, 0.5f);
  driver->render(bands, 42, a);
  driver->render(bands, 42, b);
  REQUIRE(dump_grid(a) == dump_grid(b));
}
