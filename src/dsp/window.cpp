// src/dsp/window.cpp — Hann / Blackman window tables (see dsp/window.hpp).
//
// Port of cliamp/ui/visualizer.go buildHannWindow — identical formula so the
// FFT pipeline matches the Go spectrum within float32 rounding:
//
//     w[i] = 0.5 * (1 - cos(2*pi*i/(n-1)))
//
// The (n-1) denominator (NOT n) makes the window symmetric across the n-point
// frame, exactly as in Go. Like Go's Visualizer.windowCache, callers on the
// visualizer tick should build one table per FFTSize and reuse it — these
// builders recompute a transcendental per element. All math is done in double
// and stored in the destination type (Go computes in float64).
//
// Blackman is the classic 3-term window (0.42 / -0.5 / +0.08), provided for
// future spectrum modes; cliamp itself has no Blackman window.

#include "dsp/window.hpp"

#include <cmath>

namespace bootamp::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Value of the symmetric Hann window at index i of an n-point table.
// n == 1 divides by zero and yields NaN — same behavior as Go's
// buildHannWindow (0.5*(1 - cos(0/0))).
inline double hann_value(std::size_t i, std::size_t n) {
  const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
  return 0.5 * (1.0 - std::cos(t));
}

}  // namespace

void hann(std::span<float> dst) {
  for (std::size_t i = 0; i < dst.size(); ++i) {
    dst[i] = static_cast<float>(hann_value(i, dst.size()));
  }
}

void hann_double(std::span<double> dst) {
  for (std::size_t i = 0; i < dst.size(); ++i) {
    dst[i] = hann_value(i, dst.size());
  }
}

void blackman(std::span<float> dst) {
  const std::size_t n = dst.size();
  for (std::size_t i = 0; i < n; ++i) {
    const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
    dst[i] = static_cast<float>(0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t));
  }
}

void apply_window(std::span<float> frame, WindowKind kind) {
  const std::size_t n = frame.size();
  switch (kind) {
    case WindowKind::Hann:
      for (std::size_t i = 0; i < n; ++i) {
        frame[i] *= static_cast<float>(hann_value(i, n));
      }
      break;
    case WindowKind::Blackman:
      for (std::size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
        frame[i] *= static_cast<float>(0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t));
      }
      break;
  }
}

}  // namespace bootamp::dsp
