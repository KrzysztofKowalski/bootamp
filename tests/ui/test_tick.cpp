// tests/ui/test_tick.cpp — TickLoop tests.
//
// Ports the cadence-selection tests from cliamp/ui/model/tick_test.go
// (TestTickIntervalPlayingUsesFastCadence / TestTickIntervalVisualizer60FPS
// / TestTickIntervalPausedSettlingVisualizerUsesFast) onto the C++ TickLoop.
// The Go tick is a Bubbletea message loop; here the observable is the blit
// callback, which fires on the tick thread.
//
// Timing discipline: no sleeps. Every wait is a condition-variable wait with
// a generous timeout, and cadence assertions use multi-x margins (fast
// cadences are 16-50ms, the slow cadence is 200ms).
//
// NOTE (cross-agent dependencies): the cadence tests rely on
// Visualizer::tick_interval clamping paused contexts to kTickSlow and on
// Visualizer::render filling a grid for the default Bars mode — both are the
// 1:1 Go semantics (cliamp Visualizer.TickInterval returns TickSlow when
// paused) that the visualizer agent ports.

#include "ui/tick.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace ui = bootamp::ui;
using bootamp::ui::CellGrid;
using bootamp::ui::VisAnalysisSpec;
using bootamp::ui::VisTickContext;
using bootamp::ui::Visualizer;
using bootamp::ui::TickLoop;

namespace {

// Generous zero-filled band span so drivers never call an empty analyze
// callback (the Go model always wires a real one via visualizerTickContext).
constexpr std::array<float, 256> kZeroBands{};

std::span<const float> zero_bands(const VisAnalysisSpec&) { return kZeroBands; }

std::size_t no_waveform(std::span<float>) { return 0; }
std::size_t no_stereo(std::span<std::array<float, 2>>) { return 0; }

// Fake playback context: paused => slow cadence, playing => driver cadence.
VisTickContext paused_ctx() {
  VisTickContext ctx;
  ctx.now = std::chrono::steady_clock::now();
  ctx.playing = false;
  ctx.paused = true;
  ctx.analyze = zero_bands;
  ctx.waveform_samples_into = no_waveform;
  ctx.stereo_samples_into = no_stereo;
  return ctx;
}

VisTickContext playing_ctx() {
  VisTickContext ctx = paused_ctx();
  ctx.playing = true;
  ctx.paused = false;
  return ctx;
}

// BlitRecorder counts blits, timestamps them, and keeps a copy of the last
// grid. Called on the tick thread; guarded by its own mutex.
struct BlitRecorder {
  std::mutex mu;
  std::condition_variable cv;
  std::size_t count = 0;
  std::deque<std::chrono::steady_clock::time_point> at;
  CellGrid last;

  void operator()(const CellGrid& g) {
    std::lock_guard<std::mutex> lk(mu);
    ++count;
    at.push_back(std::chrono::steady_clock::now());
    if (at.size() > 64) {
      at.pop_front();
    }
    last = g;
    cv.notify_all();
  }

  // Blocks until count >= want (or timeout) and returns the observed count.
  std::size_t wait_for_blits(std::size_t want, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, timeout, [&] { return count >= want; });
    return count;
  }

  std::size_t snapshot() {
    std::lock_guard<std::mutex> lk(mu);
    return count;
  }

  std::chrono::steady_clock::time_point latest_at() {
    std::lock_guard<std::mutex> lk(mu);
    return at.empty() ? std::chrono::steady_clock::time_point{} : at.back();
  }

  std::vector<std::chrono::steady_clock::time_point> timestamps() {
    std::lock_guard<std::mutex> lk(mu);
    return {at.begin(), at.end()};
  }
};

}  // namespace

// Tick cadence constants (cliamp ui/tick.go, renamed in the C++ contract).
TEST_CASE("Tick cadence constants match cliamp", "[tick][unit]") {
  REQUIRE(ui::kTickFast.count() == 16);    // ~60 FPS (Go TickAnim)
  REQUIRE(ui::kTickSlow.count() == 200);   // Go TickSlow
  REQUIRE(ui::kTickAnim.count() == 33);    // ~30 FPS (Go TickAnalyze)
  REQUIRE(ui::kTickWave.count() == 16);    // Go TickWave
  REQUIRE(ui::kTickAnalyze.count() == 50); // FFT cadence (Go TickFast)
}

// Port of TestInitialTickUsesFastCadence: start renders the first frame
// without waiting out a full cadence.
TEST_CASE("TickLoop start renders the first frame promptly", "[tick]") {
  Visualizer vis(44100);
  vis.set_size(80, 5);
  BlitRecorder rec;
  TickLoop tl(vis, [&](const CellGrid& g) { rec(g); });

  // stop()/wake() before start() are no-ops and must not crash.
  tl.stop();
  tl.stop();
  tl.wake();

  tl.start();
  tl.start();  // second start is a no-op
  REQUIRE(rec.wait_for_blits(1, 2s) >= 1);
  // tick() was actually driven on the tick thread.
  REQUIRE(vis.frame() >= 1);
  tl.stop();
}

TEST_CASE("TickLoop stop is idempotent and joined", "[tick]") {
  Visualizer vis(44100);
  vis.set_size(80, 5);
  BlitRecorder rec;
  TickLoop tl(vis, [&](const CellGrid& g) { rec(g); });
  tl.set_context(playing_ctx());
  tl.start();
  REQUIRE(rec.wait_for_blits(2, 2s) >= 2);

  tl.stop();
  tl.stop();  // idempotent: second stop is a no-op

  // The thread is joined: the blit count is final, and further wake()/stop()
  // calls change nothing (deterministic — no sleeps involved).
  const std::size_t after_stop = rec.snapshot();
  tl.wake();
  tl.wake();
  tl.stop();
  REQUIRE(rec.snapshot() == after_stop);

  // Restart works after a stop.
  tl.start();
  REQUIRE(rec.wait_for_blits(after_stop + 1, 2s) >= after_stop + 1);
  tl.stop();
}

TEST_CASE("TickLoop destructor stops and joins", "[tick]") {
  std::unique_ptr<TickLoop> tl;
  {
    Visualizer vis(44100);
    vis.set_size(80, 5);
    BlitRecorder rec;
    tl = std::make_unique<TickLoop>(vis, [&](const CellGrid& g) { rec(g); });
    tl->set_context(playing_ctx());
    tl->start();
    REQUIRE(rec.wait_for_blits(1, 2s) >= 1);
    const std::size_t before = rec.snapshot();
    tl.reset();  // dtor joins; no more blits afterwards
    REQUIRE(rec.snapshot() == before);
  }
}

// Port of TestTickIntervalPausedSettlingVisualizerUsesFast: the paused
// cadence is the slow one (kTickSlow, 200ms); wake() pulls a tick forward.
TEST_CASE("TickLoop wake forces an immediate tick at a slow cadence", "[tick]") {
  Visualizer vis(44100);
  vis.set_size(80, 5);
  BlitRecorder rec;
  TickLoop tl(vis, [&](const CellGrid& g) { rec(g); });
  tl.set_context(paused_ctx());  // 200ms cadence
  tl.start();
  REQUIRE(rec.wait_for_blits(1, 2s) >= 1);  // immediate first frame

  for (int i = 0; i < 3; ++i) {
    const auto t0 = rec.latest_at();
    tl.wake();
    REQUIRE(rec.wait_for_blits(i + 2, 1s) >= static_cast<std::size_t>(i + 2));
    const auto t1 = rec.latest_at();
    // A scheduled tick at the 200ms cadence arrives >= ~200ms after the
    // previous one; a wake-serviced tick arrives in microseconds. Bound the
    // gap well below one full cadence so scheduling jitter can't flip it.
    REQUIRE(t1 - t0 < 150ms);
  }
  tl.stop();
}

// Port of TestTickIntervalPlayingUsesFastCadence / the paused-settling test:
// the loop's cadence follows the context (paused -> slow, playing -> fast).
TEST_CASE("TickLoop cadence switches with the context", "[tick]") {
  Visualizer vis(44100);
  vis.set_size(80, 5);
  BlitRecorder rec;
  TickLoop tl(vis, [&](const CellGrid& g) { rec(g); });

  // Paused: cadence is kTickSlow (200ms). Collect 4 blits (t0, +200, +400,
  // +600) and require every gap to be well above the fast cadences
  // (16-50ms) — wait_for never fires early, so a 100ms bound is 2x margin.
  tl.set_context(paused_ctx());
  tl.start();
  REQUIRE(rec.wait_for_blits(4, 3s) >= 4);
  {
    const auto ts = rec.timestamps();
    REQUIRE(ts.size() >= 4);
    for (std::size_t i = 1; i < ts.size() && i < 8; ++i) {
      REQUIRE(ts[i] - ts[i - 1] >= 100ms);
    }
  }

  // Switch to playing: cadence becomes the driver's fast interval (one of
  // kTickFast/kTickAnim/kTickWave/kTickAnalyze, all <= 50ms). The loop wakes
  // immediately on set_context; within 500ms it must deliver >= 4 more
  // blits — a 200ms cadence would deliver at most ~3 in the same window.
  tl.set_context(playing_ctx());
  const std::size_t at_switch = rec.snapshot();
  {
    std::unique_lock<std::mutex> lk(rec.mu);
    rec.cv.wait_for(lk, 500ms, [&] { return rec.count >= at_switch + 4; });
    REQUIRE(rec.count >= at_switch + 4);
  }
  tl.stop();
}

// The blit hands the tick thread's grid, sized to the render target.
TEST_CASE("TickLoop blits a grid sized to the visualizer", "[tick]") {
  Visualizer vis(44100);
  vis.set_size(24, 6);
  BlitRecorder rec;
  TickLoop tl(vis, [&](const CellGrid& g) { rec(g); });
  tl.set_context(playing_ctx());
  tl.start();
  REQUIRE(rec.wait_for_blits(1, 2s) >= 1);
  {
    std::lock_guard<std::mutex> lk(rec.mu);
    REQUIRE(rec.last.rows() == 6);
    REQUIRE(rec.last.cols() == 24);
  }
  tl.stop();
}

// set_context / wake are thread-safe against the running loop.
TEST_CASE("TickLoop set_context is thread-safe", "[tick]") {
  Visualizer vis(44100);
  vis.set_size(80, 5);
  BlitRecorder rec;
  TickLoop tl(vis, [&](const CellGrid& g) { rec(g); });
  tl.start();

  std::thread toggler([&] {
    for (int i = 0; i < 50; ++i) {
      tl.set_context((i % 2) ? paused_ctx() : playing_ctx());
      tl.wake();
    }
  });
  toggler.join();

  REQUIRE(rec.wait_for_blits(2, 2s) >= 2);
  tl.stop();
  tl.stop();
}
