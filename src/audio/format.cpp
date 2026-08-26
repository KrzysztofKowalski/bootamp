// audio/format.cpp — AudioFormat helpers.
//
// Faithful port of the beep format helpers cliamp uses (gopxl/beep
// Format/SampleRate, github.com/gopxl/beep/v2 buffer.go):
//   - frame_size   = Format.Width()          (NumChannels × Precision bytes)
//   - frames_in    = SampleRate.N(duration)  (truncating frame count)
//   - duration_of  = SampleRate.D(frames)    (frames → seconds)
//
// SampleRate.N(d) is `int(d_ns * sr / 1e9)` — integer math truncating toward
// zero. The double overload below reproduces that truncation for the
// chrono::duration<double> values the engine deals in (ms-sized buffers at
// kHz rates are exactly representable, so the double product is exact).
#include "audio/format.hpp"

namespace bootamp::audio {

std::size_t frame_size(const AudioFormat& f) noexcept {
  // Port of beep (Format).Width(): NumChannels * Precision bytes per frame.
  return static_cast<std::size_t>(f.channels) * static_cast<std::size_t>(f.precision);
}

std::size_t frames_in(const AudioFormat& f, std::chrono::duration<double> d) noexcept {
  // Port of beep SampleRate.N(d): int(d * sr / time.Second), truncating.
  if (f.sample_rate <= 0) return 0;
  const double n = d.count() * static_cast<double>(f.sample_rate);
  if (n <= 0.0) return 0;
  return static_cast<std::size_t>(n);
}

std::chrono::duration<double> duration_of(const AudioFormat& f, std::size_t frames) noexcept {
  // Port of beep SampleRate.D(n): time.Second * n / sr. Returned as exact
  // double seconds rather than Go's integer nanoseconds.
  if (f.sample_rate <= 0) return std::chrono::duration<double>::zero();
  return std::chrono::duration<double>{static_cast<double>(frames) /
                                       static_cast<double>(f.sample_rate)};
}

}  // namespace bootamp::audio
