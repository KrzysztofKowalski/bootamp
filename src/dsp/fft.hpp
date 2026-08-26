// dsp/fft.hpp — FFTW3f wrapper for the real→complex power-spectrum transform.
//
// Per the plan: wrap fftwf_plan_dft_r2c (no hand FFT). The visualizer feeds
// windowed real samples in and gets |X|² out (size 2048 by default). FFTW is
// GPL v2+ → bootamp binary must be GPL-compatible (user accepted).
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <span>

// Forward-declare the FFTW types so consumers don't need fftw3.h transitively.
struct fftwf_plan_s;
using fftwf_plan = fftwf_plan_s*;

namespace bootamp::dsp {

// Default FFT size for the visualizer (cliamp defaultFFTSize = 2048).
inline constexpr std::size_t kDefaultFFTSize = 2048;

// FftPlan is a owning wrapper around an FFTW3f r2c plan + its buffers. The plan
// is created once per (size, flags) and reused across frames; FFTW wisdom is
// not used (the r2c plan is cheap to build at these sizes). NOT thread-safe —
// the visualizer tick is single-threaded.
class FftPlan {
public:
  // make builds a plan for a real-input DFT of size `n` (must be even, ideally
  // a power of two). Allocates the in/out buffers. Returns nullptr-owned
  // unique_ptr on failure (e.g. n invalid).
  static std::unique_ptr<FftPlan> make(std::size_t n, unsigned flags = 0u);

  ~FftPlan();
  FftPlan(const FftPlan&)            = delete;
  FftPlan& operator=(const FftPlan&)  = delete;
  FftPlan(FftPlan&&) noexcept;
  FftPlan& operator=(FftPlan&&) noexcept;

  std::size_t size() const noexcept { return n_; }

  // input returns the writable real input buffer (length n).
  std::span<float> input() noexcept;
  // output returns the complex output buffer (length n/2 + 1).
  std::span<std::complex<float>> output() const noexcept;

  // execute runs the r2c transform on input() → output(). Callers then read
  // magnitudes/power from output().
  void execute() noexcept;

  // power_spectrum computes |X|² into `dst` (length n/2), zeroing DC.
  // `scale` multiplies the result (1.0 for un-normalized power). Reuses the
  // internal scratch. Convenience over execute()+manual magnitude.
  void power_spectrum(std::span<float> dst, float scale = 1.0f);

private:
  FftPlan() = default;
  std::size_t                   n_  = 0;
  float*                        in_ = nullptr;
  std::complex<float>*          out_ = nullptr;
  fftwf_plan                    plan_ = nullptr;
};

}  // namespace bootamp::dsp