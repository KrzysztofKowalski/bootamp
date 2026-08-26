// src/audio/gapless.cpp — zero-gap track sequencer (current/next atomic swap).
//
// Port of cliamp/player/gapless.go (gaplessStreamer). Gapless sits at the
// bottom of the audio pipeline above the sink; stream() always fills dst
// completely (silence when nothing is playing), so the sink never stalls and
// the streamer never stops: it returns (dst.size(), true) unconditionally.
// On current-track exhaustion it continues seamlessly from the preloaded next
// track and fires on_swap(token) so the engine can close the drained pipeline
// and publish the new one (Go: player.go:102-108 handleGaplessSwap).
//
// The plan's debt fix vs Go: cliamp serializes every operation with a mutex;
// here the audio thread takes NO locks. current/next are
// atomic<shared_ptr<Streamer>>, and a monotonic generation counter
// (current_version_) detects a manual replacement (replace/clear) during a
// read. The gapless transition itself commits with a compare_exchange on
// current_: if a replacement landed while the stale streamer was reading, the
// CAS fails and the stale read falls back to silence without touching next_ —
// a stale read can never clobber a manual replacement's state (Go's
// TestGaplessManualReplaceWinsOverExhaustedStream). Every operation is
// lock-free; no exceptions on this path (the on_swap callback must not
// throw).

#include "audio/gapless.hpp"

#include <cstdint>
#include <utility>

namespace bootamp::audio {
namespace {

// Fill [from, dst.size()) with silence (Go's `clear(samples[n:])`). Full
// fills go through the shared audio::fill_silence helper from streamer.hpp.
void silence_tail(std::span<Frame> dst, std::size_t from) {
  for (std::size_t i = from; i < dst.size(); ++i) {
    dst[i] = Frame{};
  }
}

}  // namespace

std::pair<std::size_t, bool> Gapless::stream(std::span<Frame> dst) {
  // Snapshot the active streamer with its generation. Version first: reading
  // a fresh version makes the paired current_ store visible (acquire); a
  // stale version can only pair with the current-or-previous streamer, and
  // the exhaustion check below falls back to silence in that case. Either
  // way the refcount keeps the streamed object alive.
  const std::uint64_t version = current_version_.load(std::memory_order_acquire);
  const std::shared_ptr<Streamer> cur = current_.load(std::memory_order_acquire);

  if (!cur) {
    // No active track — fill silence (Go: clear(samples); drained untouched).
    fill_silence(dst);
    return {dst.size(), true};
  }

  auto [n, more] = cur->stream(dst);

  // Go: `if !ok || n < len(samples)` — only a full, ok read skips the
  // transition logic; a partial fill swaps even while ok stays true.
  if (more && n == dst.size()) {
    return {n, true};
  }

  if (current_version_.load(std::memory_order_acquire) != version) {
    // A manual replacement won while the old streamer was reading. It owns
    // the next slot and the swap callback; the stale read must not clobber
    // anything. Keep the frames already read, silence the rest (Go:
    // clear(samples[n:]) and return).
    silence_tail(dst, n);
    return {dst.size(), true};
  }

  // Read the (next, token) pair. set_next stores the streamer before the
  // token (release); reading the token first with acquire means a fresh
  // token can never be paired with the previous streamer (the acquire load
  // makes the streamer store visible). A torn read can only pair the fresh
  // streamer with the previous token — which is exactly the pipeline that is
  // being drained, so the on_swap callback stays consistent.
  const std::uint64_t token = next_token_.load(std::memory_order_acquire);
  std::shared_ptr<Streamer> next = next_.load(std::memory_order_acquire);

  if (!next) {
    // No next track — we've drained. Guard the flag with a verify CAS on
    // current_ (expected == cur, desired == cur): a concurrent replace/clear
    // either fails this CAS (skip the store) or finishes with its own
    // trailing drained_=false store, so a stale read can never leave drained
    // stuck true after a manual switch.
    std::shared_ptr<Streamer> expected = cur;
    if (current_.compare_exchange_strong(expected, cur, std::memory_order_acq_rel)) {
      drained_.store(true, std::memory_order_release);
    }
    silence_tail(dst, n);
    return {dst.size(), true};
  }

  // Commit the gapless transition: CAS current_ from cur to next. If a
  // manual replacement landed after the version check, current_ != cur and
  // the CAS fails — the stale read returns silence and next_ is untouched
  // (the replacement owns the swap).
  std::shared_ptr<Streamer> expected = cur;
  if (!current_.compare_exchange_strong(expected, next, std::memory_order_acq_rel)) {
    silence_tail(dst, n);
    return {dst.size(), true};
  }

  // Committed. Retire the token and clear the next slot — only if it still
  // holds our `next`: a concurrent set_next preload must survive and become
  // the NEXT track (same end state as Go's lock-serialized swap + SetNext).
  // Bump the generation so later reads observe the transition.
  next_token_.store(0, std::memory_order_release);
  next_.compare_exchange_strong(next, nullptr, std::memory_order_acq_rel);
  current_version_.fetch_add(1, std::memory_order_acq_rel);

  // Fill the rest of the buffer from the new current — zero gap. Go ignores
  // the ok value here (`filled, _ := next.Stream(...)`); a next that yields
  // nothing just leaves the tail to the silence fill below.
  if (n < dst.size()) {
    const auto r = next->stream(dst.subspan(n));
    n += r.first;
  }

  // Publish the transition (Go fires onSwap, then clears drained — both
  // after the swap, off the lock). Resource cleanup is the callback's job.
  if (on_swap_) {
    on_swap_(token);
  }
  drained_.store(false, std::memory_order_release);

  silence_tail(dst, n);
  return {dst.size(), true};
}

std::uint64_t Gapless::set_next(std::shared_ptr<Streamer> s) {
  if (!s) {
    next_.store(nullptr, std::memory_order_release);
    next_token_.store(0, std::memory_order_release);
    return 0;
  }
  const std::uint64_t token = next_token_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
  // Streamer first, token second (release) — see the pair-read contract in
  // stream(): a reader may pair the fresh streamer with the previous token,
  // never the reverse.
  next_.store(std::move(s), std::memory_order_release);
  next_token_.store(token, std::memory_order_release);
  return token;
}

void Gapless::replace(std::shared_ptr<Streamer> s) {
  current_.store(std::move(s), std::memory_order_release);
  next_.store(nullptr, std::memory_order_release);
  current_version_.fetch_add(1, std::memory_order_acq_rel);
  next_token_.store(0, std::memory_order_release);
  // Trailing store: a concurrent stale read that snuck past its version
  // check can only set drained=true transiently; this store always wins and
  // leaves the flag false after a manual switch (see stream()'s drain guard).
  drained_.store(false, std::memory_order_release);
}

void Gapless::clear() {
  current_.store(nullptr, std::memory_order_release);
  next_.store(nullptr, std::memory_order_release);
  current_version_.fetch_add(1, std::memory_order_acq_rel);
  next_token_.store(0, std::memory_order_release);
  drained_.store(false, std::memory_order_release);
}

std::shared_ptr<Streamer> Gapless::current() const {
  return current_.load(std::memory_order_acquire);
}

std::shared_ptr<Streamer> Gapless::next() const {
  return next_.load(std::memory_order_acquire);
}

bool Gapless::has_next() const noexcept {
  return next_.load(std::memory_order_acquire) != nullptr;
}

}  // namespace bootamp::audio
