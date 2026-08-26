// audio/engine.hpp — AudioEngine: the in-process playback engine.
//
// Port of cliamp/player/engine.go (Engine iface) + player.go (Player). Single
// foreground process, NO daemon/IPC. The engine runs an audio jthread that owns
// the DSP chain; the UI drives it via atomics (no locks on the hot path).
// Controls: atomic<double> volume/speed, array<atomic<double>,10> eq,
// atomic<bool> playing/paused/mono, atomic<int64_t> seek_gen,
// atomic<shared_ptr<const string>> stream_title. Lifecycle: Play/Preload/Seek/
// Stop/Position. std::jthread joined in dtor (no fire-and-forget close). The
// AudioSink is injected (MiniaudioSink / NullSink).
#pragma once

#include "audio/audio_sink.hpp"
#include "audio/gapless.hpp"
#include "audio/pipeline.hpp"
#include "audio/tap.hpp"
#include "dsp/biquad.hpp"
#include "dsp/resampler.hpp"
#include "foundation/playback.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bootamp::audio {

// The gapless → EQ → WSOLA chain adapter (audio-thread owned). Defined in
// engine.cpp; the header only needs the incomplete type for the unique_ptr.
class ChainStreamer;

// EngineConfig holds the immutable engine parameters set at construction.
struct EngineConfig {
  int sample_rate       = 0;    // 0 = auto-detect from device
  int bit_depth         = 16;
  int buffer_ms         = 250;
  int resample_quality  = 4;
  double volume_min_db  = -50.0;
  // audio_device names the output device to open on ("" = system default).
  // Kept as the last member: it is a preference, applied after open() — if
  // the named device fails, the engine falls back to the default.
  std::string audio_device;
};

// AudioEngine is the playback engine. One instance per app lifetime. All
// public methods are safe to call from the UI thread; the audio jthread reads
// the atomics and never takes a lock.
class AudioEngine {
public:
  AudioEngine(std::shared_ptr<AudioSink> sink, EngineConfig cfg,
              foundation::Notifier* notifier = nullptr);
  ~AudioEngine();

  AudioEngine(const AudioEngine&)            = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // --- Playback control (Engine iface) -------------------------------------
  // play opens `path`, builds the pipeline, and starts playback. known_duration
  // secs is the metadata hint (0 = unknown). Returns an error string on failure.
  std::string play(const std::string& path, double known_duration_secs = 0.0);
  // play_ytdl plays a yt-dlp URL (YT/YTM/SC/Bilibili/Bandcamp).
  std::string play_ytdl(const std::string& page_url, double known_duration_secs = 0.0);
  // preload queues the next track for gapless transition.
  std::string preload(const std::string& path, double known_duration_secs = 0.0);
  std::string preload_ytdl(const std::string& page_url, double known_duration_secs = 0.0);
  void        clear_preload();
  void        stop();
  void        close();
  void        toggle_pause();

  // --- Seeking -------------------------------------------------------------
  // seek repositions by `offset_secs` (signed, relative). For yt-dlp this is
  // seek-by-restart (generation-cancelled): build new pipeline off the audio
  // lock, swap under it, close old async.
  std::string seek(double offset_secs);
  std::string seek_ytdl(double offset_secs);
  void        cancel_seek_ytdl();

  // --- State queries ------------------------------------------------------
  bool is_playing() const      { return playing_.load(); }
  bool is_paused() const        { return paused_.load(); }
  bool drained() const          { return gapless_.drained(); }
  bool has_preload() const      { return gapless_.has_next(); }
  bool seekable() const;
  bool is_stream_seek() const;
  bool is_ytdl_seek() const     { return ytdl_seek_.load(); }
  bool gapless_advanced() const;  // consume-once (Go GaplessAdvanced CAS)

  double position_secs() const;
  double duration_secs() const;
  std::pair<double, double> position_and_duration_secs() const;

  // --- Volume / speed / mono / EQ -----------------------------------------
  void   set_volume_min(double db);
  double volume_min() const     { return volume_min_db_.load(); }
  void   set_volume(double db);
  double volume() const         { return volume_db_.load(); }
  void   set_speed(double ratio);
  double speed() const          { return speed_.load(); }
  void   toggle_mono();
  bool   mono() const            { return mono_.load(); }
  void   set_eq_band(int band, double db);
  std::array<double, 10> eq_bands() const;

  // --- Stream info --------------------------------------------------------
  std::string stream_err() const;
  // stream_title() loads the atomic shared_ptr; returns "" if none.
  std::string stream_title() const;
  std::pair<std::int64_t, std::int64_t> stream_bytes() const;
  int sample_rate() const { return device_rate_.load(std::memory_order_acquire); }  // resolved device rate

  // --- Device selection (device picker) -----------------------------------
  // list_devices returns the available output device names (delegates to the
  // sink; miniaudio enumerates natively — Go shells out to pactl).
  std::vector<std::string> list_devices() const;
  // switch_device reopens the output on `name` ("" = system default). On a
  // successful switch the resampler is reconfigured against the new device
  // rate and playback continues. On failure the engine state matches
  // whatever the sink ended up with (previous device restored, or closed —
  // see AudioSink::switch_device); playback is halted if the sink closed.
  std::expected<void, std::string> switch_device(std::string_view name);
  // current_device returns the name of the device playback is on
  // ("" = the system default).
  std::string current_device() const;

  // --- Audio samples for the visualizer (Tap ring) -----------------------
  std::size_t samples_into(std::span<float> dst) const;
  std::size_t waveform_samples_into(std::span<float> dst) const;
  std::size_t stereo_samples_into(std::span<Frame> dst) const;

private:
  void audio_loop(std::stop_token stoken);
  std::string build_and_install(const std::string& path, double known_duration,
                                  bool ytdl, std::int64_t seek_gen_snap);
  std::expected<std::shared_ptr<TrackPipeline>, std::string>
  build_track_pipeline(const std::string& path, double known_duration_secs,
                       bool ytdl, int start_sec);
  void install_current(std::shared_ptr<TrackPipeline> tp, bool mark_playing);
  void install_next(std::shared_ptr<TrackPipeline> tp);
  void async_close(std::shared_ptr<TrackPipeline> tp);
  void on_gapless_swap(std::uint64_t token);
  void set_chain_rate(int src_rate);  // audio thread only
  void configure_resampler();         // audio thread only (src-rate → device-rate)
  void refresh_eq();                  // audio thread only
  void publish_state() const;
  void wake();
  static std::size_t pull_capacity(const EngineConfig& cfg, int rate);
  static double pos_secs_of(const TrackPipeline& tp);
  static double dur_secs_of(const TrackPipeline& tp);

  std::shared_ptr<AudioSink>        sink_;
  EngineConfig                      cfg_;
  foundation::Notifier*             notifier_;
  std::jthread                      audio_thread_;

  // DSP chain state (audio-thread-owned except for the atomics the UI sets).
  dsp::EqState                      eq_;
  std::unique_ptr<dsp::Resampler>   resampler_;
  std::atomic<int>                  device_rate_ = 0;  // resolved device rate (ctor; updated by switch_device)
  Tap                               tap_;
  Gapless                           gapless_;
  std::unique_ptr<ChainStreamer>    chain_;  // gapless→EQ→WSOLA (ctor body)
  int                               chain_rate_ = 0;   // frames/s through chain
  int                               resample_dev_rate_ = 0;  // audio-thread cache of device_rate_
  std::array<double, 10>            last_eq_db_{};     // audio-thread cache
  std::shared_ptr<Streamer>         last_seen_stream_;  // gapless current() detect
  std::size_t                       pull_frames_ = 1024;

  // Current pipeline + preload registry. Guarded by swap_mu_ — the audio
  // thread takes it briefly on track changes and in the gapless-swap callback;
  // the per-buffer hot path is lock-free (change detection via
  // last_seen_stream_ vs gapless.current()).
  std::shared_ptr<TrackPipeline>    current_;
  std::shared_ptr<TrackPipeline>    next_;
  std::uint64_t                     next_token_ = 0;
  mutable std::mutex                swap_mu_;

  // Async resource closers (ffmpeg waits, HTTP teardown) — joined in dtor.
  std::mutex                        closers_mu_;
  std::vector<std::jthread>         closers_;

  // Sink state.
  bool                              sink_open_  = false;
  std::string                       sink_error_;

  // Idle-wake for the audio loop (condition_variable_any + stop_token).
  mutable std::mutex                wake_mu_;
  std::condition_variable_any       wake_cv_;

  // Controls (set by UI thread, read by audio thread).
  std::atomic<double>              volume_db_{0.0};
  std::atomic<double>              volume_min_db_{-50.0};
  std::atomic<double>              speed_{1.0};
  std::array<std::atomic<double>, 10> eq_db_{};
  std::atomic<bool>                playing_{false};
  std::atomic<bool>                paused_{false};
  std::atomic<bool>                mono_{false};
  std::atomic<std::int64_t>        seek_gen_{0};
  std::atomic<bool>                ytdl_seek_{false};
  mutable std::atomic<bool>        gapless_advanced_{false};  // consume-once (Go CAS)
  std::atomic<std::shared_ptr<const std::string>> stream_title_{nullptr};

  // Device switching: set by the UI thread around switch_device() while the
  // sink is torn down and reopened — the audio thread idles for those ticks
  // instead of writing to a half-open device (mirrors the mute-then-swap
  // pattern of seek_ytdl). current_device_ surfaces the active device name
  // for the picker's "current" marker ("" = system default).
  std::atomic<bool>                device_switching_{false};
  std::atomic<std::shared_ptr<const std::string>> current_device_{nullptr};
};

}  // namespace bootamp::audio