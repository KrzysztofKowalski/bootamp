// audio/live_prefetch.cpp — SPSC prefetch ring for live/infinite streams.
//
// Port of cliamp/player/live_prefetch.go. A background jthread (fill_loop)
// pulls frames from the source and tops up a 4s ring; stream() serves buffered
// frames or immediate silence — the audio callback never blocks on a network
// read. After an underrun, playback resumes once 500ms of buffered audio has
// accumulated again, with a 64-sample linear fade-in; the tail of a partial
// read is faded out and the remainder of the buffer zeroed, so slow chunk
// delivery does not alternate rapidly between data and silence.
//
// Concurrency (plan-locked): all state is guarded by mu_; the fill thread
// blocks on cond_ only when the ring is full, and stop_token (jthread) +
// closed_ unblock it at close/destruction. stream() takes no locks beyond
// mu_, never allocates, and never waits on I/O. NOTE (same contract as Go's
// Close): if the fill thread is blocked inside src_->stream() — e.g. a
// network read — the pipeline must close/release the source before the
// LivePrefetch is destroyed, otherwise the join waits for that read.
#include "audio/live_prefetch.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace bootamp::audio {

namespace {

// Chunk size for source reads (Go's fill() reads 4096 frames at a time).
constexpr std::size_t kFillChunkFrames = 4096;

// Zero stereo frame — underrun output is silence.
constexpr Frame kSilentFrame = {0.0f, 0.0f};

// Ring capacity: livePrefetchBufferDuration (4s) of stereo frames at rate
// (Go: max(sampleRate.N(livePrefetchBufferDuration), 1)).
std::size_t buffer_frames(int sample_rate) {
  const std::size_t rate = static_cast<std::size_t>(std::max(sample_rate, 0));
  const std::size_t frames =
      rate * static_cast<std::size_t>(kLivePrefetchBufferDuration.count()) / 1000;
  return std::max(frames, std::size_t{1});
}

// Resume threshold: livePrefetchResumeDuration (500ms) of frames, clamped to
// [1, capacity] (Go: min(max(sampleRate.N(resume), 1), capacity)).
std::size_t resume_frames(int sample_rate, std::size_t capacity) {
  const std::size_t rate = static_cast<std::size_t>(std::max(sample_rate, 0));
  const std::size_t frames =
      rate * static_cast<std::size_t>(kLivePrefetchResumeDuration.count()) / 1000;
  return std::min(std::max(frames, std::size_t{1}), capacity);
}

}  // namespace

LivePrefetch::LivePrefetch(std::shared_ptr<Streamer> src, int sample_rate)
    : src_(std::move(src)),
      sample_rate_(sample_rate),
      capacity_(buffer_frames(sample_rate)),
      resume_at_(resume_frames(sample_rate, capacity_)) {
  buf_.resize(capacity_);
  // buffering_ defaults to true (Go: newLivePrefetchStreamer sets
  // buffering: true): the first stream() calls return silence until at least
  // resume_at_ frames have accumulated.
  fill_thread_ = std::jthread([this](std::stop_token stoken) { fill_loop(stoken); });
}

LivePrefetch::~LivePrefetch() {
  {
    std::lock_guard lock(mu_);
    closed_ = true;
  }
  cond_.notify_all();
  // fill_thread_ is a std::jthread: destruction requests stop (waking a fill
  // blocked in cond_.wait via the stop_token overload) and joins. If the fill
  // is blocked inside src_->stream(), the pipeline must have closed the source
  // first (see file comment).
}

std::pair<std::size_t, bool> LivePrefetch::stream(std::span<Frame> dst) {
  std::unique_lock lock(mu_);

  const std::size_t available =
      full_ ? capacity_ : (w_ >= r_ ? w_ - r_ : capacity_ - (r_ - w_));

  // Buffering below the resume threshold: serve silence, never block (Go's
  // "Stream never blocks" — the audio callback must not wait on the network).
  if (buffering_ && !done_ && available < resume_at_) {
    std::fill(dst.begin(), dst.end(), kSilentFrame);
    was_silent_ = true;
    return {dst.size(), true};
  }
  if (available == 0) {
    if (done_) {
      // End of stream; err_ already holds the source error, if any.
      return {0, false};
    }
    std::fill(dst.begin(), dst.end(), kSilentFrame);
    buffering_ = true;
    was_silent_ = true;
    return {dst.size(), true};
  }
  buffering_ = false;

  const std::size_t n = std::min(dst.size(), available);
  const std::size_t first = std::min(n, capacity_ - r_);
  std::memcpy(dst.data(), buf_.data() + r_, first * sizeof(Frame));
  std::memcpy(dst.data() + first, buf_.data(), (n - first) * sizeof(Frame));
  r_ = (r_ + n) % capacity_;
  consumed_ += n;
  full_ = false;
  cond_.notify_all();  // wake a fill blocked on the full ring

  if (was_silent_) {
    // Fade-in after an underrun (64 samples, linear ramp). Ramp math in
    // double to match Go's float64 gains, narrowed to float32 afterwards.
    const std::size_t fade_len = std::min(n, kLivePrefetchFadeSamples);
    for (std::size_t i = 0; i < fade_len; ++i) {
      const float gain =
          static_cast<float>(static_cast<double>(i) / static_cast<double>(fade_len));
      dst[i][0] *= gain;
      dst[i][1] *= gain;
    }
  }

  const std::size_t short_read = dst.size() - n;
  if (short_read > 0 && n > 0) {
    // Fade out the tail of a partial read, then zero the rest (Go's
    // short > 0 tail fade + clear(samples[n:])).
    const std::size_t fade_len = std::min(n, kLivePrefetchFadeSamples);
    for (std::size_t i = 0; i < fade_len; ++i) {
      const float gain = static_cast<float>(
          static_cast<double>(fade_len - 1 - i) / static_cast<double>(fade_len));
      dst[n - fade_len + i][0] *= gain;
      dst[n - fade_len + i][1] *= gain;
    }
  }
  std::fill(dst.begin() + static_cast<std::ptrdiff_t>(n), dst.end(), kSilentFrame);

  was_silent_ = short_read > 0;
  if (short_read > 0 && !done_) {
    buffering_ = true;
  }
  return {dst.size(), true};
}

std::string LivePrefetch::err() const {
  std::lock_guard lock(mu_);
  return err_;
}

std::size_t LivePrefetch::position_frames() const {
  std::lock_guard lock(mu_);
  return consumed_;
}

void LivePrefetch::close() {
  {
    std::lock_guard lock(mu_);
    closed_ = true;
  }
  cond_.notify_all();
  // Does NOT join (cliamp livePrefetchStreamer.Close): if the fill thread is
  // blocked inside src_->stream(), the caller must close the source's decoder
  // first so the read returns, then call wait() to join.
}

void LivePrefetch::wait() {
  if (fill_thread_.joinable()) {
    fill_thread_.join();
  }
}

void LivePrefetch::fill_loop(std::stop_token stoken) {
  std::vector<Frame> chunk(kFillChunkFrames);

  const auto available_to_write = [this]() -> std::size_t {
    if (full_) {
      return 0;
    }
    return w_ >= r_ ? capacity_ - (w_ - r_) : r_ - w_;
  };

  for (;;) {
    {
      std::lock_guard lock(mu_);
      if (closed_) {
        return;
      }
    }

    const auto [n, ok] = src_->stream(chunk);
    if (n > 0) {
      // write(chunk[:n]): copy into the ring, waiting (stop_token-aware) only
      // when the ring is full. Returns when closed/stopped.
      std::span<const Frame> samples(chunk.data(), n);
      while (!samples.empty()) {
        std::unique_lock lock(mu_);
        cond_.wait(lock, stoken, [&] {
          return closed_ || stoken.stop_requested() || available_to_write() != 0;
        });
        if (closed_ || stoken.stop_requested()) {
          return;
        }
        const std::size_t m = std::min(samples.size(), available_to_write());
        const std::size_t first = std::min(m, capacity_ - w_);
        std::memcpy(buf_.data() + w_, samples.data(), first * sizeof(Frame));
        std::memcpy(buf_.data(), samples.data() + first, (m - first) * sizeof(Frame));
        w_ = (w_ + m) % capacity_;
        if (m > 0 && w_ == r_) {
          full_ = true;
        }
        samples = samples.subspan(m);
        cond_.notify_all();
      }
    }

    if (ok) {
      continue;
    }

    // End of stream: record the source error (if any) and wake consumers so
    // they can drain the remaining ring and observe (0, false).
    const std::string source_err = src_->err();
    {
      std::lock_guard lock(mu_);
      done_ = true;
      err_ = source_err;
    }
    cond_.notify_all();
    return;
  }
}

}  // namespace bootamp::audio
