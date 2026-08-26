// audio/audio_sink.hpp — AudioSink interface (decoupled from miniaudio).
//
// Per the plan debt fix: the AudioSink is injected so tests use NullSink and
// the engine_test doesn't depend on a real device. MiniaudioSink is the
// production impl; the miniaudio data callback drains the engine's SPSC ring.
// open() takes the device sample rate (miniaudio opened at device rate with
// its resampling OFF — the engine's swresample stage already matched the rate).
#pragma once

#include "audio/format.hpp"
#include "audio/streamer.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::audio {

// AudioSink is the abstract audio output device. The engine pushes decoded
// frames via writei(); the sink's own thread (or callback) pulls them out.
class AudioSink {
public:
  virtual ~AudioSink() = default;

  // open initializes the device at `fmt.sample_rate`, stereo, with a buffer of
  // `buffer_ms` milliseconds. Returns an error message on failure.
  virtual std::expected<void, std::string> open(const AudioFormat& fmt, int buffer_ms) = 0;

  // sample_rate returns the device sample rate actually opened (0 = not open).
  // MiniaudioSink opens at a native device rate with resampling OFF, which may
  // differ from the fmt.sample_rate requested in open(); the engine must
  // configure its swresample stage against this value. Default: 0 (unknown).
  virtual int sample_rate() const noexcept { return 0; }

  // writei writes up to frames.size() frames to the device buffer; returns the
  // number actually written (may block until space is available). The audio
  // thread is the sole caller — no internal lock needed.
  virtual std::size_t writei(std::span<const Frame> frames) = 0;

  // suspend / resume pause/resume the device (used on engine pause).
  virtual void suspend() = 0;
  virtual void resume() = 0;

  // close releases the device. Idempotent.
  virtual void close() = 0;

  // list_devices returns the available output device names (for config UI).
  // Empty list = system default only.
  virtual std::vector<std::string> list_devices() const = 0;

  // switch_device reopens the output on the device named `name` ("" = the
  // system default), keeping the format policy of the last open() — native
  // rate of the NEW device (sample_rate() reflects it), sink-side resampling
  // off, same buffer_ms/period. Returns {} when `name` already matches the
  // currently open device (no-op). On failure the previous device is kept
  // when possible; otherwise the sink is left closed and the error string
  // explains the resulting state (the engine handles it). Called from the UI
  // thread while the audio thread may be writing — the sink must never block
  // writei() indefinitely across a reopen.
  virtual std::expected<void, std::string> switch_device(std::string_view name) = 0;
};

// NullSink is a no-op sink for tests: writei() discards instantly and returns
// frames.size(); open() always succeeds. Defined inline so tests link without
// any audio backend present.
class NullSink final : public AudioSink {
public:
  std::expected<void, std::string> open(const AudioFormat&, int) override { return {}; }
  std::size_t writei(std::span<const Frame> f) override { return f.size(); }
  void suspend() override {}
  void resume() override {}
  void close() override {}
  std::vector<std::string> list_devices() const override { return {}; }
  // No-op: NullSink has no real device, so switching is a success.
  std::expected<void, std::string> switch_device(std::string_view) override { return {}; }
};

// --- Sink factories (implemented in audio_sink_null.cpp / audio_sink_miniaudio.cpp) ---

// make_null_sink returns a shared NullSink (no-op device) for tests and
// headless runs. NullSink itself is defined inline above so test binaries
// compile without linking any audio backend.
std::shared_ptr<AudioSink> make_null_sink();

// make_miniaudio_sink returns the production miniaudio-backed sink. The device
// is opened at a native rate with miniaudio's resampling OFF (see
// audio_sink_miniaudio.cpp). Declared here — not in the .cpp — so the app can
// construct it without pulling in miniaudio.h.
std::shared_ptr<AudioSink> make_miniaudio_sink();

// miniaudio_default_sample_rate probes the default playback device's native
// sample rate, preferring `preferred` when the device supports it natively
// (44.1k/48k usually both). Lets the engine auto-detect the device rate
// (EngineConfig.sample_rate == 0) before open() so swresample can be
// configured to match. Returns an error message when no device is available.
std::expected<int, std::string> miniaudio_default_sample_rate(int preferred);

}  // namespace bootamp::audio