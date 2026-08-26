// audio/gapless.hpp — zero-gap track sequencer (current/next atomic swap).
//
// Port of cliamp/player/gapless.go. Sits at the bottom of the pipeline above
// the AudioSink. stream() always fills `dst` completely (silence when idle),
// so the sink never stalls. On current-track exhaustion it seamlessly
// continues from the preloaded next track and fires on_swap(token) so the
// engine can close the old pipeline and publish the new state. The plan's
// debt fix: the current/next swap uses atomic<shared_ptr<Streamer>> instead
// of Go's mutex + version counter (the audio thread takes NO locks).
#pragma once

#include "audio/streamer.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>

namespace bootamp::audio {

// Gapless sequences two streamers with zero gap.
class Gapless {
public:
  using SwapCallback = std::function<void(std::uint64_t token)>;

  Gapless() = default;
  explicit Gapless(SwapCallback on_swap) : on_swap_(std::move(on_swap)) {}

  // stream fills dst with audio, swapping to next on exhaustion. Returns
  // (frames_written, true) always — gapless never stops the sink.
  std::pair<std::size_t, bool> stream(std::span<Frame> dst);

  // set_next preloads the next streamer; returns the token identifying this
  // pipeline (0 when s==nullptr). The token is passed to on_swap on transition.
  std::uint64_t set_next(std::shared_ptr<Streamer> s);

  // replace interrupts current and starts `s` immediately (manual skip/prev).
  void replace(std::shared_ptr<Streamer> s);

  // clear removes both current and next; outputs silence until set.
  void clear();

  // drained reports whether current ended with no next queued.
  bool drained() const noexcept { return drained_.load(std::memory_order_acquire); }

  // has_next reports whether a next track is preloaded.
  bool has_next() const noexcept;

  // current / next peek the active streamers (for position/duration queries).
  std::shared_ptr<Streamer> current() const;
  std::shared_ptr<Streamer> next() const;

private:
  std::atomic<std::shared_ptr<Streamer>> current_{nullptr};
  std::atomic<std::shared_ptr<Streamer>> next_{nullptr};
  std::atomic<std::uint64_t>              current_version_{0};
  std::atomic<std::uint64_t>              next_token_{0};
  std::atomic<std::uint64_t>              next_token_seq_{0};
  std::atomic<bool>                       drained_{false};
  SwapCallback                            on_swap_;
};

}  // namespace bootamp::audio