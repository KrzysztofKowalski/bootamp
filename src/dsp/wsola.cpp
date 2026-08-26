// dsp/wsola.cpp - WSOLA time-stretch (port of cliamp/player/speed.go).
//
// Layers, bottom-up:
//   * score_at / offset_score_* - the alignment score corr^2/norm (double).
//     The scalar kernel is bit-identical to Go's loop (same accumulation
//     order: per-frame pair sums first, then added to the running total).
//     offset_score_avx2 does the 4xdouble dot-product + FMA accumulation +
//     horizontal reduce; it matches the scalar kernel within ~1e-16 relative
//     and is the benchmark/golden SIMD target (BM_WsolaOffset_AVX2).
//   * search_best / compute_best_offset - the coarse-to-fine candidate search
//     (Go's searchBestOffset). Uses the scalar kernel exclusively so the
//     coarse/refine tie-breaking is bit-identical to Go (a near-tie between
//     adjacent pitch-period offsets must resolve exactly as cliamp resolves
//     it, or the stretch output drifts).
//   * stretch_one_step - one synthesis sequence from an externally managed
//     source buffer (Go's wsolaFrame).
// The audio engine's pull path uses WsolaStretcher (header-only, the
// speedStreamer port) which drives the same kernels.
#include "dsp/wsola.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#include <immintrin.h>
#endif

namespace bootamp::dsp {

namespace {

// Runtime AVX2+FMA gate. Mirrors biquad.cpp's avx2_supported(); kept in an
// anonymous namespace under a distinct name so this TU never collides with
// the (duplicated) definitions of that function in biquad.cpp/volume.cpp.
bool has_avx2() {
#if defined(__GNUC__) || defined(__clang__)
  static const bool ok = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  }();
  return ok;
#else
  return false;
#endif
}

// score_at - port of speedStreamer.offsetScore(off, stride). `in` is the
// interleaved source (channels doubles per frame), `tail` the interleaved
// overlap tail. Reads tail[i] against in[off+i] for i = 0, stride, ... over
// the overlap. Scalar, Go-exact: per-frame pair sums (tail[2i]*c[2i] +
// tail[2i+1]*c[2i+1]) are formed before being added to the running total,
// matching Go's `corr += a + b` grouping bit for bit.
double score_at(std::span<const double> tail, std::span<const double> in,
                std::ptrdiff_t off, std::ptrdiff_t stride, std::ptrdiff_t ch) {
  const std::ptrdiff_t tail_frames = static_cast<std::ptrdiff_t>(tail.size() / static_cast<std::size_t>(ch));
  const std::ptrdiff_t avail_frames = static_cast<std::ptrdiff_t>(in.size() / static_cast<std::size_t>(ch)) - off;
  const std::ptrdiff_t n = std::min<std::ptrdiff_t>(
      static_cast<std::ptrdiff_t>(kTsOvlp), std::min(tail_frames, avail_frames));
  double corr = 0.0, norm = 0.0;
  for (std::ptrdiff_t i = 0; i < n; i += stride) {
    const double* t = tail.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(ch);
    const double* c = in.data() +
                      static_cast<std::size_t>(off + i) * static_cast<std::size_t>(ch);
    double pc = 0.0, pn = 0.0;
    for (std::ptrdiff_t k = 0; k < ch; ++k) {
      pc += t[static_cast<std::size_t>(k)] * c[static_cast<std::size_t>(k)];
      pn += c[static_cast<std::size_t>(k)] * c[static_cast<std::size_t>(k)];
    }
    corr += pc;
    norm += pn;
  }
  if (norm < 1e-9 || corr <= 0) {
    return 0;
  }
  // corr^2/norm avoids sqrt; equivalent ranking to corr/sqrt(norm).
  return corr * corr / norm;
}

// Horizontal reduce of a 4-lane double vector: a0+a1+a2+a3.
double hsum256(__m256d v) {
  __m128d lo = _mm256_castpd256_pd128(v);
  __m128d hi = _mm256_extractf128_pd(v, 1);
  __m128d s = _mm_add_pd(lo, hi);       // [a0+a2, a1+a3]
  s = _mm_hadd_pd(s, s);                // [a0+a2+a1+a3, ...]
  return _mm_cvtsd_f64(s);
}

struct Candidate {
  std::ptrdiff_t off;   // frame offset
  double         score;
};

// search_best - port of speedStreamer.searchBestOffset over the candidate
// range [lo, hi]: a coarse pass (stride kTsCoarse) over every offset retains
// the top kTsTop candidates (insertion-sorted, ties broken by smaller offset),
// then each is refined at full resolution over +/- (kTsCoarse-1). If the
// coarse pass found nothing (all scores <= 0), a full-resolution exhaustive
// pass runs. bestOff starts as `exp` clamped into [lo, hi] - the fallback
// when nothing scores above 0.
std::ptrdiff_t search_best(std::span<const double> in, std::span<const double> tail,
                           std::ptrdiff_t lo, std::ptrdiff_t hi, std::ptrdiff_t exp,
                           std::ptrdiff_t ch) {
  std::ptrdiff_t best_off = std::min(std::max(exp, lo), hi);
  double best_score = score_at(tail, in, best_off, 1, ch);

  std::array<Candidate, kTsTop> top{};
  std::size_t top_n = 0;
  for (std::ptrdiff_t off = lo; off <= hi; ++off) {
    const double s = score_at(tail, in, off, static_cast<std::ptrdiff_t>(kTsCoarse), ch);
    if (s <= 0) {
      continue;
    }
    // Insert into the sorted top list: first slot where we beat the incumbent
    // (or tie with a larger offset - smaller offset wins ties).
    std::size_t at = top_n;
    for (std::size_t i = 0; i < top_n; ++i) {
      if (s > top[i].score || (s == top[i].score && off < top[i].off)) {
        at = i;
        break;
      }
    }
    if (at >= kTsTop) {
      continue;
    }
    if (top_n < kTsTop) {
      ++top_n;
    }
    for (std::size_t j = top_n - 1; j > at; --j) {
      top[j] = top[j - 1];
    }
    top[at] = Candidate{off, s};
  }

  for (std::size_t c = 0; c < top_n; ++c) {
    const std::ptrdiff_t refine_lo = std::max(lo, top[c].off - static_cast<std::ptrdiff_t>(kTsCoarse) + 1);
    const std::ptrdiff_t refine_hi = std::min(hi, top[c].off + static_cast<std::ptrdiff_t>(kTsCoarse) - 1);
    for (std::ptrdiff_t off = refine_lo; off <= refine_hi; ++off) {
      const double s = score_at(tail, in, off, 1, ch);
      if (s > best_score || (s == best_score && s > 0 && off < best_off)) {
        best_off = off;
        best_score = s;
      }
    }
  }
  if (top_n == 0) {
    for (std::ptrdiff_t off = lo; off <= hi; ++off) {
      const double s = score_at(tail, in, off, 1, ch);
      if (s > best_score || (s == best_score && s > 0 && off < best_off)) {
        best_off = off;
        best_score = s;
      }
    }
  }
  return best_off;
}

}  // namespace

// ---------------------------------------------------------------------------
// offset_score kernels
// ---------------------------------------------------------------------------

double offset_score_scalar(std::span<const double> prev_tail,
                           std::span<const double> candidate,
                           std::size_t channels) {
  return score_at(prev_tail, candidate, 0, 1, static_cast<std::ptrdiff_t>(channels));
}

double offset_score_avx2(std::span<const double> prev_tail,
                         std::span<const double> candidate,
                         std::size_t channels) {
#if defined(__GNUC__) || defined(__clang__)
  const std::ptrdiff_t ch = static_cast<std::ptrdiff_t>(channels);
  if (ch <= 0) {
    return 0;
  }
  const std::ptrdiff_t n_frames = std::min<std::ptrdiff_t>(
      static_cast<std::ptrdiff_t>(kTsOvlp),
      std::min(static_cast<std::ptrdiff_t>(prev_tail.size() / channels),
               static_cast<std::ptrdiff_t>(candidate.size() / channels)));
  const std::ptrdiff_t ne = n_frames * ch;  // elements
  const double* t = prev_tail.data();
  const double* c = candidate.data();

  __m256d corr = _mm256_setzero_pd();
  __m256d norm = _mm256_setzero_pd();
  std::ptrdiff_t e = 0;
  for (; e + 4 <= ne; e += 4) {
    const __m256d tv = _mm256_loadu_pd(t + e);
    const __m256d cv = _mm256_loadu_pd(c + e);
    corr = _mm256_fmadd_pd(tv, cv, corr);
    norm = _mm256_fmadd_pd(cv, cv, norm);
  }
  double corr_s = hsum256(corr);
  double norm_s = hsum256(norm);
  for (; e < ne; ++e) {
    corr_s += t[e] * c[e];
    norm_s += c[e] * c[e];
  }
  if (norm_s < 1e-9 || corr_s <= 0) {
    return 0;
  }
  return corr_s * corr_s / norm_s;
#else
  return offset_score_scalar(prev_tail, candidate, channels);
#endif
}

double offset_score(std::span<const double> prev_tail,
                    std::span<const double> candidate,
                    std::size_t channels) {
  if (has_avx2()) {
    return offset_score_avx2(prev_tail, candidate, channels);
  }
  return offset_score_scalar(prev_tail, candidate, channels);
}

// ---------------------------------------------------------------------------
// compute_best_offset
// ---------------------------------------------------------------------------

int compute_best_offset(std::span<const double> input, std::span<const double> prev_tail,
                        std::ptrdiff_t expected, std::size_t channels) {
  const std::ptrdiff_t ch = static_cast<std::ptrdiff_t>(channels);
  const std::ptrdiff_t in_frames = static_cast<std::ptrdiff_t>(input.size() / channels);
  const std::ptrdiff_t max_off = std::max<std::ptrdiff_t>(
      0, in_frames - static_cast<std::ptrdiff_t>(kTsWin));
  const std::ptrdiff_t lo = std::min(std::max<std::ptrdiff_t>(
                                         0, expected - static_cast<std::ptrdiff_t>(kTsSearch)),
                                     max_off);
  const std::ptrdiff_t hi = std::max(std::min(max_off, expected + static_cast<std::ptrdiff_t>(kTsSearch)),
                                     lo);
  return static_cast<int>(search_best(input, prev_tail, lo, hi, expected, ch));
}

// ---------------------------------------------------------------------------
// stretch_one_step
// ---------------------------------------------------------------------------

std::size_t stretch_one_step(WsolaState& state, std::span<const double> input,
                             std::span<const double> search_window,
                             std::span<double> output, std::size_t channels) {
  const std::ptrdiff_t ch = static_cast<std::ptrdiff_t>(channels);
  if (ch <= 0) {
    return 0;
  }
  const std::ptrdiff_t in_frames = static_cast<std::ptrdiff_t>(input.size() / channels);
  const std::ptrdiff_t exp = static_cast<std::ptrdiff_t>(std::round(state.input_pos));

  // Candidate range (Go: maxOff/lo/hi from inN and expected).
  const std::ptrdiff_t max_off = std::max<std::ptrdiff_t>(
      0, in_frames - static_cast<std::ptrdiff_t>(kTsWin));
  std::ptrdiff_t lo = std::min(std::max<std::ptrdiff_t>(
                                   0, exp - static_cast<std::ptrdiff_t>(kTsSearch)),
                               max_off);
  std::ptrdiff_t hi = std::max(std::min(max_off, exp + static_cast<std::ptrdiff_t>(kTsSearch)), lo);

  // Restrict candidates to the search window when it is a sub-span of input.
  if (!search_window.empty()) {
    const double* base = input.data();
    const double* w0 = search_window.data();
    if (w0 >= base && w0 + search_window.size() <= base + input.size() &&
        (w0 - base) % ch == 0) {
      const std::ptrdiff_t w_off = (w0 - base) / ch;
      const std::ptrdiff_t w_frames =
          static_cast<std::ptrdiff_t>(search_window.size() / channels);
      const std::ptrdiff_t wlo = std::max(lo, w_off);
      const std::ptrdiff_t whi = std::min(hi, w_off + w_frames -
                                                  static_cast<std::ptrdiff_t>(kTsWin));
      if (wlo <= whi) {
        lo = wlo;
        hi = whi;
      }
    }
  }

  const bool first = state.prev_overlap.empty();
  std::ptrdiff_t src_off = first ? exp : search_best(input, state.prev_overlap, lo, hi, exp, ch);

  if (src_off + static_cast<std::ptrdiff_t>(kTsWin) > in_frames) {
    src_off = std::max<std::ptrdiff_t>(0, in_frames - static_cast<std::ptrdiff_t>(kTsWin));
  }
  if (src_off + static_cast<std::ptrdiff_t>(kTsSeq) > in_frames) {
    return 0;  // cannot supply a full sequence - nothing written
  }
  if (output.size() < kTsSeq * channels) {
    return 0;
  }

  if (first) {
    // Verbatim copy of the first sequence - no crossfade.
    std::copy_n(input.data() + static_cast<std::size_t>(src_off) * channels,
                kTsSeq * channels, output.data());
  } else {
    // Crossfade the overlap region using the pre-computed alpha table.
    const double* tail = state.prev_overlap.data();
    for (std::size_t i = 0; i < kTsOvlp; ++i) {
      const double a = ts_alpha[i];
      const double b = 1.0 - a;
      const double* c = input.data() +
                        static_cast<std::size_t>(src_off + static_cast<std::ptrdiff_t>(i)) *
                            channels;
      for (std::size_t k = 0; k < channels; ++k) {
        output[i * channels + k] = b * tail[i * channels + k] + a * c[k];
      }
    }
    // Direct copy of the rest - unmodified source samples.
    std::copy_n(input.data() + static_cast<std::size_t>(src_off + static_cast<std::ptrdiff_t>(kTsOvlp)) *
                                   channels,
                (kTsSeq - kTsOvlp) * channels, output.data() + kTsOvlp * channels);
  }

  // Save the tail for the next frame's crossfade.
  const double* tail_start =
      input.data() + static_cast<std::size_t>(src_off + static_cast<std::ptrdiff_t>(kTsSeq)) *
                         channels;
  state.prev_overlap.assign(
      tail_start,
      tail_start + static_cast<std::size_t>(kTsOvlp) * channels);

  state.input_pos += static_cast<double>(kTsSeq) * state.speed;
  return static_cast<std::size_t>(std::llround(static_cast<double>(kTsSeq) * state.speed));
}

}  // namespace bootamp::dsp
