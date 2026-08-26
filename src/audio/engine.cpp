// audio/engine.cpp — AudioEngine: the in-process playback engine.
//
// Port of cliamp/player/player.go (Player) + the Engine iface from
// player/engine.go. The engine runs a single audio jthread that owns the DSP
// chain and pulls from the gapless sequencer:
//
//   gapless -> 10x Biquad EQ -> WSOLA speed -> Tap -> Volume/Mono
//          -> swresample (src-sr -> device-sr) -> device SPSC -> AudioSink
//
// The UI drives everything through atomics (no locks on the per-buffer hot
// path): volume/speed, eq bands, playing/paused/mono, seek generation and the
// ICY stream title. Pipeline build (the slow part: ffmpeg/yt-dlp spawns)
// happens OFF the audio lock; the swap is committed under a brief mutex and
// the old resources are closed asynchronously by closers jthreads that the
// dtor joins (plan debt fix vs Go's fire-and-forget goroutines).
//
// Position/duration/seek semantics mirror player.go: position is the decoder's
// frame count plus the yt-dlp stream_offset; yt-dlp seeking is seek-by-restart
// with a generation counter (seek_gen) that cancels stale rebuilds.
#include "audio/engine.hpp"

#include "audio/ytdl.hpp"
#include "dsp/volume.hpp"
#include "dsp/wsola.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <string_view>
#include <utility>

#include <sched.h>
#include <sys/mman.h>

namespace bootamp::audio {

namespace {

// Optional realtime setup: mlockall + SCHED_FIFO, enabled with BOOTAMP_RT=1.
// Failures are non-fatal (unprivileged processes lack CAP_IPC_LOCK and
// SCHED_FIFO permissions); the engine runs fine without either.
void try_realtime_setup() {
  const char* v = std::getenv("BOOTAMP_RT");
  if (v == nullptr || std::string_view(v) != "1") {
    return;
  }
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { /* non-fatal */ }
  struct sched_param sp {};
  sp.sched_priority = 10;
  if (::sched_setscheduler(0, SCHED_FIFO, &sp) != 0) { /* non-fatal */ }
}

// Chain source adapter: pulls from the gapless sequencer and applies the
// 10-band EQ, so the WSOLA stretcher above it sees EQ'd, source-rate frames.
// Duck-typed for dsp::WsolaStretcher (stream()/err()); gapless errors are
// per-track, surfaced via the pipeline's decoder — not here.
class EqGaplessSource {
 public:
  EqGaplessSource(Gapless& gapless, dsp::EqState& eq) : gapless_(gapless), eq_(eq) {}

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) {
    auto [n, ok] = gapless_.stream(dst);
    if (n > 0) {
      dsp::process_chain(eq_, dst.subspan(0, n));
    }
    return {n, ok};
  }

  std::string err() const { return {}; }

 private:
  Gapless&      gapless_;
  dsp::EqState& eq_;
};

}  // namespace

// The gapless → EQ → WSOLA adapter, exposed to the Tap as an audio::Streamer.
// Definition of the forward-declared bootamp::audio::ChainStreamer. The tap
// sits ABOVE it so the visualizer sees pre-volume frames (cliamp tap.go).
class ChainStreamer final : public Streamer {
 public:
  ChainStreamer(Gapless& gapless, dsp::EqState& eq, const std::atomic<double>* speed)
      : src_(gapless, eq), stretcher_(src_, speed) {}

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override {
    return stretcher_.stream(dst);
  }
  std::string err() const override { return stretcher_.err(); }

 private:
  EqGaplessSource                        src_;
  dsp::WsolaStretcher<EqGaplessSource>   stretcher_;
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine(std::shared_ptr<AudioSink> sink, EngineConfig cfg,
                         foundation::Notifier* notifier)
    : sink_(std::move(sink)),
      cfg_(cfg),
      notifier_(notifier),
      device_rate_(cfg_.sample_rate > 0 ? cfg_.sample_rate : 44100),
      tap_(std::max<std::size_t>(4096, pull_capacity(cfg_, device_rate_.load())),
           device_rate_.load()),
      gapless_([this](std::uint64_t token) { on_gapless_swap(token); }) {
  volume_min_db_.store(cfg_.volume_min_db);
  volume_db_.store(0.0);
  speed_.store(1.0);

  // Open the device at the working rate (stereo float32). Go: speaker.Init in
  // New(). cfg_.sample_rate == 0 means "auto-detect"; the app layer resolves
  // the device rate (cliamp main.go: DeviceSampleRate fallback 44100) — with
  // 0 we default to 44100 so NullSink tests and headless runs work.
  AudioFormat fmt;
  fmt.sample_rate = device_rate_.load();
  fmt.channels    = 2;
  fmt.precision   = 4;  // f32le
  fmt.bit_depth   = cfg_.bit_depth;
  if (auto opened = sink_->open(fmt, cfg_.buffer_ms)) {
    sink_open_ = true;
    // Suspend right away; the device callback burns CPU even on silence
    // (Go: speaker.Suspend() in New()). Resume happens on every play().
    sink_->suspend();

    // Config-selected device (optional): reopen on it. The sink keeps the
    // previous (default) device if the switch fails; only a sink left closed
    // (both devices failed) degrades to sink_open_ = false like any open
    // failure. The configured device is a preference, not a hard error.
    if (!cfg_.audio_device.empty()) {
      if (auto sw = sink_->switch_device(cfg_.audio_device)) {
        const int new_rate = sink_->sample_rate();
        if (new_rate > 0) {
          device_rate_.store(new_rate, std::memory_order_release);
          current_device_.store(
              std::make_shared<const std::string>(cfg_.audio_device),
              std::memory_order_release);
        }
      } else if (sink_->sample_rate() == 0) {
        sink_open_  = false;
        sink_error_ = "audio device \"" + cfg_.audio_device + "\": " + sw.error();
      }
      sink_->suspend();  // switch_device starts the new device — re-suspend
    }
  } else {
    sink_open_  = false;
    sink_error_ = std::move(opened.error());
  }

  pull_frames_ = std::max<std::size_t>(512, pull_capacity(cfg_, device_rate_.load()));
  chain_       = std::make_unique<ChainStreamer>(gapless_, eq_, &speed_);
  tap_.set_source(chain_.get());

  audio_thread_ = std::jthread([this](std::stop_token stoken) {
    audio_loop(std::move(stoken));
  });
  try_realtime_setup();
}

AudioEngine::~AudioEngine() {
  // 1. Stop the audio thread first so no stream is mid-pull during teardown.
  if (audio_thread_.joinable()) {
    audio_thread_.request_stop();
  }
  wake();
  if (audio_thread_.joinable()) {
    audio_thread_.join();
  }
  // 2. Detach the pipelines (no audio thread left to read them).
  std::shared_ptr<TrackPipeline> cur, nxt;
  {
    std::lock_guard lk(swap_mu_);
    cur = std::move(current_);
    nxt = std::move(next_);
  }
  gapless_.clear();
  // 3. Join the async closers (ffmpeg waits, HTTP teardown).
  std::vector<std::jthread> closers;
  {
    std::lock_guard lk(closers_mu_);
    closers.swap(closers_);
  }
  for (auto& t : closers) {
    if (t.joinable()) {
      t.join();
    }
  }
  // 4. Close whatever never got closed asynchronously, then the device.
  if (cur) {
    cur->close();
  }
  if (nxt) {
    nxt->close();
  }
  sink_->suspend();
  sink_->close();
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

std::string AudioEngine::play(const std::string& path, double known_duration_secs) {
  if (!sink_open_) {
    return sink_error_.empty() ? std::string("audio sink unavailable") : sink_error_;
  }
  sink_->resume();  // Go: resumeSpeaker() before playPipeline
  return build_and_install(path, known_duration_secs, false, -1);
}

std::string AudioEngine::play_ytdl(const std::string& page_url, double known_duration_secs) {
  if (!sink_open_) {
    return sink_error_.empty() ? std::string("audio sink unavailable") : sink_error_;
  }
  sink_->resume();

  // Probe the duration concurrently with the pipeline build (Go
  // PlayYTDL: probeYTDLDuration in a goroutine), then collect for at most
  // 2s so a hung yt-dlp can't block playback. The probe thread is detached:
  // it is bounded by its internal 10s socket timeout, touches no engine
  // state, and the shared promise keeps it safe after the future is dropped
  // (mirrors Go's goroutine that may outlive the player).
  double dur = known_duration_secs;
  if (dur <= 0.0) {
    auto pr  = std::make_shared<std::promise<double>>();
    auto fut = pr->get_future();
    std::jthread probe(
        [pr, url = page_url](std::stop_token) { pr->set_value(probe_ytdlp_duration(url).count()); });
    probe.detach();
    if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
      const double d = fut.get();
      if (d > 0.0) {
        dur = d;
      }
    }
  }
  return build_and_install(page_url, dur, true, -1);
}

std::string AudioEngine::preload(const std::string& path, double known_duration_secs) {
  auto built = build_track_pipeline(path, known_duration_secs, false, 0);
  if (!built) {
    return built.error();
  }
  install_next(std::move(*built));
  return {};
}

std::string AudioEngine::preload_ytdl(const std::string& page_url, double known_duration_secs) {
  // Go PreloadYTDL does NOT probe duration — only PlayYTDL does.
  auto built = build_track_pipeline(page_url, known_duration_secs, true, 0);
  if (!built) {
    return built.error();
  }
  install_next(std::move(*built));
  return {};
}

void AudioEngine::clear_preload() {
  std::shared_ptr<TrackPipeline> old;
  {
    std::lock_guard lk(swap_mu_);
    old = std::move(next_);
    next_ = nullptr;
    next_token_ = 0;
  }
  gapless_.set_next(nullptr);
  if (old) {
    async_close(std::move(old));
  }
}

void AudioEngine::stop() {
  // Unblock a pipe decoder stuck in stream() so the audio loop can reach the
  // cleared state (Go: active.interrupt() before gapless.Clear()).
  std::shared_ptr<TrackPipeline> cur;
  {
    std::lock_guard lk(swap_mu_);
    cur = current_;
  }
  if (cur) {
    cur->interrupt();
  }
  gapless_.clear();
  gapless_advanced_.store(false);

  std::shared_ptr<TrackPipeline> old_cur, old_next;
  {
    std::lock_guard lk(swap_mu_);
    old_cur   = std::move(current_);
    old_next  = std::move(next_);
    current_  = nullptr;
    next_     = nullptr;
    next_token_ = 0;
  }
  ytdl_seek_.store(false);
  playing_.store(false);
  paused_.store(false);
  async_close(std::move(old_cur));
  async_close(std::move(old_next));
  sink_->suspend();
  publish_state();
  wake();
}

void AudioEngine::close() {
  stop();  // Go Close(): Stop() + speaker.Clear()
}

void AudioEngine::toggle_pause() {
  if (!playing_.load(std::memory_order_acquire)) {
    return;  // Go: ctrl == nil guard
  }
  const bool p = !paused_.load(std::memory_order_acquire);
  paused_.store(p, std::memory_order_release);
  if (p) {
    sink_->suspend();
  } else {
    sink_->resume();
  }
  publish_state();
  wake();
}

// ---------------------------------------------------------------------------
// Seeking
// ---------------------------------------------------------------------------

std::string AudioEngine::seek(double offset_secs) {
  std::shared_ptr<TrackPipeline> cur;
  {
    std::lock_guard lk(swap_mu_);
    cur = current_;
  }
  if (!cur) {
    return {};
  }
  if (cur->ytdl_seek) {
    return seek_ytdl(offset_secs);  // Go: Seek delegates to SeekYTDL
  }
  if (!cur->seekable || !cur->decoder) {
    return {};
  }

  // Go relativeSeekSample: max(round((pos+d)*sr), 0), clamped to len-1.
  const std::size_t len    = cur->decoder->len();
  const double      target = pos_secs_of(*cur) + offset_secs;
  std::int64_t      frame  = std::llround(target * cur->format.sample_rate);
  if (frame < 0) {
    frame = 0;
  }
  if (len > 0 && static_cast<std::uint64_t>(frame) >= len) {
    frame = static_cast<std::int64_t>(len - 1);
  }
  const std::string err = cur->decoder->seek(static_cast<std::size_t>(frame));
  if (!err.empty()) {
    return err;
  }

  // The gapless transition point moved — invalidate the preloaded next
  // pipeline (Go: gapless.SetNext(nil) under the speaker lock).
  std::shared_ptr<TrackPipeline> old_next;
  {
    std::lock_guard lk(swap_mu_);
    old_next = std::move(next_);
    next_    = nullptr;
    next_token_ = 0;
  }
  gapless_.set_next(nullptr);
  async_close(std::move(old_next));
  if (notifier_) {
    notifier_->seeked(foundation::Seconds(offset_secs));
  }
  return {};
}

void AudioEngine::cancel_seek_ytdl() {
  seek_gen_.fetch_add(1, std::memory_order_acq_rel);
}

std::string AudioEngine::seek_ytdl(double offset_secs) {
  const std::int64_t gen = seek_gen_.load(std::memory_order_acquire);

  std::shared_ptr<TrackPipeline> cur;
  {
    std::lock_guard lk(swap_mu_);
    cur = current_;
  }
  if (!cur || !cur->ytdl_seek) {
    return {};
  }

  // Snapshot the position, then mute the chain so the old audio does not keep
  // playing at the pre-seek position while the new pipeline is built (Go:
  // curPos read + gapless.Replace(nil) under the speaker lock).
  const double cur_pos = pos_secs_of(*cur) + cur->stream_offset.count();
  gapless_.replace(nullptr);
  gapless_advanced_.store(false);

  double new_pos = std::max(cur_pos + offset_secs, 0.0);
  if (cur->known_duration.count() > 0 && new_pos >= cur->known_duration.count()) {
    new_pos = cur->known_duration.count() - 1.0;  // Go: knownDuration - time.Second
  }
  const int start_sec = static_cast<int>(new_pos);  // truncation like Go

  // Build the new pipeline OFF the audio lock (spawns yt-dlp — slow).
  auto built = build_track_pipeline(cur->path, cur->known_duration.count(), true, start_sec);
  if (!built) {
    return "yt-dlp seek: " + built.error();
  }
  auto tp = std::move(*built);
  tp->ytdl_seek = true;

  // A newer seek (CancelSeekYTDL) landed while we were building — discard.
  if (seek_gen_.load(std::memory_order_acquire) != gen) {
    async_close(std::move(tp));
    return {};
  }

  // Commit under the lock, close old async (Go player.go:510-568).
  std::shared_ptr<TrackPipeline> old, old_next;
  {
    std::lock_guard lk(swap_mu_);
    old      = std::move(current_);
    old_next = std::move(next_);
    current_ = tp;
    next_    = nullptr;
    next_token_ = 0;
  }
  if (old) {
    old->interrupt();
  }
  gapless_.replace(tp->stream);
  gapless_.set_next(nullptr);
  gapless_advanced_.store(false);
  ytdl_seek_.store(true);
  async_close(std::move(old));
  async_close(std::move(old_next));
  if (notifier_) {
    notifier_->seeked(foundation::Seconds(offset_secs));
  }
  wake();
  return {};
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool AudioEngine::seekable() const {
  std::lock_guard lk(swap_mu_);
  if (!current_) {
    return false;
  }
  // Go Seekable(): cur.seekable || (cur.ytdlSeek && cur.knownDuration > 0)
  return current_->seekable ||
         (current_->ytdl_seek && current_->known_duration.count() > 0);
}

bool AudioEngine::is_stream_seek() const {
  return false;  // Go IsStreamSeek is a stub
}

bool AudioEngine::gapless_advanced() const {
  return gapless_advanced_.exchange(false);  // Go GaplessAdvanced CAS
}

double AudioEngine::position_secs() const {
  std::lock_guard lk(swap_mu_);
  if (!current_) {
    return 0.0;
  }
  return pos_secs_of(*current_) + current_->stream_offset.count();
}

double AudioEngine::duration_secs() const {
  std::lock_guard lk(swap_mu_);
  if (!current_) {
    return 0.0;
  }
  return dur_secs_of(*current_);
}

std::pair<double, double> AudioEngine::position_and_duration_secs() const {
  std::lock_guard lk(swap_mu_);
  if (!current_) {
    return {0.0, 0.0};
  }
  return {pos_secs_of(*current_) + current_->stream_offset.count(),
          dur_secs_of(*current_)};
}

std::string AudioEngine::stream_err() const {
  std::lock_guard lk(swap_mu_);
  if (!current_) {
    return {};
  }
  if (current_->live_prefetch) {
    return current_->live_prefetch->err();
  }
  return current_->decoder ? current_->decoder->err() : std::string{};
}

std::string AudioEngine::stream_title() const {
  auto p = stream_title_.load(std::memory_order_acquire);
  return p ? *p : std::string{};
}

std::pair<std::int64_t, std::int64_t> AudioEngine::stream_bytes() const {
  std::lock_guard lk(swap_mu_);
  if (!current_) {
    return {0, 0};
  }
  const std::int64_t downloaded =
      current_->bytes_read ? current_->bytes_read->load(std::memory_order_relaxed) : 0;
  return {downloaded, current_->content_length};
}

std::size_t AudioEngine::samples_into(std::span<float> dst) const {
  return tap_.samples_into(dst);
}
std::size_t AudioEngine::waveform_samples_into(std::span<float> dst) const {
  return tap_.waveform_samples_into(dst);
}
std::size_t AudioEngine::stereo_samples_into(std::span<Frame> dst) const {
  return tap_.stereo_samples_into(dst);
}

// ---------------------------------------------------------------------------
// Device selection (device picker)
// ---------------------------------------------------------------------------

std::vector<std::string> AudioEngine::list_devices() const {
  return sink_->list_devices();
}

std::expected<void, std::string> AudioEngine::switch_device(std::string_view name) {
  if (!sink_open_) {
    return std::unexpected(sink_error_.empty() ? std::string("audio sink unavailable")
                                               : sink_error_);
  }
  // Gate the audio thread off the device while it is torn down and reopened.
  // writei() would return 0 anyway, but this keeps one consistent
  // (resampler, device-rate) pair visible to the loop — mirrors the
  // mute-then-swap pattern of seek_ytdl.
  device_switching_.store(true, std::memory_order_release);

  auto res = sink_->switch_device(name);
  if (res) {
    const int new_rate = sink_->sample_rate();
    if (new_rate > 0) {
      device_rate_.store(new_rate, std::memory_order_release);
      sink_open_ = true;  // a successful reopen re-enables playback
      sink_error_.clear();
      current_device_.store(std::make_shared<const std::string>(name),
                            std::memory_order_release);
      // A stopped/paused engine must not leave the freshly started device
      // running (the ctor suspends right after open() too).
      if (!playing_.load(std::memory_order_acquire) ||
          paused_.load(std::memory_order_acquire)) {
        sink_->suspend();
      }
    }
  } else if (sink_->sample_rate() == 0) {
    // The sink ended up closed (the new device failed and the old one could
    // not be restored): halt playback and degrade to the ctor-failure state
    // so play() surfaces the error instead of spinning into a dead sink.
    stop();
    sink_open_  = false;
    sink_error_ = "device switch to \"" + std::string(name) + "\": " + res.error();
  }
  device_switching_.store(false, std::memory_order_release);
  wake();
  return res;
}

std::string AudioEngine::current_device() const {
  auto p = current_device_.load(std::memory_order_acquire);
  return p ? *p : std::string{};
}

// ---------------------------------------------------------------------------
// Volume / speed / mono / EQ
// ---------------------------------------------------------------------------

void AudioEngine::set_volume_min(double db) {
  // Go SetVolumeMin: clamp to [-90, 0], then raise the current volume to the
  // new floor if it is below it.
  const double new_min = std::clamp(db, -90.0, 0.0);
  volume_min_db_.store(new_min, std::memory_order_relaxed);
  double cur = volume_db_.load(std::memory_order_relaxed);
  while (cur < new_min) {
    if (volume_db_.compare_exchange_weak(cur, new_min, std::memory_order_relaxed)) {
      break;
    }
  }
}

void AudioEngine::set_volume(double db) {
  volume_db_.store(std::clamp(db, volume_min_db_.load(std::memory_order_relaxed), 6.0),
                   std::memory_order_relaxed);
}

void AudioEngine::set_speed(double ratio) {
  speed_.store(std::clamp(ratio, 0.25, 2.0), std::memory_order_relaxed);
}

void AudioEngine::toggle_mono() {
  mono_.store(!mono_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void AudioEngine::set_eq_band(int band, double db) {
  if (band < 0 || band >= 10) {
    return;  // Go: out of range is a silent no-op
  }
  eq_db_[static_cast<std::size_t>(band)].store(std::clamp(db, -12.0, 12.0),
                                               std::memory_order_relaxed);
}

std::array<double, 10> AudioEngine::eq_bands() const {
  std::array<double, 10> bands{};
  for (std::size_t i = 0; i < 10; ++i) {
    bands[i] = eq_db_[i].load(std::memory_order_relaxed);
  }
  return bands;
}

// ---------------------------------------------------------------------------
// Pipeline build + install
// ---------------------------------------------------------------------------

std::string AudioEngine::build_and_install(const std::string& path,
                                           double known_duration,
                                           bool ytdl,
                                           std::int64_t seek_gen_snap) {
  auto built = build_track_pipeline(path, known_duration, ytdl, 0);
  if (!built) {
    return built.error();
  }
  if (seek_gen_snap >= 0 && seek_gen_.load(std::memory_order_acquire) != seek_gen_snap) {
    // Cancelled while building — discard silently (Go: go closePipelines(tp)).
    async_close(std::move(*built));
    return {};
  }
  install_current(std::move(*built), true);
  return {};
}

std::expected<std::shared_ptr<TrackPipeline>, std::string>
AudioEngine::build_track_pipeline(const std::string& path, double known_duration_secs,
                                  bool ytdl, int start_sec) {
  // Go buildPipeline clears the ICY title on every build.
  stream_title_.store(nullptr);

  if (ytdl) {
    auto dec = YtdlpPipeStreamer::decode_ytdlp_pipe(
        path, device_rate_.load(std::memory_order_acquire), cfg_.bit_depth, start_sec);
    if (!dec) {
      return std::unexpected("yt-dlp: " + dec.error());
    }
    auto tp            = std::make_shared<TrackPipeline>();
    tp->decoder        = std::move(*dec);
    tp->stream         = tp->decoder;
    tp->format.sample_rate = device_rate_.load(std::memory_order_acquire);  // ffmpeg -ar emits at device rate
    tp->format.channels    = 2;
    tp->format.precision   = cfg_.bit_depth == 32 ? 4 : 2;
    tp->format.bit_depth   = cfg_.bit_depth;
    tp->seekable       = false;
    tp->known_duration = foundation::Seconds(known_duration_secs);
    tp->ytdl_seek      = true;
    tp->path           = path;
    tp->stream_offset  = foundation::Seconds(start_sec);  // yt-dlp seek-by-restart origin
    return tp;
  }

  // Generic path (native decode / ffmpeg pipe / HLS / radio): the ICY title
  // callback publishes into the engine's atomic (URL sources only).
  PipelineBuilder builder(device_rate_.load(std::memory_order_acquire), cfg_.bit_depth,
                          cfg_.resample_quality);
  auto built = builder.build_pipeline(path, [this](std::string title) {
    stream_title_.store(std::make_shared<const std::string>(std::move(title)),
                        std::memory_order_release);
  });
  if (!built) {
    return std::unexpected(std::move(built.error()));
  }
  auto tp = std::shared_ptr<TrackPipeline>(std::move(*built));
  tp->set_known_duration(foundation::Seconds(known_duration_secs));
  return tp;
}

void AudioEngine::install_current(std::shared_ptr<TrackPipeline> tp, bool mark_playing) {
  // Commit the new pipeline state, then swap the gapless source. The audio
  // thread detects the swap via gapless.current() and reconfigures the chain
  // rate on its next buffer (no lock on the steady-state hot path).
  std::shared_ptr<TrackPipeline> old, old_next;
  {
    std::lock_guard lk(swap_mu_);
    old      = std::move(current_);
    old_next = std::move(next_);
    current_ = tp;
    next_    = nullptr;
    next_token_ = 0;
  }
  if (old) {
    old->interrupt();  // unblock a pipe decoder stuck in stream() (Go)
  }
  gapless_.replace(tp->stream);
  gapless_.set_next(nullptr);  // a manual skip invalidates the preload
  gapless_advanced_.store(false);
  ytdl_seek_.store(tp->ytdl_seek);
  if (mark_playing) {
    playing_.store(true);
    paused_.store(false);
  }
  async_close(std::move(old));
  async_close(std::move(old_next));
  publish_state();
  wake();
}

void AudioEngine::install_next(std::shared_ptr<TrackPipeline> tp) {
  // Register the pipeline + token atomically with respect to the gapless
  // transition callback (which takes swap_mu_): a transition can only fire
  // after set_next, and the callback then sees consistent state.
  std::shared_ptr<TrackPipeline> old;
  std::uint64_t                  token = 0;
  {
    std::lock_guard lk(swap_mu_);
    old            = std::move(next_);
    next_          = tp;
    token          = gapless_.set_next(tp->stream);
    next_token_    = token;
    tp->gapless_token = token;
  }
  if (old) {
    async_close(std::move(old));
  }
  publish_state();
}

void AudioEngine::async_close(std::shared_ptr<TrackPipeline> tp) {
  if (!tp) {
    return;
  }
  // Off the UI thread so slow closes (ffmpeg waitpid, HTTP teardown) never
  // block the caller. Joined in the dtor — no fire-and-forget.
  std::lock_guard lk(closers_mu_);
  closers_.emplace_back([tp = std::move(tp)](std::stop_token) { tp->close(); });
}

void AudioEngine::on_gapless_swap(std::uint64_t token) {
  // Called from the audio thread when the gapless sequencer continues into
  // the preloaded next track. Promote the pipeline registry and close the
  // old one async (Go handleGaplessSwap). The chain-rate reconfigure happens
  // on the next loop iteration via the gapless.current() detection.
  std::shared_ptr<TrackPipeline> next, old;
  {
    std::lock_guard lk(swap_mu_);
    next = next_;
    if (!next || next->gapless_token != token) {
      return;  // stale transition (manual replace won) — ignore
    }
    old     = std::move(current_);
    current_ = next;
    next_    = nullptr;
    next_token_ = 0;
  }
  ytdl_seek_.store(next->ytdl_seek);
  gapless_advanced_.store(true);
  async_close(std::move(old));
  publish_state();
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

void AudioEngine::audio_loop(std::stop_token stoken) {
  std::vector<Frame> chain_buf(pull_frames_);
  std::vector<float> resamp_in;
  std::vector<float> resamp_out;
  std::vector<Frame> dev_buf;
  dsp::GainCache     vol_gain;  // cached 10^(db/20) — recompute on change only

  while (!stoken.stop_requested()) {
    // Wait for play (or wake on stop). Steady-state idle costs nothing.
    {
      std::unique_lock lk(wake_mu_);
      wake_cv_.wait_for(lk, stoken, std::chrono::milliseconds(10),
                        [&] {
                          return stoken.stop_requested() ||
                                 (playing_.load(std::memory_order_acquire) &&
                                  !paused_.load(std::memory_order_acquire));
                        });
    }
    if (stoken.stop_requested()) {
      break;
    }
    if (!playing_.load(std::memory_order_acquire) ||
        paused_.load(std::memory_order_acquire)) {
      continue;  // spurious wake / state raced us
    }
    if (device_switching_.load(std::memory_order_acquire)) {
      // Device reopen in progress (UI thread, device picker) — drop this
      // tick rather than write into a half-open device.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // Pipeline swap (manual replace or gapless transition) — re-read the
    // current pipeline and reconfigure the chain rate + EQ. Steady state:
    // one atomic load, no lock.
    if (auto cur_stream = gapless_.current(); cur_stream != last_seen_stream_) {
      last_seen_stream_ = std::move(cur_stream);
      std::shared_ptr<TrackPipeline> cur;
      {
        std::lock_guard lk(swap_mu_);  // brief — only on track change
        cur = current_;
      }
      if (cur) {
        set_chain_rate(cur->format.sample_rate);
      }
    }

    // Device switch (picker) — the sink may have reopened at a different
    // native rate; reconfigure the resampler for the current chain rate
    // (set_chain_rate does not fire on its own: the track did not change).
    const int dev_rate = device_rate_.load(std::memory_order_acquire);
    if (dev_rate != resample_dev_rate_) {
      resample_dev_rate_ = dev_rate;
      configure_resampler();
    }

    refresh_eq();

    // Pull through gapless → EQ → WSOLA; the tap copies the frames into the
    // visualizer ring on the way.
    const auto [n, ok] = tap_.stream(chain_buf);
    (void)ok;  // gapless always fills; ok is forwarded for interface fidelity
    if (n == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));  // no spin
      continue;
    }
    if (n < chain_buf.size()) {
      std::fill(chain_buf.begin() + static_cast<std::ptrdiff_t>(n), chain_buf.end(),
                Frame{});
    }

    // Volume + optional mono downmix (AVX2), port of volumeStreamer.
    const float gain =
        vol_gain.gain_for(static_cast<float>(volume_db_.load(std::memory_order_relaxed)));
    const std::span<Frame> voiced(chain_buf.data(), n);
    if (mono_.load(std::memory_order_relaxed)) {
      dsp::apply_volume_mono(voiced, gain);
    } else {
      dsp::apply_volume(voiced, gain);
    }

    if (resampler_) {
      // src-rate → device-rate (libswresample, interleaved f32).
      resamp_in.resize(n * 2);
      for (std::size_t i = 0; i < n; ++i) {
        resamp_in[2 * i]     = chain_buf[i][0];
        resamp_in[2 * i + 1] = chain_buf[i][1];
      }
      const std::size_t out_cap =
          n * static_cast<std::size_t>(device_rate_.load(std::memory_order_acquire)) /
              static_cast<std::size_t>(std::max(chain_rate_, 1)) +
          1024;
      resamp_out.resize(out_cap * 2);
      const std::size_t produced = resampler_->process(resamp_in, resamp_out);
      if (produced > 0) {
        dev_buf.resize(produced);
        for (std::size_t i = 0; i < produced; ++i) {
          dev_buf[i][0] = resamp_out[2 * i];
          dev_buf[i][1] = resamp_out[2 * i + 1];
        }
        sink_->writei(dev_buf);
      }
    } else {
      sink_->writei(std::span<const Frame>(chain_buf.data(), n));
    }
  }
}

void AudioEngine::set_chain_rate(int src_rate) {
  // Audio thread only. Frames enter the volume/resample stage at the current
  // pipeline's source rate; the resampler converts them to the device rate.
  if (src_rate <= 0 || src_rate == chain_rate_) {
    return;
  }
  chain_rate_ = src_rate;
  configure_resampler();
  // Biquad coefficients are sample-rate dependent — recompute all bands.
  for (std::size_t i = 0; i < dsp::kEqBands; ++i) {
    dsp::set_band(eq_, i, last_eq_db_[i], static_cast<double>(src_rate));
  }
}

void AudioEngine::configure_resampler() {
  // Audio thread only. (Re)builds the src-rate → device-rate swresample stage
  // for the current chain rate; passthrough when the rates match. Called from
  // set_chain_rate (track change) and from the loop when the device rate
  // changed after a picker switch.
  const int dev_rate = device_rate_.load(std::memory_order_acquire);
  if (chain_rate_ <= 0 || dev_rate <= 0) {
    return;  // no valid pairing yet (idle before first track)
  }
  if (chain_rate_ == dev_rate) {
    resampler_.reset();  // passthrough — no rate conversion needed
  } else {
    if (!resampler_) {
      resampler_ = std::make_unique<dsp::Resampler>();
    }
    resampler_->configure(chain_rate_, dev_rate);
  }
}

void AudioEngine::refresh_eq() {
  // Audio thread only. Recompute a band's coefficients when its dB atomic
  // changed (Go biquad.calcCoeffs re-checks the gain on every Stream call).
  bool changed = false;
  for (std::size_t i = 0; i < dsp::kEqBands; ++i) {
    const double db = eq_db_[i].load(std::memory_order_relaxed);
    if (db != last_eq_db_[i]) {
      last_eq_db_[i] = db;
      changed        = true;
    }
  }
  if (!changed) {
    return;
  }
  const double sr = chain_rate_ > 0
                        ? static_cast<double>(chain_rate_)
                        : static_cast<double>(device_rate_.load(std::memory_order_acquire));
  for (std::size_t i = 0; i < dsp::kEqBands; ++i) {
    dsp::set_band(eq_, i, last_eq_db_[i], sr);
  }
}

void AudioEngine::publish_state() const {
  if (!notifier_) {
    return;
  }
  foundation::State st;
  st.status = paused_.load(std::memory_order_acquire)
                  ? foundation::Status::Paused
                  : (playing_.load(std::memory_order_acquire)
                         ? foundation::Status::Playing
                         : foundation::Status::Stopped);
  {
    std::lock_guard lk(swap_mu_);
    if (current_) {
      st.track.url = current_->path;
      if (auto t = stream_title_.load(std::memory_order_acquire)) {
        st.track.title = *t;
      }
    }
  }
  st.volume_db = volume_db_.load(std::memory_order_relaxed);
  st.position  = foundation::Seconds(position_secs());
  st.seekable  = seekable();
  notifier_->update(st);
}

void AudioEngine::wake() {
  wake_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------

std::size_t AudioEngine::pull_capacity(const EngineConfig& cfg, int rate) {
  // Go: sr.N(bufferMs) — the per-callback frame count.
  const std::int64_t frames =
      static_cast<std::int64_t>(rate) * cfg.buffer_ms / 1000;
  return frames > 0 ? static_cast<std::size_t>(frames) : 0;
}

double AudioEngine::pos_secs_of(const TrackPipeline& tp) {
  // Go Position(): livePrefetch.Position() or format.SampleRate.D(decoder.
  // Position()); the caller adds stream_offset for yt-dlp restarts.
  if (tp.live_prefetch) {
    return static_cast<double>(tp.live_prefetch->position_frames()) /
           tp.format.sample_rate;
  }
  if (tp.decoder) {
    return static_cast<double>(tp.decoder->position()) / tp.format.sample_rate;
  }
  return 0.0;
}

double AudioEngine::dur_secs_of(const TrackPipeline& tp) {
  // Go Duration(): live: knownDuration > 0 ? known : decodedDuration; else
  // decoder.Len() > 0 ? len/sr : knownDuration.
  if (tp.live_prefetch) {
    return tp.known_duration.count() > 0 ? tp.known_duration.count()
                                         : tp.decoded_duration.count();
  }
  if (tp.decoder) {
    const std::size_t n = tp.decoder->len();
    if (n > 0) {
      return static_cast<double>(n) / tp.format.sample_rate;
    }
  }
  return tp.known_duration.count();
}

}  // namespace bootamp::audio
