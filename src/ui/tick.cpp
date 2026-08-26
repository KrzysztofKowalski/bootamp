// ui/tick.cpp — the ~60 FPS tick loop (contract: ui/tick.hpp).
//
// Port of cliamp's tick machinery (ui/tick.go cadence constants +
// ui/model/tick.go cadence selection + ui/visualizer.go TickInterval).
// The Go side is a Bubbletea message loop (tea.Tick cmd feeding tickMsg into
// the model's Update); the C++ side is a dedicated std::jthread per the
// contract: it wakes at the visualizer's chosen cadence, runs the visualizer
// tick + render into a CellGrid, and hands the grid to the FTXUI blit.
//
// Cadence selection (cliamp Visualizer.TickInterval):
//   * active driver's interval, when neither paused nor nil;
//   * kTickSlow when paused or no active driver.
// The model-level policy in Go (playing -> TickFast floor, low-power, idle)
// is the app wiring agent's job via set_context(playing/paused/overlay).

#include "ui/tick.hpp"

#include <chrono>
#include <utility>

namespace bootamp::ui {

TickLoop::TickLoop(Visualizer& vis, BlitCallback blit)
    : vis_(vis), blit_(std::move(blit)) {}

TickLoop::~TickLoop() { stop(); }

void TickLoop::start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (running_.exchange(true)) {
    return;  // already running: no-op
  }
  thread_ = std::jthread([this](std::stop_token st) { loop(std::move(st)); });
  // Render the first frame immediately so the screen is not blank for one
  // full cadence (cliamp draws an initial View at program start; here the
  // tick is the only render driver).
  wake_.store(true);
  cond_.notify_all();
}

void TickLoop::stop() {
  std::jthread to_join;  // moved out so the join runs without holding mu_
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.exchange(false)) {
      return;  // idempotent: never started, or already stopped
    }
    to_join = std::move(thread_);
  }
  to_join.request_stop();
  cond_.notify_all();
  to_join.join();
}

void TickLoop::wake() {
  wake_.store(true);
  cond_.notify_all();
}

void TickLoop::set_context(VisTickContext ctx) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    ctx_ = std::move(ctx);
  }
  // Wake early: the cadence may have changed (playing -> paused -> slow,
  // overlay -> slow). cliamp re-derives the interval on every tick; notifying
  // here makes the switch effective immediately instead of at the next
  // scheduled wake.
  cond_.notify_all();
}

void TickLoop::loop(std::stop_token stoken) {
  CellGrid grid;
  while (!stoken.stop_requested()) {
    VisTickContext ctx;
    std::chrono::milliseconds interval;
    {
      std::unique_lock<std::mutex> lk(mu_);
      ctx = ctx_;
      // Cadence selection (cliamp Visualizer.TickInterval): the active
      // driver's interval; kTickSlow when paused or no driver. A degenerate
      // (<= 0) interval is clamped so a bad driver cannot busy-spin the loop.
      interval = vis_.tick_interval(ctx);
      if (interval <= std::chrono::milliseconds::zero()) {
        interval = kTickFast;
      }
      cond_.wait_for(lk, stoken, interval, [&] {
        return wake_.exchange(false) || stoken.stop_requested();
      });
      if (stoken.stop_requested()) {
        break;
      }
    }

    // Size the grid to the render target (the app sets the vis dimensions
    // via Visualizer::set_size before/while running). resize() only on size
    // change so steady-state frames reuse the storage.
    if (grid.rows() != vis_.rows() || grid.cols() != vis_.cols()) {
      grid.resize(vis_.rows(), vis_.cols());
    }

    vis_.tick(ctx);
    if (vis_.render(grid) && blit_) {
      blit_(grid);
    }
  }
}

}  // namespace bootamp::ui
