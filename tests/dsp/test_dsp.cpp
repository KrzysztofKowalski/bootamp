// src/tests/test_dsp.cpp - Catch2 tests for the M1 DSP core.
//
// Currently covers the WSOLA module (dsp/wsola.{hpp,cpp}), ported 1:1 from
// cliamp/player/speed.go + speed_test.go. Other dsp suites (biquad, volume,
// spectrum) land here as their ports complete.
//
// Required assertions (M1 plan + task):
//   * 1.0x passthrough ~ identity (eps 1e-5) - exact, since passthrough
//     returns source samples unmodified (float32 -> double -> float32 is
//     identity).
//   * 1.5x / 0.5x length ratio: total output ~= total input / speed.
//   * Guarded golden (golden/wsola_1p5x.json): compare against the Go
//     stretch at 1e-9 when the file exists; otherwise SKIP and verify
//     analytic invariants (length ratio, per-sample energy).
// Plus the full speed_test.go port: tsAlpha table, offsetScore semantics,
// searchBestOffset == exhaustive, bounds/fallback, sparse-phase fallback,
// multi-frame tail continuity, Err forwarding.
#include "dsp/wsola.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bdsp = bootamp::dsp;

namespace {

// ---- test streamers --------------------------------------------------------

// Constant source (Go's fakeStreamer in speed_test.go).
struct ConstantStreamer {
  std::array<float, 2> val;
  int count;
  std::pair<std::size_t, bool> stream(std::span<std::array<float, 2>> dst) {
    const std::size_t n = std::min(dst.size(), static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < n; ++i) {
      dst[i] = val;
    }
    count -= static_cast<int>(n);
    return {n, n > 0};
  }
  std::string err() const { return {}; }
};

// Sine source (Go's sineStreamer: both channels equal, float32 in bootamp).
struct SineStreamer {
  double freq;
  double sr;
  int pos = 0;
  int count;
  std::pair<std::size_t, bool> stream(std::span<std::array<float, 2>> dst) {
    const std::size_t n = std::min(dst.size(), static_cast<std::size_t>(count - pos));
    for (std::size_t i = 0; i < n; ++i) {
      const float v = static_cast<float>(0.5 * std::sin(
          2.0 * std::numbers::pi * freq * static_cast<double>(pos + static_cast<int>(i)) / sr));
      dst[i] = {v, v};
    }
    pos += static_cast<int>(n);
    return {n, pos < count};
  }
  std::string err() const { return {}; }
};

// Source with a fixed error string (Go's TestSpeedStreamerErr).
struct ErrStreamer {
  std::string e;
  std::pair<std::size_t, bool> stream(std::span<std::array<float, 2>>) {
    return {0, false};
  }
  std::string err() const { return e; }
};

// Pull from a WsolaStretcher until it reports 0 frames.
template <class Source>
std::vector<std::array<float, 2>> pull_all(bdsp::WsolaStretcher<Source>& ss,
                                           std::size_t chunk = 4096) {
  std::vector<std::array<float, 2>> out;
  std::vector<std::array<float, 2>> buf(chunk);
  for (;;) {
    const auto [n, ok] = ss.stream(std::span<std::array<float, 2>>(buf));
    (void)ok;
    if (n == 0) {
      break;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
  }
  return out;
}

// ---- search fixture (Go's searchFixture) -----------------------------------

// xorshift64 LCG, values in [-1, 1] (exact port of speed_test.go).
void lcg_fill(std::span<double> interleaved, std::uint64_t& state) {
  for (std::size_t i = 0; i < interleaved.size(); i += 2) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const double left =
        static_cast<double>(static_cast<std::int64_t>(state >> 11) % 2000000) / 1000000.0 - 1.0;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const double right =
        static_cast<double>(static_cast<std::int64_t>(state >> 11) % 2000000) / 1000000.0 - 1.0;
    interleaved[i] = left;
    interleaved[i + 1] = right;
  }
}

std::vector<double> search_fixture(std::size_t in_frames) {
  std::vector<double> in(2 * in_frames);
  std::uint64_t state = 0x9e3779b97f4a7c15ull;
  lcg_fill(in, state);
  return in;
}

// Exhaustive full-resolution search (Go's exhaustiveBestOffset helper).
int exhaustive_best_offset(std::span<const double> in, std::span<const double> tail,
                           std::ptrdiff_t expected) {
  const std::ptrdiff_t in_frames = static_cast<std::ptrdiff_t>(in.size() / 2);
  const std::ptrdiff_t max_off = std::max<std::ptrdiff_t>(
      0, in_frames - static_cast<std::ptrdiff_t>(bdsp::kTsWin));
  const std::ptrdiff_t lo = std::min(std::max<std::ptrdiff_t>(
                                         0, expected - static_cast<std::ptrdiff_t>(bdsp::kTsSearch)),
                                     max_off);
  const std::ptrdiff_t hi = std::max(std::min(max_off, expected + static_cast<std::ptrdiff_t>(bdsp::kTsSearch)),
                                     lo);
  std::ptrdiff_t best_off = std::min(std::max(expected, lo), hi);
  double best_score = bdsp::offset_score_scalar(
      tail, in.subspan(static_cast<std::size_t>(best_off * 2),
                       static_cast<std::size_t>(bdsp::kTsOvlp * 2)),
      2);
  for (std::ptrdiff_t off = lo; off <= hi; ++off) {
    const double s = bdsp::offset_score_scalar(
        tail, in.subspan(static_cast<std::size_t>(off * 2),
                         static_cast<std::size_t>(bdsp::kTsOvlp * 2)),
        2);
    if (s > best_score || (s == best_score && s > 0 && off < best_off)) {
      best_off = off;
      best_score = s;
    }
  }
  return static_cast<int>(best_off);
}

// ---- golden helper ---------------------------------------------------------

std::optional<std::filesystem::path> find_golden(const char* name) {
  for (const char* dir : {"golden/", "../golden/"}) {
    const std::filesystem::path p = std::string(dir) + name;
    if (std::filesystem::exists(p)) {
      return p;
    }
  }
  return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// speed_test.go ports
// ---------------------------------------------------------------------------

TEST_CASE("WSOLA passthrough at 1.0x is identity", "[dsp][wsola]") {
  SECTION("constant source (Go TestSpeedStreamerPassthroughAt1x)") {
    ConstantStreamer src{{0.5f, -0.5f}, 1024};
    std::atomic<double> speed{1.0};
    bdsp::WsolaStretcher<ConstantStreamer> ss(src, &speed);

    std::array<std::array<float, 2>, 128> samples{};
    const auto [n, ok] = ss.stream(samples);
    REQUIRE(n == 128);
    REQUIRE(ok);
    for (std::size_t i = 0; i < n; ++i) {
      REQUIRE(samples[i][0] == 0.5f);
      REQUIRE(samples[i][1] == -0.5f);
    }
  }

  SECTION("speed 0.0 also passes through (Go TestSpeedStreamerPassthroughAtZero)") {
    ConstantStreamer src{{0.3f, 0.3f}, 64};
    std::atomic<double> speed{0.0};
    bdsp::WsolaStretcher<ConstantStreamer> ss(src, &speed);

    std::array<std::array<float, 2>, 32> samples{};
    const auto [n, ok] = ss.stream(samples);
    REQUIRE(n == 32);
    REQUIRE(ok);
    for (std::size_t i = 0; i < n; ++i) {
      REQUIRE(samples[i][0] == 0.3f);
    }
  }

  SECTION("sine source: output == source within eps 1e-5 (plan requirement)") {
    constexpr int kFrames = 8192;
    SineStreamer src{440.0, 44100.0, 0, kFrames};
    std::atomic<double> speed{1.0};
    bdsp::WsolaStretcher<SineStreamer> ss(src, &speed);

    // Reference: the same sine the source generates.
    float max_err = 0.0f;
    std::array<std::array<float, 2>, 128> samples{};
    int pos = 0;
    for (;;) {
      const auto [n, ok] = ss.stream(samples);
      (void)ok;
      if (n == 0) {
        break;
      }
      for (std::size_t i = 0; i < n; ++i) {
        const float want = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(pos) / 44100.0));
        max_err = std::max(max_err, std::abs(samples[i][0] - want));
        ++pos;
      }
    }
    REQUIRE(pos == kFrames);
    REQUIRE(max_err < 1e-5f);
  }
}

TEST_CASE("WSOLA speed length ratio", "[dsp][wsola]") {
  constexpr std::size_t kFrames = 1'000'000;
  for (const double speed : {1.5, 0.5}) {
    CAPTURE(speed);
    SineStreamer src{437.0, 44100.0, 0, static_cast<int>(kFrames)};
    std::atomic<double> spd{speed};
    bdsp::WsolaStretcher<SineStreamer> ss(src, &spd);

    const std::vector<std::array<float, 2>> out = pull_all(ss);
    // Each frame emits exactly kTsSeq samples; inPos advances kTsSeq*speed per
    // frame and the stretch stops when round(k*tsSeq*speed) + tsSeq > N, so
    // total output = N/speed within ~1% for a 1M-frame source.
    const double ratio =
        static_cast<double>(out.size()) / (static_cast<double>(kFrames) / speed);
    CAPTURE(out.size(), ratio);
    REQUIRE(std::abs(ratio - 1.0) < 0.02);
  }
}

TEST_CASE("WSOLA 2x/0.5x produce output (Go 2x/HalfSpeed tests)", "[dsp][wsola]") {
  for (const double speed : {2.0, 0.5}) {
    CAPTURE(speed);
    SineStreamer src{440.0, 44100.0, 0, 8192};
    std::atomic<double> spd{speed};
    bdsp::WsolaStretcher<SineStreamer> ss(src, &spd);

    std::vector<std::array<float, 2>> samples(4096);
    const auto [n, ok] = ss.stream(samples);
    REQUIRE(n > 0);
    REQUIRE(ok);
  }
}

TEST_CASE("WSOLA tsAlpha table (Go TestTsAlphaTable)", "[dsp][wsola]") {
  REQUIRE(bdsp::ts_alpha[0] == 0.0);
  const std::size_t last = bdsp::kTsOvlp - 1;
  REQUIRE(std::abs(bdsp::ts_alpha[last] - static_cast<double>(last) / static_cast<double>(bdsp::kTsOvlp)) < 1e-9);
  for (std::size_t i = 1; i < bdsp::kTsOvlp; ++i) {
    REQUIRE(bdsp::ts_alpha[i] > bdsp::ts_alpha[i - 1]);
  }
}

TEST_CASE("WSOLA offsetScore semantics (Go offsetScore)", "[dsp][wsola]") {
  // Deterministic non-trivial tail.
  std::array<double, 2 * bdsp::kTsOvlp> tail{};
  for (std::size_t i = 0; i < bdsp::kTsOvlp; ++i) {
    const double t = static_cast<double>(i % 7) * 0.125 - 0.4;
    tail[2 * i] = t;
    tail[2 * i + 1] = t * 0.5;
  }

  SECTION("identical candidate: score == corr^2/norm == norm (sum of squares)") {
    double sum = 0.0;
    for (const double v : tail) {
      sum += v * v;
    }
    const double s = bdsp::offset_score_scalar(tail, tail, 2);
    REQUIRE(std::abs(s - sum) < 1e-9);  // corr == norm -> score == norm
  }

  SECTION("silence candidate returns 0") {
    std::array<double, 2 * bdsp::kTsOvlp> zeros{};
    REQUIRE(bdsp::offset_score_scalar(tail, zeros, 2) == 0.0);
    REQUIRE(bdsp::offset_score(tail, zeros, 2) == 0.0);
  }

  SECTION("negative correlation returns 0") {
    std::array<double, 2 * bdsp::kTsOvlp> neg{};
    for (std::size_t i = 0; i < neg.size(); ++i) {
      neg[i] = -tail[i];
    }
    REQUIRE(bdsp::offset_score_scalar(tail, neg, 2) == 0.0);
    REQUIRE(bdsp::offset_score(tail, neg, 2) == 0.0);
  }

  SECTION("scalar and AVX2 kernels agree within 1e-9 (epsilon per golden README)") {
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
      const double a = bdsp::offset_score_scalar(tail, tail, 2);
      const double b = bdsp::offset_score_avx2(tail, tail, 2);
      REQUIRE(std::abs(a - b) < 1e-9);
      // Mono (channels=1) layout too.
      std::array<double, bdsp::kTsOvlp> mono{};
      for (std::size_t i = 0; i < bdsp::kTsOvlp; ++i) {
        mono[i] = tail[2 * i];
      }
      REQUIRE(std::abs(bdsp::offset_score_scalar(mono, mono, 1) -
                       bdsp::offset_score_avx2(mono, mono, 1)) < 1e-9);
    }
#endif
  }
}

TEST_CASE("WSOLA searchBestOffset matches exhaustive (Go TestSearchBestOffsetMatchesExhaustive)",
          "[dsp][wsola]") {
  struct Case {
    const char* name;
    std::ptrdiff_t expected;
    std::ptrdiff_t target;
    std::size_t in_frames;
  };
  const Case cases[] = {
      {"middle", static_cast<std::ptrdiff_t>(bdsp::kTsSearch) + 300,
       static_cast<std::ptrdiff_t>(bdsp::kTsSearch) + 437,
       2 * bdsp::kTsSearch + bdsp::kTsWin + 600},
      {"lower bound", 20, 7, bdsp::kTsSearch + bdsp::kTsWin + 100},
      {"upper bound", static_cast<std::ptrdiff_t>(bdsp::kTsSearch) + 300,
       static_cast<std::ptrdiff_t>(2 * bdsp::kTsSearch) + 290,
       2 * bdsp::kTsSearch + bdsp::kTsWin + 290},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    std::vector<double> in = search_fixture(c.in_frames);
    std::array<double, 2 * bdsp::kTsOvlp> tail{};
    std::copy_n(in.data() + static_cast<std::size_t>(c.target) * 2, tail.size(), tail.data());

    const int want = exhaustive_best_offset(in, tail, c.expected);
    const int got = bdsp::compute_best_offset(in, tail, c.expected, 2);
    REQUIRE(got == want);
    REQUIRE(got == static_cast<int>(c.target));
  }
}

TEST_CASE("WSOLA searchBestOffset bounds and fallback (Go TestSearchBestOffsetBoundsAndFallback)",
          "[dsp][wsola]") {
  // All-zero source and tail: every score is 0, so the fallback (expected
  // clamped into [0, inN-tsWin]) must win.
  std::vector<double> in(2 * (bdsp::kTsWin + 100), 0.0);
  std::array<double, 2 * bdsp::kTsOvlp> tail{};

  SECTION("expected below range -> 0") {
    REQUIRE(bdsp::compute_best_offset(in, tail, -50, 2) == 0);
  }
  SECTION("expected inside range -> expected") {
    REQUIRE(bdsp::compute_best_offset(in, tail, 50, 2) == 50);
  }
  SECTION("expected above range -> inN - tsWin") {
    REQUIRE(bdsp::compute_best_offset(in, tail, static_cast<std::ptrdiff_t>(bdsp::kTsWin) + 500, 2) == 100);
  }
}

TEST_CASE("WSOLA searchBestOffset sparse-phase fallback (Go TestSearchBestOffsetSparsePhaseFallback)",
          "[dsp][wsola]") {
  // Signal energy placed only on coarse-stride sample indices: the coarse
  // pass scores 0 everywhere, so the full-resolution exhaustive fallback
  // must locate the planted target.
  const std::ptrdiff_t expected = static_cast<std::ptrdiff_t>(bdsp::kTsSearch) + 100;
  const std::ptrdiff_t target = expected + 173;
  const std::size_t in_frames = 2 * bdsp::kTsSearch + bdsp::kTsWin + 300;
  std::vector<double> in(2 * in_frames, 0.0);
  std::array<double, 2 * bdsp::kTsOvlp> tail{};
  for (std::size_t i = 1; i < bdsp::kTsOvlp; i += bdsp::kTsCoarse) {
    const double v = static_cast<double>(i + 1);
    tail[2 * i] = v;
    tail[2 * i + 1] = -v / 2.0;
    in[static_cast<std::size_t>(target + static_cast<std::ptrdiff_t>(i)) * 2] = v;
    in[static_cast<std::size_t>(target + static_cast<std::ptrdiff_t>(i)) * 2 + 1] = -v / 2.0;
  }

  const int want = exhaustive_best_offset(in, tail, expected);
  const int got = bdsp::compute_best_offset(in, tail, expected, 2);
  REQUIRE(got == want);
  REQUIRE(got == static_cast<int>(target));
}

TEST_CASE("WSOLA multi-frame tail continuity (Go TestSpeedStreamerMultiFrameTailContinuity)",
          "[dsp][wsola]") {
  for (const double ratio : {0.5, 2.0}) {
    CAPTURE(ratio);
    SineStreamer src{437.0, 44100.0, 0, 200'000};
    std::atomic<double> spd{ratio};
    bdsp::WsolaStretcher<SineStreamer> ss(src, &spd);

    std::vector<std::array<float, 2>> out(bdsp::kTsSeq);
    for (std::size_t frame = 0; frame < 6; ++frame) {
      const double want_first_l = ss.prev_tail()[0];
      const double want_first_r = ss.prev_tail()[1];
      const auto [n, ok] = ss.stream(out);
      REQUIRE(n == bdsp::kTsSeq);
      REQUIRE(ok);
      if (frame > 0) {
        // a=0 at i=0: out[0] = 1.0*tail[0] + 0.0*in[...] == tail[0] exactly.
        REQUIRE(out[0][0] == static_cast<float>(want_first_l));
        REQUIRE(out[0][1] == static_cast<float>(want_first_r));
      }
      REQUIRE(ss.tail_valid());
    }
  }
}

TEST_CASE("WSOLA err forwards (Go TestSpeedStreamerErr)", "[dsp][wsola]") {
  ErrStreamer src{"boom"};
  std::atomic<double> speed{0.0};  // never touched: passthrough at 0
  bdsp::WsolaStretcher<ErrStreamer> ss(src, &speed);
  REQUIRE(ss.err() == "boom");
}

// ---------------------------------------------------------------------------
// Golden (guarded) + analytic invariants
// ---------------------------------------------------------------------------

TEST_CASE("WSOLA golden 1.5x stretch", "[dsp][wsola][golden]") {
  // Generation spec (golden/README.md): a throwaway TestExport*Golden in
  // cliamp dumps golden/wsola_1p5x.json as {"input": [...], "output": [...]},
  // both flat interleaved stereo float64 arrays. input is a 440 Hz sine,
  // amplitude 0.5, 44.1 kHz, 1'000'000 frames; output is the Go
  // speedStreamer result at 1.5x over that input (double path, no float32
  // round trip).
  constexpr double kSr = 44100.0;
  constexpr std::size_t kFrames = 1'000'000;
  std::vector<double> in(2 * kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) {
    const double s =
        0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kSr);
    in[2 * i] = s;
    in[2 * i + 1] = s;
  }
  bdsp::WsolaState st;
  st.speed = 1.5;
  std::vector<double> out;
  out.reserve(kFrames);  // ~2/3 of the input at 1.5x
  std::vector<double> frame(2 * bdsp::kTsSeq);
  for (;;) {
    const std::size_t consumed = bdsp::stretch_one_step(st, in, in, frame, 2);
    if (consumed == 0) {
      break;
    }
    out.insert(out.end(), frame.begin(), frame.end());
  }

  const auto golden = find_golden("wsola_1p5x.json");
  if (!golden) {
    // Golden not generated yet - SKIP the file compare and verify analytic
    // invariants instead (per golden/README.md: never fail on a missing
    // golden).
    INFO("golden/wsola_1p5x.json missing - verifying analytic invariants");
    REQUIRE(out.size() % (2 * bdsp::kTsSeq) == 0);
    const double out_frames = static_cast<double>(out.size() / 2);
    const double expected = static_cast<double>(kFrames) / 1.5;
    REQUIRE(std::abs(out_frames - expected) / expected < 0.02);

    // Energy preservation: the output is source samples (verbatim copies +
    // crossfades of near-identical content), so per-sample energy stays
    // ~0.5^2 * E[sin^2] = 0.125.
    double energy = 0.0;
    for (const double v : out) {
      energy += v * v;
    }
    const double mean_e = energy / static_cast<double>(out.size());
    REQUIRE(std::abs(mean_e - 0.125) < 0.01);

    // Continuity: max sample-to-sample step bounded by the sine slope
    // (2*pi*440/44100 * 0.5) plus slack for the crossfade blend.
    double max_step = 0.0;
    for (std::size_t i = 1; i < out.size(); ++i) {
      max_step = std::max(max_step, std::abs(out[i] - out[i - 1]));
    }
    REQUIRE(max_step < 0.05);
    return;
  }

  // Golden exists: compare against the Go stretch (both double paths).
  std::ifstream f(*golden);
  REQUIRE(f.good());
  const nlohmann::json j = nlohmann::json::parse(f);
  const std::vector<double> go_out = j.at("output").get<std::vector<double>>();
  const std::vector<double> go_in = j.at("input").get<std::vector<double>>();
  if (go_in.size() == in.size()) {
    REQUIRE(go_in == in);  // fixture matches the documented generation spec
  }
  REQUIRE(go_out.size() == out.size());  // same algorithm -> same frame count
  const std::size_t n = std::min(go_out.size(), out.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (std::abs(go_out[i] - out[i]) >= 1e-9) {
      FAIL("wsola golden mismatch at sample " << i << ": Go=" << go_out[i]
                                              << " C++=" << out[i]);
    }
  }
}
