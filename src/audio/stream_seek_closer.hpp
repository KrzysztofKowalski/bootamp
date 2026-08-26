// audio/stream_seek_closer.hpp — StreamSeekCloser: Streamer + Len/Position/Seek/Close.
//
// cliamp's beep.StreamSeekCloser adds Len/Position/Seek/Close on top of Streamer.
// bootamp keeps the same surface; seeking for yt-dlp / live radio is handled
// out-of-band by the engine (seek-by-restart), so Seek() on a non-seekable
// stream is a no-op (returns {}).
#pragma once

#include "audio/streamer.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace bootamp::audio {

class StreamSeekCloser : public Streamer {
public:
  // Len returns the total frame count (0 = unknown/unbounded).
  virtual std::size_t len() const = 0;
  // Position returns the current frame index (decoded so far).
  virtual std::size_t position() const = 0;
  // Seek repositions to `frame` (clamped to [0, len()]). Returns an error
  // message on failure, "" on success. No-op for non-seekable streams.
  virtual std::string seek(std::size_t frame) = 0;
  // Close releases all resources (processes, pipes, file handles). Idempotent.
  virtual void close() = 0;
};

// --- StreamSeekCloser helpers (implemented in streamer.cpp) ------------------

// FrameSource is a read-only StreamSeekCloser over a contiguous frame buffer —
// the port of beep Buffer.Streamer (used by tests, gapless probes, golden
// replay). Two ownership modes: a borrowed span (the caller must keep the data
// alive) or an owning shared vector. close() is a no-op; seek() clamps to
// [0, len()] per the contract above. Stream() never throws.
class FrameSource final : public StreamSeekCloser {
public:
  explicit FrameSource(std::span<const Frame> data);
  explicit FrameSource(std::shared_ptr<const std::vector<Frame>> data);

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) noexcept override;
  std::string err() const override { return {}; }
  std::size_t len() const noexcept override;
  std::size_t position() const noexcept override;
  std::string seek(std::size_t frame) noexcept override;
  void close() noexcept override {}

private:
  std::shared_ptr<const std::vector<Frame>> owner_;  // null when borrowed
  std::span<const Frame>                    data_;
  std::size_t                               pos_ = 0;
};

// make_frame_source builds a shared FrameSource from a borrowed span or an
// owning shared vector.
std::shared_ptr<StreamSeekCloser> make_frame_source(std::span<const Frame> data);
std::shared_ptr<StreamSeekCloser> make_frame_source(std::shared_ptr<const std::vector<Frame>> data);

}  // namespace bootamp::audio