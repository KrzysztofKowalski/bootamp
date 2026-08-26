// app/main.cpp — bootamp entry point and application wiring (M5).
//
// Port of cliamp/main.go run() — the wiring order is preserved 1:1:
// config load -> CLI overrides -> logging -> search shorthand -> yt-dlp
// settings -> providers -> playlist/default-radio/CLI args -> remote URL
// resolution -> engine (sink + sample rate) -> ApplyPlayer/ApplyPlaylist ->
// padding -> resume state -> UI shell -> autoPlay -> run -> resume save.
//
// Architecture (single foreground process, no daemon/IPC):
//   * cli::parse_cli produces config::Overrides + positional args.
//   * config::load + apply_overrides produce the effective Config.
//   * An AudioEngine owns the audio jthread (atomics for controls). The sink
//     is miniaudio when <miniaudio.h> is available at build time, else a
//     NullSink with a printed warning.
//   * playlist::Playlist + provider::radio + provider::local + resolve build
//     the queue; every CLI arg is classified by resolve::args (local file /
//     dir / m3u / pls / URL / yt-dlp).
//   * A PlaybackController (engine + playlist) implements the Go model's
//     playTrack/nextTrack/prevTrack/togglePlayPause/doSeek/preloadNext and a
//     200ms watchdog jthread that owns end-of-playback (the engine's audio
//     loop does not stop itself on drain) and gapless advance. Play starts
//     are async: play_track raises a UI-visible buffering flag and enqueues
//     the track to a single-flight play worker jthread — HTTP connect/TLS,
//     m3u/pls fetch and ffmpeg/yt-dlp spawns never run on the FTXUI loop
//     thread (the freeze fix; Go runs them in tea.Cmd goroutines).
//   * ui::FtxuiApp drives the visualizer (via its own TickLoop) and forwards
//     keys to the controller; a 50ms refresher jthread keeps the
//     VisTickContext's `now` and callbacks fresh (TickLoop copies the context
//     as-is and never refreshes ctx.now). The queue/browse/EQ/help/device
//     screens are plain-C++ models driven from the shell's key callback
//     (app_key) and
//     rendered by the overlay composite installed on the shell (the shell
//     swaps the vis frame for the active screen). When FTXUI is absent the
//     app runs headless: play and wait for the playlist to end or Ctrl+C.
//
// Deviations from Go are listed in the task report; the notable ones:
//   * Pending URLs (feeds/M3U/PLS/yt-dlp) resolve synchronously at startup
//     (Go's TUI defers them to a background loop; the daemon path — which we
//     follow — resolves them up front).
//   * Feed (RSS/podcast) URLs are skipped with a warning: the C++ resolve
//     module has no feed resolver yet.
//   * The TUI starts playback immediately when CLI args were given (Go waits
//     for space unless auto_play; the plan's `bootamp play <file>` examples
//     imply playback).
//   * No yt-dlp seek debounce (every seek key rebuilds the pipeline).
#include "app/cli.hpp"

#include "audio/audio_sink.hpp"
#include "audio/engine.hpp"
#include "config/config.hpp"
#include "dsp/spectrum.hpp"
#include "foundation/appdir.hpp"
#include "foundation/applog.hpp"
#include "foundation/resume.hpp"
#include "playlist/playlist.hpp"
#include "playlist/provider.hpp"
#include "provider/local/provider.hpp"
#include "provider/radio/radio.hpp"
#include "provider/track.hpp"
#include "resolve/resolve.hpp"
#include "resolve/wrapper.hpp"
#include "resolve/ytdl.hpp"
#include "ui/ftxui_app.hpp"
#include "ui/ftxui_app_impl.hpp"
#include "ui/screens/browse.hpp"
#include "ui/screens/device.hpp"
#include "ui/screens/eq_overlay.hpp"
#include "ui/screens/help.hpp"
#include "ui/screens/info.hpp"
#include "ui/screens/jump.hpp"
#include "ui/screens/pl_picker.hpp"
#include "ui/screens/queue.hpp"
#include "ui/screens/url.hpp"
#include "ui/styles.hpp"
#include "ui/tick.hpp"
#include "ui/visualizer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The screens' FTXUI component factories and the shell's overlay hosting are
// only available in a full FTXUI build (the bootamp executable is only built
// then anyway; this guard mirrors ui/screens/*.hpp).
#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#endif  // BOOTAMP_HAS_FTXUI

// Whether the miniaudio sink implementation is available in this build. The
// executable target is gated on BOOTAMP_HAS_MINIAUDIO (CMakeLists.txt); when
// the header is absent we fall back to a NullSink with a printed warning.
#if __has_include(<miniaudio.h>)
#define BOOTAMP_APP_HAS_MINIAUDIO 1
#else
#define BOOTAMP_APP_HAS_MINIAUDIO 0
#endif

namespace bootamp::app {

namespace {

using std::chrono_literals::operator""ms;

// ---------------------------------------------------------------------------
// Config adapters
// ---------------------------------------------------------------------------

// AudioEngine and Playlist implement the operations of config::PlayerConfig /
// config::PlaylistConfig but not the abstract interfaces themselves, so the
// wiring adapts them (config::apply_player / config::apply_playlist).
class EnginePlayerConfig final : public config::PlayerConfig {
public:
  explicit EnginePlayerConfig(audio::AudioEngine& engine) : engine_(engine) {}
  void set_volume_min(double db) override { engine_.set_volume_min(db); }
  void set_volume(double db) override { engine_.set_volume(db); }
  void set_speed(double ratio) override { engine_.set_speed(ratio); }
  void set_eq_band(int band, double db) override {
    engine_.set_eq_band(band, db);
  }
  void toggle_mono() override { engine_.toggle_mono(); }

private:
  audio::AudioEngine& engine_;
};

class PlaylistPlayerConfig final : public config::PlaylistConfig {
public:
  explicit PlaylistPlayerConfig(playlist::Playlist& pl) : pl_(pl) {}
  void cycle_repeat() override { pl_.cycle_repeat(); }
  void toggle_shuffle() override { pl_.toggle_shuffle(); }

private:
  playlist::Playlist& pl_;
};

// ---------------------------------------------------------------------------
// PlaybackController — the app-side playback brain (Go ui/model)
// ---------------------------------------------------------------------------

// PlaybackController owns the engine+playlist pair: playTrack/nextTrack/
// prevTrack/togglePlayPause/doSeek (cliamp ui/model/{playback,seek}.go),
// preloadNext + the gapless policy (cliamp ui/model/preload.go), and the
// end-of-playback watchdog (Go DrainedMsg -> nextTrack).
//
// Play starts are ASYNC: play_track raises the buffering flag and hands the
// track to a single-flight play worker (jthread + mutex-protected job slot)
// instead of building the pipeline on the UI thread. All blocking work —
// HTTP connect/TLS (http_socket.cpp), m3u/pls fetch (resolve), ffmpeg/yt-dlp
// spawns (ffmpeg_pipe.cpp/ytdl.cpp) — runs on that worker (Go: playStreamCmd
// runs in a tea.Cmd goroutine, never on the Bubbletea update loop). The
// worker clears the buffering flag when the job completes (success or
// error); play errors are surfaced via applog::user_error, which also feeds
// the on-screen status ring.
class PlaybackController {
public:
  PlaybackController(audio::AudioEngine& engine, playlist::Playlist& pl)
      : engine_(engine), pl_(pl) {}

  // Resume state (cliamp m.resume): applied once to the matching track,
  // then cleared.
  foundation::ResumeState resume;

  // pending_seek_pct_ holds the armed Nj seek digit (Go m.jumpPercent):
  // -1 = none; a digit 0-9 sets it (the status ring shows "Nj → N0%") and
  // 'j' executes the jump to duration*pct/10.
  int pending_seek_pct_ = -1;

  // play_track starts `t` (Go playTrack: player.Play + applyResume +
  // preloadNext) ASYNC: raises the buffering flag immediately so the status
  // line shows loading (Go playback.go sets m.buffering before the async
  // playStreamCmd), enqueues the track for the play worker, and returns at
  // once. The worker runs run_play (all network/spawn work) and clears the
  // flag.
  void play_track(const playlist::Track& t) {
    enqueue([this, t] { run_play(t); });
  }

  // enqueue_play_job runs a caller job on the play worker — the browse
  // Enter path (Go fetchTracksCmd: provider tracks + wrapper-URL resolve +
  // play) so m3u/pls network fetches never block the UI thread. The
  // buffering flag is raised until the job completes. Single-flight: a
  // newer job supersedes a pending one.
  void enqueue_play_job(std::function<void()> job) {
    enqueue(std::move(job));
  }

  // buffering is the UI-visible loading flag (Go model.go:318 m.buffering);
  // the status line renders "[buffering]" while it is set.
  bool buffering() const { return buffering_.load(std::memory_order_acquire); }

  // run_play executes a play start ON the play worker: engine play (the
  // slow part — open_source HTTP connect/TLS, ffmpeg/yt-dlp spawn), resume
  // application, and the gapless preload arm. Feed tracks warn + return.
  // Errors surface via applog::user_error ("play: <err>") — file + the
  // on-screen status ring. Public so the browse Enter job can call it
  // directly after its own wrapper-URL expansion (avoids a self-enqueue).
  void run_play(const playlist::Track& t) {
    const double known_dur = static_cast<double>(t.duration_secs);
    if (t.feed) {
      // Feed tracks are resolved before they reach us; nothing to do here.
      foundation::applog::user_warn("feed track not yet supported: {}", t.path);
      return;
    }
    std::string err;
    if (playlist::is_ytdl(t.path)) {
      err = engine_.play_ytdl(t.path, known_dur);
    } else {
      err = engine_.play(t.path, known_dur);
    }
    if (!err.empty()) {
      foundation::applog::user_error("play: {}", err);
      return;
    }
    // Resume (cliamp applyResume): consumed once; applied as an absolute
    // seek on a fresh pipeline (offset 0 == position).
    if (!resume.path.empty() && t.path == resume.path &&
        resume.position_sec > 0) {
      if (engine_.is_ytdl_seek()) {
        engine_.seek_ytdl(static_cast<double>(resume.position_sec));
      } else {
        engine_.seek(static_cast<double>(resume.position_sec));
      }
      resume = {};
    }
    preload_next();
  }

  // stop_play_worker stops the play worker at shutdown: a pending (not yet
  // started) request is dropped; an in-flight run_play is joined to
  // completion (bounded by the pipeline timeouts: 30s connect/TLS, 15s
  // ffmpeg initial-audio wait). Call before engine.stop().
  void stop_play_worker() {
    {
      std::lock_guard lk(play_mu_);
      play_stop_ = true;
      play_pending_ = false;
    }
    play_cv_.notify_all();
    if (play_thread_.joinable()) {
      play_thread_.join();
    }
  }

  // next_track plays the next playlist track (Go nextTrack). The playlist
  // owns repeat/shuffle/queue semantics; a no-op at the very end.
  void next_track() {
    auto [next, ok] = pl_.next();
    if (ok) {
      play_track(next);
    }
  }

  // prev_track: restart the current track when it is more than ~3s in,
  // otherwise play the previous track (Go prevTrack).
  void prev_track() {
    const double pos = engine_.position_secs();
    if (pos > 3.0) {
      if (engine_.is_ytdl_seek()) {
        engine_.seek_ytdl(-pos);
      } else {
        engine_.seek(-pos);
      }
      return;
    }
    auto [prev, ok] = pl_.prev();
    if (ok) {
      play_track(prev);
    }
  }

  // toggle_play_pause (Go togglePlayPause): playing -> pause; paused ->
  // resume; stopped -> play the current track again.
  void toggle_play_pause() {
    if (!engine_.is_playing()) {
      auto [track, idx] = pl_.current();
      if (idx >= 0) {
        play_track(track);
      }
      return;
    }
    engine_.toggle_pause();
  }

  // seek repositions by `offset_secs` (signed, relative) — Go doSeek:
  // local/HTTP tracks seek directly, yt-dlp seeks by pipeline rebuild. The
  // yt-dlp debounce window is skipped for the MVP.
  void seek(double offset_secs) {
    const std::string err =
        engine_.is_ytdl_seek() ? engine_.seek_ytdl(offset_secs)
                               : engine_.seek(offset_secs);
    if (!err.empty()) {
      foundation::applog::user_warn("seek: {}", err);
    }
  }

  // stop halts playback (Go player.Stop — the 's' key). End-of-playback
  // bookkeeping stays with the watchdog; a stopped engine just idles.
  void stop() { engine_.stop(); }

  // change_speed nudges the playback speed by `delta`, clamped to
  // [0.25, 4.0] (Go changeSpeed — the ]/[ keys; the status line mirrors
  // the current speed).
  void change_speed(double delta) {
    engine_.set_speed(std::clamp(engine_.speed() + delta, 0.25, 4.0));
  }

  // toggle_mono mirrors the 'm' key (Go toggleMono — engine mono downmix).
  void toggle_mono() { engine_.toggle_mono(); }

  // seek_to_percent jumps to an absolute percent of the current track (Go
  // jumpPercent → doSeek — the Nj keys): duration * pct / 100. Uses the
  // same seek path as seek() — relative offset for local/HTTP, pipeline
  // rebuild for yt-dlp (a no-op while the duration is unknown).
  void seek_to_percent(int pct) {
    const auto pd = engine_.position_and_duration_secs();
    if (pd.second <= 0.0) {
      return;  // unknown duration: nothing to jump to
    }
    seek(pd.second * static_cast<double>(pct) / 100.0 - pd.first);
  }

  // seek_to_abs jumps to an absolute position (Go jumpTo — the ctrl+j
  // overlay submit), through the same seek path as seek().
  void seek_to_abs(double secs) { seek(secs - engine_.position_secs()); }

  // is_playing mirrors the engine flag (the 'x' remove-current path).
  bool is_playing() const { return engine_.is_playing(); }

  // volume_step nudges the volume by `db`, clamped to [volume_min, +24] like
  // Go's volume stepping.
  void volume_step(double db) {
    const double min = engine_.volume_min();
    double next = engine_.volume() + db;
    if (next < min) {
      next = min;
    }
    if (next > 24.0) {
      next = 24.0;
    }
    engine_.set_volume(next);
  }

  // cycle_repeat / toggle_shuffle mirror the r / z keymap: change the mode,
  // drop any armed preload (it may point at a stale neighbor), re-arm.
  void cycle_repeat() {
    engine_.clear_preload();
    pl_.cycle_repeat();
    preload_next();
  }

  void toggle_shuffle() {
    engine_.clear_preload();
    pl_.toggle_shuffle();
    preload_next();
  }

  // watchdog_step runs on a 200ms jthread: consume the gapless-advance
  // event (playlist next + re-arm), handle end-of-playback, and preload on
  // lead time. The engine's audio loop does NOT stop itself on drain — the
  // app owns end-of-playback.
  void watchdog_step() {
    if (engine_.gapless_advanced()) {
      // The engine swapped to the preloaded track: advance the app-side
      // playlist to match (repeat/shuffle/queue semantics live here), then
      // arm the preload for the following track.
      pl_.next();
      preload_next();
      return;
    }
    if (engine_.drained() && !engine_.has_preload() &&
        !preloading_.load()) {
      // The current track ended with nothing queued (repeat off, or a failed
      // preload): replay-next (Go DrainedMsg -> nextTrack). At the very end
      // this stops playback.
      auto [next, ok] = pl_.next();
      if (!ok) {
        engine_.stop();
        foundation::applog::info("playback finished");
      } else {
        play_track(next);
      }
      return;
    }
    if (engine_.is_playing() && !engine_.has_preload() &&
        !preloading_.load()) {
      preload_next();
    }
  }

private:
  // enqueue registers `job` for the play worker and raises the buffering
  // flag; the worker clears the flag when the job completes. Single-flight:
  // the slot holds at most one job and a newer request supersedes a pending
  // one (the worker only ever runs the newest pending job). The worker
  // thread starts lazily on the first request.
  void enqueue(std::function<void()> job) {
    buffering_.store(true, std::memory_order_release);
    std::lock_guard lk(play_mu_);
    if (!play_thread_.joinable()) {
      play_thread_ = std::jthread([this] { play_loop(); });
    }
    play_job_ = std::move(job);
    play_pending_ = true;
    play_cv_.notify_all();
  }

  // play_loop (play worker thread): pop the pending job, run it — the
  // blocking build/install (HTTP connect/TLS, m3u/pls fetch, ffmpeg/yt-dlp
  // spawn) — then clear the buffering flag unless a newer job was enqueued
  // while this one ran (that enqueue re-raised the flag; it stays up until
  // the last queued job completes). stop_play_worker wakes the loop into
  // exit; an in-flight job runs to completion (bounded by the pipeline
  // timeouts) before the thread stops.
  void play_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lk(play_mu_);
        play_cv_.wait(lk, [&] { return play_stop_ || play_pending_; });
        if (play_stop_) {
          return;
        }
        job = std::move(play_job_);
        play_pending_ = false;
      }
      job();
      {
        std::lock_guard lk(play_mu_);
        if (!play_pending_) {
          buffering_.store(false, std::memory_order_release);
        }
      }
    }
  }

  // preload_next arms the gapless preload for the track after the current
  // one, following cliamp preload.go's policy:
  //   - live streams are never preloaded;
  //   - the yt-dlp next preloads only when remaining playback <= 15s;
  //   - the HTTP-stream next only when remaining <= 3s AND the current track
  //     has a known duration (duration > 0);
  //   - local files preload immediately.
  // Errors are ignored (logged at debug), matching Go's silent failure.
  void preload_next() {
    if (preloading_.exchange(true)) {
      return;  // one preload in flight at a time
    }
    do_preload_next();
    preloading_.store(false);
  }

  void do_preload_next() {
    if (engine_.has_preload()) {
      return;
    }
    auto [next, ok] = pl_.peek_next();
    if (!ok) {
      return;  // end of playlist
    }
    if (next.is_live()) {
      return;  // live streams are never preloaded
    }
    const bool ytdl = playlist::is_ytdl(next.path);
    const bool stream = !ytdl && playlist::is_url(next.path);
    if (ytdl || stream) {
      const auto pd = engine_.position_and_duration_secs();
      if (pd.second <= 0.0) {
        return;  // can't estimate the remaining time
      }
      const double remaining = pd.second - pd.first;
      const double lead = ytdl ? 15.0 : 3.0;  // cliamp preload.go leads
      if (remaining > lead) {
        return;  // too early
      }
    }
    const double known_dur = static_cast<double>(next.duration_secs);
    const std::string err =
        ytdl ? engine_.preload_ytdl(next.path, known_dur)
             : engine_.preload(next.path, known_dur);
    if (!err.empty()) {
      foundation::applog::debug("preload {}: {}", next.path, err);
    }
  }

  audio::AudioEngine&      engine_;
  playlist::Playlist&      pl_;
  std::atomic<bool>        preloading_{false};

  // Play worker state. play_mu_ guards play_job_/play_pending_/play_stop_;
  // buffering_ is set by enqueue (UI thread) and cleared by the worker.
  std::atomic<bool>        buffering_{false};
  std::mutex               play_mu_;
  std::condition_variable  play_cv_;
  std::function<void()>    play_job_;
  bool                     play_pending_ = false;
  bool                     play_stop_    = false;
  std::jthread             play_thread_;
};

// ---------------------------------------------------------------------------
// Visualizer tick context (Go ui/model/tick.go visualizerTickContext)
// ---------------------------------------------------------------------------

// VisContextState owns the app-side spectrum analyzer and scratch buffer that
// the tick context's analyze callback uses. Only the tick thread touches it
// (the refresher thread merely builds new lambda copies referencing it).
struct VisContextState {
  dsp::SpectrumAnalyzer analyzer;
  std::vector<float>    scratch;
  // engine / volume_linked are bound once at construction (main() step 11,
  // before any thread starts) and never mutated afterwards, so the tick
  // thread can read them with no race against the refresher. This lets the
  // analyze lambda capture a single pointer (8B) instead of two refs + a
  // bool (24B), which the std::function SBO (16B) cannot hold and would
  // heap-allocate on every refresher rebuild.
  audio::AudioEngine* engine        = nullptr;
  bool                volume_linked = false;
  VisContextState(double sample_rate, audio::AudioEngine& eng,
                  bool vol_linked)
      : analyzer(sample_rate), engine(&eng), volume_linked(vol_linked) {}
};

// build_tick_context builds a VisTickContext wired to the engine: playback
// flags, the analyze callback (pull mono tap samples, gain, FFT via
// dsp::SpectrumAnalyzer), and the waveform/stereo taps (volume-linked and
// mono-mixed like Go's visualizerTickContext). Called by the 50ms refresher.
ui::VisTickContext build_tick_context(audio::AudioEngine& engine,
                                      VisContextState& state,
                                      bool volume_linked) {
  ui::VisTickContext ctx;
  ctx.now = std::chrono::steady_clock::now();
  ctx.playing = engine.is_playing() && !engine.is_paused();
  ctx.paused = engine.is_paused();
  ctx.overlay_active = false;

  ctx.waveform_samples_into =
      [&engine, volume_linked](std::span<float> dst) -> std::size_t {
    const std::size_t n = engine.waveform_samples_into(dst);
    if (volume_linked) {
      const double gain = std::pow(10.0, engine.volume() / 20.0);
      if (gain != 1.0) {
        for (std::size_t i = 0; i < n; ++i) {
          dst[i] = static_cast<float>(static_cast<double>(dst[i]) * gain);
        }
      }
    }
    return n;
  };

  ctx.stereo_samples_into = [&engine, volume_linked](
                                std::span<std::array<float, 2>> dst)
      -> std::size_t {
    const std::size_t n = engine.stereo_samples_into(dst);
    const bool mono = engine.mono();
    const double gain =
        volume_linked ? std::pow(10.0, engine.volume() / 20.0) : 1.0;
    for (std::size_t i = 0; i < n; ++i) {
      float l = dst[i][0];
      float r = dst[i][1];
      if (mono) {
        const float m = (l + r) * 0.5f;
        l = m;
        r = m;
      }
      dst[i][0] = static_cast<float>(static_cast<double>(l) * gain);
      dst[i][1] = static_cast<float>(static_cast<double>(r) * gain);
    }
    return n;
  };

  ctx.analyze =
      [&state](const ui::VisAnalysisSpec& spec)
      -> std::span<const float> {
    // dsp::VisAnalysisSpec is size_t-typed; the ui spec is int-typed.
    dsp::VisAnalysisSpec ds;
    ds.band_count =
        static_cast<std::size_t>(spec.band_count < 0 ? 0 : spec.band_count);
    ds.fft_size = static_cast<std::size_t>(spec.fft_size <= 0 ? 2048
                                                              : spec.fft_size);
    ds = dsp::normalize_analysis_spec(ds);
    if (state.scratch.size() < ds.fft_size) {
      state.scratch.resize(ds.fft_size);
    }
    if (state.engine->is_paused()) {
      // No new samples while paused: feed silence so the spectrum eases down
      // (Go: vis.Analyze(nil, spec)).
      return state.analyzer.analyze({}, ds);
    }
    const std::size_t want = ds.fft_size;
    const std::size_t got = state.engine->samples_into(
        std::span<float>(state.scratch.data(), want));
    if (state.volume_linked) {
      const double gain = std::pow(10.0, state.engine->volume() / 20.0);
      if (gain != 1.0) {
        for (std::size_t i = 0; i < got; ++i) {
          state.scratch[i] = static_cast<float>(
              static_cast<double>(state.scratch[i]) * gain);
        }
      }
    }
    // samples_into fills at most `want`; keep Go's max(0, n-fftSize) tail.
    const std::size_t start = got > want ? got - want : 0;
    return state.analyzer.analyze(
        std::span<const float>(state.scratch.data() + start, got - start),
        ds);
  };

  return ctx;
}

// ---------------------------------------------------------------------------
// Remote URL resolution (cliamp resolve.Remote, daemon path)
// ---------------------------------------------------------------------------

// resolve_pending resolves the pending (remote) URLs found by resolve::args
// synchronously at startup — the daemon-mode semantics of Go's resolve.Remote
// (cliamp main.go:277-284). Feed URLs have no C++ resolver yet and are
// skipped with a warning instead of failing the session.
std::expected<std::vector<playlist::Track>, std::string>
resolve_pending(const std::vector<std::string>& pending) {
  std::vector<playlist::Track> out;
  for (const std::string& url : pending) {
    std::expected<std::vector<playlist::Track>, std::string> got =
        std::unexpected("no resolver");
    if (playlist::is_ytdl(url)) {
      got = resolve::resolve_ytdl(url);
    } else if (playlist::is_feed(url)) {
      foundation::applog::user_warn(
          "skipping feed URL (not yet supported): {}", url);
      continue;
    } else if (playlist::is_m3u(url)) {
      got = resolve::resolve_m3u(url);
    } else if (playlist::is_pls(url)) {
      got = resolve::resolve_pls(url);
    } else {
      // xiaoyuzhou episodes and sniffed feeds: no resolver in the C++ tree.
      foundation::applog::user_warn("skipping unsupported remote URL: {}",
                                    url);
      continue;
    }
    if (!got) {
      return std::unexpected("resolving " + url + ": " + got.error());
    }
    out.insert(out.end(), got->begin(), got->end());
  }
  return out;
}

// ---------------------------------------------------------------------------
// Status line + key dispatch
// ---------------------------------------------------------------------------

std::string fmt_clock(double secs) {
  if (secs < 0) {
    secs = 0;
  }
  const int total = static_cast<int>(secs + 0.5);
  return std::format("{}:{:02}", total / 60, total % 60);
}

// status_line feeds the FTXUI shell's status line: playback state (a
// "[buffering]" slot while a play start is in flight on the play worker),
// current track (stream title when set), position, volume, speed, vis mode,
// and the newest on-screen status-ring message (applog::ring_entries — the
// channel user_error/user_warn/status messages land on, Go's footer
// logLines). Called on the FTXUI loop thread each repaint.
std::string status_line(const audio::AudioEngine& engine,
                        const playlist::Playlist& pl,
                        const ui::Visualizer& vis,
                        bool buffering) {
  std::string out;
  if (buffering) {
    out += "[buffering] ";
  } else if (engine.is_playing()) {
    out += engine.is_paused() ? "[paused] " : "[playing] ";
  } else {
    out += "[stopped] ";
  }
  auto [track, idx] = pl.current();
  if (idx >= 0) {
    std::string title = engine.stream_title();
    if (title.empty()) {
      title = !track.title.empty() ? track.title : track.path;
    }
    out += title;
    if (!track.artist.empty()) {
      out += " — " + track.artist;
    }
    const auto pd = engine.position_and_duration_secs();
    out += "  " + fmt_clock(pd.first) + "/" + fmt_clock(pd.second);
  } else {
    out += "no tracks";
  }
  out += std::format("  vol {:.1f} dB", engine.volume());
  if (engine.speed() != 1.0) {
    out += std::format("  {:.2f}x", engine.speed());
  }
  out += "  " + vis.mode_name();
  // On-screen status ring: the newest user-facing message ("play: <err>",
  // favorites, browse errors). Oldest entries rotate out at max_entries.
  const auto ring = foundation::applog::ring_entries();
  if (!ring.empty()) {
    out += "  ·  " + ring.back().text;
  }
  return out;
}

// handle_key dispatches the global player keys — the last dispatch stage of
// app_key, after the active screen's model and the mode switch. Port of
// cliamp ui/model/keys.go global key table; the shell owns v / V / q / ctrl+c
// in-process, and the queue/browse/EQ/help screens are driven by app_key.
void handle_key(std::string_view key, PlaybackController& ctl,
                const config::Config& cfg, playlist::Playlist& pl,
                provider::radio::Provider& radio_prov) {
  if (key == "space") {
    ctl.toggle_play_pause();
  } else if (key == "left") {
    ctl.seek(-5.0);
  } else if (key == "right") {
    ctl.seek(5.0);
  } else if (key == "shift+left") {
    ctl.seek(-static_cast<double>(cfg.seek_step_large_duration()));
  } else if (key == "shift+right") {
    ctl.seek(static_cast<double>(cfg.seek_step_large_duration()));
  } else if (key == "+" || key == "=") {
    // Go volume: +/- only — the arrows are the playlist cursor.
    ctl.volume_step(1.0);
  } else if (key == "-") {
    ctl.volume_step(-1.0);
  } else if (key == "n" || key == ">" || key == ".") {
    ctl.next_track();
  } else if (key == "<" || key == ",") {
    ctl.prev_track();
  } else if (key == "r") {
    ctl.cycle_repeat();
  } else if (key == "z") {
    ctl.toggle_shuffle();
  } else if (key == "s") {
    // Go 's' is stop (z is shuffle).
    ctl.stop();
  } else if (key == "]") {
    // Go changeSpeed: +0.25, clamped [0.25, 4.0] (the status line shows
    // the speed whenever it is not 1.0).
    ctl.change_speed(0.25);
  } else if (key == "[") {
    ctl.change_speed(-0.25);
  } else if (key == "m") {
    // Go toggleMono.
    ctl.toggle_mono();
  } else if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
    // Nj seek (Go handleJumpPercent): a digit arms the jump; the status
    // ring shows the armed percent ("Nj → N0%").
    ctl.pending_seek_pct_ = key[0] - '0';
    foundation::applog::status("{}j → {}0%", key, ctl.pending_seek_pct_);
  } else if (key == "j") {
    // 'j' executes the armed Nj seek (Go handleJumpKey); ignored when no
    // digit was pressed.
    if (ctl.pending_seek_pct_ >= 0) {
      ctl.seek_to_percent(ctl.pending_seek_pct_ * 10);
      ctl.pending_seek_pct_ = -1;
    }
  } else if (key == "a") {
    // Go 'a': toggle the current track's play-next queue membership
    // (queue_position() > 0 = queued; positions are 1-based).
    const int idx = pl.index();
    if (idx >= 0) {
      if (pl.queue_position(idx) > 0) {
        pl.dequeue(idx);
      } else {
        pl.queue(idx);
      }
    }
  } else if (key == "x") {
    // Go 'x': remove the current track; stop playback when the removed
    // track was playing (the watchdog re-evaluates end-of-playback).
    const int idx = pl.index();
    if (idx < 0) {
      return;
    }
    if (ctl.is_playing()) {
      ctl.stop();
    }
    pl.remove(idx);
    const int n = pl.len();
    if (n > 0) {
      pl.set_index(std::clamp(idx, 0, n - 1));
    }
  } else if (key == "up") {
    // Playlist cursor (Go playlist-view up): the vis panel highlight
    // follows pl.index() via the shell's revision-keyed snapshot.
    const int n = pl.len();
    if (n > 0) {
      pl.set_index((pl.index() - 1 + n) % n);
    }
  } else if (key == "down") {
    const int n = pl.len();
    if (n > 0) {
      pl.set_index((pl.index() + 1) % n);
    }
  } else if (key == "pageup" || key == "pgup" || key == "ctrl+u") {
    // Go playlist-view page up: 8 rows, clamped (canonical key names are
    // "pageup"/"pagedown"; "pgup"/"pgdown" kept for the screens' legacy
    // spelling).
    const int n = pl.len();
    if (n > 0) {
      pl.set_index(std::max(0, pl.index() - 8));
    }
  } else if (key == "pagedown" || key == "pgdown" || key == "ctrl+d") {
    const int n = pl.len();
    if (n > 0) {
      pl.set_index(std::min(n - 1, pl.index() + 8));
    }
  } else if (key == "home" || key == "g") {
    if (pl.len() > 0) {
      pl.set_index(0);
    }
  } else if (key == "end" || key == "G") {
    const int n = pl.len();
    if (n > 0) {
      pl.set_index(n - 1);
    }
  } else if (key == "enter") {
    // Vis-mode enter: play the highlighted (current) track.
    auto [track, idx] = pl.current();
    if (idx >= 0) {
      ctl.play_track(track);
    }
  } else if (key == "shift+up") {
    // Go shift+up: swap the current track with the one above (no wrap).
    const int idx = pl.index();
    if (idx > 0 && pl.move(idx, idx - 1)) {
      pl.set_index(idx - 1);
    }
  } else if (key == "shift+down") {
    const int idx = pl.index();
    if (idx >= 0 && idx < pl.len() - 1 && pl.move(idx, idx + 1)) {
      pl.set_index(idx + 1);
    }
  } else if (key == "f") {
    // Extension over Go: 'f' favorites the current RADIO station, persisted
    // through the radio provider (the browse screen keeps its own 'f').
    // Go's main-context 'f' is a local-playlist bookmark
    // (toggleBookmarkFavorite) that has no persistence in this MVP, so
    // non-realtime tracks are a logged no-op. Only the radio provider marks
    // tracks realtime, so Track::realtime discriminates radio stations from
    // YouTube/local tracks.
    auto [track, idx] = pl.current();
    if (idx < 0) {
      foundation::applog::info("favorite: no current track");
    } else if (!track.realtime) {
      foundation::applog::info(
          "favorite: \"{}\" is not a live radio stream; bookmarks are not "
          "persisted in this MVP",
          track.display_name());
    } else {
      const std::string name =
          track.title.empty() ? track.path : track.title;
      auto result = radio_prov.toggle_favorite_by_url(track.path, track.title);
      if (!result) {
        foundation::applog::user_error("favorite: {}", result.error());
      } else if (*result) {
        foundation::applog::status("★ {} added to favorites", name);
      } else {
        foundation::applog::status("★ {} removed from favorites", name);
      }
    }
  }
  // Everything else is unhandled for the MVP.
}

// ---------------------------------------------------------------------------
// Screen hosting (queue / browse / EQ / help / device) + the mode switch
// ---------------------------------------------------------------------------

// UiMode — the active top-level screen (Go: which overlay is visible). Vis is
// the default frame (visualizer + status line).
enum class UiMode : std::uint8_t {
  Vis,          // visualizer + status line
  Queue,        // queue manager (l / tab)
  Browse,       // radio provider browser (R / b)
  EqOverlay,    // 10-band equalizer (e)
  Help,         // keybinding help (? / h / ctrl+k)
  DevicePicker, // audio device picker (d)
};

// ScreenRefs bundles the screen models + the active mode so the shell's
// KeyCallback and the overlay composite share one reference. All fields are
// owned by run(); every access happens on the FTXUI loop thread (the
// KeyCallback dispatches there and document() renders the overlay there), so
// no locking is needed.
struct ScreenRefs {
  UiMode&                         mode;
  ui::screens::QueueModel&        queue;
  ui::screens::BrowseModel&       browse;
  ui::screens::EqModel&           eq;
  ui::screens::HelpModel&         help;
  ui::screens::DevicePickerModel& device;
  // Inline overlays (Go inline_overlays.go): URL input (u), jump (ctrl+j),
  // track info (i) and the playlist picker (w). They render above every
  // screen while active (see the overlay composite in run()).
  ui::screens::UrlModel&          url;
  ui::screens::JumpModel&         jump;
  ui::screens::InfoModel&         info;
  ui::screens::PlPickerModel&     pl_picker;
  // Local-provider browse ('L'): local_browse is the second BrowseModel
  // (nullptr when the local provider is unavailable — browse_local is then
  // never set); browse_local selects which model the Browse mode drives.
  ui::screens::BrowseModel*       local_browse = nullptr;
  bool&                           browse_local;  // true → local_browse
};

// set_screen switches the active screen (or back to the visualizer) and
// performs the per-screen entry work (Go overlay open()/fetch semantics).
// The shell is told whether a screen is now visible so its document() swaps
// the frame.
void set_screen(ScreenRefs& s, UiMode next, ui::FtxuiAppImpl* shell) {
  s.mode = next;
  if (next == UiMode::Queue) {
    s.queue.open();
  } else if (next == UiMode::Browse) {
    // Go: entering the provider browser fetches the provider playlists
    // (the radio provider for 'R'/'b', the local provider for 'L').
    (void)(s.browse_local && s.local_browse ? s.local_browse : &s.browse)
        ->refresh();
  } else if (next == UiMode::Help) {
    s.help.open();
  } else if (next == UiMode::DevicePicker) {
    // Go keys.go "d": open with cursor/scroll reset; the device list is
    // lazy-loaded when it was never fetched (Go listDevicesCmd).
    s.device.open();
    if (s.device.loading()) {
      (void)s.device.load();
    }
  }
  // EqOverlay needs no entry work. Leaving a screen is the models' business
  // where they own a close key (queue/help close on esc); app_key detects the
  // close via the model's visible() flag and calls back here.
  if (shell) {
    shell->set_screen_visible(next != UiMode::Vis);
  }
}

// active_browse returns the BrowseModel the Browse mode currently drives —
// the radio provider's ('R'/'b') or the local provider's ('L'). Safe when
// local_browse is unavailable (browse_local is never set then).
ui::screens::BrowseModel& active_browse(ScreenRefs& s) {
  if (s.browse_local && s.local_browse) {
    return *s.local_browse;
  }
  return s.browse;
}

// app_key dispatches one canonical key (the shell forwards every claimed key
// here; v / V / q / ctrl+c are app-owned in the shell and never arrive).
// Dispatch order mirrors Go's update loop: the inline overlays first, then the
// active screen's model, then screen-local text input, then screen-mode
// switching, then the global player keys.
//
// Text input: the shell claims all keys, so the screens' FTXUI Inputs are
// display-only (they render the models' live query buffers) and printable
// characters are fed to the browse search prompt / help filter here.
void app_key(std::string_view key, PlaybackController& ctl,
             const config::Config& cfg, playlist::Playlist& pl,
             provider::radio::Provider& radio_prov,
             provider::local::Provider* local_prov, ScreenRefs& s,
             ui::FtxuiAppImpl* shell) {
  // 0. Inline overlays (Go inline_overlays.go) swallow all keys while
  //    active: URL input (u), jump (ctrl+j), track info (i) and the
  //    playlist picker (w). Esc cancels/closes each; the shell's
  //    screen-visible flag follows the active overlay so the overlay
  //    composite renders above the vis frame.
  if (s.url.active()) {
    if (key == "enter") {
      s.url.submit();
      if (shell && !s.url.active()) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    } else if (key == "esc") {
      s.url.cancel();
      if (shell && !s.url.active()) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    } else if (key == "backspace") {
      std::string& q = s.url.query();
      if (!q.empty()) {
        q.pop_back();
      }
    } else if (key == "ctrl+u") {
      // Go editText ctrl+u: clear the whole query line.
      s.url.query().clear();
    } else if (key == "space") {
      s.url.query() += " ";
    } else if (key.size() == 1 && key[0] >= 0x21 && key[0] <= 0x7E) {
      s.url.query() += key;
    }
    return;
  }
  if (s.jump.active()) {
    if (key == "enter") {
      s.jump.submit();
      if (shell && !s.jump.active()) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    } else if (key == "esc") {
      s.jump.cancel();
      if (shell && !s.jump.active()) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    } else if (key == "backspace") {
      std::string& q = s.jump.query();
      if (!q.empty()) {
        q.pop_back();
      }
    } else if (key == "ctrl+u") {
      s.jump.query().clear();
    } else if (key == "space") {
      s.jump.query() += " ";
    } else if (key.size() == 1 && key[0] >= 0x21 && key[0] <= 0x7E) {
      s.jump.query() += key;
    }
    return;
  }
  if (s.info.active()) {
    if (key == "esc") {
      s.info.close();
      if (shell && !s.info.active()) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    } else if (key == "up" || key == "k") {
      s.info.scroll(-1);
    } else if (key == "down" || key == "j") {
      s.info.scroll(1);
    }
    return;
  }
  if (s.pl_picker.active()) {
    if (key == "up" || key == "k") {
      s.pl_picker.move(-1);
    } else if (key == "down" || key == "j") {
      s.pl_picker.move(1);
    } else if (key == "enter") {
      s.pl_picker.submit();
      if (shell && !s.pl_picker.active()) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    } else if (key == "esc") {
      s.pl_picker.cancel();
      if (shell && !s.pl_picker.active()) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    }
    return;
  }

  // 1. The active screen's model consumes its own keys first. Queue and help
  //    can close themselves (esc / A / c / enter / h) — detected via
  //    visible() and mirrored back to the mode switch. The browse screen
  //    reports search-close so the host clears the provider-side search (the
  //    model has no hook for that).
  switch (s.mode) {
    case UiMode::Queue:
      if (s.queue.handle_key(key)) {
        if (!s.queue.visible()) {
          set_screen(s, UiMode::Vis, shell);  // esc / A / c closed it
        }
        return;
      }
      break;
    case UiMode::Browse: {
      ui::screens::BrowseModel& bm = active_browse(s);
      const bool was_searching = bm.search_active();
      if (bm.handle_key(key)) {
        if (was_searching && !bm.search_active()) {
          // Search screen closed (esc, or an empty catalog submit): clear the
          // provider-side search and restore the full catalog list. A
          // successful catalog search (enter) also closes the prompt, but the
          // provider keeps returning the search results — keep them (Go
          // handleCatalogSearchKey: only esc/empty-enter call restoreCatalog).
          if (!bm.search_results_kept()) {
            if (!s.browse_local) {
              radio_prov.clear_search();  // radio-only; local has no search
            }
            (void)bm.refresh();
          }
        }
        return;
      }
      break;
    }
    case UiMode::EqOverlay:
      if (s.eq.handle_key(key)) {
        return;
      }
      break;
    case UiMode::Help:
      if (s.help.handle_key(key)) {
        if (!s.help.visible()) {
          set_screen(s, UiMode::Vis, shell);  // enter / l / esc / h closed it
        }
        return;
      }
      break;
    case UiMode::DevicePicker:
      if (s.device.handle_key(key)) {
        if (!s.device.visible()) {
          set_screen(s, UiMode::Vis, shell);  // esc / d / enter closed it
        }
        return;
      }
      break;
    case UiMode::Vis:
      break;
  }

  // 2. Text input while typing in the browse search prompt / help filter.
  if (s.mode == UiMode::Browse) {
    ui::screens::BrowseModel& bm = active_browse(s);
    if (bm.search_active()) {
      if (key == "space") {
        bm.set_search_query(bm.search_query() + " ");
        return;
      }
      if (key == "backspace") {
        std::string q = bm.search_query();
        if (!q.empty()) {
          q.pop_back();
          bm.set_search_query(std::move(q));
        }
        return;
      }
      if (key == "ctrl+u") {
        // Go editText ctrl+u: clear the whole query line.
        bm.set_search_query("");
        return;
      }
      if (key.size() == 1 && key[0] >= 0x21 && key[0] <= 0x7E) {
        bm.set_search_query(bm.search_query() + key);
        return;
      }
    }
  }
  if (s.mode == UiMode::Help && s.help.filtering()) {
    if (key == "space") {
      s.help.set_filter(s.help.filter() + " ");
      return;
    }
    if (key.size() == 1 && key[0] >= 0x21 && key[0] <= 0x7E) {
      s.help.set_filter(s.help.filter() + key);
      return;
    }
  }

  // 3. Screen-mode switching. The key that opened a screen closes it again;
  //    esc always returns to the visualizer. q stays global (the shell owns
  //    it in-process). R opens the radio provider browser (Go keys.go
  //    switchToProvider("radio")); b is the bootamp shorthand; A opens the
  //    queue manager (Go keys.go "A"); L / Y / ctrl+f switch into the browse
  //    screen over the local provider / a YouTube net search (Go keys.go
  //    switchToProvider("local") / handleNetSearchKey).
  UiMode next = UiMode::Vis;
  bool   switching = true;
  if (key == "l" || key == "tab" || key == "A") {
    next = s.mode == UiMode::Queue ? UiMode::Vis : UiMode::Queue;
  } else if (key == "b" || key == "R") {
    s.browse_local = false;
    next = s.mode == UiMode::Browse ? UiMode::Vis : UiMode::Browse;
  } else if (key == "L") {
    // Go 'L': browse the local provider's playlists.
    if (!s.local_browse) {
      foundation::applog::user_error("local browse: provider unavailable");
      return;
    }
    const bool closing = s.mode == UiMode::Browse && s.browse_local;
    s.browse_local = true;
    next = closing ? UiMode::Vis : UiMode::Browse;
  } else if (key == "Y" || key == "ctrl+f") {
    // Go 'Y' / ctrl+f (outside the browse screen — in-browse ctrl+f is the
    // browse model's own): open the browse screen and start a YouTube net
    // search.
    s.browse_local = false;
    set_screen(s, UiMode::Browse, shell);
    active_browse(s).start_search(
        ui::screens::BrowseModel::SearchMode::YouTube);
    return;
  } else if (key == "u") {
    // Go 'u': open the URL input overlay.
    s.url.open();
    if (shell) {
      shell->set_screen_visible(true);
    }
    return;
  } else if (key == "i") {
    // Go 'i': track info overlay (toggles closed).
    if (s.info.active()) {
      s.info.close();
      if (shell) {
        shell->set_screen_visible(s.mode != UiMode::Vis);
      }
    } else {
      auto [track, idx] = pl.current();
      if (idx >= 0) {
        s.info.open(track);
        if (shell) {
          shell->set_screen_visible(true);
        }
      }
    }
    return;
  } else if (key == "w") {
    // Go 'w': open the playlist picker over the local provider's playlists;
    // on_pick (wired in run()) saves the current track into the choice.
    if (!local_prov) {
      foundation::applog::user_error(
          "save to playlist: local provider unavailable");
      return;
    }
    auto lists = local_prov->playlists();
    if (!lists) {
      foundation::applog::user_error("save to playlist: {}", lists.error());
      return;
    }
    std::vector<std::string> names;
    names.reserve(lists->size());
    for (const playlist::PlaylistInfo& li : *lists) {
      names.push_back(li.name);
    }
    s.pl_picker.open(std::move(names));
    if (shell) {
      shell->set_screen_visible(true);
    }
    return;
  } else if (key == "ctrl+j") {
    // Go ctrl+j opens the jump-to-time overlay (keys.go openJumpMode); kept
    // 1:1 with the Go binding name. UNREACHABLE: FTXUI 7.0.3 normalizes CR→LF
    // at the input parser and Event::Return == Event::CtrlJ (both byte 0x0A),
    // so the Ctrl+J keypress is indistinguishable from Enter and arrives here
    // as "enter" — which plays the cursor track in the vis view (handle_key).
    // The overlay's open/submit/esc machinery stays wired so a future input
    // path that can distinguish the two can re-open it with this exact key.
    s.jump.open();
    if (shell) {
      shell->set_screen_visible(true);
    }
    return;
  } else if (key == "ctrl+h") {
    // Go ctrl+h: toggle album headers in the playlist panel (shell-side).
    if (shell) {
      shell->set_album_headers(!shell->album_headers());
    }
    return;
  } else if (key == "e") {
    next = s.mode == UiMode::EqOverlay ? UiMode::Vis : UiMode::EqOverlay;
  } else if (key == "?" || key == "h" || key == "ctrl+k") {
    next = s.mode == UiMode::Help ? UiMode::Vis : UiMode::Help;
  } else if (key == "d") {
    // Go keys.go "d": open the audio device picker (close when already
    // open — the picker itself also closes on d/esc/enter via step 1).
    next = s.mode == UiMode::DevicePicker ? UiMode::Vis : UiMode::DevicePicker;
  } else if (key == "esc") {
    next = UiMode::Vis;
  } else {
    switching = false;
  }
  if (switching) {
    set_screen(s, next, shell);
    return;
  }

  // 4. Global player keys (space/arrows/n/p/r/z/s/f/... — the Go main-context
  //    table). The screen models consumed their own keys in step 1, so these
  //    work unchanged in every mode (Go: screens fall through to the global
  //    table).
  handle_key(key, ctl, cfg, pl, radio_prov);
}

// Headless mode: SIGINT (and only SIGINT) ends the wait loop.
std::atomic<bool> g_interrupted{false};
void on_sigint(int) { g_interrupted.store(true); }

// ---------------------------------------------------------------------------
// run() — the wiring (port of cliamp main.go run())
// ---------------------------------------------------------------------------

int run(config::Overrides overrides, std::vector<std::string> positional);

}  // namespace

}  // namespace bootamp::app

namespace bootamp::app {
int run_main(int argc, char** argv);
}  // namespace bootamp::app

int main(int argc, char** argv) { return bootamp::app::run_main(argc, argv); }

namespace bootamp::app {

int run_main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  auto cli = parse_cli(args);
  if (!cli) {
    std::fprintf(stderr, "bootamp: %s\n\n%s", cli.error().c_str(),
                 cli_usage().c_str());
    return 1;
  }
  if (cli->show_version) {
    std::puts(cli_version().c_str());
    return 0;
  }
  if (cli->show_help) {
    std::fputs(cli_usage().c_str(), stdout);
    return 0;
  }
  return run(std::move(cli->overrides), std::move(cli->positional));
}

namespace {

int run(config::Overrides overrides, std::vector<std::string> positional) {
  // MVP convenience for the plan's examples: cliamp's `play` subcommand is
  // folded into the flat CLI — a leading "play" argument is dropped when
  // followed by at least one more argument (a sole "play" stays a path).
  if (positional.size() >= 2 && positional[0] == "play") {
    positional.erase(positional.begin());
  }

  // 1. Config load + CLI overrides (cliamp main.go: config.Load -> Apply).
  auto cfg = config::load();
  if (!cfg) {
    std::fprintf(stderr, "bootamp: config: %s\n", cfg.error().c_str());
    return 1;
  }
  config::Config& cfg_ref = *cfg;
  config::apply_overrides(cfg_ref, overrides);

  // 2. Logging init (cliamp initLogging): failures continue without a file
  //    log.
  std::function<void()> close_log = [] {};
  if (auto dir = foundation::config_dir(); dir) {
    auto lvl = foundation::applog::parse_level(cfg_ref.log_level);
    if (!lvl) {
      std::fprintf(stderr, "bootamp: logging: %s (continuing without file "
                           "log)\n",
                   lvl.error().c_str());
    } else {
      auto log = foundation::applog::init(*dir / "bootamp.log", *lvl);
      if (!log) {
        std::fprintf(stderr, "bootamp: logging: %s (continuing without file "
                             "log)\n",
                     log.error().c_str());
      } else {
        close_log = std::move(*log);
      }
    }
  }
  foundation::applog::info("bootamp starting (version={})", kBootampVersion);

  // 3. Search shorthand (cliamp main.go): `search <query>` /
  //    `search-sc <query>` become ytsearch1:/scsearch1: prefixed URLs.
  if (!positional.empty() &&
      (positional[0] == "search" || positional[0] == "search-sc")) {
    if (positional.size() == 1) {
      std::fprintf(stderr, "bootamp: search requires a query string\n");
      return 1;
    }
    const std::string prefix =
        positional[0] == "search-sc" ? "scsearch1:" : "ytsearch1:";
    std::string query = positional[1];
    for (std::size_t i = 2; i < positional.size(); ++i) {
      query += " " + positional[i];
    }
    positional = {prefix + query};
  }

  // 4. yt-dlp settings from config (cliamp main.go: ExpandYTPlaylist +
  //    SetYTDLCookiesFrom).
  if (cfg_ref.ytmusic.expand_playlist) {
    resolve::set_expand_yt_playlist(*cfg_ref.ytmusic.expand_playlist);
  }
  if (!cfg_ref.ytmusic.cookies_from.empty()) {
    resolve::set_ytdl_cookies_from(cfg_ref.ytmusic.cookies_from);
  }

  // 5. Providers (cliamp main.go): radio always; local when the config dir
  //    resolves. radio_prov is kept alive for the lifetime of the app — it
  //    backs the browse screen (playlists/catalog/search/favorites) and the
  //    default radio list.
  auto radio_prov = std::make_shared<provider::radio::Provider>();
  auto local_prov = provider::local::Provider::new_provider();

  // 6. Playlist: config playlist, default radio, or the resolved CLI args
  //    (cliamp main.go:248-272).
  playlist::Playlist pl;
  const bool default_radio =
      positional.empty() &&
      (cfg_ref.provider.empty() || cfg_ref.provider == "radio");
  if (!cfg_ref.playlist.empty()) {
    if (!local_prov) {
      std::fprintf(stderr, "bootamp: local provider unavailable\n");
      return 1;
    }
    auto tracks = local_prov->tracks(cfg_ref.playlist);
    if (!tracks) {
      std::fprintf(stderr, "bootamp: playlist \"%s\": %s\n",
                   cfg_ref.playlist.c_str(), tracks.error().c_str());
      return 1;
    }
    pl.add(*tracks);
  } else if (default_radio) {
    // cliamp main.go:258-270 — the default radio station list.
    pl.add({
        playlist::Track{.path = "http://radio.cliamp.stream/lofi/stream",
                        .title = "Lofi Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/synthwave/stream",
                        .title = "Synthwave Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/edm/stream",
                        .title = "EDM Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/ncs/stream",
                        .title = "NCS Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/ncs-house/stream",
                        .title = "NCS House Stream", .stream = true,
                        .realtime = true},
        playlist::Track{
            .path = "http://radio.cliamp.stream/ncs-dubstep/stream",
            .title = "NCS Dubstep Stream", .stream = true, .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/ncs-dnb/stream",
                        .title = "NCS Drum & Bass Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/ncs-trap/stream",
                        .title = "NCS Trap Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/ncs-phonk/stream",
                        .title = "NCS Phonk Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/ncs-pop/stream",
                        .title = "NCS Pop Stream", .stream = true,
                        .realtime = true},
        playlist::Track{.path = "http://radio.cliamp.stream/ncs-chill/stream",
                        .title = "NCS Chill Stream", .stream = true,
                        .realtime = true},
    });
  }

  // 7. Classify the CLI args (local file / dir / m3u / pls / URL / yt-dlp)
  //    via resolve::args (cliamp resolve.Args).
  auto resolved = resolve::args(positional);
  if (!resolved) {
    std::fprintf(stderr, "bootamp: %s\n", resolved.error().c_str());
    return 1;
  }
  if (!resolved->pending.empty()) {
    std::fprintf(stderr, "bootamp: resolving %zu remote URL(s)...\n",
                 resolved->pending.size());
    auto remote = resolve_pending(resolved->pending);
    if (!remote) {
      std::fprintf(stderr, "bootamp: resolve remote: %s\n",
                   remote.error().c_str());
      return 1;
    }
    pl.add(*remote);
  }
  pl.add(resolved->tracks);

  // 8. Audio sink + engine (cliamp: player.New; sink = miniaudio when the
  //    header is present, else NullSink with a printed warning). The sample
  //    rate auto-detects from the device when cfg.sample_rate is 0.
  audio::EngineConfig ecfg;
  ecfg.sample_rate = cfg_ref.sample_rate;
  ecfg.bit_depth = cfg_ref.bit_depth;
  ecfg.buffer_ms = cfg_ref.buffer_ms;
  ecfg.resample_quality = cfg_ref.resample_quality;
  ecfg.volume_min_db = cfg_ref.volume_min;
  ecfg.audio_device = cfg_ref.audio_device;  // empty = system default

  std::shared_ptr<audio::AudioSink> sink;
#if BOOTAMP_APP_HAS_MINIAUDIO
  if (ecfg.sample_rate == 0) {
    auto detected = audio::miniaudio_default_sample_rate(44100);
    ecfg.sample_rate = detected ? *detected : 44100;
  }
  sink = audio::make_miniaudio_sink();
#else
  std::fprintf(stderr,
               "bootamp: miniaudio not available in this build — audio "
               "output is disabled (NullSink). Install the miniaudio package "
               "and rebuild.\n");
  if (ecfg.sample_rate == 0) {
    ecfg.sample_rate = 44100;
  }
  sink = audio::make_null_sink();
#endif

  audio::AudioEngine engine(sink, ecfg);

  // 9. Apply the config to the engine + playlist (cliamp cfg.ApplyPlayer /
  //    ApplyPlaylist), and the frame padding (cliamp ui.SetPadding).
  EnginePlayerConfig engine_cfg(engine);
  config::apply_player(cfg_ref, engine_cfg);
  PlaylistPlayerConfig playlist_cfg(pl);
  config::apply_playlist(cfg_ref, playlist_cfg);
  ui::set_padding(cfg_ref.padding_h, cfg_ref.padding_v);

  // 10. Visualizer in the configured mode (cliamp ui.NewVisualizer +
  //     SetMode from cfg.Visualizer).
  ui::Visualizer vis(static_cast<double>(engine.sample_rate()));
  if (!cfg_ref.visualizer.empty()) {
    auto [mode, ok] = ui::string_to_vis_mode_exact(cfg_ref.visualizer);
    if (ok) {
      vis.set_mode(mode);
    } else {
      std::fprintf(stderr, "bootamp: unknown visualizer \"%s\" (using "
                           "default)\n",
                   cfg_ref.visualizer.c_str());
    }
  }

  // 11. Playback controller, tick-context state, resume (cliamp main.go:
  //     448-452 — only when not the default radio and CLI args were given).
  PlaybackController ctl(engine, pl);
  VisContextState vis_state(static_cast<double>(engine.sample_rate()), engine,
                            cfg_ref.vis_volume_linked);
  if (!default_radio && !positional.empty()) {
    auto rs = foundation::resume_load();
    if (rs && !rs->path.empty() && rs->position_sec > 0) {
      ctl.resume = *rs;
    }
  }

  // 12. Screen models (queue/browse/EQ/help) + the FTXUI shell. The screens
  //     are plain-C++ models (cliamp ui/model overlay state) driven from the
  //     shell's KeyCallback; their FTXUI components are composed by the
  //     overlay installed below (the shell swaps the vis frame for the active
  //     screen's render).
  UiMode ui_mode = UiMode::Vis;

  ui::screens::QueueModel queue_model(
      pl, ui::screens::QueueActions{
              // enter — play the queued entry (Go daemon queue.play twin:
              // SetIndex + play current).
              .on_play = [&ctl, &pl](int track_index) {
                pl.set_index(track_index);
                auto [track, idx] = pl.current();
                if (idx >= 0) {
                  ctl.play_track(track);
                }
              },
              // d — remove the queue entry at pos (Go RemoveQueueAt).
              .on_remove_at = [&pl](int pos) { pl.remove_queue_at(pos); },
              // c — clear the whole queue (Go ClearQueue; the model closes).
              .on_clear = [&pl] { pl.clear_queue(); },
              // s / r — via the controller so the gapless preload is also
              // re-armed (identical to the global z/r keys).
              .on_toggle_shuffle = [&ctl] { ctl.toggle_shuffle(); },
              .on_cycle_repeat = [&ctl] { ctl.cycle_repeat(); },
              // f — favorite the selected queue entry's radio station by its
              // resolved stream URL (same path as the main-screen 'f'; non-
              // realtime tracks are a logged no-op there, mirrored here).
              .on_toggle_favorite = [radio_prov, &pl](int pos) {
                const std::vector<playlist::QueueEntry> entries =
                    pl.queue_entries();
                if (pos < 0 ||
                    static_cast<std::size_t>(pos) >= entries.size()) {
                  foundation::applog::info("favorite: no queue entry at {}",
                                           pos);
                  return;
                }
                const playlist::Track& t =
                    entries[static_cast<std::size_t>(pos)].track;
                if (!t.realtime) {
                  foundation::applog::info(
                      "favorite: \"{}\" is not a live radio stream; "
                      "bookmarks are not persisted in this MVP",
                      t.display_name());
                  return;
                }
                const std::string name =
                    t.title.empty() ? t.path : t.title;
                auto result =
                    radio_prov->toggle_favorite_by_url(t.path, t.title);
                if (!result) {
                  foundation::applog::user_error("favorite: {}",
                                                 result.error());
                } else if (*result) {
                  foundation::applog::status("★ {} added to favorites", name);
                } else {
                  foundation::applog::status("★ {} removed from favorites",
                                             name);
                }
              },
          });

  ui::screens::BrowseModel browse_model =
      ui::screens::BrowseModel::for_provider(*radio_prov);
  ui::screens::BrowseActions browse_actions;
  browse_actions.on_select = [&ctl, &pl, radio_prov](std::string_view id) {
    // Radio playlist id -> provider tracks (net-search results are handled
    // by on_play_track/on_append_track below, not here). The
    // whole body runs on the play worker (Go fetchTracksCmd runs in a tea.Cmd
    // goroutine): provider tracks (file-backed, fast) + wrapper-URL PLS/M3U
    // expansion (HTTP fetch, 30s timeout) + the play start — never on the
    // UI thread. The provider and the playlist are internally locked, so the
    // worker can touch them safely.
    ctl.enqueue_play_job([&ctl, &pl, radio_prov, id = std::string(id)]() {
      std::vector<playlist::Track> tracks;
      auto got = radio_prov->tracks(id);
      if (got) {
        tracks = std::move(*got);
      } else if (playlist::is_url(id) || playlist::is_ytdl(id)) {
        tracks.push_back(playlist::track_from_path(id));
      } else {
        foundation::applog::user_error("browse: {}", got.error());
        return;
      }
      // Go fetchTracksCmd: resolve PLS/M3U wrapper URLs to the actual stream
      // tracks, then put the stations into the playlist and play the first
      // one (next/prev cycle through the rest). run_play directly (not
      // play_track) — buffering is already raised by enqueue_play_job and a
      // nested enqueue would double-enqueue.
      auto expansion = resolve::expand_wrapper_urls(tracks);
      const int first = pl.len();
      pl.add(expansion->tracks);
      if (first < pl.len()) {
        pl.set_index(first);
        auto [track, idx] = pl.current();
        if (idx >= 0) {
          ctl.run_play(track);
        }
      }
    });
  };
  browse_actions.on_play_track = [&ctl, &pl](const playlist::Track& t) {
    // Net-search result enter (Go handleNetSearchResultsKey enter:
    // playTrackImmediate): append the resolved track and play it. The play
    // start itself is async on the play worker (play_track).
    const int idx = pl.len();
    pl.add({t});
    pl.set_index(idx);
    ctl.play_track(t);
  };
  browse_actions.on_append_track = [&pl](const playlist::Track& t) {
    // Net-search result 'a' (Go appendTrack): append only.
    pl.add({t});
  };
  browse_actions.on_favorite = [&radio_prov,
                                &browse_model](std::string_view id) {
    auto result = radio_prov->toggle_favorite(id);
    if (!result) {
      foundation::applog::user_error("favorite: {}", result.error());
    }
    (void)browse_model.refresh();  // Go: re-fetch the lists after the toggle
  };
  browse_actions.on_search_submitted = [](std::string_view query) {
    foundation::applog::info("radio search: {}", query);
  };
  browse_model.set_actions(std::move(browse_actions));

  ui::screens::EqModel eq_model(
      [&engine] { return engine.eq_bands(); },
      [&engine](int band, double db) { engine.set_eq_band(band, db); });
  ui::screens::HelpModel help_model;

  // Audio device picker (Go: devicePickerState + listDevicesCmd). The model
  // is plain-C++; the host wires the engine's device API: enumeration via
  // the injected LoadFn (lazy — only when the list was never fetched) and
  // the switch via on_switch. After a switch the picker re-marks the active
  // device from engine.current_device() (the engine keeps the previous
  // device, or halts playback, if the switch fails — the error is logged).
  ui::screens::DevicePickerModel device_model;
  ui::screens::DeviceActions device_actions;
  device_actions.on_switch = [&engine, &device_model](std::string_view name) {
    auto res = engine.switch_device(name);
    if (!res) {
      foundation::applog::user_error("audio device: {}", res.error());
    }
    device_model.set_current_device(engine.current_device());
  };
  device_model.set_actions(std::move(device_actions));
  device_model.set_load([&engine, &device_model]()
                            -> std::expected<std::vector<std::string>,
                                             std::string> {
    std::vector<std::string> devs = engine.list_devices();
    device_model.set_current_device(engine.current_device());
    return devs;
  });

  // Local-provider browse ('L'): a second BrowseModel over the local
  // provider (the radio one stays the 'b'/'R' target). Constructed only
  // when the provider is available; 'L' reports an error otherwise.
  std::unique_ptr<ui::screens::BrowseModel> local_browse_model;
  if (local_prov) {
    local_browse_model.reset(new ui::screens::BrowseModel(
        ui::screens::BrowseModel::for_provider(*local_prov)));
  }

  // Inline overlays (Go ui/model/inline_overlays.go): URL input ('u'),
  // jump-to-time (ctrl+j), track info ('i') and the playlist picker ('w').
  // Plain-C++ models like the other screens; the host wires the actions to
  // the controller / providers here.
  ui::screens::UrlModel url_model;
  ui::screens::UrlActions url_actions;
  url_actions.on_submit = [&ctl, &pl](std::string_view query) {
    // Go loadURL: resolve the input like a CLI arg (yt-dlp / m3u / pls /
    // feed), expand PLS/M3U wrapper URLs, then queue the tracks and play
    // the first. The blocking resolve runs on the play worker (same as the
    // browse Enter path) so the UI thread stays live.
    ctl.enqueue_play_job([&ctl, &pl, q = std::string(query)]() {
      auto got = resolve_pending({q});
      if (!got) {
        foundation::applog::user_error("load URL: {}", got.error());
        return;
      }
      auto expansion = resolve::expand_wrapper_urls(*got);
      const int first = pl.len();
      pl.add(expansion->tracks);
      if (first < pl.len()) {
        pl.set_index(first);
        auto [track, idx] = pl.current();
        if (idx >= 0) {
          ctl.run_play(track);  // buffering already raised by the job
        }
      }
    });
  };
  url_actions.on_cancel = [] {};
  url_model.set_actions(std::move(url_actions));

  ui::screens::JumpModel jump_model;
  ui::screens::JumpActions jump_actions;
  jump_actions.on_jump = [&ctl](double secs) {
    // Go jumpTo: absolute reposition through the controller's seek path
    // (the yt-dlp pipeline-rebuild variant included).
    ctl.seek_to_abs(secs);
  };
  jump_actions.on_cancel = [] {};
  jump_model.set_actions(std::move(jump_actions));

  ui::screens::InfoModel info_model;

  ui::screens::PlPickerModel pl_picker_model;
  ui::screens::PlPickerActions pl_picker_actions;
  pl_picker_actions.on_pick = [local_prov = local_prov.get(), &pl](
                                  std::string_view name) {
    // Go saveToPlaylist: append the current track to the chosen local
    // playlist (PlaylistInfo::name == playlist file name for local).
    auto [track, idx] = pl.current();
    if (idx < 0) {
      foundation::applog::info("save to playlist: no current track");
      return;
    }
    auto result = local_prov->add_track_to_playlist(name, track);
    if (!result) {
      foundation::applog::user_error("save to playlist: {}", result.error());
    } else {
      foundation::applog::status("{} → \"{}\"", track.display_name(), name);
    }
  };
  pl_picker_actions.on_cancel = [] {};
  pl_picker_model.set_actions(std::move(pl_picker_actions));

  bool browse_local = false;
  ScreenRefs screen_refs{ui_mode,      queue_model, browse_model,
                         eq_model,     help_model,  device_model,
                         url_model,    jump_model,  info_model,
                         pl_picker_model, local_browse_model.get(),
                         browse_local};

  auto status = [&engine, &pl, &vis, &ctl]() {
    return status_line(engine, pl, vis, ctl.buffering());
  };
  ui::FtxuiAppImpl* app_impl = nullptr;
  auto on_key = [&ctl, &cfg_ref, &pl, &radio_prov,
                 local_prov = local_prov.get(), &screen_refs, &app_impl](
                    std::string_view key) {
    app_key(key, ctl, cfg_ref, pl, *radio_prov, local_prov, screen_refs,
            app_impl);
  };
  std::unique_ptr<ui::FtxuiApp> app =
      ui::make_ftxui_app(vis, std::move(on_key), std::move(status));
  app_impl = dynamic_cast<ui::FtxuiAppImpl*>(app.get());

  // Playlist panel feed: the shell's Vis frame renders the station list (Go
  // renderPlaylist — visible from startup) from this provider. Revision-keyed:
  // the shell asks each repaint with the revision it last rendered; an
  // unchanged playlist answers nullopt (one atomic load) instead of
  // rebuilding the titles, so the per-frame path copies strings only when
  // the playlist actually changed (n/p moves the highlight too — index
  // changes bump the revision).
  if (app_impl) {
    app_impl->set_playlist_provider(
        [&pl](std::uint64_t seen_revision)
            -> std::optional<ui::FtxuiAppImpl::PlaylistSnapshot> {
          if (pl.revision() == seen_revision) {
            return std::nullopt;
          }
          ui::FtxuiAppImpl::PlaylistSnapshot snap;
          snap.revision = pl.revision();
          snap.current_index = pl.index();
          for (const playlist::Track& t : pl.tracks()) {
            snap.titles.push_back(t.display_name());
            snap.albums.push_back(t.album);  // album-run headers (ctrl+h)
          }
          return snap;
        });
  }

#if BOOTAMP_HAS_FTXUI
  if (app_impl) {
    // The screens composite: renders the active screen's component (the
    // shell calls Render() from its document() on the loop thread) and an
    // empty element while no screen is open. Components are built once so
    // their internal state (menu selection, search input buffer) persists
    // across mode switches.
    auto queue_comp = ui::screens::make_queue_component(queue_model);
    auto browse_comp = ui::screens::make_browse_component(browse_model);
    auto eq_comp = ui::screens::make_eq_component(eq_model);
    auto help_comp = ui::screens::make_help_component(help_model);
    auto device_comp = ui::screens::make_device_component(device_model);
    auto url_comp = ui::screens::make_url_component(url_model);
    auto jump_comp = ui::screens::make_jump_component(jump_model);
    auto info_comp = ui::screens::make_info_component(info_model);
    auto pl_picker_comp = ui::screens::make_pl_picker_component(pl_picker_model);
    auto screens_overlay = ftxui::Renderer(
        [&ui_mode, &url_model, &jump_model, &info_model, &pl_picker_model,
         queue_comp, browse_comp, eq_comp, help_comp, device_comp, url_comp,
         jump_comp, info_comp, pl_picker_comp]() -> ftxui::Element {
          // Inline overlays (url/jump/info/playlist picker) render above
          // everything while active (Go inline_overlays.go).
          if (url_model.active()) {
            return url_comp->Render();
          }
          if (jump_model.active()) {
            return jump_comp->Render();
          }
          if (info_model.active()) {
            return info_comp->Render();
          }
          if (pl_picker_model.active()) {
            return pl_picker_comp->Render();
          }
          switch (ui_mode) {
            case UiMode::Queue:
              return queue_comp->Render();
            case UiMode::Browse:
              return browse_comp->Render();
            case UiMode::EqOverlay:
              return eq_comp->Render();
            case UiMode::Help:
              return help_comp->Render();
            case UiMode::DevicePicker:
              return device_comp->Render();
            case UiMode::Vis:
              break;
          }
          return ftxui::emptyElement();
        });
    app_impl->set_overlay_component(screens_overlay);
    // Keep the screens' scroll windows sized to the terminal (invoked from
    // document() on the loop thread each repaint, so resizes are live).
    app_impl->set_resize_hook([&queue_model, &browse_model, &help_model,
                               &device_model](int /*cols*/, int rows) {
      const int avail = std::max(rows - 2, 0);  // status + help lines
      queue_model.set_visible_rows(avail);
      browse_model.set_visible_rows(avail);
      help_model.set_visible_rows(avail);
      device_model.set_visible_rows(avail);
    });
  }
#endif  // BOOTAMP_HAS_FTXUI

  // 13. Start playback (cliamp ui/model/init.go autoPlayMsg). The TUI starts
  //     when cfg.auto_play is set; the MVP additionally starts immediately
  //     when CLI args were given (the plan's `bootamp play <file>` examples)
  //     and always in headless mode.
  const bool play_now =
      pl.len() > 0 && (cfg_ref.auto_play || !positional.empty() || !app);
  if (play_now) {
    auto [track, idx] = pl.current();
    if (idx >= 0) {
      ctl.play_track(track);
    }
  }

  // 14. Watchdog jthread (200ms): gapless advance / preload / end of
  //     playback (step 11 of the design — the engine never stops itself).
  std::jthread watchdog([&ctl](std::stop_token stoken) {
    while (!stoken.stop_requested()) {
      ctl.watchdog_step();
      std::this_thread::sleep_for(ui::kTickSlow);
    }
  });

  // 15. VisTickContext refresher: TickLoop::set_context copies the context
  //     as-is and never refreshes ctx.now, so a kTickAnalyze (50ms) refresher
  //     thread keeps the analysis gate and the smoothing dt honest.
  std::jthread ctx_refresher;
  if (app_impl) {
    ui::VisTickContext ctx =
        build_tick_context(engine, vis_state, cfg_ref.vis_volume_linked);
    ctx.focused = app_impl->focused();
    app_impl->set_tick_context(std::move(ctx));
    ctx_refresher =
        std::jthread([app_impl, &engine, &vis_state,
                      &cfg_ref](std::stop_token stoken) {
          while (!stoken.stop_requested()) {
            ui::VisTickContext ctx =
                build_tick_context(engine, vis_state,
                                   cfg_ref.vis_volume_linked);
            ctx.focused = app_impl->focused();
            app_impl->set_tick_context(std::move(ctx));
            // Analysis freshness is bounded by the 50ms analysis gate
            // (kTickAnalyze), so a 50ms refresh cadence keeps ctx.now honest
            // at half the rebuild rate of the old 16ms loop; it also carries
            // the current focus state so a focus change reaches the tick
            // cadence within one refresh.
            std::this_thread::sleep_for(ui::kTickAnalyze);
          }
        });
  }

  // 16. Run the UI (or the headless wait loop).
  if (app) {
    app->run();
  } else {
    std::fprintf(stderr,
                 "bootamp: FTXUI not available in this build — running "
                 "headless (Ctrl+C to quit)\n");
    std::signal(SIGINT, on_sigint);
    while (!g_interrupted.load() && engine.is_playing()) {
      std::this_thread::sleep_for(ui::kTickSlow);
    }
  }

  // 17. Clean shutdown: stop the UI threads, then the engine, then save the
  //     resume state (cliamp: on exit, fm.ResumeState -> resume.Save).
  ctx_refresher.request_stop();
  if (ctx_refresher.joinable()) {
    ctx_refresher.join();
  }
  watchdog.request_stop();
  if (watchdog.joinable()) {
    watchdog.join();
  }
  // Join the play worker before the engine stops: a pending request is
  // dropped; an in-flight play start runs to completion first (bounded by
  // the pipeline timeouts: 30s connect/TLS, 15s ffmpeg initial-audio wait).
  ctl.stop_play_worker();
  engine.stop();
  if (!default_radio) {
    auto [track, idx] = pl.current();
    if (idx >= 0 && !track.is_live()) {
      const auto pd = engine.position_and_duration_secs();
      if (pd.first > 0) {
        (void)foundation::resume_save(foundation::ResumeState{
            track.path, static_cast<int>(pd.first), cfg_ref.playlist});
      }
    }
  }
  foundation::applog::info("bootamp stopped");
  close_log();
  return 0;
}

}  // namespace

}  // namespace bootamp::app
