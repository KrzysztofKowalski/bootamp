// src/tests/test_audio.cpp — Catch2 tests for the M2 audio engine modules.
//
// Currently covers LivePrefetch (audio/live_prefetch.{hpp,cpp}), ported 1:1
// from cliamp/player/live_prefetch.go. Required assertions:
//   * short read -> silence: stream() never blocks while the source stalls
//     (underrun serves silence, "Stream never blocks")
//   * resume: after an underrun, playback restarts with a 64-sample fade-in
//     once the ring refills past resumeAt; partial reads fade out and zero
//     the tail; EOF surfaces as (0, false)
//   * stop: the destructor (closed_ + stop_token) unblocks a fill thread
//     waiting on a full ring and joins cleanly
// Timing is orchestrated through a controllable stub source (serve -> stall
// -> release -> serve data2 -> EOF), so no wall-clock sleep is load-bearing
// for correctness: every assertion holds for any interleaving.
#include "audio/engine.hpp"
#include "audio/ffmpeg_pipe.hpp"
#include "audio/live_prefetch.hpp"
#include "audio/metadata_poller.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace ba = bootamp::audio;

namespace {

using Frame = ba::Frame;

bool is_silent(const Frame& f) { return f[0] == 0.0f && f[1] == 0.0f; }

// Controllable source: serves `data`, then (if `stall`) blocks until
// release(), then serves `data2`, then EOF. Lets tests orchestrate
// underrun/resume timing deterministically.
class StubStreamer final : public ba::Streamer {
public:
  std::vector<Frame> data;   // first phase
  std::vector<Frame> data2;  // second phase (after release)
  bool stall = false;        // wait for release() after `data` is exhausted
  std::string source_err;

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override {
    std::unique_lock lock(mu_);
    if (!second_) {
      if (pos_ < data.size()) {
        const std::size_t n = std::min(dst.size(), data.size() - pos_);
        std::copy(data.begin() + static_cast<std::ptrdiff_t>(pos_),
                  data.begin() + static_cast<std::ptrdiff_t>(pos_ + n), dst.begin());
        pos_ += n;
        return {n, true};
      }
      if (stall) {
        cv_.wait(lock, [this] { return released_; });
      }
      second_ = true;
      pos_ = 0;
    }
    if (pos_ < data2.size()) {
      const std::size_t n = std::min(dst.size(), data2.size() - pos_);
      std::copy(data2.begin() + static_cast<std::ptrdiff_t>(pos_),
                data2.begin() + static_cast<std::ptrdiff_t>(pos_ + n), dst.begin());
      pos_ += n;
      return {n, true};
    }
    return {0, false};
  }

  std::string err() const override { return source_err; }

  void release() {
    {
      std::lock_guard lock(mu_);
      released_ = true;
    }
    cv_.notify_all();
  }

private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool released_ = false;
  bool second_ = false;
  std::size_t pos_ = 0;
};

}  // namespace

TEST_CASE("LivePrefetch: short read yields silence without blocking",
          "[audio][live_prefetch]") {
  // rate 1000 -> ring capacity 4000 frames, resume threshold 500 frames.
  constexpr int kRate = 1000;
  auto src = std::make_shared<StubStreamer>();
  src->data.assign(64, Frame{0.5f, 0.5f});  // far below the resume threshold
  src->stall = true;                        // then stall: slow network chunk

  {
    ba::LivePrefetch pf(src, kRate);

    // While the source is stalled the ring can never reach resumeAt, so every
    // stream() returns immediately with silence. If stream() ever waited on
    // the network this test would hang right here.
    for (int i = 0; i < 8; ++i) {
      std::array<Frame, 256> dst{};
      const auto [n, ok] = pf.stream(dst);
      REQUIRE(n == 256);
      REQUIRE(ok);
      for (const Frame& f : dst) {
        REQUIRE(is_silent(f));
      }
    }
    REQUIRE(pf.position_frames() == 0);  // nothing consumed while buffering

    // Release the source: the fill thread hits EOF. Consumers first drain the
    // 64 buffered frames — n = min(256, 64) = 64, so the fade-in (i/64) and
    // the tail fade-out ((63-i)/64) ramps overlap exactly — then observe
    // (0, false).
    src->release();

    bool saw_data = false;
    bool eof = false;
    for (int i = 0; i < 1000 && !eof; ++i) {
      std::array<Frame, 256> dst{};
      const auto [n, ok] = pf.stream(dst);
      if (!ok) {
        REQUIRE(n == 0);
        REQUIRE(saw_data);
        eof = true;
        break;
      }
      REQUIRE(n == 256);
      if (saw_data) {
        continue;  // everything after the data call is silence
      }
      bool has_data = false;
      for (const Frame& f : dst) {
        has_data = has_data || !is_silent(f);
      }
      if (has_data) {
        saw_data = true;
        for (std::size_t i = 0; i < 64; ++i) {
          const double g = (static_cast<double>(i) / 64.0) *
                           (static_cast<double>(63 - static_cast<int>(i)) / 64.0);
          REQUIRE(dst[i][0] == Catch::Approx(0.5 * g));
          REQUIRE(dst[i][1] == Catch::Approx(0.5 * g));
        }
        for (std::size_t i = 64; i < 256; ++i) {
          REQUIRE(is_silent(dst[i]));  // zeroed tail
        }
      }
    }
    REQUIRE(saw_data);
    REQUIRE(eof);
    REQUIRE(pf.position_frames() == 64);
  }  // dtor: fill already exited at EOF -> joins immediately
}

TEST_CASE("LivePrefetch: resumes after refill with 64-sample fade",
          "[audio][live_prefetch]") {
  // rate 1000 -> capacity 4000, resume threshold 500.
  constexpr int kRate = 1000;
  constexpr std::size_t kChunk = 600;
  auto src = std::make_shared<StubStreamer>();
  src->data.assign(kChunk, Frame{0.25f, 0.5f});
  src->data2.assign(kChunk, Frame{0.75f, 0.25f});
  src->stall = true;

  {
    ba::LivePrefetch pf(src, kRate);

    // 1) Underrun: drain all 600 frames. The fill thread writes them in one
    //    go, so serves are deterministic: two full 256-frame reads, then an
    //    88-frame partial read (fade-out tail + zeroed rest), then silence.
    std::size_t pos = pf.position_frames();
    bool underrun = false;
    for (int i = 0; i < 1000 && !underrun; ++i) {
      std::array<Frame, 256> d{};
      const auto [n, ok] = pf.stream(d);
      REQUIRE(ok);
      REQUIRE(n == 256);
      const std::size_t before = pos;
      pos = pf.position_frames();
      if (pos == 600) {
        // Partial read: source frames 512..599 land at d[0..87]; the last 64
        // (d[24..87]) are faded out, d[88..255] is zeroed.
        for (std::size_t k = 0; k < 24; ++k) {
          REQUIRE(d[k][0] == 0.25f);  // untouched prefix
          REQUIRE(d[k][1] == 0.5f);
        }
        REQUIRE(d[24][0] == Catch::Approx(0.25 * 63.0 / 64.0));
        REQUIRE(d[24][1] == Catch::Approx(0.5 * 63.0 / 64.0));
        REQUIRE(is_silent(d[87]));  // fade-out end: exactly zero
        for (std::size_t k = 88; k < 256; ++k) {
          REQUIRE(is_silent(d[k]));
        }
        underrun = true;
      } else if (pos > before) {
        REQUIRE(pos - before == 256);  // full reads only
      } else {
        REQUIRE(pos == before);  // silence, nothing consumed
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    REQUIRE(underrun);

    // Still stalled: one more read is immediate silence.
    {
      std::array<Frame, 256> d{};
      const auto [n, ok] = pf.stream(d);
      REQUIRE(n == 256);
      REQUIRE(ok);
      for (const Frame& f : d) {
        REQUIRE(is_silent(f));
      }
    }

    // 2) Resume: release the source; the ring refills to 600 frames and the
    //    next serve carries data2 with a 64-sample fade-in.
    src->release();
    bool resumed = false;
    for (int i = 0; i < 1000 && !resumed; ++i) {
      std::array<Frame, 256> d{};
      const auto [n, ok] = pf.stream(d);
      REQUIRE(ok);
      REQUIRE(n == 256);
      if (is_silent(d[64])) {  // still silence while the refill is in flight
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      resumed = true;
      REQUIRE(is_silent(d[0]));  // fade-in starts at zero gain
      REQUIRE(d[63][0] == Catch::Approx(0.75 * 63.0 / 64.0));
      REQUIRE(d[63][1] == Catch::Approx(0.25 * 63.0 / 64.0));
      REQUIRE(d[64][0] == 0.75f);  // past the fade: unchanged
      REQUIRE(d[64][1] == 0.25f);
      REQUIRE(d[255][0] == 0.75f);
      REQUIRE(d[255][1] == 0.25f);
    }
    REQUIRE(resumed);

    // 3) Drain data2 to EOF: (0, false) once the ring empties.
    std::size_t drained_pos = 0;
    bool eof = false;
    for (int i = 0; i < 1000 && !eof; ++i) {
      std::array<Frame, 256> d{};
      const auto [n, ok] = pf.stream(d);
      if (!ok) {
        REQUIRE(n == 0);
        eof = true;
        break;
      }
      REQUIRE(n == 256);
      drained_pos = pf.position_frames();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(eof);
    REQUIRE(drained_pos == 2 * kChunk);  // all 1200 frames served
  }
}

TEST_CASE("LivePrefetch: stop unblocks fill waiting on a full ring",
          "[audio][live_prefetch]") {
  // rate 1 -> 4-frame ring, resume threshold 1. The 100-frame source makes
  // the fill thread spend its time blocked in cond_.wait on the full ring.
  constexpr int kRate = 1;
  auto src = std::make_shared<StubStreamer>();
  src->data.assign(100, Frame{0.5f, 0.5f});

  {
    ba::LivePrefetch pf(src, kRate);

    // Let the fill thread fill the ring, then drain a few 4-frame reads. Each
    // returns (4, true) — data or silence, never EOF (100 source frames vs 16
    // consumed here). The source has no stall, so no release is needed.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int i = 0; i < 4; ++i) {
      std::array<Frame, 4> dst{};
      const auto [n, ok] = pf.stream(dst);
      REQUIRE(n == 4);
      REQUIRE(ok);
    }
    REQUIRE(pf.position_frames() > 0);  // the ring actually served data

    // The fill thread is (almost surely) blocked in cond_.wait on the full
    // ring. The destructor sets closed_, requests stop via the jthread's
    // stop_token and joins — it must return without hanging. If the
    // stop/closed machinery were broken, this test would hang here.
  }
}

// ---------------------------------------------------------------------------
// FfmpegPipe (cliamp/player/ffmpeg.go)
//
// decodePCMFrame s16le/f32le vectors, ffmpegPCMArgs/pcmFrameSize, FirstError
// first-wins publishing, LimitedBuffer 64KB cap, PipeStreamState position,
// stream() over a real pipe(2) pair (decode, position advance, EOF, partial
// frame drop, live unexpected-EOF, poisoned state) and wait_for_audio_bytes
// (peek-without-consuming, EOF wrap, timeout). No ffmpeg subprocess is
// spawned: the stdout side is a pipe pair whose write end the test feeds.
// ---------------------------------------------------------------------------

namespace {

// Build a byte buffer from explicit byte values (decode test vectors).
std::vector<std::byte> bytes(std::initializer_list<unsigned int> vals) {
  std::vector<std::byte> v;
  v.reserve(vals.size());
  for (const unsigned int b : vals) {
    v.push_back(static_cast<std::byte>(static_cast<unsigned char>(b)));
  }
  return v;
}

}  // namespace

TEST_CASE("FfmpegPipe s16le frame decode vectors", "[audio][ffmpeg_pipe]") {
  SECTION("silence frame") {
    // {0x0000, 0x0000} -> {0, 0}
    const ba::Frame f = ba::decode_pcm_frame(bytes({0x00, 0x00, 0x00, 0x00}), false);
    REQUIRE(f == ba::Frame{0.0f, 0.0f});
  }
  SECTION("Go decodePCMFrame: left = int16(LE)/32768") {
    // {0x0001, 0x7fff} -> {1/32768, 32767/32768}
    const ba::Frame f = ba::decode_pcm_frame(bytes({0x01, 0x00, 0xff, 0x7f}), false);
    REQUIRE(f == ba::Frame{1.0f / 32768.0f, 32767.0f / 32768.0f});
  }
  SECTION("negative values: two's complement little-endian") {
    // {0x8000, 0xffff} -> {-32768/32768, -1/32768} = {-1, -1/32768}
    const ba::Frame f = ba::decode_pcm_frame(bytes({0x00, 0x80, 0xff, 0xff}), false);
    REQUIRE(f == ba::Frame{-1.0f, -1.0f / 32768.0f});
  }
  SECTION("full range {-32768, 32767}") {
    const ba::Frame f = ba::decode_pcm_frame(bytes({0x00, 0x80, 0xff, 0x7f}), false);
    REQUIRE(f == ba::Frame{-1.0f, 32767.0f / 32768.0f});
  }
  SECTION("short buffer decodes to silence instead of reading OOB") {
    const ba::Frame f = ba::decode_pcm_frame(bytes({0x01, 0x00}), false);
    REQUIRE(f == ba::Frame{0.0f, 0.0f});
  }
}

TEST_CASE("FfmpegPipe f32le frame decode is a bit_cast (NaN/Inf preserved)",
          "[audio][ffmpeg_pipe]") {
  SECTION("1.0f and -1.0f round-trip") {
    // 1.0f = 0x3f800000, -1.0f = 0xbf800000 (little-endian byte order)
    const std::array<std::byte, 8> buf = {
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3f},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0xbf}};
    const ba::Frame f = ba::decode_pcm_frame(buf, true);
    REQUIRE(f == ba::Frame{1.0f, -1.0f});
  }
  SECTION("quiet NaN decodes to NaN, not zero (no fast-math)") {
    // NaN = 0x7fc00000
    const std::array<std::byte, 8> buf = {
        std::byte{0x00}, std::byte{0x00}, std::byte{0xc0}, std::byte{0x7f},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3f}};
    const ba::Frame f = ba::decode_pcm_frame(buf, true);
    REQUIRE(std::isnan(f[0]));
    REQUIRE(f[1] == 1.0f);
  }
  SECTION("+Inf decodes to +Inf") {
    // +Inf = 0x7f800000
    const std::array<std::byte, 8> buf = {
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x7f},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const ba::Frame f = ba::decode_pcm_frame(buf, true);
    REQUIRE(std::isinf(f[0]));
    REQUIRE(f[0] > 0.0f);
  }
}

TEST_CASE("FfmpegPipe pcm args and frame sizes (Go ffmpegPCMArgs/pcmFrameSize)",
          "[audio][ffmpeg_pipe]") {
  SECTION("16-bit: s16le") {
    REQUIRE(ba::ffmpeg_pcm_args(16).format == std::string_view("s16le"));
    REQUIRE(ba::ffmpeg_pcm_args(16).codec == std::string_view("pcm_s16le"));
    REQUIRE(ba::ffmpeg_pcm_args(16).precision == 2);
  }
  SECTION("32-bit: f32le") {
    REQUIRE(ba::ffmpeg_pcm_args(32).format == std::string_view("f32le"));
    REQUIRE(ba::ffmpeg_pcm_args(32).codec == std::string_view("pcm_f32le"));
    REQUIRE(ba::ffmpeg_pcm_args(32).precision == 4);
  }
  SECTION("frame byte sizes") {
    REQUIRE(ba::pcm_frame_size(false) == 4);  // 2 ch x 2 bytes
    REQUIRE(ba::pcm_frame_size(true) == 8);   // 2 ch x 4 bytes
  }
}

TEST_CASE("FfmpegPipe FirstError publishes exactly once", "[audio][ffmpeg_pipe]") {
  ba::FirstError e;
  REQUIRE(e.load().empty());
  e.publish("boom");
  e.publish("second error");  // first wins
  e.publish("");              // empty is ignored (Go publish(nil))
  REQUIRE(e.load() == "boom");
}

TEST_CASE("FfmpegPipe LimitedBuffer caps stderr at 64KB", "[audio][ffmpeg_pipe]") {
  ba::LimitedBuffer b;
  std::vector<std::byte> chunk(ba::kSubprocessStderrLimit, std::byte{'a'});

  SECTION("exactly at the cap: no truncation marker") {
    b.write(chunk);
    REQUIRE(b.str() == std::string(ba::kSubprocessStderrLimit, 'a'));
  }
  SECTION("overflow appends the truncation marker and drops the tail") {
    b.write(chunk);
    b.write(std::span<std::byte>(chunk).first(16));
    REQUIRE(b.str() ==
            std::string(ba::kSubprocessStderrLimit, 'a') + "\n[stderr truncated]");
  }
}

TEST_CASE("FfmpegPipe PipeStreamState tracks position atomically",
          "[audio][ffmpeg_pipe]") {
  ba::PipeStreamState st(42);
  REQUIRE(st.pos.load() == 42);
  st.pos.fetch_add(7);
  REQUIRE(st.pos.load() == 49);
}

TEST_CASE("FfmpegPipe stream decodes s16le from the stdout pipe",
          "[audio][ffmpeg_pipe]") {
  int p[2] = {-1, -1};
  REQUIRE(::pipe2(p, O_CLOEXEC) == 0);
  auto fp = std::make_unique<ba::FfmpegPipe>();
  fp->stdout_fd = p[0];
  fp->state = std::make_shared<ba::PipeStreamState>(0);

  // Two s16le frames: {1, -1} and {32767, -32768}.
  const std::array<std::byte, 8> pcm = {
      std::byte{0x01}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0x7f}, std::byte{0x00}, std::byte{0x80}};
  REQUIRE(::write(p[1], pcm.data(), pcm.size()) == static_cast<ssize_t>(pcm.size()));
  ::close(p[1]);

  std::array<ba::Frame, 8> dst{};
  const auto [n, ok] = fp->stream(dst);
  REQUIRE(ok);
  REQUIRE(n == 2);
  REQUIRE(dst[0] == ba::Frame{1.0f / 32768.0f, -1.0f / 32768.0f});
  REQUIRE(dst[1] == ba::Frame{32767.0f / 32768.0f, -1.0f});
  REQUIRE(fp->position() == 2);
  REQUIRE(fp->err().empty());

  // EOF: no error published, ok=false, position unchanged, error still empty.
  const auto [n2, ok2] = fp->stream(dst);
  REQUIRE(n2 == 0);
  REQUIRE_FALSE(ok2);
  REQUIRE(fp->position() == 2);
  REQUIRE(fp->err().empty());
}

TEST_CASE("FfmpegPipe stream drops a partial trailing frame (Go n = nBytes/fs)",
          "[audio][ffmpeg_pipe]") {
  int p[2] = {-1, -1};
  REQUIRE(::pipe2(p, O_CLOEXEC) == 0);
  auto fp = std::make_unique<ba::FfmpegPipe>();
  fp->stdout_fd = p[0];
  fp->state = std::make_shared<ba::PipeStreamState>(0);

  // One full frame {1, 0} plus a 2-byte orphan tail that must be dropped.
  const std::array<std::byte, 6> pcm = {
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x02}, std::byte{0x00}};
  REQUIRE(::write(p[1], pcm.data(), pcm.size()) == static_cast<ssize_t>(pcm.size()));
  ::close(p[1]);

  std::array<ba::Frame, 8> dst{};
  const auto [n, ok] = fp->stream(dst);
  REQUIRE(ok);  // n == 1 > 0 -> ok
  REQUIRE(n == 1);
  REQUIRE(dst[0] == ba::Frame{1.0f / 32768.0f, 0.0f});
  REQUIRE(fp->position() == 1);
}

TEST_CASE("FfmpegPipe stream honors a published error and an empty request",
          "[audio][ffmpeg_pipe]") {
  SECTION("published error poisons the stream") {
    auto fp = std::make_unique<ba::FfmpegPipe>();
    fp->state = std::make_shared<ba::PipeStreamState>(0);
    fp->state->err.publish("boom");
    std::array<ba::Frame, 4> dst{};
    const auto [n, ok] = fp->stream(dst);
    REQUIRE(n == 0);
    REQUIRE_FALSE(ok);
    REQUIRE(fp->err() == "boom");
    REQUIRE(fp->position() == 0);
  }
  SECTION("empty request returns (0, true) without touching the pipe") {
    auto fp = std::make_unique<ba::FfmpegPipe>();
    fp->state = std::make_shared<ba::PipeStreamState>(0);
    const auto [n, ok] = fp->stream({});
    REQUIRE(n == 0);
    REQUIRE(ok);
  }
}

TEST_CASE("FfmpegPipe live stream publishes unexpected EOF on EOF",
          "[audio][ffmpeg_pipe]") {
  int p[2] = {-1, -1};
  REQUIRE(::pipe2(p, O_CLOEXEC) == 0);
  ::close(p[1]);  // upstream already gone
  auto fp = std::make_unique<ba::FfmpegPipe>();
  fp->stdout_fd = p[0];
  fp->state = std::make_shared<ba::PipeStreamState>(0);
  fp->live = true;

  std::array<ba::Frame, 4> dst{};
  const auto [n, ok] = fp->stream(dst);
  REQUIRE(n == 0);
  REQUIRE_FALSE(ok);
  // Go ffmpegPipe.Stream: live && no error -> io.ErrUnexpectedEOF.
  REQUIRE(fp->err() == "unexpected EOF");
  fp->interrupt();  // close stdout_fd (no proc/stdin in this fixture)
}

TEST_CASE("FfmpegPipe wait_for_audio_bytes peeks without consuming",
          "[audio][ffmpeg_pipe]") {
  int p[2] = {-1, -1};
  REQUIRE(::pipe2(p, O_CLOEXEC) == 0);
  auto fp = std::make_unique<ba::FfmpegPipe>();
  fp->stdout_fd = p[0];
  fp->state = std::make_shared<ba::PipeStreamState>(0);

  const std::array<std::byte, 4> pcm = {
      std::byte{0x01}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff}};
  REQUIRE(::write(p[1], pcm.data(), pcm.size()) == static_cast<ssize_t>(pcm.size()));
  ::close(p[1]);

  const std::string err =
      fp->wait_for_audio_bytes(4, std::chrono::milliseconds(1000));
  REQUIRE(err.empty());

  // The peeked bytes must still be readable by stream() (bufio.Peek leaves
  // them in the buffer — Go's reader.Peek does not consume).
  std::array<ba::Frame, 4> dst{};
  const auto [n, ok] = fp->stream(dst);
  REQUIRE(ok);
  REQUIRE(n == 1);
  REQUIRE(dst[0] == ba::Frame{1.0f / 32768.0f, -1.0f / 32768.0f});
  REQUIRE(fp->position() == 1);
}

TEST_CASE("FfmpegPipe wait_for_audio_bytes EOF and timeout paths",
          "[audio][ffmpeg_pipe]") {
  SECTION("EOF before n bytes: wrapped peek error") {
    int p[2] = {-1, -1};
    REQUIRE(::pipe2(p, O_CLOEXEC) == 0);
    ::close(p[1]);  // no data at all
    auto fp = std::make_unique<ba::FfmpegPipe>();
    fp->stdout_fd = p[0];
    fp->state = std::make_shared<ba::PipeStreamState>(0);

    const std::string err =
        fp->wait_for_audio_bytes(4, std::chrono::milliseconds(500));
    REQUIRE(err == "waiting for audio data: EOF");
    fp->interrupt();  // close stdout_fd so the fixture leaks no fd
  }
  SECTION("timeout: interrupts the pipe and reports the deadline") {
    int p[2] = {-1, -1};
    REQUIRE(::pipe2(p, O_CLOEXEC) == 0);
    auto fp = std::make_unique<ba::FfmpegPipe>();
    fp->stdout_fd = p[0];
    fp->state = std::make_shared<ba::PipeStreamState>(0);

    const std::string err =
        fp->wait_for_audio_bytes(4, std::chrono::milliseconds(50));
    REQUIRE(err == "timed out waiting for audio data (50ms)");

    // stop() closed stdout_fd: the stream now sees EOF.
    std::array<ba::Frame, 4> dst{};
    const auto [n, ok] = fp->stream(dst);
    REQUIRE(n == 0);
    REQUIRE_FALSE(ok);
  }
  SECTION("n == 0 returns immediately (Go bufio.Peek(0))") {
    auto fp = std::make_unique<ba::FfmpegPipe>();
    fp->stdout_fd = -1;
    fp->state = std::make_shared<ba::PipeStreamState>(0);
    REQUIRE(fp->wait_for_audio_bytes(0, std::chrono::milliseconds(50)).empty());
  }
}

// ---------------------------------------------------------------------------
// MetadataPoller (audio/metadata_poller.{hpp,cpp}) — ICY/Vorbis title
// surfacing, port of player.go setStreamTitle + pollStreamMetadata.
// Semantics under test:
//   * a change in the watched atomic<shared_ptr<const string>> is surfaced to
//     the callback exactly once (value compare, not pointer identity);
//   * an unchanged value produces no callback (the engine re-publishes the
//     same StreamTitle on every ICY metadata block);
//   * clearing the atomic (null) surfaces "" so a stale ICY title cannot
//     clobber the next track's status line (daemon.go applyStreamTitle only
//     shows stream titles for stream tracks);
//   * the callback runs on the poller thread, never the caller/UI thread;
//   * rapid updates may coalesce (latest-wins), but the final value always
//     surfaces, and the destructor joins a parked thread without hanging.
// All timing is driven through poller.wake() + wait_last(): no wall-clock
// sleep is load-bearing for correctness.
namespace {

// Thread-safe recorder for titles surfaced by the poller callback.
class TitleRecorder {
public:
  void on_title(std::string title) {
    std::lock_guard lock(mu_);
    if (id_ == std::thread::id{}) {
      id_ = std::this_thread::get_id();
    }
    titles_.push_back(std::move(title));
    cv_.notify_all();
  }

  std::vector<std::string> snapshot() const {
    std::lock_guard lock(mu_);
    return titles_;
  }

  // Blocks until the recorded tail equals `want`, or the timeout elapses.
  bool wait_last(const std::string& want, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [&] { return !titles_.empty() && titles_.back() == want; });
  }

  std::thread::id callback_thread_id() const {
    std::lock_guard lock(mu_);
    return id_;
  }

private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::string> titles_;
  std::thread::id id_;
};

}  // namespace

TEST_CASE("MetaPoller: surfaces title changes, and only changes",
          "[audio][meta_poller]") {
  std::atomic<std::shared_ptr<const std::string>> src{nullptr};
  TitleRecorder rec;
  {
    ba::MetadataPoller poller(
        src, [&rec](std::string title) { rec.on_title(std::move(title)); });

    // No title yet: wake() (and the 1s poll deadline) must not produce a
    // callback. This also covers the wake-before-thread-started race: the
    // early flag is consumed by the first wait, harmlessly.
    poller.wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(rec.snapshot().empty());

    // A published title surfaces exactly once, on the poller thread.
    src.store(std::make_shared<const std::string>("Artist - Song"));
    poller.wake();
    REQUIRE(rec.wait_last("Artist - Song", std::chrono::seconds(2)));
    REQUIRE(rec.snapshot() == std::vector<std::string>{"Artist - Song"});
    REQUIRE(rec.callback_thread_id() != std::this_thread::get_id());

    // Unchanged value: the engine re-publishes the same StreamTitle on every
    // ICY metadata block — no duplicate callback (value compare, not pointer
    // identity: a fresh allocation must not retrigger).
    src.store(std::make_shared<const std::string>("Artist - Song"));
    poller.wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(rec.snapshot() == std::vector<std::string>{"Artist - Song"});

    // Clearing the atomic (new track / stream ended) surfaces "": a stale
    // ICY title must not clobber the next track's status line.
    src.store(nullptr);
    poller.wake();
    REQUIRE(rec.wait_last("", std::chrono::seconds(2)));
    REQUIRE(rec.snapshot() ==
            std::vector<std::string>{"Artist - Song", ""});

    // Destructor joins while the poller is parked on the deadline.
  }
}

TEST_CASE("MetaPoller: callbacks always run on the poller thread",
          "[audio][meta_poller]") {
  std::atomic<std::shared_ptr<const std::string>> src{nullptr};
  TitleRecorder rec;
  {
    ba::MetadataPoller poller(
        src, [&rec](std::string title) { rec.on_title(std::move(title)); });

    for (int i = 0; i < 4; ++i) {
      src.store(std::make_shared<const std::string>("t" + std::to_string(i)));
      poller.wake();
      REQUIRE(rec.wait_last("t" + std::to_string(i), std::chrono::seconds(2)));
    }
    // The UI thread never observes the atomic directly; every delivered
    // callback came from the one poller thread (single-threaded delivery).
    REQUIRE(rec.callback_thread_id() != std::this_thread::get_id());
    const std::vector<std::string> titles = rec.snapshot();
    REQUIRE(titles.size() == 4);
    REQUIRE(titles == std::vector<std::string>{"t0", "t1", "t2", "t3"});
  }
}

TEST_CASE("MetaPoller: rapid updates coalesce but the final title always lands",
          "[audio][meta_poller]") {
  std::atomic<std::shared_ptr<const std::string>> src{nullptr};
  TitleRecorder rec;
  constexpr int kPublish = 50;
  {
    ba::MetadataPoller poller(
        src, [&rec](std::string title) { rec.on_title(std::move(title)); });

    // A writer thread publishes distinct titles as fast as it can, like ICY
    // metadata blocks racing the poller's 1s deadline. The poller re-reads
    // the atomic after every wake, so intermediate values may be coalesced
    // (latest-wins) but the final one is guaranteed to surface before the
    // poller is destroyed.
    std::jthread writer([&] {
      for (int i = 0; i < kPublish; ++i) {
        const std::string title = "Track " + std::to_string(i);
        src.store(std::make_shared<const std::string>(title));
        // wake() from another thread exercises the lock-free flag + notify
        // path concurrently with the poller's own wait.
        poller.wake();
      }
    });
    writer.join();

    // Deliver the tail by waking and waiting for the final value.
    const std::string final_title = "Track " + std::to_string(kPublish - 1);
    poller.wake();
    REQUIRE(rec.wait_last(final_title, std::chrono::seconds(2)));

    const std::vector<std::string> titles = rec.snapshot();
    REQUIRE(titles.back() == final_title);
    // Everything surfaced is a value we published, in publication order, and
    // never duplicated (change-only delivery).
    int last_idx = -1;
    for (const std::string& t : titles) {
      REQUIRE(t.size() > 6);
      REQUIRE(t.compare(0, 6, "Track ") == 0);
      const int idx = std::stoi(t.substr(6));
      REQUIRE(idx > last_idx);  // strictly increasing => ordered, no dupes
      REQUIRE(idx < kPublish);  // within the published set
      last_idx = idx;
    }
  }
}

TEST_CASE("MetaPoller: destructor joins a parked poller without hanging",
          "[audio][meta_poller]") {
  std::atomic<std::shared_ptr<const std::string>> src{nullptr};
  {
    ba::MetadataPoller poller(src, [](std::string) {});
    // Let the poller park in wait_for with no wake and no title. The
    // destructor requests stop and joins; if the stop/join machinery were
    // broken this test would hang here (TSan would also flag the atomic).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  SUCCEED();
}

// ---------------------------------------------------------------------------
// EngineTests (audio/engine.{hpp,cpp}) — port of cliamp/player/player.go.
//
// The engine is driven through atomics by the UI thread and runs a single
// audio jthread. These tests drive it exactly like the UI does: play a PCM
// buffer (a WAV file) through NullSink (the injectable no-op device), then
// observe Position/Duration and the controls. NullSink discards instantly,
// so the 60s fixture drains in milliseconds — every assertion below polls
// with a deadline and is timing-tolerant (no wall-clock sleep is
// load-bearing for correctness).
//
// yt-dlp paths (play_ytdl/seek_ytdl) are NOT exercised here: they spawn real
// subprocesses and network. The engine's yt-dlp branch shares the same
// install/seek machinery as the generic path, which IS covered.
namespace {

// Poll `pred` every 1ms until it returns true or `timeout` elapses.
template <class Fn>
bool wait_until(Fn&& pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

// RAII temp-file guard: the WAV fixture is removed when the static is torn
// down at process exit.
struct TempWav {
  std::string path;
  ~TempWav() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

// Write `seconds` of stereo 16-bit PCM (a `freq` Hz sine, amplitude 0.3) as a
// RIFF/WAVE file; returns a guard owning the path.
TempWav make_temp_wav(double seconds, double freq) {
  namespace fs = std::filesystem;
  static std::atomic<std::uint64_t> counter{0};
  const std::string path =
      (fs::temp_directory_path() /
       ("bootamp_engine_" + std::to_string(::getpid()) + "_" +
        std::to_string(counter.fetch_add(1)) + ".wav"))
          .string();

  constexpr int kRate   = 44100;
  const std::size_t frames = static_cast<std::size_t>(seconds * kRate);
  const std::uint32_t data_size = static_cast<std::uint32_t>(frames) * 4;  // 2ch x 2B

  std::ofstream out(path, std::ios::binary);
  REQUIRE(out.good());
  const auto put = [&](const void* p, std::size_t n) {
    out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
  };
  const auto put_u32 = [&](std::uint32_t v) { put(&v, 4); };
  const auto put_u16 = [&](std::uint16_t v) { put(&v, 2); };

  put("RIFF", 4);
  put_u32(36 + data_size);
  put("WAVE", 4);
  put("fmt ", 4);
  put_u32(16);           // fmt chunk size
  put_u16(1);            // PCM
  put_u16(2);            // channels
  put_u32(kRate);
  put_u32(static_cast<std::uint32_t>(kRate) * 4);  // byte rate
  put_u16(4);            // block align
  put_u16(16);           // bits per sample
  put("data", 4);
  put_u32(data_size);

  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const std::int16_t s = static_cast<std::int16_t>(
        std::lround(0.3 * std::sin(2.0 * std::numbers::pi * freq * t) * 32767.0));
    put(&s, 2);
    put(&s, 2);  // mono content, stereo layout
  }
  out.close();
  REQUIRE(out.good());
  return TempWav{path};
}

// Fresh engine with a NullSink and a 10ms device buffer (small pull quantum
// so any race window after pause/seek is ~11ms of audio, not 250ms).
ba::AudioEngine make_engine() {
  return ba::AudioEngine(std::make_shared<ba::NullSink>(),
                         ba::EngineConfig{.sample_rate = 44100,
                                          .bit_depth   = 16,
                                          .buffer_ms   = 10});
}

}  // namespace

TEST_CASE("EngineTests: play PCM buffer through NullSink", "[audio][engine]") {
  static const TempWav wav  = make_temp_wav(60.0, 440.0);  // long: survives the fast NullSink drain
  static const TempWav wav2 = make_temp_wav(0.5, 660.0);

  SECTION("play starts playback; position advances, duration is known") {
    ba::AudioEngine engine = make_engine();
    REQUIRE(engine.sample_rate() == 44100);
    REQUIRE_FALSE(engine.is_playing());
    REQUIRE_FALSE(engine.is_paused());
    REQUIRE(engine.position_secs() == 0.0);
    REQUIRE(engine.duration_secs() == 0.0);
    REQUIRE_FALSE(engine.seekable());
    REQUIRE_FALSE(engine.drained());

    REQUIRE(engine.play(wav.path).empty());
    REQUIRE(engine.is_playing());
    REQUIRE(engine.seekable());          // WAV is a seekable native decode
    REQUIRE(engine.duration_secs() == Catch::Approx(60.0));

    REQUIRE(wait_until([&] { return engine.position_secs() > 0.1; },
                       std::chrono::seconds(5)));
    REQUIRE(engine.position_secs() < 59.0);  // mid-play, not at EOF yet
    REQUIRE(engine.stream_title().empty());  // local file: no ICY title
    REQUIRE(engine.volume() == 0.0);
    REQUIRE(engine.speed() == 1.0);
    engine.stop();
    REQUIRE_FALSE(engine.is_playing());
  }

  SECTION("tap ring captures pre-volume samples while playing") {
    ba::AudioEngine engine = make_engine();
    REQUIRE(engine.play(wav.path).empty());

    // The 60s fixture drains in a few milliseconds even into NullSink; spin
    // without sleeping so the poll loop reads the ring while the sine is
    // still in it (it stays until 4096 post-EOF silence frames overwrite it).
    bool saw_signal = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!saw_signal && std::chrono::steady_clock::now() < deadline) {
      std::array<float, 64> buf{};
      const std::size_t n = engine.samples_into(buf);
      if (n == buf.size()) {
        for (const float v : buf) {
          if (std::fabs(v) > 0.05f) {
            saw_signal = true;
            break;
          }
        }
      }
    }
    REQUIRE(saw_signal);  // EQ flat (all bands bypassed at 0 dB), gain 1.0
    engine.stop();
  }

  SECTION("pause freezes position exactly; toggle resumes") {
    ba::AudioEngine engine = make_engine();
    REQUIRE(engine.play(wav.path).empty());
    REQUIRE(wait_until([&] { return engine.position_secs() > 0.1; },
                       std::chrono::seconds(5)));
    REQUIRE(engine.position_secs() < 59.0);

    engine.toggle_pause();
    REQUIRE(engine.is_paused());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // loop parks
    const double p1 = engine.position_secs();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(engine.position_secs() == p1);  // no frames decoded while paused

    engine.toggle_pause();
    REQUIRE_FALSE(engine.is_paused());
    REQUIRE(wait_until([&] { return engine.position_secs() > p1 + 0.1; },
                       std::chrono::seconds(5)));
    engine.stop();
  }

  SECTION("seek clamps to [0, len-1]; paused seek lands exactly") {
    ba::AudioEngine engine = make_engine();
    REQUIRE(engine.play(wav.path).empty());
    REQUIRE(wait_until([&] { return engine.position_secs() > 0.1; },
                       std::chrono::seconds(5)));

    engine.toggle_pause();  // paused: decoder position is frozen, seek exact
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Negative overshoot clamps to frame 0 (Go relativeSeekSample: max(...,0)).
    REQUIRE(engine.seek(-1000.0).empty());
    REQUIRE(engine.position_secs() == 0.0);

    // Positive offset lands at the exact frame.
    REQUIRE(engine.seek(1000.0).empty());
    REQUIRE(engine.position_secs() == Catch::Approx(1000.0 / 44100.0));

    // Overshoot past the end clamps to len-1 (Go: min(round(...), len-1)).
    REQUIRE(engine.seek(2.0e9).empty());
    REQUIRE(engine.position_secs() ==
            Catch::Approx((60.0 * 44100.0 - 1.0) / 44100.0));

    // Resuming plays from the new position to EOF.
    engine.toggle_pause();
    REQUIRE(wait_until([&] { return engine.position_secs() > 59.99; },
                       std::chrono::seconds(5)));
    engine.stop();
  }

  SECTION("EOF: gapless drains when the track ends") {
    ba::AudioEngine engine = make_engine();
    REQUIRE(engine.play(wav.path).empty());
    REQUIRE(wait_until([&] { return engine.drained(); }, std::chrono::seconds(5)));
    REQUIRE(engine.position_secs() >= 59.99);
    REQUIRE_FALSE(engine.gapless_advanced());
    engine.stop();
  }

  SECTION("stop ends playback and clears position; play restarts") {
    ba::AudioEngine engine = make_engine();
    REQUIRE(engine.play(wav.path).empty());
    REQUIRE(wait_until([&] { return engine.position_secs() > 0.1; },
                       std::chrono::seconds(5)));

    engine.stop();
    REQUIRE_FALSE(engine.is_playing());
    REQUIRE(engine.position_secs() == 0.0);
    REQUIRE(engine.duration_secs() == 0.0);
    REQUIRE_FALSE(engine.seekable());

    // Play again on the same engine (Go: Stop then Play on the same Player).
    REQUIRE(engine.play(wav.path).empty());
    REQUIRE(engine.is_playing());
    REQUIRE(wait_until([&] { return engine.position_secs() > 0.1; },
                       std::chrono::seconds(5)));
    engine.stop();
  }

  SECTION("preload queues a track without starting playback; clear drops it") {
    ba::AudioEngine engine = make_engine();
    REQUIRE(engine.preload(wav2.path).empty());
    REQUIRE(engine.has_preload());
    REQUIRE_FALSE(engine.is_playing());  // preload must not start playback
    REQUIRE(engine.position_secs() == 0.0);

    engine.clear_preload();
    REQUIRE_FALSE(engine.has_preload());

    // Preload errors surface like play() errors.
    REQUIRE_FALSE(engine.preload("/nonexistent/bootamp-test.wav").empty());
    REQUIRE_FALSE(engine.has_preload());

    // play() invalidates a pending preload (the next track no longer matches).
    REQUIRE(engine.preload(wav2.path).empty());
    REQUIRE(engine.has_preload());
    REQUIRE(engine.play(wav.path).empty());
    REQUIRE_FALSE(engine.has_preload());
    engine.stop();
  }

  SECTION("controls clamp to their ranges and report back") {
    ba::AudioEngine engine = make_engine();
    // Volume: clamped to [volume_min, +24]; floor defaults to -50 and raises
    // the current volume (Go SetVolumeMin).
    REQUIRE(engine.volume() == 0.0);
    engine.set_volume(-80.0);
    REQUIRE(engine.volume() == -50.0);
    engine.set_volume_min(-30.0);
    REQUIRE(engine.volume_min() == -30.0);
    engine.set_volume(-40.0);  // below the floor -> floor
    REQUIRE(engine.volume() == -30.0);
    engine.set_volume(20.0);   // above +24 -> +24
    REQUIRE(engine.volume() == 24.0);
    engine.set_volume(-3.0);
    REQUIRE(engine.volume() == -3.0);

    // Speed: clamped to [0.25, 2.0].
    REQUIRE(engine.speed() == 1.0);
    engine.set_speed(0.1);
    REQUIRE(engine.speed() == 0.25);
    engine.set_speed(9.0);
    REQUIRE(engine.speed() == 2.0);
    engine.set_speed(1.5);
    REQUIRE(engine.speed() == 1.5);

    // Mono toggle.
    REQUIRE_FALSE(engine.mono());
    engine.toggle_mono();
    REQUIRE(engine.mono());
    engine.toggle_mono();
    REQUIRE_FALSE(engine.mono());

    // EQ: 10 bands, clamped to [-12, +12], out-of-range bands are no-ops.
    for (const double b : engine.eq_bands()) {
      REQUIRE(b == 0.0);
    }
    engine.set_eq_band(-1, 5.0);  // out of range: silent no-op
    engine.set_eq_band(10, 5.0);
    REQUIRE(engine.eq_bands()[0] == 0.0);
    engine.set_eq_band(3, 25.0);
    REQUIRE(engine.eq_bands()[3] == 12.0);
    engine.set_eq_band(3, -20.0);
    REQUIRE(engine.eq_bands()[3] == -12.0);
    engine.set_eq_band(3, -6.0);
    REQUIRE(engine.eq_bands()[3] == -6.0);
  }

  SECTION("error path: a missing file fails play without playback state") {
    ba::AudioEngine engine = make_engine();
    REQUIRE_FALSE(engine.play("/nonexistent/bootamp-missing.wav").empty());
    REQUIRE_FALSE(engine.is_playing());
    REQUIRE(engine.position_secs() == 0.0);
    REQUIRE_FALSE(engine.seekable());
  }
}
