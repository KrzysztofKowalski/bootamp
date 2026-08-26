// dsp/spectrum.hpp — log rebin + attack/decay smoothing + silence gate.
//
// Port of cliamp/ui/visualizer.go Analyze(). The pipeline is:
//   raw samples → Hann window → FFTW3f r2c → |X|² → log-rebin into N bands
//   → dB-like scaling (10*log10(power)+10)/50 clamped [0,1] → fast-attack /
//   slow-decay temporal smoothing → silence gate (max|s| < 1e-5 ⇒ decay 0.8).
// Two band counts are supported: 10 (legacy spectrum edges) and 64.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace bootamp::dsp {

// Forward-declared so SpectrumAnalyzer can hold an owning FFTW3f plan without
// pulling fft.hpp into every consumer of this header. Defined in fft.hpp.
class FftPlan;

inline constexpr std::size_t kDefaultSpectrumBands = 10;
inline constexpr double      kMinSpectrumHz        = 20.0;
inline constexpr double      kMaxSpectrumHz        = 20000.0;
inline constexpr double      kSilenceGateAbs       = 1e-5;  // max|s| below ⇒ silent

// Legacy log spectrum edges (cliamp legacySpectrumEdges): 20,100,...,16000,20000.
inline constexpr std::array<double, kDefaultSpectrumBands + 1> kLegacySpectrumEdges = {
  kMinSpectrumHz, 100, 200, 400, 800, 1600, 3200, 6400, 12800, 16000, kMaxSpectrumHz};

// build_spectrum_edges returns `count+1` log-interpolated edges between the
// legacy anchors (cliamp buildSpectrumEdges). Empty for count==0.
std::vector<double> build_spectrum_edges(std::size_t count);

// sample_band_linear returns the linearly-interpolated value of `bands` at the
// fractional position `pos` (cliamp sampleBandLinear). Endpoint-clamped.
double sample_band_linear(std::span<const float> bands, double pos);

// average_spectrum_range_linear averages `magnitudes` over the fractional index
// range [loPos, hiPos] using up-sampled linear interpolation (cliamp
// averageSpectrumRangeLinear). Returns 0 for empty input.
double average_spectrum_range_linear(std::span<const float> magnitudes,
                                      double loPos, double hiPos);

// classic_peak_step eases `current` toward `target` over `dt` seconds using the
// classicPeak bar rates — 34/s while rising, 10/s while falling (cliamp
// classicPeakStep in vis_classic_peak.go). The sub-tick easing used by
// advance_smoothing; computed in double like the Go original.
float classic_peak_step(float current, float target, double dt);

// VisAnalysisSpec selects an analysis pass (cliamp VisAnalysisSpec). band_count
// == 0 ⇒ raw-sample mode (no FFT, the caller reads waveform samples directly).
struct VisAnalysisSpec {
  std::size_t band_count = kDefaultSpectrumBands;
  std::size_t fft_size   = 2048;  // kDefaultFFTSize (kept literal to avoid fft.hpp dep)
  bool operator==(const VisAnalysisSpec&) const = default;
};

// normalize_analysis_spec clamps band_count>=0 and fft_size>0 (defaults
// fft_size to kDefaultFFTSize when <=0). Port of cliamp NormalizeAnalysisSpec.
VisAnalysisSpec normalize_analysis_spec(VisAnalysisSpec spec);

// SpectrumAnalyzer holds the per-spec FFT plan, Hann window, edges, and
// previous band state across frames. Cached by (band_count, fft_size) so
// consecutive ticks with the same spec allocate nothing.
class SpectrumAnalyzer {
public:
  SpectrumAnalyzer() = default;
  explicit SpectrumAnalyzer(double sample_rate);
  // Out-of-line: the owning FftPlan is only complete inside spectrum.cpp
  // (fft.hpp); declaring the dtor here keeps spectrum.hpp free of FFTW types.
  ~SpectrumAnalyzer();

  // analyze runs the full pipeline on `samples` and returns a span of `band_count`
  // band levels in [0,1]. The returned span is valid until the next analyze()
  // call with the same spec. When spec.band_count == 0, returns an empty span
  // (raw-sample mode — caller reads waveform separately).
  std::span<const float> analyze(std::span<const float> samples, const VisAnalysisSpec& spec);

  // smoothed_bands returns the eased per-frame band values (classicPeak step).
  // Falls back to raw bands until smoothing has run at least once.
  std::span<const float> smoothed_bands() const noexcept {
    if (!smoothed_.empty() && smoothed_.size() == bands_.size()) {
      return smoothed_;
    }
    return bands_;
  }

  // advance_smoothing eases smoothed_ toward the latest analyze() output by
  // `dt` seconds (fast-attack / slow-decay, port of advanceSmoothing).
  void advance_smoothing(double dt);

  // reset_history clears the prev-band state (used on driver/spec change).
  void reset_history();

private:
  // ensure_cache rebuilds the per-spec state (prev bands, log edges, Hann
  // window, power scratch, FFT plan) whenever the analysis spec changes.
  void ensure_cache(const VisAnalysisSpec& spec);

  struct Cached {
    VisAnalysisSpec    spec;
    std::vector<float>  prev;        // previous-frame bands for attack/decay
    std::vector<double> edges;
  };
  double               sr_ = 0;
  std::vector<float>   bands_;      // latest raw analysis output
  std::vector<float>   smoothed_;   // eased output
  // Single shared cache slot: the visualizer only ever runs one spec at a time.
  std::unique_ptr<FftPlan> fft_;    // FFTW3f r2c plan for cache_.spec.fft_size
  std::vector<float>   window_;     // cached Hann window (cache_.spec.fft_size)
  std::vector<float>   powers_;     // scratch |X|² buffer (fft_size/2)
  Cached               cache_;
};

}  // namespace bootamp::dsp