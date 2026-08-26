// audio/stall_reader.cpp — per-read stall timeout for live HTTP bodies.
//
// Port of cliamp/player/decode.go stallReader. Each read arms a stop_token
// watcher thread that invokes cancel (closing the underlying socket) if the
// read does not complete in 10s; a healthy read stops the watcher via the
// jthread's stop_token. Live radio connections can go half-open behind CDNs
// and load balancers, so without this the audio thread parks in read forever
// and deadlocks every caller that needs the engine. On timeout cancel closes
// the connection so the blocked read returns promptly.
#include "audio/stall_reader.hpp"

#include "audio/decode.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace bootamp::audio {

StallReader::StallReader(std::unique_ptr<IcyByteSource> src,
                         std::function<void()> cancel,
                         std::chrono::milliseconds timeout)
    : src_(std::move(src)),
      cancel_(std::move(cancel)),
      timeout_(timeout) {}

StallReader::~StallReader() = default;

std::pair<std::size_t, bool> StallReader::read(std::span<std::byte> dst) {
  // Arm a stop_token watcher that fires cancel_ if this read exceeds
  // timeout_. The watcher is joined when read() returns (jthread dtor), so
  // the locals it references are safe. This mirrors Go's
  // time.AfterFunc + Timer.Stop: a healthy read stops the watcher before it
  // fires; if the deadline already elapsed, cancel still runs (Go's Stop
  // would have returned false).
  std::mutex m;
  std::condition_variable_any cv;
  bool fired = false;
  std::jthread watcher([this, &m, &cv, &fired](std::stop_token st) {
    std::unique_lock lk(m);
    cv.wait_until(lk, st, std::chrono::steady_clock::now() + timeout_,
                  [&] { return fired; });
    if (st.stop_requested()) return;  // healthy read finished in time
    fired = true;
    lk.unlock();
    if (cancel_) cancel_();
  });

  auto [n, ok] = src_->read(dst);
  return {n, ok};  // watcher dtor (request_stop + join) runs here
}

void StallReader::close() {
  if (cancel_) cancel_();  // Go: s.cancel()
  src_->close();
}

std::string stall_ext_from_content_type(std::string_view ct) {
  // Forward to decode.hpp's helper (cliamp extFromContentType) so callers of
  // the radio pipeline don't need decode.hpp.
  return ext_from_content_type(ct);
}

}  // namespace bootamp::audio
