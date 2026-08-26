// src/tests/bench_dsp.cpp — google-benchmark micro-benchmarks for the M1 DSP core.
//
// Benchmarks (per the M1 plan):
//   BM_BiquadProcess_Scalar / BM_BiquadProcess_AVX2  — 10-band EQ chain over a
//     frame block, scalar vs explicit AVX2 SoA kernel.
//   BM_WsolaOffset_Scalar / BM_WsolaOffset_AVX2     — WSOLA offset_score
//     (double dot-product + horizontal reduce), scalar vs AVX2.
//   BM_VolumeScalar / BM_VolumeAVX2                 — apply_volume (gain) on a
//     stereo frame block, scalar vs AVX2. BM_MonoScalar/BM_MonoAVX2 cover the
//     mono downmix path, and the combined gain+downmix (apply_volume_mono) is
//     exercised inside BM_TickPipeline.
//   BM_TickPipeline                                — simulated full per-tick
//     DSP chain (EQ → WSOLA score → volume+mono → spectrum analyze) with a
//     hard runtime assertion that one frame block completes in < 16 µs
//     (the 60 FPS UI tick budget — plan: "BM_TickPipeline < 16µs/frame").
//
// The DSP kernels are pure functions with no `audio/` dependency, so the
// benchmarks link only against the `dsp` TUs (+ FFTW3f for the spectrum path).
// AVX2 benchmarks skip with an error when the running CPU lacks AVX2+FMA
// (runtime dispatch is the engine's job; here we exercise the explicit kernels
// so scalar/AVX2 are directly comparable).
//
// Build (linked, by the CMake bench target — NOT run here):
//   g++ -std=c++2c -O3 -march=x86-64-v3 src/tests/bench_dsp.cpp
//       src/dsp/*.cpp -lbenchmark -lfftw3f -lm -o bench_dsp
#include "dsp/biquad.hpp"
#include "dsp/fft.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/volume.hpp"
#include "dsp/wsola.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace bdsp = bootamp::dsp;

namespace {

// One stereo frame block size used across the benchmarks. 1024 frames is a
// representative tick block (~23 ms at 44.1 kHz) and keeps per-iteration work
// large enough to measure cleanly without dominating the 16 µs tick budget.
inline constexpr std::size_t kFrameBlockSize = 1024;
inline constexpr double      kSampleRate     = 44100.0;

// Fill a stereo frame buffer with a deterministic low-amplitude sine so the
// DSP has realistic, non-denormal input (and golden-comparable behavior).
void fill_sine_frames(std::span<std::array<float, 2>> frames, double freq, double sr) {
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    const float  s = static_cast<float>(0.5 * std::sin(2.0 * M_PI * freq * t));
    frames[i] = {s, s};
  }
}

// Fill a double buffer (used by WSOLA) with the same kind of deterministic
// signal, centered around zero so the dot-product ranking is meaningful.
void fill_sine_double(std::span<double> dst, double freq, double sr) {
  for (std::size_t i = 0; i < dst.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    dst[i] = 0.5 * std::sin(2.0 * M_PI * freq * t);
  }
}

// Build an EqState with all 10 bands active at a non-trivial gain so no band is
// skipped (the |dB| < kEqSkipDb gate would bypass a flat band). 3 dB peaking
// across the board matches the plan's `biquad_sine1k_gain3db` golden fixture.
bdsp::EqState make_eq_state(double sr) {
  bdsp::EqState eq;
  eq.sample_rate = sr;
  for (std::size_t i = 0; i < bdsp::kEqBands; ++i) {
    bdsp::set_band(eq, i, 3.0, sr);
  }
  return eq;
}

// ───────────────────────────────────────────────────────────────────────────
// BM_BiquadProcess — 10-band EQ chain over a stereo frame block.
// ───────────────────────────────────────────────────────────────────────────
template <void (*Kernel)(bdsp::EqState&, std::span<std::array<float, 2>>)>
void BM_BiquadProcess(benchmark::State& state) {
  std::vector<std::array<float, 2>> frames(kFrameBlockSize);
  fill_sine_frames(frames, 1000.0, kSampleRate);
  bdsp::EqState eq = make_eq_state(kSampleRate);

  for (auto _ : state) {
    Kernel(eq, frames);
    benchmark::DoNotOptimize(frames.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kFrameBlockSize));
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(kFrameBlockSize * sizeof(std::array<float, 2>)));
}

// ───────────────────────────────────────────────────────────────────────────
// BM_WsolaOffset — offset_score (double dot-product + horizontal reduce).
// prev_tail / candidate are kTsOvlp * channels doubles (512 * 2 = 1024).
// ───────────────────────────────────────────────────────────────────────────
template <double (*Kernel)(std::span<const double>, std::span<const double>, std::size_t)>
void BM_WsolaOffset(benchmark::State& state) {
  constexpr std::size_t kWin = bdsp::kTsOvlp * 2;  // stereo
  std::vector<double> prev_tail(kWin);
  std::vector<double> candidate(kWin);
  fill_sine_double(prev_tail, 1000.0, kSampleRate);
  fill_sine_double(candidate, 1010.0, kSampleRate);  // slightly offset phase

  double score = 0.0;
  for (auto _ : state) {
    score = Kernel(prev_tail, candidate, 2);
    benchmark::DoNotOptimize(score);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kWin));
}

// ───────────────────────────────────────────────────────────────────────────
// BM_Volume — apply_volume (gain) over a stereo frame block.
// BM_VolumeMono — apply_volume_mono (gain + mono downmix in one pass).
// ───────────────────────────────────────────────────────────────────────────
template <void (*Kernel)(std::span<std::array<float, 2>>, float)>
void BM_Volume(benchmark::State& state) {
  std::vector<std::array<float, 2>> frames(kFrameBlockSize);
  fill_sine_frames(frames, 1000.0, kSampleRate);
  const float gain = bdsp::db_to_gain(-6.0f);  // 0.5-ish, exercises the multiply

  for (auto _ : state) {
    Kernel(frames, gain);
    benchmark::DoNotOptimize(frames.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kFrameBlockSize));
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(kFrameBlockSize * sizeof(std::array<float, 2>)));
}

template <void (*Kernel)(std::span<std::array<float, 2>>)>
void BM_VolumeMono(benchmark::State& state) {
  std::vector<std::array<float, 2>> frames(kFrameBlockSize);
  fill_sine_frames(frames, 1000.0, kSampleRate);

  for (auto _ : state) {
    Kernel(frames);
    benchmark::DoNotOptimize(frames.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kFrameBlockSize));
}

// ───────────────────────────────────────────────────────────────────────────
// BM_TickPipeline — simulated full per-tick DSP chain with a < 16 µs assertion.
//
// One iteration represents one UI tick's worth of audio work: EQ the block,
// rank one WSOLA alignment, apply volume+mono, and run the spectrum analysis
// (Hann → FFTW3f r2c → |X|² → log-rebin → smoothing) that feeds the visualizer.
// After the timed loop, a steady_clock measurement over a fixed sub-batch
// asserts the average per-frame cost stays under the 16 µs tick budget
// (plan: "BM_TickPipeline < 16µs/frame"). The assertion is a hard check —
// a regression that pushes a frame block past the budget aborts the bench.
// ───────────────────────────────────────────────────────────────────────────
inline constexpr double kTickBudgetMicros = 16.0;
inline constexpr int    kBudgetSamples    = 2000;

void run_tick_pipeline(bdsp::EqState& eq, std::span<std::array<float, 2>> frames,
                       std::span<const double> prev_tail, std::span<const double> candidate,
                       std::span<float> mono_samples, bdsp::SpectrumAnalyzer& analyzer,
                       const bdsp::VisAnalysisSpec& spec, float gain) {
  bdsp::process_chain(eq, frames);
  // WSOLA alignment ranking (double). Only the score is consumed here; the
  // full stretch is exercised by the dedicated BM_WsolaOffset benchmarks.
  (void)bdsp::offset_score(prev_tail, candidate, 2);
  bdsp::apply_volume_mono(frames, gain);
  // Downmix to mono for the visualizer spectrum path.
  for (std::size_t i = 0; i < frames.size() && i * 2 < mono_samples.size(); ++i) {
    mono_samples[i] = frames[i][0];
  }
  (void)analyzer.analyze(mono_samples, spec);
}

void BM_TickPipeline(benchmark::State& state) {
  std::vector<std::array<float, 2>> frames(kFrameBlockSize);
  fill_sine_frames(frames, 1000.0, kSampleRate);
  bdsp::EqState eq = make_eq_state(kSampleRate);

  constexpr std::size_t kWin = bdsp::kTsOvlp * 2;
  std::vector<double> prev_tail(kWin);
  std::vector<double> candidate(kWin);
  fill_sine_double(prev_tail, 1000.0, kSampleRate);
  fill_sine_double(candidate, 1010.0, kSampleRate);

  bdsp::VisAnalysisSpec spec;
  spec.band_count = bdsp::kDefaultSpectrumBands;
  spec.fft_size   = bdsp::kDefaultFFTSize;
  spec            = bdsp::normalize_analysis_spec(spec);

  std::vector<float> mono_samples(bdsp::kDefaultFFTSize);
  bdsp::SpectrumAnalyzer analyzer(kSampleRate);
  const float gain = bdsp::db_to_gain(-6.0f);

  for (auto _ : state) {
    run_tick_pipeline(eq, frames, prev_tail, candidate, mono_samples, analyzer, spec, gain);
    benchmark::DoNotOptimize(frames.data());
    benchmark::DoNotOptimize(mono_samples.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kFrameBlockSize));
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(kFrameBlockSize * sizeof(std::array<float, 2>)));
  state.SetLabel("full DSP chain: EQ+WSOLA+volume+mono+spectrum");

  // Hard budget assertion: measure a warm sub-batch and require the average
  // per-frame-block cost to stay under the 16 µs tick budget. This is the
  // plan's "BM_TickPipeline < 16µs/frame" gate, enforced at runtime.
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kBudgetSamples; ++i) {
    run_tick_pipeline(eq, frames, prev_tail, candidate, mono_samples, analyzer, spec, gain);
    benchmark::DoNotOptimize(frames.data());
  }
  auto t1 = std::chrono::steady_clock::now();
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() /
                    static_cast<double>(kBudgetSamples);
  if (!(us < kTickBudgetMicros)) {
    std::fprintf(stderr,
                 "BM_TickPipeline: budget violation — %.3f µs/frame >= %.1f µs/frame\n",
                 us, kTickBudgetMicros);
    std::abort();
  }
}

}  // namespace

// ── Registrations ───────────────────────────────────────────────────────────
BENCHMARK(BM_BiquadProcess<&bdsp::process_chain_scalar>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_BiquadProcess_Scalar");
BENCHMARK(BM_BiquadProcess<&bdsp::process_chain_avx2>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_BiquadProcess_AVX2");

BENCHMARK(BM_WsolaOffset<&bdsp::offset_score_scalar>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_WsolaOffset_Scalar");
BENCHMARK(BM_WsolaOffset<&bdsp::offset_score_avx2>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_WsolaOffset_AVX2");

BENCHMARK(BM_Volume<&bdsp::apply_volume_scalar>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_VolumeScalar");
BENCHMARK(BM_Volume<&bdsp::apply_volume_avx2>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_VolumeAVX2");
BENCHMARK(BM_VolumeMono<&bdsp::apply_mono_scalar>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_MonoScalar");
BENCHMARK(BM_VolumeMono<&bdsp::apply_mono_avx2>)
    ->Unit(benchmark::kMicrosecond)->Name("BM_MonoAVX2");

BENCHMARK(BM_TickPipeline)->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK_MAIN();