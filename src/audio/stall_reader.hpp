// audio/stall_reader.hpp — per-read stall timeout for live HTTP bodies.
//
// Port of cliamp/player/decode.go stallReader. Each read arms a timer that
// cancels the underlying socket if the read doesn't complete in 10s; a healthy
// read stops the timer. Live radio connections can go half-open behind CDNs,
// so without this the audio thread parks in read forever and deadlocks every
// caller that needs the engine. On timeout the cancel closes the connection
// so the blocked read returns promptly. stop_token watcher cancels the socket.
#pragma once

#include "audio/icy.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <stop_token>
#include <thread>

namespace bootamp::audio {

inline constexpr std::chrono::seconds kStreamStallTimeout{10};

// StallReader wraps an IcyByteSource and enforces a per-read stall timeout.
// Implements IcyByteSource so it slots into the chain transparently.
class StallReader final : public IcyByteSource {
public:
  // `src` is the raw-socket HTTP body (or any IcyByteSource). `cancel` is
  // invoked when a read exceeds `timeout` (closes the underlying socket).
  StallReader(std::unique_ptr<IcyByteSource> src,
              std::function<void()> cancel,
              std::chrono::milliseconds timeout = kStreamStallTimeout);
  ~StallReader() override;

  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override;
  void close() override;

private:
  std::unique_ptr<IcyByteSource>     src_;
  std::function<void()>              cancel_;
  std::chrono::milliseconds          timeout_;
};

// ext_from_content_type delegates to decode.hpp's helper (forwarded here so
// callers of the radio pipeline don't need decode.hpp). Defined in .cpp.
std::string stall_ext_from_content_type(std::string_view ct);

}  // namespace bootamp::audio