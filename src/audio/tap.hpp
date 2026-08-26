// audio/tap.hpp — SPSC overwrite ring capturing pre-volume samples for the UI.
//
// Port of cliamp/player/tap.go. The audio thread (sole writer) copies each
// stereo frame into a fixed-size overwrite ring; the visualizer thread reads
// mono/waveform/stereo snapshots without a lock. Minor tearing at the read
// boundary is invisible in the visualizer. Three read helpers mirror the Go
// methods: SamplesInto (mono mix), WaveformSamplesInto (real-time position),
// StereoSamplesInto (raw stereo).
#pragma once

#include "audio/streamer.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace bootamp::audio {

// Tap wraps a Streamer and copies every frame into an overwrite ring. The
// write position is atomic; readers race-free modulo single-frame tearing.
class Tap {
public:
  // Construct a tap of capacity `size` frames at `sample_rate`.
  Tap(std::size_t size, int sample_rate);

  // stream passes through `src` while copying frames into the ring. The
  // wrapped streamer's ok flag is propagated; silence-fill is the caller's job.
  std::pair<std::size_t, bool> stream(std::span<Frame> dst);

  // The wrapped streamer's error, forwarded.
  std::string err() const { return src_ ? src_->err() : std::string{}; }

  // set_source attaches/replaces the wrapped streamer (the gapless sequencer
  // swaps the source on track change). nullptr ⇒ stream() returns silence.
  void set_source(Streamer* s) { src_ = s; }

  // samples_into copies a mono mix ((l+r)/2) of the last dst.size() frames
  // into dst, returning the number actually copied. Matches cliamp SamplesInto.
  std::size_t samples_into(std::span<float> dst) const;

  // waveform_samples_into copies samples from the most recent output buffer at
  // its real-time playback position, keeping raw visualizers moving between
  // audio-backend refills. Matches cliamp WaveformSamplesInto.
  std::size_t waveform_samples_into(std::span<float> dst) const;

  // stereo_samples_into copies the last dst.size() stereo frames into dst.
  std::size_t stereo_samples_into(std::span<Frame> dst) const;

  int sample_rate() const noexcept { return sample_rate_; }
  std::size_t size() const noexcept { return buf_.size(); }

private:
  // Shared by samples_into / waveform_samples_into: mono-mix ring read ending
  // at absolute frame index `end` (port of Go samplesIntoAt).
  std::size_t samples_into_at_impl(std::span<float> dst, std::int64_t end) const;

  Streamer*                                  src_         = nullptr;
  std::vector<Frame>                         buf_;
  std::size_t                                size_;
  int                                        sample_rate_;
  std::atomic<std::int64_t>                   pos_         {0};  // write cursor
  std::atomic<std::int64_t>                   written_     {0};  // total frames
  std::atomic<std::int64_t>                   write_at_    {0};  // ns of latest write
  std::atomic<std::int64_t>                   write_frames_{0};  // frames in latest write
  std::function<std::chrono::steady_clock::time_point()> now_;
};

}  // namespace bootamp::audio