// dsp/volume.cpp — gain (10^(db/20)) + mono downmix in one pass, AVX2.
//
// Port of cliamp/player/volume.go. The Go volumeStreamer reads `vol` (dB as
// Float64bits) and `mono` via atomics, recomputes the linear gain only when the
// dB value changes, then for each stereo frame multiplies both channels by the
// gain and (if mono) replaces both channels with (l+r)/2. bootamp keeps the
// same caching pattern via GainCache (a direct port of the Go cachedDB/
// cachedGain fields — gain recomputed only when dB changes, never per buffer);
// this file provides the per-buffer DSP kernels (scalar + AVX2+FMA), the
// dB→gain helper, and the cache. NEVER -ffast-math — NaN/Inf must propagate
// through unchanged.
#include "dsp/biquad.hpp"  // avx2_supported (runtime dispatch gate)
#include "dsp/volume.hpp"

#include <cmath>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#include <immintrin.h>
#endif

namespace bootamp::dsp {

// db_to_gain returns 10^(db/20). Uses expf(db * ln10/20). Matches Go's
// math.Pow(10, db/20). NaN/Inf preserved (no fast-math).
float db_to_gain(float db) {
  constexpr float kLn10Over20 = 2.30258509299404568401799145468436421f / 20.0f;
  return std::exp(db * kLn10Over20);
}

// gain_for recomputes the cached gain only when `db` changed — the same
// compare-and-recompute the Go volumeStreamer does per Stream() call. Because
// cached_db starts as NaN (Go: math.NaN()), the first call always recomputes;
// NaN db never compares equal, so it recomputes each call (Go behaves
// identically).
float GainCache::gain_for(float db) {
  if (db != cached_db) {
    cached_gain = db_to_gain(db);
    cached_db = db;
  }
  return cached_gain;
}

// avx2_supported is declared in dsp/biquad.hpp and defined once in
// dsp/biquad.cpp (the shared runtime dispatch gate). volume.cpp calls it.

// ---------------------------------------------------------------------------
// Scalar kernels
// ---------------------------------------------------------------------------

void apply_volume_scalar(std::span<std::array<float, 2>> frames, float gain) {
  for (auto& f : frames) {
    f[0] *= gain;
    f[1] *= gain;
  }
}

void apply_mono_scalar(std::span<std::array<float, 2>> frames) {
  for (auto& f : frames) {
    const float mid = (f[0] + f[1]) * 0.5f;
    f[0] = mid;
    f[1] = mid;
  }
}

// apply_volume_mono_scalar does both in one pass — matches Go's volumeStreamer
// loop which multiplies by gain first and then, if mono, sets both to (l+r)/2.
// Note: the Go code computes mid from the already-scaled samples; we keep that
// ordering so the result is bit-identical.
static void apply_volume_mono_scalar(std::span<std::array<float, 2>> frames,
                                     float gain) {
  for (auto& f : frames) {
    f[0] *= gain;
    f[1] *= gain;
    const float mid = (f[0] + f[1]) * 0.5f;
    f[0] = mid;
    f[1] = mid;
  }
}

// ---------------------------------------------------------------------------
// AVX2+FMA kernels
// ---------------------------------------------------------------------------
// Stereo samples are interleaved L,R,L,R,... so a 256-bit load grabs 4 frames
// (8 floats). gain is broadcast. Volume-only: multiply. Mono: compute mid=(L+R)/2
// and broadcast to both channels. Volume+mono: multiply then mid-broadcast.

void apply_volume_avx2(std::span<std::array<float, 2>> frames, float gain) {
#if defined(__GNUC__) || defined(__clang__)
  const __m256 g = _mm256_set1_ps(gain);
  const std::size_t n = frames.size();
  const std::size_t vframes = n / 4;  // 4 frames per 256-bit vector
  const float* p = &frames[0][0];
  float*       o = &frames[0][0];
  for (std::size_t i = 0; i < vframes; ++i, p += 8, o += 8) {
    __m256 s = _mm256_loadu_ps(p);
    s = _mm256_mul_ps(s, g);
    _mm256_storeu_ps(o, s);
  }
  // Tail: remaining frames (0..3).
  for (std::size_t i = vframes * 4; i < n; ++i) {
    frames[i][0] *= gain;
    frames[i][1] *= gain;
  }
#else
  apply_volume_scalar(frames, gain);
#endif
}

void apply_mono_avx2(std::span<std::array<float, 2>> frames) {
#if defined(__GNUC__) || defined(__clang__)
  const __m256 half = _mm256_set1_ps(0.5f);
  const std::size_t n = frames.size();
  const std::size_t vframes = n / 4;
  const float* p = &frames[0][0];
  float*       o = &frames[0][0];
  // Shuffle mask: for each pair (L,R) produce (mid,mid). mid = (L+R)*0.5.
  // We take 8 floats = 4 frames = 4 (L,R) pairs. Compute L+R, scale by 0.5,
  // then duplicate each result into both lanes.
  // Lane layout: [L0,R0,L1,R1,L2,R2,L3,R3]. Sum adjacent pairs:
  //   sum = [L0+R0, L0+R0, L1+R1, L1+R1, L2+R2, L2+R2, L3+R3, L3+R3]
  // via permute+blend. Easiest: use _mm256_shuffle_ps to duplicate.
  // Even lanes (L): shuffle 0,0,2,2,4,4,6,6 — but shuffle works on 128-bit
  // lanes with 32-bit granularity within each lane. Simpler approach:
  //   L = broadcast even-indexed; R = broadcast odd-indexed.
  // Use unpacklo/hi to deinterleave then add.
  for (std::size_t i = 0; i < vframes; ++i, p += 8, o += 8) {
    __m256 s = _mm256_loadu_ps(p);
    // deinterleave: lo = [L0,L1,L2,L3] across 128-bit lanes, hi = [R0,R1,R2,R3]
    __m256 lo = _mm256_unpacklo_ps(s, s);  // [L0,L0,R0,R0, L2,L2,R2,R2]
    __m256 hi = _mm256_unpackhi_ps(s, s);  // [L1,L1,R1,R1, L3,L3,R3,R3]
    // Reorder so all Ls together and all Rs together.
    __m256 l = _mm256_shuffle_ps(lo, hi, 0x88);  // [L0,L0,L1,L1, L2,L2,L3,L3]
    __m256 r = _mm256_shuffle_ps(lo, hi, 0xdd);  // [R0,R0,R1,R1, R2,R2,R3,R3]
    __m256 mid = _mm256_mul_ps(_mm256_add_ps(l, r), half);
    _mm256_storeu_ps(o, mid);
  }
  for (std::size_t i = vframes * 4; i < n; ++i) {
    const float mid = (frames[i][0] + frames[i][1]) * 0.5f;
    frames[i][0] = mid;
    frames[i][1] = mid;
  }
#else
  apply_mono_scalar(frames);
#endif
}

static void apply_volume_mono_avx2(std::span<std::array<float, 2>> frames,
                                  float gain) {
#if defined(__GNUC__) || defined(__clang__)
  const __m256 g    = _mm256_set1_ps(gain);
  const __m256 half = _mm256_set1_ps(0.5f);
  const std::size_t n = frames.size();
  const std::size_t vframes = n / 4;
  const float* p = &frames[0][0];
  float*       o = &frames[0][0];
  for (std::size_t i = 0; i < vframes; ++i, p += 8, o += 8) {
    __m256 s = _mm256_loadu_ps(p);
    s = _mm256_mul_ps(s, g);  // apply gain first (matches Go ordering)
    __m256 lo = _mm256_unpacklo_ps(s, s);
    __m256 hi = _mm256_unpackhi_ps(s, s);
    __m256 l = _mm256_shuffle_ps(lo, hi, 0x88);
    __m256 r = _mm256_shuffle_ps(lo, hi, 0xdd);
    __m256 mid = _mm256_mul_ps(_mm256_add_ps(l, r), half);
    _mm256_storeu_ps(o, mid);
  }
  for (std::size_t i = vframes * 4; i < n; ++i) {
    frames[i][0] *= gain;
    frames[i][1] *= gain;
    const float mid = (frames[i][0] + frames[i][1]) * 0.5f;
    frames[i][0] = mid;
    frames[i][1] = mid;
  }
#else
  apply_volume_mono_scalar(frames, gain);
#endif
}

// ---------------------------------------------------------------------------
// Dispatch entry points
// ---------------------------------------------------------------------------

void apply_volume(std::span<std::array<float, 2>> frames, float gain) {
  if (avx2_supported()) {
    apply_volume_avx2(frames, gain);
  } else {
    apply_volume_scalar(frames, gain);
  }
}

void apply_mono(std::span<std::array<float, 2>> frames) {
  if (avx2_supported()) {
    apply_mono_avx2(frames);
  } else {
    apply_mono_scalar(frames);
  }
}

void apply_volume_mono(std::span<std::array<float, 2>> frames, float gain) {
  if (avx2_supported()) {
    apply_volume_mono_avx2(frames, gain);
  } else {
    apply_volume_mono_scalar(frames, gain);
  }
}

}  // namespace bootamp::dsp