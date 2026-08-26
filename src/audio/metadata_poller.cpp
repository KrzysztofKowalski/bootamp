// audio/metadata_poller.cpp — background ICY/Vorbis title surfacing.
//
// Port of cliamp's metadata poller (player.go pollStreamMetadata +
// setStreamTitle). In cliamp the ICY reader goroutine stores titles into
// streamTitle (atomic.Value) and a second goroutine polls a resolver API on an
// interval for stations without inline ICY metadata. Here the poller is the
// surface layer: it watches the engine's
// atomic<shared_ptr<const string>> stream_title (written on the data path by
// the ICY reader / Vorbis comment parsing) and copies each *change* out to the
// UI Notifier callback, so the status line updates without the UI thread
// touching audio state.
//
// Semantics (1:1 with the Go title path):
//   * non-empty titles are surfaced as soon as they appear (wake() makes the
//     wait return immediately, mirroring "fetch immediately then on tick");
//   * an unchanged value produces no callback (no spurious UI churn — the
//     engine may re-publish the same title on every ICY metadata block);
//   * a transition to "no title" (null atomic) surfaces "" so a stale ICY
//     title cannot clobber the next track's status line — the UI guards with
//     apply_stream_title (daemon.go:1099): stream title is only shown for
//     stream tracks.
//
// The thread parks in condvar_any::wait_for (stop_token-aware); the 1s
// deadline is a safety net for races where wake() lands between the flag clear
// and the next wait — the loop body re-reads the atomic on every iteration, so
// no title update is ever lost. Destructor: jthread requests stop and joins
// (wait_for unblocks via the stop_token). No locks on any audio-thread path —
// this thread only reads the atomic (loads of shared_ptr are lock-free w.r.t.
// the writer).
#include "audio/metadata_poller.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace bootamp::audio {

namespace {

// Poll deadline: how long the thread may park before re-checking the atomic.
// ICY titles arrive on the data path and wake() fires immediately, so this is
// only a bound on surfacing latency after a title write that raced the wait.
constexpr auto kPollInterval = std::chrono::milliseconds(1000);

}  // namespace

MetadataPoller::MetadataPoller(SourceRef source, Callback cb)
    : source_(source),
      cb_(std::move(cb)),
      thread_([this](std::stop_token stoken) { loop(std::move(stoken)); }) {}

MetadataPoller::~MetadataPoller() {
  // Wake any in-flight wait so the join is prompt; the jthread destructor
  // requests stop (also unblocking wait_for) and joins. Member order
  // (thread_ before cond_/mu_) guarantees the join happens before the
  // synchronization objects are destroyed.
  wake();
}

void MetadataPoller::wake() {
  // Lock-free: the atomic flag is part of the wait predicate, notify_all is
  // safe without holding the mutex. If this lands before the waiter's next
  // wait_for, the predicate sees wake_==true and returns immediately; if it
  // lands mid-wait, notify_all wakes it. The loop body always re-reads the
  // atomic, so a race here can only delay, never lose, an update.
  wake_.store(true);
  cond_.notify_all();
}

void MetadataPoller::loop(std::stop_token stoken) {
  std::string last;  // last title surfaced ("" = none)
  std::unique_lock lock(mu_);
  while (!stoken.stop_requested()) {
    cond_.wait_for(lock, stoken, kPollInterval,
                   [this, &stoken] { return wake_.load() || stoken.stop_requested(); });
    wake_.store(false);

    // Copy the latest title out of the atomic. Compare by value, not pointer:
    // the engine may allocate a fresh shared_ptr for an identical title.
    const std::shared_ptr<const std::string> cur = source_.load();
    const std::string title = cur ? *cur : std::string{};
    if (title != last) {
      last = title;
      if (!stoken.stop_requested()) {  // discard titles after cancellation
        // Call the callback without holding the mutex: it may re-enter the
        // poller (e.g. engine publish -> UI -> next-track -> wake()).
        lock.unlock();
        cb_(title);
        lock.lock();
      }
    }
  }
}

}  // namespace bootamp::audio
