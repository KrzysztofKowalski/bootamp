// audio/audio_sink_miniaudio.cpp — MiniaudioSink: production audio output.
//
// Port of cliamp's beep/speaker usage (player/player.go:76-110, beep/v2
// speaker.Init/Suspend/Resume) onto miniaudio (pacman `miniaudio`, verified
// 0.11.25). Design per the locked plan decisions:
//
//   - RESAMPLING OFF (decision #3): the engine's swresample stage already
//     resamples src→device rate, so miniaudio must never resample again.
//     Miniaudio's own resampler engages whenever the requested rate differs
//     from the device's native rate, so open() picks a rate from the default
//     device's nativeDataFormats (preferring the requested fmt.sample_rate)
//     and additionally sets alsa.noAutoResample so ALSA's plug layer can't
//     silently resample either. The rate actually opened is exposed via
//     sample_rate(); the engine configures swresample against it. When the
//     device is unavailable/formatless, open() falls back to the requested
//     rate (miniaudio then resamples — degraded mode, last resort only).
//   - BUFFER SPLIT mirrors beep speaker.Init's 50/50: half of buffer_ms goes
//     to the device period (periodSizeInMilliseconds), half to the SPSC ring
//     that sits between the engine thread and the data callback.
//   - suspend() = beep speaker.Suspend (ALSA drop semantics): stop the device
//     and discard the ring contents; resume() = speaker.Resume.
//   - switch_device(name) = cliamp player.SwitchAudioDevice, native instead of
//     Go's pactl: enumerate the playback devices, tear down the current device
//     and reopen on the named one with the same format/buffer policy. The old
//     device is brought back (best effort) if the new one fails to start; the
//     engine reconfigures its swresample stage against the new sample_rate().
//
// Concurrency (plan): the engine audio thread is the sole ring writer and the
// miniaudio data callback the sole reader — classic SPSC on atomics, no
// locks, no exceptions on the callback path. writei() may block (spin+sleep)
// while the ring is full; it returns early (partial/0) when the device is
// suspended or closed so the audio thread can never deadlock.
//
// Requires miniaudio.h (pacman pkg `miniaudio`). Without the package the
// file compiles to link-safe fallbacks (see the #else branch at the bottom)
// and main.cpp's BOOTAMP_APP_HAS_MINIAUDIO guard routes to NullSink instead.

#if __has_include(<miniaudio.h>)

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio/audio_sink.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bootamp::audio {

namespace {

// ma_err renders a miniaudio result code for error messages.
std::string ma_err(ma_result rc) {
  if (rc == MA_SUCCESS) return {};
  const char* msg = ma_result_description(rc);
  return std::string{"miniaudio: "} + (msg != nullptr ? msg : "error");
}

// next_pow2 rounds `n` up to the next power of two (minimum 64).
std::size_t next_pow2(std::size_t n) {
  std::size_t p = 64;
  while (p < n) p <<= 1;
  return p;
}

// pick_native_rate selects the device rate for open(): `preferred` when the
// device lists it among its native formats (miniaudio's resampler stays a
// pass-through), else the device's first native rate, else `preferred`
// unchanged as a last resort (device gave no format list — miniaudio will
// resample; see the degraded-mode note above).
int pick_native_rate(const ma_device_info& info, int preferred) {
  if (info.nativeDataFormatCount > 0) {
    for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) {
      if (static_cast<int>(info.nativeDataFormats[i].sampleRate) == preferred) {
        return preferred;
      }
    }
    return static_cast<int>(info.nativeDataFormats[0].sampleRate);
  }
  return preferred;
}

// enumerate_playback returns the playback device infos from a throwaway
// context. The sink's own ma_context is per-open (created in open()), so
// enumeration always probes a fresh context — shared by list_devices() and
// the name→ID resolution in switch_device(). Empty on failure.
std::vector<ma_device_info> enumerate_playback() {
  std::vector<ma_device_info> infos;
  ma_context ctx;
  if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) {
    return infos;
  }
  ma_device_info* playback = nullptr;
  ma_uint32       count    = 0;
  if (ma_context_get_devices(&ctx, &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
    infos.assign(playback, playback + count);
  }
  ma_context_uninit(&ctx);
  return infos;
}

// MiniaudioSink: engine writes (writei) into an SPSC ring; the miniaudio data
// callback pulls from it and silence-fills on underrun. Never blocks the
// callback; never locks on the audio path.
class MiniaudioSink final : public AudioSink {
public:
  ~MiniaudioSink() override { close(); }

  std::expected<void, std::string> open(const AudioFormat& fmt, int buffer_ms) override;
  std::size_t writei(std::span<const Frame> frames) override;
  void suspend() override;
  void resume() override;
  void close() override;
  std::vector<std::string> list_devices() const override;
  std::expected<void, std::string> switch_device(std::string_view name) override;
  int sample_rate() const noexcept override { return rate_; }

  // Diagnostics (not part of the interface): callback invocations that had to
  // silence-fill (ring underruns).
  std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

private:
  // --- SPSC ring -------------------------------------------------------------
  // head_/tail_ are monotonically increasing frame counters; the buffer index
  // is `counter & mask_`. Writer: store data → head_.store(release); reader:
  // head_.load(acquire) → read data → tail_.store(release). Writer loads
  // tail_ with acquire, reader loads head_ with acquire. Single producer
  // (engine thread) + single consumer (miniaudio thread), no locks.
  std::vector<Frame>              ring_;
  std::size_t                     mask_ = 0;  // capacity-1 (power-of-two cap)
  std::atomic<std::size_t>        head_{0};
  std::atomic<std::size_t>        tail_{0};
  std::atomic<bool>               stopped_{true};  // device not consuming
  std::atomic<bool>               closed_{true};   // no device until open()
  std::atomic<std::uint64_t>      underruns_{0};

  ma_context                       ctx_{};
  ma_device                        dev_{};
  bool                             ctx_init_ = false;
  bool                             dev_init_ = false;
  int                              rate_ = 0;

  // --- device switching (switch_device) ------------------------------------
  // fmt_/buffer_ms_ snapshot the last open() params so a reopen on another
  // device keeps the same format/buffer policy. current_device_ names the
  // device currently open ("" = the system default).
  AudioFormat                      fmt_;
  int                              buffer_ms_ = 0;
  std::string                      current_device_;

  // start_device prepares the ring and starts the playback device on `wanted`
  // (nullptr = system default) at a native rate picked from `preferred_rate`.
  // Requires an initialized ctx_ and valid fmt_/buffer_ms_. On success the
  // device is left running; on failure it is left uninitialized (callers
  // close()).
  std::expected<void, std::string> start_device(int preferred_rate,
                                                const ma_device_id* wanted);
  // open_named tears down the current device and reopens it on `name`
  // ("" = system default) at `preferred_rate`. Used by switch_device and by
  // its failure-restore path.
  std::expected<void, std::string> open_named(const std::string& name,
                                              int preferred_rate);

  // clear_ring discards everything buffered (port of beep Suspend's ALSA
  // drop). Call only when the device thread is stopped.
  void clear_ring() noexcept {
    tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }

  // pull is the reader side: copies up to `frames` frames into the device
  // output, silence-fills the rest. Runs on the miniaudio thread — must never
  // block, throw, or lock.
  void pull(float* out, std::size_t frames) noexcept;

  static void data_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames);
};

// --- data callback (miniaudio thread, no exceptions / no locks) --------------

void MiniaudioSink::data_cb(ma_device* dev, void* out, const void*, ma_uint32 frames) {
  auto* self = static_cast<MiniaudioSink*>(dev->pUserData);
  self->pull(static_cast<ma_float*>(out), frames);
}

void MiniaudioSink::pull(float* out, std::size_t frames) noexcept {
  static_assert(sizeof(Frame) == 2 * sizeof(float), "Frame must be 2 contiguous floats");
  const std::size_t head = head_.load(std::memory_order_acquire);
  const std::size_t tail = tail_.load(std::memory_order_relaxed);
  const std::size_t n    = std::min(head - tail, frames);
  std::size_t       idx  = tail & mask_;
  std::size_t       i    = 0;
  // Copy in at most two contiguous runs across the ring wrap.
  while (i < n) {
    const std::size_t run = std::min(n - i, mask_ + 1 - idx);
    std::memcpy(out + i * 2, ring_.data() + idx, run * sizeof(Frame));
    i += run;
    idx = (idx + run) & mask_;
  }
  // Silence-fill whatever the ring could not provide (underrun).
  for (; i < frames; ++i) {
    out[i * 2]     = 0.0f;
    out[i * 2 + 1] = 0.0f;
  }
  if (n < frames) {
    underruns_.fetch_add(1, std::memory_order_relaxed);
  }
  if (n > 0) {
    tail_.store(tail + n, std::memory_order_release);
  }
}

// --- interface ---------------------------------------------------------------

std::expected<void, std::string> MiniaudioSink::open(const AudioFormat& fmt, int buffer_ms) {
  if (fmt.sample_rate <= 0 || fmt.channels != 2 || buffer_ms <= 0) {
    return std::unexpected{
        "miniaudio: open requires stereo, sample_rate > 0 and buffer_ms > 0"};
  }

  close();  // idempotent: a second open() replaces the previous device
  fmt_ = fmt;
  buffer_ms_ = buffer_ms;
  closed_.store(false, std::memory_order_release);

  ma_result rc = ma_context_init(nullptr, 0, nullptr, &ctx_);
  if (rc != MA_SUCCESS) return std::unexpected{ma_err(rc)};
  ctx_init_ = true;

  // Device rate: a native rate whenever the default device can tell us one
  // (miniaudio's resampler stays a pass-through; alsa.noAutoResample below).
  auto started = start_device(fmt.sample_rate, nullptr);
  if (!started) {
    close();
    return started;
  }
  return {};
}

std::expected<void, std::string> MiniaudioSink::start_device(int preferred_rate,
                                                             const ma_device_id* wanted) {
  // Pick a native rate for the target device (preferring the previous rate
  // on a switch; the original open() rate on first open). Records the
  // device name for the no-op check in switch_device().
  int rate = preferred_rate;
  {
    ma_device_info info;
    if (ma_context_get_device_info(&ctx_, ma_device_type_playback, wanted, &info) ==
        MA_SUCCESS) {
      rate            = pick_native_rate(info, preferred_rate);
      current_device_ = info.name;
    }
  }
  rate_ = rate;

  // Ring capacity = half the buffer, matching beep speaker.Init's 50/50 split
  // (driver buffer : player buffer). The device period gets the other half.
  const AudioFormat dev_fmt{rate, fmt_.channels, fmt_.precision, fmt_.bit_depth};
  const std::size_t ring_frames =
      next_pow2(std::max<std::size_t>(64, frames_in(dev_fmt, std::chrono::milliseconds{buffer_ms_ / 2})));
  if (ring_.size() != ring_frames) {
    ring_.assign(ring_frames, Frame{0.0f, 0.0f});  // first open only: close()
  }                                                 // keeps the buffer alive (see close)
  mask_ = ring_frames - 1;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  underruns_.store(0, std::memory_order_relaxed);

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format  = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate        = static_cast<ma_uint32>(rate);
  cfg.periodSizeInMilliseconds = static_cast<ma_uint32>(std::max(10, buffer_ms_ / 2));
  cfg.alsa.noAutoResample      = MA_TRUE;  // ALSA plug must not resample either
  cfg.dataCallback             = &MiniaudioSink::data_cb;
  cfg.pUserData                = this;
  if (wanted != nullptr) {
    cfg.playback.pDeviceID = wanted;
  }

  ma_result rc = ma_device_init(&ctx_, &cfg, &dev_);
  if (rc != MA_SUCCESS) {
    return std::unexpected{ma_err(rc)};
  }
  dev_init_ = true;

  rc = ma_device_start(&dev_);
  if (rc != MA_SUCCESS) {
    ma_device_uninit(&dev_);
    dev_init_ = false;
    return std::unexpected{ma_err(rc)};
  }
  stopped_.store(false, std::memory_order_release);
  return {};
}

std::expected<void, std::string> MiniaudioSink::open_named(const std::string& name,
                                                           int preferred_rate) {
  // Resolve the name to a device ID by enumerating playback devices the same
  // way list_devices() does (the context is per-open here, so a throwaway
  // context is used; the ID is copied out before it is freed).
  ma_device_id        id{};
  const ma_device_id* wanted = nullptr;
  if (!name.empty()) {
    bool found = false;
    for (const ma_device_info& info : enumerate_playback()) {
      if (name == info.name) {
        id     = info.id;
        wanted = &id;
        found  = true;
        break;
      }
    }
    if (!found) {
      return std::unexpected{"miniaudio: no playback device named \"" + name + "\""};
    }
  }

  close();  // idempotent — keeps ring memory for reuse (see close())
  closed_.store(false, std::memory_order_release);

  ma_result rc = ma_context_init(nullptr, 0, nullptr, &ctx_);
  if (rc != MA_SUCCESS) return std::unexpected{ma_err(rc)};
  ctx_init_ = true;

  auto started = start_device(preferred_rate, wanted);
  if (!started) {
    close();
    return started;
  }
  current_device_ = name;
  return {};
}

std::expected<void, std::string> MiniaudioSink::switch_device(std::string_view name) {
  // Same device as currently open ("" == default == default) — no-op.
  if (name == current_device_) {
    return {};
  }
  if (buffer_ms_ <= 0) {
    return std::unexpected{"miniaudio: device switch requires open() first"};
  }

  // Snapshot the current device so it can be brought back if the new one
  // fails to start (best effort — the sink is left closed only if the
  // restore fails too; the returned error describes whichever state holds).
  const std::string old_device = current_device_;
  const int         old_rate   = rate_;

  const auto opened = open_named(std::string(name), old_rate);
  if (opened) {
    return {};
  }
  const std::string fail = opened.error();

  if (auto restored = open_named(old_device, old_rate)) {
    return std::unexpected{fail + " (previous device restored)"};
  }
  return std::unexpected{
      fail + "; restoring the previous device also failed — sink is closed"};
}

std::size_t MiniaudioSink::writei(std::span<const Frame> frames) {
  if (frames.empty() || closed_.load(std::memory_order_acquire)) return 0;

  const std::size_t mask    = mask_;
  std::size_t       written = 0;
  while (written < frames.size()) {
    if (stopped_.load(std::memory_order_acquire) ||
        closed_.load(std::memory_order_acquire)) {
      // Device not consuming (suspend/close) — return what we wrote so the
      // audio thread can never block forever on a stopped device. Mirrors
      // beep, where a suspended speaker stops pulling and the audio side
      // waits; here the engine polls on the next tick instead.
      break;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t avail = mask + 1 - (head - tail);
    if (avail == 0) {
      // Ring full: wait for the device callback to drain. Lock-free — the
      // callback (or a concurrent suspend/close) makes progress meanwhile.
      std::this_thread::sleep_for(std::chrono::microseconds{100});
      continue;
    }
    const std::size_t n   = std::min(avail, frames.size() - written);
    std::size_t       idx = head & mask;
    std::size_t       done = 0;
    while (done < n) {
      const std::size_t run = std::min(n - done, mask + 1 - idx);
      std::memcpy(ring_.data() + idx, frames.data() + written + done, run * sizeof(Frame));
      done += run;
      idx = (idx + run) & mask;
    }
    head_.store(head + n, std::memory_order_release);
    written += n;
  }
  return written;
}

void MiniaudioSink::suspend() {
  if (!dev_init_) return;
  stopped_.store(true, std::memory_order_release);
  // Port of beep speaker.Suspend (ALSA drop): stop the device, then discard
  // any audio still buffered so stale frames don't play after resume.
  ma_device_stop(&dev_);  // synchronous: no data callback after this returns
  clear_ring();
}

void MiniaudioSink::resume() {
  if (!dev_init_ || closed_.load(std::memory_order_acquire)) return;
  if (ma_device_start(&dev_) == MA_SUCCESS) {
    stopped_.store(false, std::memory_order_release);
  }
}

void MiniaudioSink::close() {
  closed_.store(true, std::memory_order_release);
  stopped_.store(true, std::memory_order_release);
  if (dev_init_) {
    ma_device_uninit(&dev_);  // synchronous: joins the device thread
    dev_init_ = false;
  }
  if (ctx_init_) {
    ma_context_uninit(&ctx_);
    ctx_init_ = false;
  }
  // Note: ring_ memory is deliberately kept alive. close() may race with a
  // writei() already past its closed_ check; freeing the buffer here would be
  // UB, and the ring is tiny (≈ buffer_ms/2 of frames), so we leave it for the
  // next open() to reuse. A subsequent open() resets the cursors, so any
  // straggler writes are never read. The engine joins its audio thread before
  // destroying the sink, so the vector's real teardown happens in the dtor.
  rate_ = 0;
}

std::vector<std::string> MiniaudioSink::list_devices() const {
  const auto            infos = enumerate_playback();
  std::vector<std::string> names;
  names.reserve(infos.size());
  for (const ma_device_info& info : infos) {
    names.emplace_back(info.name);
  }
  return names;
}

}  // namespace

std::shared_ptr<AudioSink> make_miniaudio_sink() {
  return std::make_shared<MiniaudioSink>();
}

std::expected<int, std::string> miniaudio_default_sample_rate(int preferred) {
  ma_context ctx;
  ma_result  rc = ma_context_init(nullptr, 0, nullptr, &ctx);
  if (rc != MA_SUCCESS) return std::unexpected{ma_err(rc)};
  int rate = -1;
  ma_device_info info;
  rc = ma_context_get_device_info(&ctx, ma_device_type_playback, nullptr, &info);
  if (rc == MA_SUCCESS) {
    rate = pick_native_rate(info, preferred);
  }
  ma_context_uninit(&ctx);
  if (rate < 0) return std::unexpected{"miniaudio: no default playback device"};
  return rate;
}

}  // namespace bootamp::audio

#else  // !__has_include(<miniaudio.h>)

// miniaudio.h not installed: link-safe fallbacks so bootamp_audio still
// builds without the package. main.cpp's BOOTAMP_APP_HAS_MINIAUDIO guard
// already routes to make_null_sink() in this build; these definitions only
// satisfy the linker (and never run).

#include "audio/audio_sink.hpp"

namespace bootamp::audio {

std::shared_ptr<AudioSink> make_miniaudio_sink() { return make_null_sink(); }

std::expected<int, std::string> miniaudio_default_sample_rate(int preferred) {
  return preferred;
}

}  // namespace bootamp::audio

#endif  // __has_include(<miniaudio.h>)
