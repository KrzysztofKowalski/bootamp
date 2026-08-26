// tests/audio/test_stall_reader.cpp — Catch2 tests for the per-read stall
// timeout (cliamp player/decode.go stallReader -> audio/stall_reader.cpp).
//
// Ports TestStallReaderTimesOutOnStall (a blocked read is woken by cancel via
// the stop_token watcher within the timeout), TestStallReaderPassesThroughFastRead
// (healthy read returns immediately; the stopped watcher never fires cancel),
// and TestStallReaderCloseCancels (close invokes cancel).
#include <catch2/catch_test_macros.hpp>

#include "audio/stall_reader.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

using namespace bootamp::audio;

namespace {

// BlockingSource models a live HTTP body. With `fast` data set, read() returns
// it immediately; otherwise read() blocks until unblock() (simulating the
// request-context cancel tearing down the connection, which makes the
// underlying read return (0, error)).
class BlockingSource final : public IcyByteSource {
public:
  explicit BlockingSource(std::string fast = {}) : fast_(std::move(fast)) {}

  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override {
    if (!fast_.empty()) {
      std::size_t n = std::min(dst.size(), fast_.size());
      std::memcpy(dst.data(), fast_.data(), n);
      fast_.erase(0, n);
      return {n, true};
    }
    std::unique_lock lk(m_);
    cv_.wait(lk, [this] { return unblocked_.load(); });
    return {0, false};  // errStalledConn
  }

  void close() override {}

  void unblock() {
    unblocked_.store(true);
    cv_.notify_all();
  }

private:
  std::string fast_;
  std::atomic<bool> unblocked_{false};
  std::mutex m_;
  std::condition_variable cv_;
};

}  // namespace

TEST_CASE("stall_reader times out on stall", "[audio][stall]") {
  auto src = std::make_unique<BlockingSource>();
  BlockingSource* raw = src.get();

  // cancel unblocks the stalled read, mimicking the connection being closed
  // when the request context is cancelled (Go: close(unblock) once).
  std::atomic<bool> cancelled{false};
  StallReader sr(std::move(src),
                 [raw, &cancelled] {
                   if (!cancelled.exchange(true)) raw->unblock();
                 },
                 std::chrono::milliseconds{20});

  std::array<std::byte, 16> buf{};
  auto start = std::chrono::steady_clock::now();
  auto [n, ok] = sr.read(std::span(buf));
  auto elapsed = std::chrono::steady_clock::now() - start;

  (void)n;
  CHECK(!ok);  // expected error from the stalled read
  CHECK(elapsed < std::chrono::seconds{1});  // timed out promptly
}

TEST_CASE("stall_reader passes through fast reads", "[audio][stall]") {
  auto src = std::make_unique<BlockingSource>("hello");
  std::atomic<int> cancels{0};
  StallReader sr(std::move(src), [&cancels] { cancels.fetch_add(1); },
                 std::chrono::milliseconds{50});

  std::array<std::byte, 16> buf{};
  auto [n, ok] = sr.read(std::span(buf));
  CHECK(ok);
  CHECK(n == 5);
  CHECK(std::memcmp(buf.data(), "hello", 5) == 0);

  // Give any stray (but stopped) watcher a chance to misfire.
  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  CHECK(cancels.load() == 0);
}

TEST_CASE("stall_reader close cancels", "[audio][stall]") {
  auto src = std::make_unique<BlockingSource>("x");
  std::atomic<int> cancels{0};
  StallReader sr(std::move(src), [&cancels] { cancels.fetch_add(1); },
                 std::chrono::hours{1});

  sr.close();
  CHECK(cancels.load() > 0);
}
