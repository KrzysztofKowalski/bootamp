// audio/metadata_poller.hpp — background ICY/Vorbis title surfacing.
//
// Port of cliamp's metadata poller. A jthread wakes on a condvar_any wait_for
// deadline and copies the latest atomic<shared_ptr<const string>> stream_title
// into the engine state, so the UI status line updates without polling the
// audio thread. stop_token-owned; joined in the dtor.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace bootamp::audio {

// MetadataPoller watches an atomic shared_ptr<string> and surfaces changes to
// a callback (the engine publishes the title to the UI Notifier). Constructed
// with the source atomic + the callback; runs until stop().
class MetadataPoller {
public:
  using SourceRef = const std::atomic<std::shared_ptr<const std::string>>&;
  using Callback  = std::function<void(std::string title)>;

  MetadataPoller(SourceRef source, Callback cb);
  ~MetadataPoller();
  MetadataPoller(const MetadataPoller&)            = delete;
  MetadataPoller& operator=(const MetadataPoller&) = delete;

  // wake signals the poller to re-read immediately (e.g. after a known title set).
  void wake();

private:
  void loop(std::stop_token stoken);

  SourceRef                                  source_;
  Callback                                   cb_;
  // thread_ declares LAST on purpose: member destruction runs in reverse,
  // so the jthread dtor (request_stop + join) fires while cond_/mu_/wake_
  // are still alive. The earlier order (thread_ first) destroyed the cv and
  // mutex BEFORE joining, leaving the parked loop waiting on dead objects.
  std::condition_variable_any                cond_;
  std::mutex                                 mu_;
  std::atomic<bool>                          wake_{false};
  std::jthread                               thread_;
};

}  // namespace bootamp::audio