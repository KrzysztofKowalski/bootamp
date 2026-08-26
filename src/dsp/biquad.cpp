// src/dsp/biquad.cpp — 10-band parametric EQ (Audio EQ Cookbook peaking biquads).
//
// Port of cliamp/player/eq.go (calcCoeffs + Stream) and cliamp/player/player.go:225
// (10-band chain at Q=1.4). The hot path is Direct Form I — y = b0*x + b1*x1 +
// b2*x2 - a1*y1 - a2*y2 — matching the Go state fields x1/x2/y1/y2 exactly (1:1
// semantics, identical ordering of state updates). A band is bypassed when its
// |db| < kEqSkipDb (no phase shift, no work). Two code paths: scalar and AVX2+FMA
// (SoA over a 4-sample block per band per channel: the FIR part of each band is
// vectorized 4-wide, the IIR feedback stays a 4-step forward substitution).
// Runtime dispatch via __builtin_cpu_supports("avx2"). NEVER -ffast-math
// (preserves NaN/Inf at edge gains); the audio hot path never locks and never
// throws.

#include "dsp/biquad.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace bootamp::dsp {
namespace {

// Cached AVX2+FMA capability probe. Set once on first use; __builtin_cpu_supports
// is documented as cheap but we avoid re-querying on every process_chain call.
// Lock-free: a thread that loses the init race may read a stale `false` and
// dispatch to the scalar path once — always correct, never a torn value.
std::atomic<bool> g_avx2_cache{false};
std::atomic<bool> g_avx2_inited{false};

void init_cpu_flags() {
  bool expected = false;
  if (g_avx2_inited.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
#if defined(__x86_64__) && defined(__AVX2__) && defined(__FMA__)
    // The AVX2 kernel is only compiled when the build targets AVX2+FMA
    // (-march=x86-64-v3 defines both __AVX2__ and __FMA__); without them the
    // probe can never be true, so fall back to a cached `false`.
    __builtin_cpu_init();
    g_avx2_cache.store(__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"),
                       std::memory_order_release);
#else
    g_avx2_cache.store(false, std::memory_order_release);
#endif
  }
}

}  // namespace

BiquadCoeffs calc_coeffs(double center_freq, double q, double db, double sample_rate) {
  BiquadCoeffs c{};
  // Skip gate: a flat band adds no work and no phase shift (matches eq.go's
  // `dB > -0.1 && dB < 0.1` check; exactly ±0.1 dB is still processed).
  if (db > -kEqSkipDb && db < kEqSkipDb) {
    c.active = false;
    return c;
  }
  c.active = true;

  // Audio EQ Cookbook peakingEQ. Literal port of eq.go calcCoeffs.
  const double a     = std::pow(10.0, db / 40.0);
  const double w0    = 2.0 * std::numbers::pi * center_freq / sample_rate;
  const double sinW0 = std::sin(w0);
  const double cosW0 = std::cos(w0);
  const double alpha = sinW0 / (2.0 * q);

  const double b0 = 1.0 + alpha * a;
  const double b1 = -2.0 * cosW0;
  const double b2 = 1.0 - alpha * a;
  const double a0 = 1.0 + alpha / a;
  const double a1 = -2.0 * cosW0;
  const double a2 = 1.0 - alpha / a;

  // Normalize by a0 and fold the subtract sign into the stored a1/a2 so the hot
  // path computes y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2 with plain multiplies.
  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;  // sign already negative; hot path subtracts
  c.a2 = a2 / a0;
  return c;
}

void process_one(const BiquadCoeffs& c, Biquad& ch_l, Biquad& ch_r,
                 float& l, float& r) {
  if (!c.active) return;
  // Left channel.
  {
    const double x = static_cast<double>(l);
    const double y = c.b0 * x + c.b1 * ch_l.x1 + c.b2 * ch_l.x2
                    - c.a1 * ch_l.y1 - c.a2 * ch_l.y2;
    ch_l.x2 = ch_l.x1;
    ch_l.x1 = x;
    ch_l.y2 = ch_l.y1;
    ch_l.y1 = y;
    l = static_cast<float>(y);
  }
  // Right channel.
  {
    const double x = static_cast<double>(r);
    const double y = c.b0 * x + c.b1 * ch_r.x1 + c.b2 * ch_r.x2
                    - c.a1 * ch_r.y1 - c.a2 * ch_r.y2;
    ch_r.x2 = ch_r.x1;
    ch_r.x1 = x;
    ch_r.y2 = ch_r.y1;
    ch_r.y1 = y;
    r = static_cast<float>(y);
  }
}

void set_band(EqState& eq, std::size_t i, double db, double sr) {
  if (i >= kEqBands) return;
  if (sr != eq.sample_rate) {
    // Sample-rate change invalidates every band's coefficients; reset all
    // state so no band runs with stale state against fresh coefficients. (A
    // pure dB change keeps the state — the Go biquad never resets on gain
    // changes either, so live EQ edits are click-free.)
    eq.sample_rate = sr;
    eq.left.fill(Biquad{});
    eq.right.fill(Biquad{});
  }
  eq.coeffs[i] = calc_coeffs(kEqCenters[i], kEqQs[i], db, sr);
}

bool avx2_supported() {
  init_cpu_flags();
  return g_avx2_cache.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Scalar chain: faithful 1:1 port of eq.go Stream() over 10 bands × 2 channels.
// ---------------------------------------------------------------------------
void process_chain_scalar(EqState& eq, std::span<std::array<float, 2>> frames) {
  for (auto& frame : frames) {
    float l = frame[0];
    float r = frame[1];
    for (std::size_t b = 0; b < kEqBands; ++b) {
      process_one(eq.coeffs[b], eq.left[b], eq.right[b], l, r);
    }
    frame[0] = l;
    frame[1] = r;
  }
}

#if defined(__x86_64__) && defined(__AVX2__) && defined(__FMA__)
namespace {

// Apply one band to `block` (4 samples of one channel) using Direct Form I.
// The FIR part p[k] = b0*x[k] + b1*x[k-1] + b2*x[k-2] is vectorized 4-wide
// with AVX2+FMA; the IIR feedback (a1*y1 + a2*y2) is a 4-step forward
// substitution because y[n+k] depends on y[n+k-1]. State is updated to the
// last two input/output samples of the block.
inline void process_band_block_avx2(const BiquadCoeffs& c, Biquad& st,
                                    double* __restrict__ block) {
  if (!c.active) return;

  const __m256d b0 = _mm256_set1_pd(c.b0);
  const __m256d b1 = _mm256_set1_pd(c.b1);
  const __m256d b2 = _mm256_set1_pd(c.b2);

  // Load this band's inputs FIRST and save them — `block` gets overwritten
  // below with this band's outputs (the next band's inputs), so the input
  // history can't be recovered afterwards.
  const __m256d x0 = _mm256_loadu_pd(block);
  alignas(32) double xin[4];
  _mm256_store_pd(xin, x0);

  // prev1[k] = x[k-1] and prev2[k] = x[k-2] (x[-1] = st.x1, x[-2] = st.x2).
  // Build them by shifting the loaded vector right by one/two lanes and
  // blending the carried history into the vacated lane 0.
  // permute4x64(x0, 0b10010001) = [x1, x0, x1, x2]: lanes 1..3 are the
  // right-shifted block, lane 0 is a don't-care the blend replaces.
  const __m256d sh1   = _mm256_permute4x64_pd(x0, 0b10010001);
  const __m256d prev1 = _mm256_blend_pd(sh1, _mm256_set1_pd(st.x1), 0b0001);
  // prev1 = [x[-1], x[0], x[1], x[2]]
  const __m256d sh2   = _mm256_permute4x64_pd(prev1, 0b10010001);
  const __m256d prev2 = _mm256_blend_pd(sh2, _mm256_set1_pd(st.x2), 0b0001);
  // prev2 = [x[-2], x[-1], x[0], x[1]]

  // FIR part, 4-wide: p[k] = b0*x[k] + b1*x[k-1] + b2*x[k-2].
  const __m256d p = _mm256_fmadd_pd(
      b0, x0, _mm256_add_pd(_mm256_mul_pd(b1, prev1), _mm256_mul_pd(b2, prev2)));

  // IIR feedback: 4-step forward substitution.
  // y[k] = p[k] - a1*y[k-1] - a2*y[k-2]  (y[-1] = st.y1, y[-2] = st.y2)
  alignas(32) double pv[4];
  _mm256_store_pd(pv, p);
  const double y0 = pv[0] - c.a1 * st.y1 - c.a2 * st.y2;
  const double y1 = pv[1] - c.a1 * y0 - c.a2 * st.y1;
  const double y2 = pv[2] - c.a1 * y1 - c.a2 * y0;
  const double y3 = pv[3] - c.a1 * y2 - c.a2 * y1;

  block[0] = y0; block[1] = y1; block[2] = y2; block[3] = y3;

  // State: this band's last two inputs (from the saved copy) and outputs.
  st.x2 = xin[2];
  st.x1 = xin[3];
  st.y2 = y2;
  st.y1 = y3;
}

}  // namespace

void process_chain_avx2(EqState& eq, std::span<std::array<float, 2>> frames) {
  // Block-SoA layout: deinterleave up to 4 consecutive samples per channel
  // into aligned double buffers, run every active band over the block (a
  // cascade of LTI filters — band-major and frame-major orderings agree in
  // exact arithmetic), then reinterleave. Tail (<4 samples) falls back to
  // the scalar per-sample path. State carries exactly across blocks, so this
  // is bit-compatible with the scalar chain up to fp association order.
  constexpr std::size_t kBlock = 4;
  const std::size_t n = frames.size();
  const std::size_t nblocks = n / kBlock;

  alignas(32) double lblock[kBlock];
  alignas(32) double rblock[kBlock];

  for (std::size_t blk = 0; blk < nblocks; ++blk) {
    const std::size_t base = blk * kBlock;
    // Deinterleave 4 stereo frames → 4 doubles per channel (SoA over block).
    for (std::size_t j = 0; j < kBlock; ++j) {
      lblock[j] = static_cast<double>(frames[base + j][0]);
      rblock[j] = static_cast<double>(frames[base + j][1]);
    }
    for (std::size_t b = 0; b < kEqBands; ++b) {
      process_band_block_avx2(eq.coeffs[b], eq.left[b], lblock);
      process_band_block_avx2(eq.coeffs[b], eq.right[b], rblock);
    }
    for (std::size_t j = 0; j < kBlock; ++j) {
      frames[base + j][0] = static_cast<float>(lblock[j]);
      frames[base + j][1] = static_cast<float>(rblock[j]);
    }
  }

  // Scalar tail.
  for (std::size_t i = nblocks * kBlock; i < n; ++i) {
    float l = frames[i][0];
    float r = frames[i][1];
    for (std::size_t b = 0; b < kEqBands; ++b) {
      process_one(eq.coeffs[b], eq.left[b], eq.right[b], l, r);
    }
    frames[i][0] = l;
    frames[i][1] = r;
  }
}
#else  // no AVX2+FMA at compile time → fall back to scalar at runtime too.
void process_chain_avx2(EqState& eq, std::span<std::array<float, 2>> frames) {
  process_chain_scalar(eq, frames);
}
#endif

void process_chain(EqState& eq, std::span<std::array<float, 2>> frames) {
  if (avx2_supported()) {
    process_chain_avx2(eq, frames);
  } else {
    process_chain_scalar(eq, frames);
  }
}

}  // namespace bootamp::dsp
