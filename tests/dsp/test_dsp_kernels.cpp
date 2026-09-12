// tests/dsp/test_dsp_kernels.cpp — Catch2 tests for the dsp kernels that the
// wsola suite (test_dsp.cpp) does not cover: the window tables (dsp/window.*),
// the FFTW3f r2c wrapper (dsp/fft.*), the volume/mono kernels (dsp/volume.*),
// the 10-band biquad EQ (dsp/biquad.*), the spectrum pipeline helpers
// (dsp/spectrum.*) and the libswresample wrapper (dsp/resampler.*).
//
// These are behavioral tests of the shipped implementations (no mocks): window
// formulas are checked against their closed forms, the peaking biquad against
// the Audio EQ Cookbook plus its unity-DC/Nyquist and center-frequency gain
// invariants, the FFT against analytic impulse/sine spectra, and the resampler
// against the rate-ratio frame counts. Stable modules only — no network, no
// sinks, no FTXUI.
#include "dsp/biquad.hpp"
#include "dsp/fft.hpp"
#include "dsp/resampler.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/volume.hpp"
#include "dsp/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace bdsp = bootamp::dsp;

using Catch::Approx;

namespace {

constexpr double kPi = std::numbers::pi;

// Interleaved stereo buffer (both channels carry the same sine).
std::vector<std::array<float, 2>> stereo_sine(double freq, double sr,
                                              std::size_t frames, double amp) {
  std::vector<std::array<float, 2>> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const float v =
        static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / sr));
    out[i] = {v, v};
  }
  return out;
}

// Mono float buffer (the SpectrumAnalyzer's raw-sample input).
std::vector<float> mono_sine(double freq, double sr, std::size_t n, double amp) {
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / sr));
  }
  return out;
}

// Copy an analyzer's returned band span before the next analyze() call
// invalidates it (the span points into the analyzer's internal bands_).
std::vector<float> snap(std::span<const float> bands) {
  return {bands.begin(), bands.end()};
}

// RMS of channel 0 over [begin, end).
double rms_ch0(const std::vector<std::array<float, 2>>& frames, std::size_t begin,
               std::size_t end) {
  double sum = 0;
  for (std::size_t i = begin; i < end; ++i) {
    sum += static_cast<double>(frames[i][0]) * static_cast<double>(frames[i][0]);
  }
  return std::sqrt(sum / static_cast<double>(end - begin));
}

}  // namespace

// ---------------------------------------------------------------------------
// dsp/window.cpp — Hann / Blackman tables (Go buildHannWindow port)
// ---------------------------------------------------------------------------

TEST_CASE("hann is zero at the endpoints, one at the center, and symmetric",
          "[dsp][window]") {
  constexpr std::size_t n = 9;
  std::vector<float> w(n);
  bdsp::hann(std::span<float>(w));

  CHECK(w[0] == Approx(0.0f).margin(1e-6));            // cos(0) = 1
  CHECK(w[n - 1] == Approx(0.0f).margin(1e-6));        // cos(2*pi) = 1
  CHECK(w[(n - 1) / 2] == Approx(1.0f).margin(1e-6));  // cos(pi) = -1

  for (std::size_t i = 0; i < n; ++i) {
    CHECK(w[i] == Approx(w[n - 1 - i]).margin(1e-6));
  }
}

TEST_CASE("hann matches the closed form (n-1 denominator) in float and double",
          "[dsp][window]") {
  constexpr std::size_t n = 64;
  std::vector<float>  wf(n);
  std::vector<double> wd(n);
  bdsp::hann(std::span<float>(wf));
  bdsp::hann_double(std::span<double>(wd));

  for (std::size_t i = 0; i < n; ++i) {
    const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
    const double expect = 0.5 * (1.0 - std::cos(t));
    // The (n-1) denominator distinguishes the Go-matching symmetric window
    // from the periodic variant (which would use n).
    CHECK(wf[i] == Approx(static_cast<float>(expect)).margin(1e-6));
    CHECK(wd[i] == Approx(expect).margin(1e-12));
  }
}

TEST_CASE("blackman is zero at the endpoints and one at the center",
          "[dsp][window]") {
  constexpr std::size_t n = 33;  // odd so the exact center index exists
  std::vector<float> w(n);
  bdsp::blackman(std::span<float>(w));

  CHECK(w[0] == Approx(0.0f).margin(1e-6));      // 0.42 - 0.5 + 0.08
  CHECK(w[n - 1] == Approx(0.0f).margin(1e-6));  // symmetric
  CHECK(w[(n - 1) / 2] == Approx(1.0f).margin(1e-6));  // 0.42 + 0.5 + 0.08

  for (std::size_t i = 0; i < n; ++i) {
    const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
    const double expect = 0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t);
    CHECK(w[i] == Approx(static_cast<float>(expect)).margin(1e-6));
  }
}

TEST_CASE("apply_window multiplies in place with the named window",
          "[dsp][window]") {
  constexpr std::size_t n = 32;
  std::vector<float> hann_ref(n);
  bdsp::hann(std::span<float>(hann_ref));
  std::vector<float> blackman_ref(n);
  bdsp::blackman(std::span<float>(blackman_ref));

  std::vector<float> frame(n);
  for (std::size_t i = 0; i < n; ++i) {
    frame[i] = static_cast<float>(i) * 0.25f;
  }

  auto frame_hann = frame;
  bdsp::apply_window(std::span<float>(frame_hann), bdsp::WindowKind::Hann);
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(frame_hann[i] == Approx(frame[i] * hann_ref[i]).margin(1e-6));
  }

  auto frame_blackman = frame;
  bdsp::apply_window(std::span<float>(frame_blackman), bdsp::WindowKind::Blackman);
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(frame_blackman[i] == Approx(frame[i] * blackman_ref[i]).margin(1e-6));
  }

  // Empty span must be a no-op (no division by zero on the size).
  std::vector<float> empty;
  bdsp::apply_window(std::span<float>(empty), bdsp::WindowKind::Hann);
  CHECK(empty.empty());
}

// ---------------------------------------------------------------------------
// dsp/volume.cpp — db_to_gain, GainCache, apply_volume/apply_mono
// ---------------------------------------------------------------------------

TEST_CASE("db_to_gain is 10^(db/20)", "[dsp][volume]") {
  CHECK(bdsp::db_to_gain(0.0f) == Approx(1.0f).margin(1e-6));
  CHECK(bdsp::db_to_gain(20.0f) == Approx(10.0f).margin(1e-4));
  CHECK(bdsp::db_to_gain(-20.0f) == Approx(0.1f).margin(1e-5));
  CHECK(bdsp::db_to_gain(-6.0f) == Approx(std::pow(10.0, -0.3)).margin(1e-5));
  CHECK(bdsp::db_to_gain(6.0f) == Approx(std::pow(10.0, 0.3)).margin(1e-5));
}

TEST_CASE("GainCache recomputes only when db changes", "[dsp][volume]") {
  bdsp::GainCache cache;

  // NaN-seeded cache: the first call always recomputes.
  const float g1 = cache.gain_for(-6.0f);
  CHECK(g1 == Approx(std::pow(10.0, -0.3)).margin(1e-5));
  CHECK(cache.cached_db == -6.0f);

  // Same dB: the cached gain is returned unchanged.
  CHECK(cache.gain_for(-6.0f) == g1);
  CHECK(cache.cached_db == -6.0f);

  // A changed dB recomputes.
  CHECK(cache.gain_for(0.0f) == Approx(1.0f).margin(1e-6));
  CHECK(cache.cached_db == 0.0f);
  CHECK(cache.gain_for(-6.0f) == Approx(g1).margin(1e-7));

  // NaN dB never compares equal, so it recomputes each call and propagates.
  bdsp::GainCache nan_cache;
  CHECK(std::isnan(nan_cache.gain_for(std::numeric_limits<float>::quiet_NaN())));
  CHECK(std::isnan(nan_cache.cached_db));
}

TEST_CASE("apply_volume scales both channels; scalar and dispatched paths agree",
          "[dsp][volume]") {
  std::vector<std::array<float, 2>> frames(97);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    frames[i] = {static_cast<float>(i) * 0.1f - 4.0f, static_cast<float>(i) * -0.05f + 2.0f};
  }
  auto expected = frames;
  bdsp::apply_volume_scalar(std::span<std::array<float, 2>>(expected), 0.5f);

  auto dispatched = frames;
  bdsp::apply_volume(std::span<std::array<float, 2>>(dispatched), 0.5f);
  CHECK(dispatched == expected);

  // A 0 dB gain (1.0) leaves the samples untouched.
  auto untouched = frames;
  bdsp::apply_volume(std::span<std::array<float, 2>>(untouched), 1.0f);
  CHECK(untouched == frames);
}

TEST_CASE("apply_mono downmixes and apply_volume_mono scales before mixing",
          "[dsp][volume]") {
  std::vector<std::array<float, 2>> frames(64);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    frames[i] = {static_cast<float>(i) * 0.25f, -static_cast<float>(i) * 0.25f};
  }
  auto expected = frames;
  bdsp::apply_mono_scalar(std::span<std::array<float, 2>>(expected));

  auto dispatched = frames;
  bdsp::apply_mono(std::span<std::array<float, 2>>(dispatched));
  CHECK(dispatched == expected);
  CHECK(dispatched[3][0] == Approx(0.0f).margin(1e-7));  // (0.75 + -0.75) / 2

  // Combined pass == gain first, then downmix (the Go volumeStreamer order).
  auto composed = frames;
  bdsp::apply_volume(std::span<std::array<float, 2>>(composed), 2.0f);
  bdsp::apply_mono_scalar(std::span<std::array<float, 2>>(composed));

  auto one_pass = frames;
  bdsp::apply_volume_mono(std::span<std::array<float, 2>>(one_pass), 2.0f);
  CHECK(one_pass == composed);
}

// ---------------------------------------------------------------------------
// dsp/biquad.cpp — 10-band peaking EQ (Audio EQ Cookbook)
// ---------------------------------------------------------------------------

TEST_CASE("calc_coeffs follows the Audio EQ Cookbook peakingEQ", "[dsp][biquad]") {
  const double freq = 320.0, q = 1.4, db = 6.0, sr = 44100.0;
  const bdsp::BiquadCoeffs c = bdsp::calc_coeffs(freq, q, db, sr);

  // Recompute the cookbook formulas independently (RBJ peaking EQ, A=10^(db/40)).
  const double a     = std::pow(10.0, db / 40.0);
  const double w0    = 2.0 * kPi * freq / sr;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0    = 1.0 + alpha / a;
  CHECK(c.active);
  CHECK(c.b0 == Approx((1.0 + alpha * a) / a0).margin(1e-9));
  CHECK(c.b1 == Approx(-2.0 * std::cos(w0) / a0).margin(1e-9));
  CHECK(c.b2 == Approx((1.0 - alpha * a) / a0).margin(1e-9));
  CHECK(c.a1 == Approx(-2.0 * std::cos(w0) / a0).margin(1e-9));
  CHECK(c.a2 == Approx((1.0 - alpha / a) / a0).margin(1e-9));
  // b1 and a1 share the -2*cos(w0) numerator — the FIR/IIR symmetry of RBJ.
  CHECK(c.b1 == Approx(c.a1).margin(1e-12));
}

TEST_CASE("calc_coeffs skip gate bypasses flat bands at +-0.1 dB", "[dsp][biquad]") {
  CHECK_FALSE(bdsp::calc_coeffs(320.0, 1.4, 0.0, 44100.0).active);
  CHECK_FALSE(bdsp::calc_coeffs(320.0, 1.4, 0.05, 44100.0).active);
  CHECK_FALSE(bdsp::calc_coeffs(320.0, 1.4, -0.05, 44100.0).active);
  // Exactly +-kEqSkipDb is still processed (strict inequality in the gate).
  CHECK(bdsp::calc_coeffs(320.0, 1.4, 0.1, 44100.0).active);
  CHECK(bdsp::calc_coeffs(320.0, 1.4, -0.1, 44100.0).active);
  CHECK(bdsp::calc_coeffs(320.0, 1.4, 3.0, 44100.0).active);
  // An inactive band carries zeroed coefficients.
  const bdsp::BiquadCoeffs flat = bdsp::calc_coeffs(320.0, 1.4, 0.0, 44100.0);
  CHECK(flat.b0 == 0.0);
  CHECK(flat.a1 == 0.0);
}

TEST_CASE("peaking biquad has unity gain at DC and Nyquist", "[dsp][biquad]") {
  for (const double db : {-12.0, -3.0, 3.0, 12.0}) {
    const bdsp::BiquadCoeffs c = bdsp::calc_coeffs(1000.0, 1.4, db, 44100.0);
    // H(1)  = (b0+b1+b2)/(1+a1+a2) and H(-1) = (b0-b1+b2)/(1-a1+a2): the
    // cookbook normalizes both sums to the same (2-2cos w0)/a0, so a peaking
    // band must leave the band edges of the spectrum untouched.
    CHECK(c.b0 + c.b1 + c.b2 == Approx(1.0 + c.a1 + c.a2).margin(1e-9));
    CHECK(c.b0 - c.b1 + c.b2 == Approx(1.0 - c.a1 + c.a2).margin(1e-9));
  }
}

TEST_CASE("process_one with an inactive band is a no-op", "[dsp][biquad]") {
  const bdsp::BiquadCoeffs inactive = bdsp::calc_coeffs(600.0, 1.4, 0.0, 44100.0);
  bdsp::Biquad ch_l, ch_r;
  float l = 0.25f, r = -0.75f;
  bdsp::process_one(inactive, ch_l, ch_r, l, r);
  CHECK(l == 0.25f);
  CHECK(r == -0.75f);
  CHECK(ch_l.x1 == 0.0);
  CHECK(ch_l.y1 == 0.0);
  CHECK(ch_r.x2 == 0.0);
}

TEST_CASE("a flat 10-band chain leaves samples untouched", "[dsp][biquad]") {
  bdsp::EqState eq;
  for (std::size_t i = 0; i < bdsp::kEqBands; ++i) {
    bdsp::set_band(eq, i, 0.0, 44100.0);  // every band bypassed
  }
  std::vector<std::array<float, 2>> frames = stereo_sine(440.0, 44100.0, 512, 0.5);
  auto expected = frames;
  bdsp::process_chain_scalar(eq, std::span<std::array<float, 2>>(frames));
  CHECK(frames == expected);
}

TEST_CASE("a peaking band scales its center frequency by 10^(db/20)",
          "[dsp][biquad]") {
  constexpr double sr = 44100.0;
  bdsp::EqState eq;
  eq.sample_rate = sr;
  bdsp::set_band(eq, 4, 12.0, sr);  // 1000 Hz band, +12 dB

  std::vector<std::array<float, 2>> frames = stereo_sine(1000.0, sr, 16384, 0.5);
  const double in_rms = rms_ch0(frames, 0, 8192);
  bdsp::process_chain_scalar(eq, std::span<std::array<float, 2>>(frames));

  // The tail [8192, 16384) is past the transient, so its RMS over the input's
  // RMS is the steady-state gain of the band at its center frequency.
  const double measured = rms_ch0(frames, 8192, 16384) / in_rms;
  const double expect   = std::pow(10.0, 12.0 / 20.0);
  CHECK(measured == Approx(expect).margin(0.05 * expect));
}

TEST_CASE("a cut band attenuates its center frequency by 10^(db/20)",
          "[dsp][biquad]") {
  constexpr double sr = 44100.0;
  bdsp::EqState eq;
  eq.sample_rate = sr;
  bdsp::set_band(eq, 4, -12.0, sr);

  std::vector<std::array<float, 2>> frames = stereo_sine(1000.0, sr, 16384, 0.5);
  const double in_rms = rms_ch0(frames, 0, 8192);
  bdsp::process_chain_scalar(eq, std::span<std::array<float, 2>>(frames));
  const double measured = rms_ch0(frames, 8192, 16384) / in_rms;
  const double expect   = std::pow(10.0, -12.0 / 20.0);
  CHECK(measured == Approx(expect).margin(0.05 * expect));
}

TEST_CASE("set_band resets state on a sample-rate change and keeps it on a gain "
          "change", "[dsp][biquad]") {
  bdsp::EqState eq;
  bdsp::set_band(eq, 0, 6.0, 44100.0);
  std::vector<std::array<float, 2>> frames = stereo_sine(70.0, 44100.0, 8, 0.5);
  bdsp::process_chain_scalar(eq, std::span<std::array<float, 2>>(frames));
  CHECK(eq.left[0].y1 != 0.0);  // state is dirty after processing

  // Same rate + new gain: state survives (live EQ edits stay click-free).
  bdsp::set_band(eq, 1, 3.0, 44100.0);
  CHECK(eq.left[0].y1 != 0.0);

  // A sample-rate change resets every band's state.
  bdsp::set_band(eq, 2, 3.0, 48000.0);
  CHECK(eq.sample_rate == 48000.0);
  for (std::size_t b = 0; b < bdsp::kEqBands; ++b) {
    CHECK(eq.left[b].x1 == 0.0);
    CHECK(eq.left[b].y1 == 0.0);
    CHECK(eq.right[b].x2 == 0.0);
  }
  CHECK(eq.coeffs[2].active);

  // Out-of-range band index is ignored (no crash, no write).
  bdsp::set_band(eq, bdsp::kEqBands, 3.0, 48000.0);
}

// ---------------------------------------------------------------------------
// dsp/spectrum.cpp — log rebin, smoothing, and the analyzer pipeline
// ---------------------------------------------------------------------------

TEST_CASE("build_spectrum_edges anchors at 20 Hz and 20 kHz", "[dsp][spectrum]") {
  CHECK(bdsp::build_spectrum_edges(0).empty());
  CHECK(bdsp::build_spectrum_edges(10) ==
        std::vector<double>(bdsp::kLegacySpectrumEdges.begin(),
                            bdsp::kLegacySpectrumEdges.end()));

  const std::vector<double> one = bdsp::build_spectrum_edges(1);
  CHECK(one.size() == 2);
  CHECK(one.front() == Approx(bdsp::kMinSpectrumHz).margin(1e-9));
  CHECK(one.back() == Approx(bdsp::kMaxSpectrumHz).margin(1e-9));

  const std::vector<double> wide = bdsp::build_spectrum_edges(64);
  CHECK(wide.size() == 65);
  CHECK(wide.front() == Approx(bdsp::kMinSpectrumHz).margin(1e-9));
  CHECK(wide.back() == Approx(bdsp::kMaxSpectrumHz).margin(1e-9));
  for (std::size_t i = 1; i < wide.size(); ++i) {
    CHECK(wide[i] > wide[i - 1]);  // strictly log-increasing
  }
}

TEST_CASE("sample_band_linear interpolates and clamps at the endpoints",
          "[dsp][spectrum]") {
  const std::vector<float> bands = {1.0f, 3.0f, 5.0f};
  CHECK(bdsp::sample_band_linear(bands, 0.0) == Approx(1.0).margin(1e-9));
  CHECK(bdsp::sample_band_linear(bands, 2.0) == Approx(5.0).margin(1e-9));
  CHECK(bdsp::sample_band_linear(bands, 0.5) == Approx(2.0).margin(1e-9));
  CHECK(bdsp::sample_band_linear(bands, 1.25) == Approx(3.5).margin(1e-9));
  CHECK(bdsp::sample_band_linear(bands, -1.0) == Approx(1.0).margin(1e-9));  // clamped
  CHECK(bdsp::sample_band_linear(bands, 9.0) == Approx(5.0).margin(1e-9));   // clamped
  CHECK(bdsp::sample_band_linear({}, 1.0) == 0.0);

  const std::vector<float> single = {7.0f};
  CHECK(bdsp::sample_band_linear(single, 0.0) == Approx(7.0).margin(1e-9));
  CHECK(bdsp::sample_band_linear(single, 5.0) == Approx(7.0).margin(1e-9));
}

TEST_CASE("average_spectrum_range_linear clamps away from the DC bin",
          "[dsp][spectrum]") {
  const std::vector<float> flat(8, 2.0f);
  // lo 0 and hi 7 both clamp into [1, 7]; a constant field averages to itself.
  CHECK(bdsp::average_spectrum_range_linear(flat, 0.0, 7.0) == Approx(2.0).margin(1e-9));
  CHECK(bdsp::average_spectrum_range_linear({}, 1.0, 2.0) == 0.0);

  // Zero-span ranges fall back to a single linear sample at the position.
  const std::vector<float> ramp = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
  CHECK(bdsp::average_spectrum_range_linear(ramp, 3.0, 3.0) ==
        Approx(bdsp::sample_band_linear(ramp, 3.0)).margin(1e-9));

  // Averaging over a real span stays inside the field's min/max.
  const double avg = bdsp::average_spectrum_range_linear(ramp, 1.0, 7.0);
  CHECK(avg > 0.0);
  CHECK(avg < 6.0);
}

TEST_CASE("classic_peak_step rises at 34/s and falls at 10/s", "[dsp][spectrum]") {
  // Rising over one 16 ms tick: cur + (target-cur)*(1-exp(-34*dt)).
  const double rise = bdsp::classic_peak_step(0.0f, 1.0f, 0.016);
  CHECK(rise == Approx(1.0 - std::exp(-34.0 * 0.016)).margin(1e-6));
  // Falling over the same tick is visibly slower.
  const double fall = bdsp::classic_peak_step(1.0f, 0.0f, 0.016);
  CHECK(fall == Approx(std::exp(-10.0 * 0.016)).margin(1e-6));
  CHECK(1.0 - rise < fall);

  // dt == 0 moves nothing.
  CHECK(bdsp::classic_peak_step(0.42f, 1.0f, 0.0) == Approx(0.42f).margin(1e-7));
}

TEST_CASE("normalize_analysis_spec defaults a zero fft_size", "[dsp][spectrum]") {
  bdsp::VisAnalysisSpec spec;
  spec.band_count = 64;
  spec.fft_size   = 0;
  const bdsp::VisAnalysisSpec norm = bdsp::normalize_analysis_spec(spec);
  CHECK(norm.fft_size == bdsp::kDefaultFFTSize);
  CHECK(norm.band_count == 64);

  bdsp::VisAnalysisSpec keep;
  keep.band_count = 0;  // raw-sample mode is passed through
  keep.fft_size   = 512;
  const bdsp::VisAnalysisSpec norm_keep = bdsp::normalize_analysis_spec(keep);
  CHECK(norm_keep.fft_size == 512);
  CHECK(norm_keep.band_count == 0);
}

TEST_CASE("SpectrumAnalyzer gates silence and decays bands by 0.8",
          "[dsp][spectrum]") {
  bdsp::SpectrumAnalyzer analyzer(44100.0);
  bdsp::VisAnalysisSpec spec;  // 10 legacy bands, 2048 FFT

  // Effectively silent input on a fresh analyzer: every band is 0 (0 * 0.8).
  std::vector<float> quiet(2048, 1e-7f);
  const std::vector<float> silent = snap(analyzer.analyze(quiet, spec));
  CHECK(silent.size() == bdsp::kDefaultSpectrumBands);
  for (const float b : silent) {
    CHECK(b == 0.0f);
  }

  // A loud 1 kHz sine produces non-zero bands; remember the levels.
  const std::vector<float> loud = mono_sine(1000.0, 44100.0, 2048, 0.8);
  const std::vector<float> prev = snap(analyzer.analyze(loud, spec));
  CHECK(prev.size() == bdsp::kDefaultSpectrumBands);
  CHECK(*std::max_element(prev.begin(), prev.end()) > 0.2f);

  // Quiet audio now decays every band to exactly 0.8 of the previous level.
  const std::vector<float> decayed = snap(analyzer.analyze(quiet, spec));
  for (std::size_t b = 0; b < prev.size(); ++b) {
    CHECK(decayed[b] == Approx(prev[b] * 0.8f).margin(1e-6));
  }

  // Empty input follows the same decay path.
  const std::vector<float> decayed2 = snap(analyzer.analyze({}, spec));
  for (std::size_t b = 0; b < decayed.size(); ++b) {
    CHECK(decayed2[b] == Approx(decayed[b] * 0.8f).margin(1e-6));
  }
}

TEST_CASE("SpectrumAnalyzer puts a 1 kHz sine's peak in the 800-1600 Hz band",
          "[dsp][spectrum]") {
  bdsp::SpectrumAnalyzer analyzer(44100.0);
  bdsp::VisAnalysisSpec spec;  // legacy 10-band edges: ... 800, 1600 ...

  const std::vector<float> loud  = mono_sine(1000.0, 44100.0, 2048, 0.8);
  const std::vector<float> bands = snap(analyzer.analyze(loud, spec));
  REQUIRE(bands.size() == bdsp::kDefaultSpectrumBands);

  const auto peak = std::max_element(bands.begin(), bands.end());
  const auto peak_band = static_cast<std::size_t>(peak - bands.begin());
  CHECK(peak_band == 4);  // edges[4]=800 .. edges[5]=1600 covers 1000 Hz
  CHECK(*peak > 0.3f);
}

TEST_CASE("SpectrumAnalyzer raw-sample mode and reset_history", "[dsp][spectrum]") {
  bdsp::SpectrumAnalyzer analyzer(44100.0);

  bdsp::VisAnalysisSpec raw;
  raw.band_count = 0;  // waveform modes: no FFT, empty output
  const std::vector<float> short_in = mono_sine(440.0, 44100.0, 512, 0.5);
  CHECK(analyzer.analyze(short_in, raw).empty());

  bdsp::VisAnalysisSpec spec;
  const std::vector<float> loud  = mono_sine(440.0, 44100.0, 2048, 0.8);
  const std::vector<float> bands = snap(analyzer.analyze(loud, spec));
  CHECK(bands.size() == bdsp::kDefaultSpectrumBands);

  // reset_history clears the prev state, so a silent frame decays from zero.
  analyzer.reset_history();
  const std::vector<float> after = snap(analyzer.analyze({}, spec));
  for (const float b : after) {
    CHECK(b == 0.0f);
  }
}

TEST_CASE("SpectrumAnalyzer smoothing snaps on the first tick then eases",
          "[dsp][spectrum]") {
  bdsp::SpectrumAnalyzer analyzer(44100.0);
  bdsp::VisAnalysisSpec spec;

  const std::vector<float> loud  = mono_sine(1000.0, 44100.0, 2048, 0.8);
  const std::vector<float> bands = snap(analyzer.analyze(loud, spec));

  // Before any smoothing tick the raw bands are handed out directly.
  CHECK(snap(analyzer.smoothed_bands()) == bands);

  // The first tick snaps (size mismatch), so the eased output equals bands.
  analyzer.advance_smoothing(0.016);
  CHECK(snap(analyzer.smoothed_bands()) == bands);

  // A second tick with unchanged bands cannot move the eased values.
  analyzer.advance_smoothing(0.016);
  CHECK(snap(analyzer.smoothed_bands()) == bands);

  // Now let the input fall silent: bands decay to 0.8x and the eased values
  // fall toward them by exactly one classicPeak fall step (fall rate 10/s).
  std::vector<float> quiet(2048, 1e-7f);
  const std::vector<float> decayed = snap(analyzer.analyze(quiet, spec));
  const std::vector<float> eased   = snap(analyzer.smoothed_bands());
  analyzer.advance_smoothing(0.016);
  const std::vector<float> after = snap(analyzer.smoothed_bands());
  for (std::size_t i = 0; i < eased.size(); ++i) {
    CHECK(after[i] ==
          Approx(bdsp::classic_peak_step(eased[i], decayed[i], 0.016)).margin(1e-6));
  }
  // Aggregate movement: the eased values followed the decay downward.
  CHECK(after != bands);

  // Long gaps (pause / sleep) are clamped to one 16 ms tick.
  analyzer.advance_smoothing(5.0);
  const std::vector<float> clamped = snap(analyzer.smoothed_bands());
  for (std::size_t i = 0; i < after.size(); ++i) {
    CHECK(clamped[i] ==
          Approx(bdsp::classic_peak_step(after[i], decayed[i], 0.016)).margin(1e-6));
  }
}

// ---------------------------------------------------------------------------
// dsp/fft.cpp — FFTW3f r2c wrapper
// ---------------------------------------------------------------------------

TEST_CASE("FftPlan::make rejects odd and tiny sizes", "[dsp][fft]") {
  CHECK(bdsp::FftPlan::make(0) == nullptr);
  CHECK(bdsp::FftPlan::make(1) == nullptr);
  CHECK(bdsp::FftPlan::make(3) == nullptr);
  CHECK(bdsp::FftPlan::make(7) == nullptr);

  auto small = bdsp::FftPlan::make(2);
  REQUIRE(small != nullptr);
  CHECK(small->size() == 2);
  CHECK(small->input().size() == 2);
  CHECK(small->output().size() == 2);  // n/2 + 1
}

TEST_CASE("r2c of an impulse is flat across every bin", "[dsp][fft]") {
  constexpr std::size_t n = 16;
  auto plan = bdsp::FftPlan::make(n);
  REQUIRE(plan != nullptr);

  std::fill(plan->input().begin(), plan->input().end(), 0.0f);
  plan->input()[0] = 1.0f;  // delta at t=0 → all-ones spectrum
  plan->execute();

  const auto out = plan->output();
  CHECK(out[0].real() == Approx(1.0f).margin(1e-5));
  CHECK(out[0].imag() == Approx(0.0f).margin(1e-5));
  CHECK(out[n / 2].real() == Approx(1.0f).margin(1e-5));
  for (std::size_t j = 0; j < out.size(); ++j) {
    CHECK(std::abs(out[j]) == Approx(1.0f).margin(1e-5));
  }
}

TEST_CASE("a bin-aligned sine lands on its bin and power_spectrum zeroes DC",
          "[dsp][fft]") {
  constexpr std::size_t n   = 8;
  constexpr std::size_t bin = 1;
  auto plan = bdsp::FftPlan::make(n);
  REQUIRE(plan != nullptr);

  for (std::size_t i = 0; i < n; ++i) {
    plan->input()[i] = static_cast<float>(
        std::sin(2.0 * kPi * static_cast<double>(bin) * static_cast<double>(i) /
                 static_cast<double>(n)));
  }
  plan->execute();
  CHECK(std::abs(plan->output()[bin]) ==
        Approx(static_cast<double>(n) / 2.0).margin(1e-4));

  // Full-length power spectrum: DC zeroed, the sine's bin at (N/2)^2, the
  // other non-mirror bins empty. Nyquist (index n/2) is never written.
  std::vector<float> powers(n / 2, -1.0f);
  plan->power_spectrum(std::span<float>(powers));
  CHECK(powers[0] == 0.0f);  // Go: powers[0] = 0
  CHECK(powers[bin] ==
        Approx(static_cast<float>(n) * static_cast<float>(n) / 4.0f).margin(1e-3));
  CHECK(powers[2] == Approx(0.0f).margin(1e-3));
  CHECK(powers[3] == Approx(0.0f).margin(1e-3));

  // A shorter dst is clamped instead of overflowing.
  std::vector<float> short_dst(2, -1.0f);
  plan->power_spectrum(std::span<float>(short_dst));
  CHECK(short_dst[0] == 0.0f);
  CHECK(short_dst[1] == Approx(16.0f).margin(1e-3));

  // An empty dst is a no-op (no crash).
  std::vector<float> empty_dst;
  plan->power_spectrum(std::span<float>(empty_dst));
}

// ---------------------------------------------------------------------------
// dsp/resampler.cpp — libswresample wrapper
// ---------------------------------------------------------------------------

namespace {

// Feed `interleaved` in chunks through `rs`, collecting every produced output
// frame (process output + repeated flush), preserving order.
std::vector<float> drain(bdsp::Resampler& rs, const std::vector<float>& interleaved,
                         std::size_t chunk_frames, std::size_t out_cap_frames) {
  std::vector<float> out;
  std::vector<float> buf(out_cap_frames * 2);
  const std::size_t total_frames = interleaved.size() / 2;
  for (std::size_t pos = 0; pos < total_frames; pos += chunk_frames) {
    const std::size_t n = std::min(chunk_frames, total_frames - pos);
    const std::size_t produced =
        rs.process(std::span<const float>(interleaved.data() + pos * 2, n * 2),
                   std::span<float>(buf));
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<long>(produced * 2));
  }
  for (;;) {
    const std::size_t produced = rs.flush(std::span<float>(buf));
    if (produced == 0) {
      break;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<long>(produced * 2));
  }
  return out;
}

}  // namespace

TEST_CASE("Resampler accessors report the configured rates", "[dsp][resampler]") {
  bdsp::Resampler rs;
  CHECK(rs.configure(44100, 88200));
  CHECK(rs.src_rate() == 44100);
  CHECK(rs.dst_rate() == 88200);
  // Reconfigure in place (the engine's rate-change path).
  CHECK(rs.configure(48000, 44100));
  CHECK(rs.src_rate() == 48000);
  CHECK(rs.dst_rate() == 44100);
}

TEST_CASE("same-rate conversion passes samples through unchanged",
          "[dsp][resampler]") {
  bdsp::Resampler rs;
  REQUIRE(rs.configure(44100, 44100));

  std::vector<float> in;
  in.reserve(200);
  for (int i = 0; i < 100; ++i) {
    in.push_back(static_cast<float>(i) * 0.25f);
    in.push_back(static_cast<float>(-i) * 0.25f);
  }
  const std::vector<float> out = drain(rs, in, /*chunk_frames=*/10, /*out_cap=*/1024);

  // No rate conversion: every input frame must come back, in order, bit-exact.
  REQUIRE(out.size() == in.size());
  CHECK(out == in);

  // Nothing is left buffered after a full drain.
  std::vector<float> buf(2048);
  CHECK(rs.flush(std::span<float>(buf)) == 0);
}

TEST_CASE("up- and downsampling scale the frame count by the rate ratio",
          "[dsp][resampler]") {
  std::vector<float> in;
  for (int i = 0; i < 200; ++i) {
    in.push_back(static_cast<float>(i) * 0.01f);
    in.push_back(0.0f);
  }
  {
    bdsp::Resampler up;
    REQUIRE(up.configure(44100, 88200));
    const std::vector<float> out = drain(up, in, /*chunk_frames=*/20, /*out_cap=*/1024);
    CHECK(out.size() / 2 >= 398);  // 2x within +-2 frames
    CHECK(out.size() / 2 <= 402);
  }
  {
    bdsp::Resampler down;
    REQUIRE(down.configure(88200, 44100));
    const std::vector<float> out = drain(down, in, /*chunk_frames=*/20, /*out_cap=*/1024);
    CHECK(out.size() / 2 >= 98);  // 0.5x within +-2 frames
    CHECK(out.size() / 2 <= 102);
  }
}

TEST_CASE("Resampler ignores degenerate spans", "[dsp][resampler]") {
  bdsp::Resampler rs;
  REQUIRE(rs.configure(44100, 44100));

  std::vector<float> buf(64);
  CHECK(rs.process({}, std::span<float>(buf)) == 0);  // no input
  CHECK(rs.process(std::span<const float>(buf.data(), 1), std::span<float>(buf)) ==
        0);  // < 1 frame
  CHECK(rs.process(std::span<const float>(buf.data(), 4), std::span<float>{}) ==
        0);  // no output room

  // A fresh context has nothing buffered to flush.
  bdsp::Resampler fresh;
  REQUIRE(fresh.configure(44100, 44100));
  CHECK(fresh.flush(std::span<float>(buf)) == 0);
}