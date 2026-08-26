// audio/streamer.hpp — the Streamer interface (audio hot path).
//
// cliamp's beep.Streamer returns (n, ok); bootamp's Streamer returns the number
// of stereo frames written and a bool that is false at end-of-stream. The
// signature uses std::span (no raw arrays). Implementations: native decoders
// (libsndfile/libFLAC/libvorbis), ffmpeg pipe, yt-dlp pipe, live-prefetch
// wrapper, gapless sequencer. No exceptions on the hot path — errors surface
// via err().
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bootamp::audio {

// Frame is one stereo float32 sample. The whole engine is stereo float32.
using Frame = std::array<float, 2>;

// Streamer is the audio-source interface. stream() writes up to dst.size()
// frames and returns the count written; ok==false marks end-of-stream.
// Implementations MUST NOT throw — record the error in err() instead.
class Streamer {
public:
  virtual ~Streamer() = default;
  // Returns (frames_written, more). On EOF, frames_written may be < dst.size()
  // and more==false. Silence-fill is the caller's responsibility (gapless).
  virtual std::pair<std::size_t, bool> stream(std::span<Frame> dst) = 0;
  // err() returns the first non-EOF error recorded on the hot path, or "".
  virtual std::string err() const = 0;
};

// --- Streamer helpers (implemented in streamer.cpp) --------------------------
//
// Ports of the beep helpers cliamp builds on (gopxl/beep/v2 streamers.go,
// buffer.go): Silence, StreamerFunc, and Buffer.Streamer. All stream()
// implementations are noexcept — the audio thread never throws.

// fill_silence zeroes dst. Hot-path helper (gapless idle, underrun fill).
void fill_silence(std::span<Frame> dst) noexcept;

// StreamerFunc wraps a callable as a Streamer (port of beep.StreamerFunc).
// err() always returns "" — the callable reports errors itself.
class StreamerFunc final : public Streamer {
public:
  using Fn = std::function<std::pair<std::size_t, bool>(std::span<Frame>)>;
  explicit StreamerFunc(Fn fn);
  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override;
  std::string err() const override;

private:
  Fn fn_;
};

// SilenceStreamer streams `frames` frames of silence, then EOF (port of
// beep.Silence): frames == 0 ⇒ immediate EOF; frames < 0 ⇒ silence forever.
class SilenceStreamer final : public Streamer {
public:
  explicit SilenceStreamer(std::ptrdiff_t frames) noexcept : remaining_(frames) {}
  std::pair<std::size_t, bool> stream(std::span<Frame> dst) noexcept override;
  std::string err() const override { return {}; }

private:
  std::ptrdiff_t remaining_;
};

// make_silence_streamer returns a shared SilenceStreamer (frames < 0 = forever).
std::shared_ptr<Streamer> make_silence_streamer(std::ptrdiff_t frames);

}  // namespace bootamp::audio