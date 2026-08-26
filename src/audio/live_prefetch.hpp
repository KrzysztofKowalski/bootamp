// audio/live_prefetch.hpp — SPSC prefetch ring for live/infinite streams.
//
// Port of cliamp/player/live_prefetch.go. A background jthread fills a 4s ring
// from the source; stream() returns immediately (silence on underrun) so the
// audio callback never blocks on a network read. 500ms resume threshold, 64-
// sample fade-in/out on underrun recovery. "Stream never blocks." Concurrency:
// std::jthread + std::stop_token + std::condition_variable_any (plan-locked).
#pragma once

#include "audio/streamer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace bootamp::audio {

inline constexpr std::chrono::milliseconds kLivePrefetchBufferDuration{4000};
inline constexpr std::chrono::milliseconds kLivePrefetchResumeDuration {500};
inline constexpr std::size_t               kLivePrefetchFadeSamples    = 64;

// LivePrefetch decodes a live stream off the audio thread. stream() returns
// buffered samples or silence; the fill jthread tops up the ring in the
// background. Close (via stop_token) unblocks a waiting fill.
class LivePrefetch : public Streamer {
public:
  // Construct wrapping `src` at `sample_rate`. Starts the fill jthread.
  LivePrefetch(std::shared_ptr<Streamer> src, int sample_rate);
  ~LivePrefetch() override;

  // stream() — see header comment. Never blocks on network I/O.
  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override;
  std::string err() const override;

  // Position consumed so far (frames → duration at sample_rate).
  std::size_t position_frames() const;

  // close signals the fill loop to stop (sets closed_ + notify_all, like
  // cliamp livePrefetchStreamer.Close) and does NOT join. If the fill thread
  // is blocked in a read on the wrapped source, close the source's decoder
  // afterwards so the read returns, then call wait() to join. Idempotent.
  void close();

  // wait() blocks until the fill jthread exits (for close sequencing).
  void wait();

private:
  void fill_loop(std::stop_token stoken);

  std::shared_ptr<Streamer>                 src_;
  int                                       sample_rate_;
  std::vector<Frame>                        buf_;
  std::size_t                                capacity_  = 0;
  std::size_t                                resume_at_ = 0;
  std::size_t                                r_ = 0, w_ = 0;
  bool                                       full_      = false;
  bool                                       buffering_ = true;
  bool                                       was_silent_= false;
  bool                                       done_      = false;
  bool                                       closed_    = false;
  std::size_t                                consumed_  = 0;
  std::string                                err_;
  mutable std::mutex                         mu_;
  std::condition_variable_any                cond_;
  std::jthread                               fill_thread_;
};

}  // namespace bootamp::audio