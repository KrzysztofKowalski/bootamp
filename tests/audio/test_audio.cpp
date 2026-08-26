// tests/audio/test_audio.cpp — Catch2 tests for the decode/pipeline port
// (cliamp player/decode.go + player/pipeline.go).
//
// Section "Pipeline" covers: format helpers (format_ext/needs_ffmpeg/is_hls/
// ext_from_content_type/is_url/supported_exts), TrackPipeline lifecycle
// (close/interrupt/set_known_duration, close ordering with a real
// LivePrefetch), close_pipelines, FdSource, open_local, a generated-WAV
// decode_with_ext round trip (analytic invariants — zero crossings ≈ 880/s,
// peak ≥ 0.4 LSB-scale, len == 44100, seek/position), and build_pipeline on a
// real temp WAV (native libsndfile path).
//
// No golden files are used: the WAV fixture is generated in-place, and every
// comparison is an analytic invariant (SKIP guards where the environment may
// be missing something, e.g. ffprobe for probe_frames). Tests compile
// standalone (headers only); the CMake test_audio target (glob:
// tests/audio/*.cpp) links them against bootamp_audio.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "audio/decode.hpp"
#include "audio/live_prefetch.hpp"
#include "audio/pipeline.hpp"
#include "audio/streamer.hpp"
#include "audio/stream_seek_closer.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace bootamp::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

// ffprobe_on_path scans PATH for an executable ffprobe (probe_frames needs
// it; the test SKIPs when absent so CI without ffmpeg still passes).
bool ffprobe_on_path() {
  const char* p = std::getenv("PATH");
  if (!p) return false;
  std::string rest(p);
  std::size_t pos = 0;
  for (;;) {
    auto colon = rest.find(':', pos);
    std::string dir = rest.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
    std::string cand = dir.empty() ? "ffprobe" : dir + "/ffprobe";
    if (::access(cand.c_str(), X_OK) == 0) return true;
    if (colon == std::string::npos) break;
    pos = colon + 1;
  }
  return false;
}

// TempWav writes a 1-second 440 Hz sine as RIFF PCM16 stereo 44100 Hz and
// removes it on destruction. Returns false from write() if the file could not
// be created (callers SKIP then).
class TempWav {
public:
  TempWav() : path_(fs::temp_directory_path() / "bootamp_test_pipeline.wav") {}

  bool write() {
    std::error_code ec;
    fs::remove(path_, ec);
    constexpr std::uint32_t sr = 44100, ch = 2, bits = 16, seconds = 1;
    constexpr std::uint32_t data_bytes = sr * ch * (bits / 8) * seconds;
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    auto u16 = [&f](std::uint16_t v) {
      char b[2] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
      f.write(b, 2);
    };
    auto u32 = [&f](std::uint32_t v) {
      char b[4];
      for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
      f.write(b, 4);
    };
    f.write("RIFF", 4);
    u32(36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    u32(16);            // fmt chunk size
    u16(1);             // PCM
    u16(static_cast<std::uint16_t>(ch));
    u32(sr);
    u32(sr * ch * bits / 8);
    u16(static_cast<std::uint16_t>(ch * bits / 8));
    u16(static_cast<std::uint16_t>(bits));
    f.write("data", 4);
    u32(data_bytes);
    for (std::uint32_t i = 0; i < sr; ++i) {
      double t = static_cast<double>(i) / sr;
      auto v = static_cast<std::int16_t>(0.5 * 32767.0 * std::sin(2.0 * kPi * 440.0 * t));
      u16(static_cast<std::uint16_t>(v));
      u16(static_cast<std::uint16_t>(v));
    }
    f.close();
    return static_cast<bool>(f);
  }

  ~TempWav() {
    std::error_code ec;
    fs::remove(path_, ec);
  }

  const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

// ---- fakes -------------------------------------------------------------------

// FakeSSC records close() for lifecycle tests.
struct FakeSSC final : public StreamSeekCloser {
  std::pair<std::size_t, bool> stream(std::span<Frame>) override { return {0, false}; }
  std::string err() const override { return {}; }
  std::size_t len() const override { return 1000; }
  std::size_t position() const override { return 0; }
  std::string seek(std::size_t) override { return {}; }
  void close() override { closed = true; }
  bool closed = false;
};

// FakePipeDecoder is both a decoder and a PipeDecoder: interrupt() and
// known_duration_hint() are recorded.
struct FakePipeDecoder final : public StreamSeekCloser, public PipeDecoder {
  std::pair<std::size_t, bool> stream(std::span<Frame>) override { return {0, false}; }
  std::string err() const override { return {}; }
  std::size_t len() const override { return 0; }
  std::size_t position() const override { return 0; }
  std::string seek(std::size_t) override { return {}; }
  void close() override { closed = true; }
  void interrupt() override { interrupted = true; }
  void known_duration_hint(std::chrono::duration<double> d) override { hint = d; }
  bool interrupted = false;
  bool closed = false;
  std::chrono::duration<double> hint{};
};

// EosStreamer ends immediately (returns (0,false)) — used to drive a
// LivePrefetch fill thread that exits promptly.
struct EosStreamer final : public Streamer {
  std::pair<std::size_t, bool> stream(std::span<Frame>) override { return {0, false}; }
  std::string err() const override { return {}; }
};

// zero_crossings counts sign changes of channel 0 in `frames`.
std::size_t zero_crossings(std::span<const Frame> frames) {
  std::size_t n = 0;
  bool was_neg = false;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    bool neg = frames[i][0] < 0.0f;
    if (i > 0 && neg != was_neg) ++n;
    was_neg = neg;
  }
  return n;
}

}  // namespace

TEST_CASE("pipeline format helpers", "[Pipeline]") {
  SECTION("format_ext for local paths") {
    CHECK(format_ext("song.FLAC") == ".flac");
    CHECK(format_ext("/tmp/Artist - Title.ogg") == ".ogg");
    CHECK(format_ext("song") == "");
    CHECK(format_ext("song.") == ".");
    CHECK(format_ext("a.b.mp3") == ".mp3");
  }

  SECTION("format_ext for URLs") {
    CHECK(format_ext("https://x.example/a/song.mp3") == ".mp3");
    CHECK(format_ext("https://x.example/a/song.mp3?x=1&y=2") == ".mp3");
    CHECK(format_ext("https://x.example/a/song.MP3") == ".mp3");
    CHECK(format_ext("https://x.example/a?format=flac") == ".flac");
    CHECK(format_ext("https://x.example/a?format=AAC") == ".aac");
    CHECK(format_ext("https://x.example/a.view?format=ogg") == ".ogg");
    CHECK(format_ext("https://x.example/a.view") == ".mp3");
    CHECK(format_ext("https://x.example/a") == ".mp3");
    CHECK(format_ext("https://x.example") == ".mp3");
    CHECK(format_ext("https://x.example/a/b/c") == ".mp3");
  }

  SECTION("needs_ffmpeg") {
    for (const char* e : {".m4a", ".aac", ".aacp", ".m4b", ".alac", ".wma", ".opus", ".webm"}) {
      INFO(e);
      CHECK(needs_ffmpeg(e));
    }
    for (const char* e : {".mp3", ".wav", ".flac", ".ogg", ".m3u8", ""}) {
      INFO(e);
      CHECK_FALSE(needs_ffmpeg(e));
    }
  }

  SECTION("is_hls") {
    CHECK(is_hls(".m3u8"));
    CHECK_FALSE(is_hls(".mp3"));
    CHECK_FALSE(is_hls(""));
  }

  SECTION("is_url") {
    CHECK(is_url("http://x.example/a.mp3"));
    CHECK(is_url("https://x.example/a.mp3"));
    CHECK_FALSE(is_url("/tmp/a.mp3"));
    CHECK_FALSE(is_url("a.mp3"));
    CHECK_FALSE(is_url(""));
  }

  SECTION("ext_from_content_type") {
    CHECK(ext_from_content_type("audio/aacp; charset=utf-8") == ".aac");
    CHECK(ext_from_content_type("audio/aac") == ".aac");
    CHECK(ext_from_content_type("audio/x-aac") == ".aac");
    CHECK(ext_from_content_type("audio/mpeg") == ".mp3");
    CHECK(ext_from_content_type("audio/mp3") == ".mp3");
    CHECK(ext_from_content_type("audio/ogg") == ".ogg");
    CHECK(ext_from_content_type("application/ogg") == ".ogg");
    CHECK(ext_from_content_type("audio/flac") == ".flac");
    CHECK(ext_from_content_type("audio/wav") == ".wav");
    CHECK(ext_from_content_type("audio/x-wav") == ".wav");
    CHECK(ext_from_content_type("audio/mp4") == ".m4a");
    CHECK(ext_from_content_type("audio/x-m4a") == ".m4a");
    CHECK(ext_from_content_type("audio/opus") == ".opus");
    CHECK(ext_from_content_type("text/plain") == "");
    CHECK(ext_from_content_type("") == "");
  }

  SECTION("supported_exts") {
    for (const char* e : {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac", ".aacp",
                          ".m4b", ".alac", ".wma", ".opus", ".webm"}) {
      CHECK(supported_exts().count(e) == 1);
    }
    CHECK(supported_exts().count(".m3u8") == 0);
  }
}

TEST_CASE("open_local and FdSource", "[Pipeline]") {
  SECTION("open_local missing file errors") {
    auto fd = open_local("/nonexistent/bootamp/definitely-missing.mp3");
    REQUIRE_FALSE(fd.has_value());
    CHECK(fd.error().find("open ") == 0);
  }

  TempWav wav;
  REQUIRE(wav.write());

  SECTION("open_local + FdSource over a regular file") {
    auto fd = open_local(wav.path().string());
    REQUIRE(fd.has_value());
    FdSource src(*fd);  // takes ownership of the fd
    CHECK(src.seekable());
    CHECK(src.length() == 176444);  // 44 header + 44100*4 data bytes
    std::array<std::byte, 8> buf{};
    auto [n, ok] = src.read(buf);
    CHECK(n == 8);
    CHECK(ok);
    CHECK(src.tell() == 8);
    CHECK(src.seek(44, SEEK_SET));
    CHECK(src.tell() == 44);
    CHECK_FALSE(src.seek(-1, SEEK_SET));  // before start
    src.close();
    src.close();  // idempotent
  }

  SECTION("FdSource over a pipe is not seekable") {
    int fds[2];
    REQUIRE(::pipe2(fds, O_CLOEXEC) == 0);
    FdSource src(fds[0]);  // read end
    CHECK_FALSE(src.seekable());
    CHECK(src.tell() == -1);
    CHECK(src.length() == -1);
    CHECK_FALSE(src.seek(0, SEEK_SET));
    const std::byte b = std::byte{0x42};
    REQUIRE(::write(fds[1], &b, 1) == 1);
    std::array<std::byte, 4> buf{};
    auto [n, ok] = src.read(buf);
    CHECK(n == 1);
    CHECK(ok);
    CHECK(buf[0] == std::byte{0x42});
    src.close();
    ::close(fds[1]);
  }
}

TEST_CASE("TrackPipeline lifecycle", "[Pipeline]") {
  SECTION("set_known_duration dispatches to PipeDecoder and stores the hint") {
    auto tp = std::make_unique<TrackPipeline>();
    auto dec = std::make_shared<FakePipeDecoder>();
    tp->decoder = dec;
    tp->stream = dec;

    tp->set_known_duration(std::chrono::duration<double>{0});
    CHECK(tp->known_duration == std::chrono::duration<double>{0});
    CHECK_FALSE(dec->hint.count() > 0);  // d <= 0 → no dispatch

    tp->set_known_duration(std::chrono::duration<double>{5.25});
    CHECK(tp->known_duration.count() == Catch::Approx(5.25));
    CHECK(dec->hint.count() == Catch::Approx(5.25));
  }

  SECTION("set_known_duration is a no-op for plain decoders (Go type-switch default)") {
    auto tp = std::make_unique<TrackPipeline>();
    auto dec = std::make_shared<FakeSSC>();
    tp->decoder = dec;
    tp->set_known_duration(std::chrono::duration<double>{3.0});
    CHECK(tp->known_duration.count() == Catch::Approx(3.0));
    CHECK_FALSE(dec->closed);
  }

  SECTION("interrupt reaches PipeDecoder only") {
    auto pipe = std::make_shared<FakePipeDecoder>();
    auto plain = std::make_shared<FakeSSC>();

    auto tp = std::make_unique<TrackPipeline>();
    tp->decoder = plain;
    tp->interrupt();  // must not crash, no-op
    CHECK_FALSE(plain->closed);

    tp->decoder = pipe;
    tp->interrupt();
    CHECK(pipe->interrupted);
  }

  SECTION("close() closes the decoder and releases the owned counter") {
    auto tp = std::make_unique<TrackPipeline>();
    auto dec = std::make_shared<FakePipeDecoder>();
    tp->decoder = dec;
    auto counter = std::make_unique<std::atomic<std::int64_t>>(42);
    tp->bytes_read = counter.get();
    tp->owned_bytes_read = std::move(counter);
    tp->close();
    CHECK(dec->closed);
    tp->close();  // idempotent
  }

  SECTION("close ordering with a real LivePrefetch: signal, decoder close, join") {
    auto tp = std::make_unique<TrackPipeline>();
    auto dec = std::make_shared<FakeSSC>();
    auto eos = std::make_shared<EosStreamer>();
    auto prefetch = std::make_shared<LivePrefetch>(eos, 44100);
    tp->decoder = dec;
    tp->stream = prefetch;
    tp->live_prefetch = prefetch;
    tp->close();  // livePrefetch.Close (signal) → decoder.Close → livePrefetch.Wait
    CHECK(dec->closed);
    tp->close();  // idempotent
  }
}

TEST_CASE("close_pipelines", "[Pipeline]") {
  auto a = std::make_shared<FakeSSC>();
  auto b = std::make_shared<FakeSSC>();
  auto ta = std::make_unique<TrackPipeline>();
  auto tb = std::make_unique<TrackPipeline>();
  ta->decoder = a;
  tb->decoder = b;
  close_pipelines({nullptr, ta.get(), tb.get()});  // nullptr skipped
  CHECK(a->closed);
  CHECK(b->closed);
}

TEST_CASE("decode_with_ext on a generated WAV (libsndfile)", "[Pipeline]") {
  TempWav wav;
  REQUIRE(wav.write());

  int fd = ::open(wav.path().c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE(fd >= 0);
  auto res = decode_with_ext(fd, ".wav", wav.path().string(), 44100, 16);
  REQUIRE(res.has_value());
  CHECK(res->format.sample_rate == 44100);
  CHECK(res->format.channels == 2);
  CHECK(res->format.precision == 2);

  auto& dec = res->decoder;
  CHECK(dec->len() == 44100);
  CHECK(dec->err().empty());

  // Analytic invariants on the first second of a 440 Hz sine.
  std::vector<Frame> buf(44100);
  auto [n, more] = dec->stream(buf);
  CHECK(n == 44100);
  CHECK_FALSE(more);  // exactly one second consumed in one pull? sndfile may
  // return fewer frames per call if read returns short — accept n == len here
  // but tolerate the invariant: n > 0.

  // The stream contract allows short reads, so collect everything.
  std::size_t total = n;
  while (more) {
    auto [k, m] = dec->stream(buf);
    total += k;
    more = m;
  }
  CHECK(total == 44100);
  CHECK(dec->position() == 44100);

  // Channel 0 carries the sine: ~880 zero crossings per second at 440 Hz.
  std::size_t zc = zero_crossings(buf);
  CHECK(zc >= 880 - 40);
  CHECK(zc <= 880 + 40);
  // Peak amplitude: 0.5 * 32767/32768 ≈ 0.5 LSB-scale.
  float peak = 0.0f;
  for (const auto& f : buf) peak = std::max(peak, std::abs(f[0]));
  CHECK(peak >= 0.4f);
  CHECK(peak <= 0.51f);
  // Both channels identical (mono→stereo duplication by the writer).
  bool same = true;
  for (const auto& f : buf) {
    if (f[0] != f[1]) { same = false; break; }
  }
  CHECK(same);

  SECTION("seek and position") {
    CHECK(dec->seek(22050).empty());
    CHECK(dec->position() == 22050);
    CHECK(dec->len() == 44100);
    std::vector<Frame> tail(512);
    auto [k, m] = dec->stream(tail);
    CHECK(k == 512);
    CHECK(m);
    CHECK(dec->position() == 22562);
    CHECK(dec->seek(50000).empty());  // clamps to len()
    CHECK(dec->position() == 44100);
    CHECK(dec->seek(44100).empty());
  }

  dec->close();
  dec->close();  // idempotent
}

TEST_CASE("decode_with_ext rejects unknown extensions", "[Pipeline]") {
  TempWav wav;
  REQUIRE(wav.write());
  int fd = ::open(wav.path().c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE(fd >= 0);
  auto res = decode_with_ext(fd, ".xyz", wav.path().string(), 44100, 16);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().find("unsupported file extension") == 0);
}

TEST_CASE("build_pipeline on a local WAV (native path)", "[Pipeline]") {
  TempWav wav;
  REQUIRE(wav.write());

  PipelineBuilder builder(44100, 16);
  auto tp = builder.build_pipeline(wav.path().string());
  REQUIRE(tp.has_value());

  auto& p = **tp;  // expected::operator* → unique_ptr; dereference again for the pipeline
  CHECK(p.seekable);
  CHECK(p.format.sample_rate == 44100);
  CHECK(p.path == wav.path().string());
  CHECK_FALSE(p.live);
  CHECK(p.content_length == -1);
  CHECK(p.bytes_read == nullptr);
  CHECK(p.ytdl_seek == false);
  CHECK(p.live_prefetch == nullptr);          // local files: prefetch off
  CHECK(p.decoded_duration == std::chrono::duration<double>{0});
  REQUIRE(p.decoder != nullptr);
  CHECK(p.decoder->len() == 44100);
  CHECK(p.stream == p.decoder);               // no resample at matching rate

  // Analytic invariant: 440 Hz sine playback.
  std::vector<Frame> buf(4096);
  auto [n, more] = p.stream->stream(buf);
  CHECK(n == 4096);
  CHECK(more);
  float peak = 0.0f;
  for (const auto& f : buf) peak = std::max(peak, std::abs(f[0]));
  CHECK(peak >= 0.4f);
  CHECK(p.stream->err().empty());

  p.close();
  p.close();  // idempotent
}

TEST_CASE("build_pipeline resample wrap at mismatched rate", "[Pipeline]") {
  TempWav wav;
  REQUIRE(wav.write());

  // Engine at 48000: the 44100 Hz WAV must go through ResampleStreamer.
  PipelineBuilder builder(48000, 16);
  auto tp = builder.build_pipeline(wav.path().string());
  REQUIRE(tp.has_value());
  auto& p = **tp;  // expected::operator* → unique_ptr; dereference again
  REQUIRE(p.stream != nullptr);
  CHECK(p.stream != p.decoder);

  std::vector<Frame> buf(48000);
  auto [n, more] = p.stream->stream(buf);
  CHECK(n > 0);
  // One second of 44100 Hz resampled to 48000 Hz ≈ 48000 frames total.
  std::size_t total = n;
  while (more) {
    auto [k, m] = p.stream->stream(buf);
    total += k;
    more = m;
  }
  CHECK(total >= 47000);
  CHECK(total <= 49000);
  CHECK(p.stream->err().empty());
  p.close();
}

TEST_CASE("probe_frames via ffprobe", "[Pipeline]") {
  if (!ffprobe_on_path()) {
    SKIP("ffprobe not on PATH — probe_frames needs it; analytic invariants " \
         "below are verified in the WAV decode tests instead");
  }
  TempWav wav;
  REQUIRE(wav.write());
  std::size_t frames = probe_frames(wav.path().string(), 44100);
  CHECK(frames >= 43950);  // ffprobe rounds duration; allow ±0.5%
  CHECK(frames <= 44250);
}
