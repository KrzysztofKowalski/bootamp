// audio/tap.cpp — Tap implementation. Port of cliamp/player/tap.go.
//
// The audio thread is the sole writer: every frame pulled through is copied
// into a fixed-size overwrite ring. The visualizer thread reads mono/waveform/
// stereo snapshots without a lock; minor tearing at the read boundary is
// invisible in visualization. All four atomics are written in Go's order
// (writeFrames → writeAt → pos → written) so the waveform clock sees a
// consistent (frames, timestamp) pair.

#include "audio/tap.hpp"

#include <algorithm>
#include <cstdint>

namespace bootamp::audio {

namespace {
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

std::int64_t to_ns(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             t.time_since_epoch())
      .count();
}

std::size_t to_sz(std::int64_t v) {
  return static_cast<std::size_t>(v < 0 ? 0 : v);
}
}  // namespace

Tap::Tap(std::size_t size, int sample_rate)
    : buf_(size), size_(size), sample_rate_(sample_rate),
      now_(std::chrono::steady_clock::now) {}

std::pair<std::size_t, bool> Tap::stream(std::span<Frame> dst) {
  if (!src_) {
    // No source attached (gapless swap window): pass silence through.
    fill_silence(dst);
    return {dst.size(), true};
  }
  const auto [n, ok] = src_->stream(dst);
  std::int64_t p = pos_.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < n; ++i) {
    buf_[to_sz(p)] = dst[i];
    p = (p + 1) % static_cast<std::int64_t>(size_);
  }
  write_frames_.store(static_cast<std::int64_t>(n), std::memory_order_relaxed);
  write_at_.store(to_ns(now_()), std::memory_order_relaxed);
  pos_.store(p, std::memory_order_relaxed);
  written_.fetch_add(static_cast<std::int64_t>(n), std::memory_order_relaxed);
  return {n, ok};
}

std::size_t Tap::samples_into(std::span<float> dst) const {
  return samples_into_at_impl(dst, written_.load(std::memory_order_relaxed));
}

std::size_t Tap::waveform_samples_into(std::span<float> dst) const {
  const std::int64_t frames = write_frames_.load(std::memory_order_relaxed);
  if (frames <= 0 || sample_rate_ <= 0) return 0;
  const std::int64_t elapsed_ns = to_ns(now_()) -
      write_at_.load(std::memory_order_relaxed);
  const std::int64_t elapsed =
      elapsed_ns * sample_rate_ / kNsPerSecond;  // frames since the last write
  const std::int64_t clamped = std::clamp(elapsed, std::int64_t{0}, frames);
  const std::int64_t end =
      written_.load(std::memory_order_relaxed) - frames + clamped;
  return samples_into_at_impl(dst, end);
}

std::size_t Tap::stereo_samples_into(std::span<Frame> dst) const {
  const std::size_t n = std::min(dst.size(), size_);
  if (n == 0) return 0;
  const std::int64_t p = pos_.load(std::memory_order_relaxed);
  const std::size_t start =
      to_sz(p - static_cast<std::int64_t>(n) + static_cast<std::int64_t>(size_)) %
      size_;
  const std::size_t first = std::min(n, size_ - start);
  std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(start),
            buf_.begin() + static_cast<std::ptrdiff_t>(start + first),
            dst.begin());
  std::copy(buf_.begin(),
            buf_.begin() + static_cast<std::ptrdiff_t>(n - first),
            dst.begin() + static_cast<std::ptrdiff_t>(first));
  return n;
}

std::size_t Tap::samples_into_at_impl(std::span<float> dst,
                                      std::int64_t end) const {
  std::size_t n = std::min(dst.size(), size_);
  if (n == 0 || end <= 0) return 0;
  n = std::min(n, static_cast<std::size_t>(end));
  const std::int64_t start =
      (end - static_cast<std::int64_t>(n)) % static_cast<std::int64_t>(size_);
  const std::size_t s = to_sz(start);
  const std::size_t first = std::min(n, size_ - s);
  // Mono mix (l+r)/2, ring read without per-sample modulo (Go's two-copy loop).
  for (std::size_t i = 0; i < first; ++i) {
    dst[i] = (buf_[s + i][0] + buf_[s + i][1]) * 0.5f;
  }
  for (std::size_t i = 0; i < n - first; ++i) {
    dst[first + i] = (buf_[i][0] + buf_[i][1]) * 0.5f;
  }
  return n;
}

}  // namespace bootamp::audio
