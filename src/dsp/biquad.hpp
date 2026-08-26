// dsp/biquad.hpp — 10-band parametric EQ (Audio EQ Cookbook biquads).
//
// Port of cliamp/player/eq.go. Each band is a peaking/notch biquad computed via
// calc_coeffs(center_freq, q, db, sample_rate). The hot path is a transposed
// Direct Form II; per-channel state (x1/x2/y1/y2). A band is bypassed when its
// |db| < 0.1 (skip). Two code paths: scalar and AVX2+FMA (SoA across 10 bands ×
// 2 channels). Runtime dispatch via __builtin_cpu_supports("avx2"). NEVER
// -ffast-math (preserves NaN/Inf at edge gains).
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace bootamp::dsp {

// 10-band EQ, matching cliamp's fixed band centers (Hz) and Q values.
// Centers: 70/180/320/600/1k/3k/6k/12k/14k/16k. Q = 1.4 for every band — matches
// cliamp/player/player.go:225 (`newBiquad(s, eqFreqs[i], 1.4, ...)`).
inline constexpr std::size_t kEqBands   = 10;
inline constexpr std::array<double, kEqBands> kEqCenters = {
  70.0, 180.0, 320.0, 600.0, 1000.0, 3000.0, 6000.0, 12000.0, 14000.0, 16000.0};
inline constexpr std::array<double, kEqBands> kEqQs = {
  1.4, 1.4, 1.4, 1.4, 1.4, 1.4, 1.4, 1.4, 1.4, 1.4};

// Band is skipped (bypassed) when |db| is below this threshold — matches
// cliamp's eq.go skip gate so a flat band adds no work and no phase shift.
inline constexpr double kEqSkipDb = 0.1;

// BiquadCoeffs — Audio EQ Cookbook peaking filter coefficients (a1/a2 negated
// so the difference form is y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2).
struct BiquadCoeffs {
  double b0 = 0, b1 = 0, b2 = 0;
  double a1 = 0, a2 = 0;  // signs already folded (subtract on use)
  bool   active = false;  // false ⇒ band is bypassed (|db| < kEqSkipDb)
};

// calc_coeffs returns the peaking biquad coefficients for a band at
// center_freq Hz, quality factor q, gain db, at sample_rate Hz. Port of
// cliamp eq.go calcCoeffs (Audio EQ Cookbook, peakingEQ). When |db| < kEqSkipDb
// the band is marked inactive (active=false) so the hot path skips it.
BiquadCoeffs calc_coeffs(double center_freq, double q, double db, double sample_rate);

// Biquad holds per-channel Direct Form II transposed state. One instance per
// channel per band; the 10×2 grid is the EQ state.
struct Biquad {
  double x1 = 0, x2 = 0;  // previous inputs
  double y1 = 0, y2 = 0;  // previous outputs
};

// process_one applies a single band to one stereo sample (in place). Used by
// the scalar fallback chain. Skips inactive bands (returns input unchanged).
void process_one(const BiquadCoeffs& c, Biquad& ch_l, Biquad& ch_r,
                 float& l, float& r);

// EqState is the full 10-band × 2-channel EQ: coefficients + per-channel
// state. coefficients are refreshed when sample_rate or db changes.
struct EqState {
  std::array<BiquadCoeffs, kEqBands> coeffs;
  std::array<Biquad, kEqBands>       left;   // left channel per band
  std::array<Biquad, kEqBands>       right;  // right channel per band
  double sample_rate = 0;
};

// set_band updates band `i` to gain `db` at the fixed center/Q for `sr`,
// recomputing that band's coefficients and resetting its state.
void set_band(EqState& eq, std::size_t i, double db, double sr);

// process_chain applies all 10 bands to a frame buffer of stereo float
// samples. Dispatches to the AVX2 SoA kernel when the CPU supports it, else
// the scalar loop. Hot path: never locks, never throws.
void process_chain(EqState& eq, std::span<std::array<float, 2>> frames);

// process_chain_scalar / process_chain_avx2 are the explicit kernels exposed
// for benchmarking and golden tests (BM_BiquadProcess_Scalar/AVX2).
void process_chain_scalar(EqState& eq, std::span<std::array<float, 2>> frames);
void process_chain_avx2(EqState& eq, std::span<std::array<float, 2>> frames);

// avx2_supported reports whether the running CPU supports AVX2+FMA (cached at
// first call from __builtin_cpu_supports).
bool avx2_supported();

}  // namespace bootamp::dsp