// ui/tick.hpp — the ~60 FPS tick loop driving analyze→render→blit.
//
// Port of cliamp's tick loop (ui/model). A jthread wakes at the driver's chosen
// interval (TickFast ~60 FPS while playing, TickSlow when paused/overlay), runs
// the visualizer tick + render into a CellGrid, then hands the grid to the FTXUI
// blit. stop_token-owned; joined in the dtor. The benchmark target is
// BM_TickPipeline < 16µs/frame (analyze+render, no blit).
#pragma once

#include "ui/visualizer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace bootamp::ui {

// Tick cadence constants (cliamp TickFast/TickSlow/TickAnim/TickWave/
// TickAnalyze/TickIdle; kTickSpectrum is Go TickFast).
inline constexpr std::chrono::milliseconds kTickFast     {16};   // ~60 FPS: raw-sample modes + tick-loop refresher
inline constexpr std::chrono::milliseconds kTickSpectrum {50};   // 20 FPS: spectrum/band modes while playing (Go TickFast)
inline constexpr std::chrono::milliseconds kTickSlow     {200};
inline constexpr std::chrono::milliseconds kTickAnim     {33};   // ~30 FPS animation
inline constexpr std::chrono::milliseconds kTickWave     {16};   // raw-sample modes (Go TickAnim/TickWave)
inline constexpr std::chrono::milliseconds kTickAnalyze  {33};   // FFT cadence (Go TickAnalyze)
inline constexpr std::chrono::milliseconds kTickIdle     {1500}; // fully idle cadence (Go TickIdle)

// TickLoop drives the visualizer at the driver-selected cadence. The blit
// callback is invoked on the tick thread with the freshly-rendered CellGrid;
// the FTXUI backend owns the actual screen draw (thread-safe via its own loop).
class TickLoop {
public:
  using BlitCallback = std::function<void(const CellGrid&)>;

  TickLoop(Visualizer& vis, BlitCallback blit);
  ~TickLoop();
  TickLoop(const TickLoop&)            = delete;
  TickLoop& operator=(const TickLoop&) = delete;

  // start / stop the jthread. stop() is idempotent and joins.
  void start();
  void stop();

  // wake forces an immediate tick (e.g. on a keypress that changes vis mode).
  void wake();

  // set_context updates the playback context the tick uses for cadence + the
  // analyze/stereo callbacks. Thread-safe.
  void set_context(VisTickContext ctx);

private:
  void loop(std::stop_token stoken);

  Visualizer&                          vis_;
  BlitCallback                         blit_;
  std::jthread                         thread_;
  std::condition_variable_any          cond_;
  std::mutex                           mu_;
  VisTickContext                       ctx_;
  std::atomic<bool>                    wake_{false};
  std::atomic<bool>                    running_{false};
};

}  // namespace bootamp::ui