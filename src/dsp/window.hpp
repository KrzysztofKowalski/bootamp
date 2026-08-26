// dsp/window.hpp — Hann / Blackman window functions for FFT pre-processing.
//
// Port of cliamp/ui/visualizer.go buildHannWindow. The visualizer applies a
// Hann window before the FFTW3f r2c transform; Blackman is provided for future
// spectrum modes. Both fill `dst` with the symmetric window (denominator
// size-1, matching Go).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace bootamp::dsp {

// hann fills `dst` with a Hann window: w[i] = 0.5*(1 - cos(2*pi*i/(n-1))).
// Matches cliamp buildHannWindow exactly (n-1 denominator).
void hann(std::span<float> dst);

// hann_double — same as hann() but for double-precision buffers (WSOLA uses
// double search ranking; the window itself is fine in float, but this avoids
// a redundant conversion when the caller already works in double).
void hann_double(std::span<double> dst);

// blackman fills `dst` with a Blackman window (classic 3-term).
void blackman(std::span<float> dst);

// WindowKind selects a window by name for the spectrum pipeline.
enum class WindowKind : std::uint8_t { Hann, Blackman };

// apply_window multiplies `frame` element-wise by the named window, in place.
void apply_window(std::span<float> frame, WindowKind kind);

}  // namespace bootamp::dsp