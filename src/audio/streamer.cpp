// audio/streamer.cpp — Streamer / StreamSeekCloser helpers.
//
// Faithful ports of the beep helpers cliamp builds on (gopxl/beep/v2):
//   - StreamerFunc      (interface.go:96)   callable → Streamer
//   - Silence           (streamers.go:7)    N frames of silence then EOF
//   - Buffer.Streamer   (buffer.go:206)     read-only seekable frame source
//
// All stream() implementations here are noexcept: the audio thread never
// throws. Errors surface via err() (always "" for these helpers, matching
// beep's Err() behavior).
#include "audio/streamer.hpp"
#include "audio/stream_seek_closer.hpp"

#include <algorithm>
#include <utility>

namespace bootamp::audio {

void fill_silence(std::span<Frame> dst) noexcept {
  std::fill(dst.begin(), dst.end(), Frame{0.0f, 0.0f});
}

// --- StreamerFunc (port of beep.StreamerFunc) --------------------------------

StreamerFunc::StreamerFunc(Fn fn) : fn_(std::move(fn)) {}

std::pair<std::size_t, bool> StreamerFunc::stream(std::span<Frame> dst) {
  return fn_(dst);
}

std::string StreamerFunc::err() const {
  // Port of beep.StreamerFunc.Err: always nil — the callable reports errors.
  return {};
}

// --- SilenceStreamer (port of beep.Silence) ----------------------------------

std::pair<std::size_t, bool> SilenceStreamer::stream(std::span<Frame> dst) noexcept {
  // beep.Silence: num == 0 ⇒ (0, false); 0 < num < len ⇒ truncate; num < 0 ⇒
  // forever (remaining_ never decremented below 0 once negative).
  if (remaining_ == 0) return {0, false};
  std::size_t n = dst.size();
  if (remaining_ > 0 && remaining_ < static_cast<std::ptrdiff_t>(n)) {
    n = static_cast<std::size_t>(remaining_);
  }
  fill_silence(dst.first(n));
  if (remaining_ > 0) remaining_ -= static_cast<std::ptrdiff_t>(n);
  return {n, true};
}

std::shared_ptr<Streamer> make_silence_streamer(std::ptrdiff_t frames) {
  return std::make_shared<SilenceStreamer>(frames);
}

// --- FrameSource (port of beep Buffer.Streamer / bufferStreamer) -------------

FrameSource::FrameSource(std::span<const Frame> data) : data_(data) {}

FrameSource::FrameSource(std::shared_ptr<const std::vector<Frame>> data)
  : owner_(std::move(data)), data_(*owner_) {}

std::pair<std::size_t, bool> FrameSource::stream(std::span<Frame> dst) noexcept {
  // beep bufferStreamer.Stream: read until dst or the buffer is exhausted;
  // ok==false only on a call that starts at (or past) EOF.
  if (pos_ >= data_.size()) return {0, false};
  const std::size_t n = std::min(dst.size(), data_.size() - pos_);
  std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), n, dst.begin());
  pos_ += n;
  return {n, true};
}

std::size_t FrameSource::len() const noexcept {
  return data_.size();
}

std::size_t FrameSource::position() const noexcept {
  return pos_;
}

std::string FrameSource::seek(std::size_t frame) noexcept {
  // beep returned an error for out-of-range seeks; bootamp's StreamSeekCloser
  // contract clamps to [0, len()] instead.
  pos_ = std::min(frame, data_.size());
  return {};
}

std::shared_ptr<StreamSeekCloser> make_frame_source(std::span<const Frame> data) {
  return std::make_shared<FrameSource>(data);
}

std::shared_ptr<StreamSeekCloser> make_frame_source(std::shared_ptr<const std::vector<Frame>> data) {
  return std::make_shared<FrameSource>(std::move(data));
}

}  // namespace bootamp::audio
