// src/dsp/fft.cpp — FFTW3f r2c wrapper (see dsp/fft.hpp).
//
// Locked decision: FFTW3f, no hand FFT. We wrap fftwf_plan_dft_r2c (size 2048
// by default). Input is a plain real float array — SoA real samples — which
// drops the wasted imaginary lane of the Go code (cliamp/ui/fft.go stored the
// windowed samples in complex128 with a zeroed imag part before its hand-rolled
// radix-2 FFT). Output is the n/2+1 complex spectrum; power_spectrum() reduces
// each bin's re/im pair to |X|^2. dB scaling stays the caller's business:
// 10*log10(power) == 20*log10(magnitude).
//
// cliamp semantics preserved (ui/visualizer.go Analyze):
//   - DC bin zeroed in the power output (Go: powers[0] = 0)
//   - Nyquist bin (index n/2) excluded from the power output (Go: i < n/2)
//
// Threading: FFTW planning is not thread-safe and a plan must not be executed
// concurrently; the visualizer tick is single-threaded (per fft.hpp). Plans
// are created with FFTW_ESTIMATE — no wisdom, no setup cost (an r2c plan at
// 2048 is cheap to build). FFTW is GPL v2+ → the bootamp binary must be
// GPL-compatible (user accepted).

#include "dsp/fft.hpp"

#include <fftw3.h>

#include <algorithm>
#include <new>
#include <utility>

namespace bootamp::dsp {

// FFTW's fftwf_complex is float[2]; interleaved re/im matches
// std::complex<float> on all supported compilers, and the plan is fed the
// buffer through a cast.
static_assert(sizeof(std::complex<float>) == 2 * sizeof(float),
              "FFTW r2c output layout requires interleaved re/im floats");

namespace {

// FFTW requires exactly one timing flag (ESTIMATE/MEASURE/PATIENT/EXHAUSTIVE).
// Default to ESTIMATE unless the caller explicitly picked a slower mode.
// FFTW_MEASURE is 0, so flags == 0 also lands on ESTIMATE.
unsigned plan_flags(unsigned flags) {
  constexpr unsigned kTiming =
      FFTW_ESTIMATE | FFTW_MEASURE | FFTW_PATIENT | FFTW_EXHAUSTIVE;
  return (flags & kTiming) != 0 ? flags : (flags | FFTW_ESTIMATE);
}

}  // namespace

FftPlan::~FftPlan() {
  if (plan_ != nullptr) fftwf_destroy_plan(plan_);
  if (in_ != nullptr) fftwf_free(in_);
  if (out_ != nullptr) fftwf_free(out_);
}

FftPlan::FftPlan(FftPlan&& other) noexcept
    : n_(std::exchange(other.n_, 0)),
      in_(std::exchange(other.in_, nullptr)),
      out_(std::exchange(other.out_, nullptr)),
      plan_(std::exchange(other.plan_, nullptr)) {}

FftPlan& FftPlan::operator=(FftPlan&& other) noexcept {
  if (this != &other) {
    if (plan_ != nullptr) fftwf_destroy_plan(plan_);
    if (in_ != nullptr) fftwf_free(in_);
    if (out_ != nullptr) fftwf_free(out_);
    n_ = std::exchange(other.n_, 0);
    in_ = std::exchange(other.in_, nullptr);
    out_ = std::exchange(other.out_, nullptr);
    plan_ = std::exchange(other.plan_, nullptr);
  }
  return *this;
}

std::unique_ptr<FftPlan> FftPlan::make(std::size_t n, unsigned flags) {
  // r2c needs an even size >= 2 so the n/2+1 output and the power spectrum
  // are well-defined.
  if (n < 2 || (n & 1u) != 0) return nullptr;

  std::unique_ptr<FftPlan> p(new (std::nothrow) FftPlan());
  if (!p) return nullptr;
  p->n_ = n;
  p->in_ = static_cast<float*>(fftwf_malloc(n * sizeof(float)));
  p->out_ = static_cast<std::complex<float>*>(
      fftwf_malloc((n / 2 + 1) * sizeof(std::complex<float>)));
  if (p->in_ == nullptr || p->out_ == nullptr) return nullptr;  // dtor frees

  const int rank_n = static_cast<int>(n);  // FFTW takes int dimensions
  p->plan_ = fftwf_plan_dft_r2c(1, &rank_n, p->in_,
                                reinterpret_cast<fftwf_complex*>(p->out_),
                                plan_flags(flags));
  if (p->plan_ == nullptr) return nullptr;
  return p;
}

std::span<float> FftPlan::input() noexcept {
  return std::span<float>(in_, n_);
}

std::span<std::complex<float>> FftPlan::output() const noexcept {
  return std::span<std::complex<float>>(out_, n_ / 2 + 1);
}

void FftPlan::execute() noexcept {
  fftwf_execute(plan_);
}

void FftPlan::power_spectrum(std::span<float> dst, float scale) {
  execute();
  const std::size_t half = n_ / 2;
  const std::size_t count = std::min(half, dst.size());
  if (count == 0) return;
  dst[0] = 0.0f;  // DC zeroed, matching Go's powers[0] = 0
  for (std::size_t i = 1; i < count; ++i) {
    const std::complex<float>& c = out_[i];
    dst[i] = scale * (c.real() * c.real() + c.imag() * c.imag());
  }
}

}  // namespace bootamp::dsp
