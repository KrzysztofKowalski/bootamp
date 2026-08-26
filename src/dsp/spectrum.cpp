// dsp/spectrum.cpp — Hann → FFTW3f r2c → |X|² → log-rebin → attack/decay smoothing.
//
// Port of cliamp/ui/visualizer.go Analyze() plus the free helpers it uses:
// buildSpectrumEdges, sampleBandLinear, averageSpectrumRangeLinear, and
// classicPeakStep (vis_classic_peak.go). The Go visualizer runs a hand-rolled
// radix-2 FFT over complex128; bootamp uses FFTW3f (plan decision #2), so the
// power spectrum is float32, but every averaging / dB / smoothing step is kept
// in double exactly like Go (and the WSOLA rule: search ranking stays in
// double). NEVER -ffast-math — the log10/clamp math must stay NaN-safe.
//
// The pipeline per analyze() call (1:1 with Go):
//   1. silence gate — max|s| < 1e-5 (or empty input) skips the FFT, decaying
//      every band by ×0.8 (two orders cheaper than the FFT; fires on pause /
//      between tracks / quiet audio)
//   2. window the first min(len, fftSize) samples into the plan's real input
//      buffer with the cached Hann window, zero the tail
//   3. FFTW3f r2c → |X|² for bins [0, fftSize/2), DC zeroed
//   4. per band: averageSpectrumRangeLinear over the log-rebinned edge range,
//      dB-like scale (10*log10(sum)+10)/50, clamp [0,1], then temporal
//      smoothing — fast attack (0.6/0.4) when rising, slow decay (0.25/0.75)
//      when falling; the result becomes next frame's prev state.
#include "dsp/spectrum.hpp"

#include "dsp/fft.hpp"
#include "dsp/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace bootamp::dsp {

namespace {

// Smoothing cadence inside advance_smoothing (cliamp TickAnim = 16 ms). dt is
// clamped to this step so long gaps (sleep, paused, stalled frame) ease like
// ~1 frame instead of integrating over a huge interval (cliamp
// maxSmoothDtFrames = 10).
inline constexpr double kSmoothTickSeconds = 0.016;
inline constexpr double kMaxSmoothDtFrames = 10.0;

// Silence decay factor (cliamp Analyze: bands[b] = prev[b] * 0.8).
inline constexpr double kSilentDecay = 0.8;

// Attack/decay blend weights (cliamp Analyze). Rising: bands = val*0.6 +
// prev*0.4; falling: bands = val*0.25 + prev*0.75.
inline constexpr double kAttackPrevWeight = 0.4;
inline constexpr double kDecayPrevWeight  = 0.75;

}  // namespace

// ---------------------------------------------------------------------------
// Free helpers (ports of the Go functions in ui/visualizer.go)
// ---------------------------------------------------------------------------

std::vector<double> build_spectrum_edges(std::size_t count) {
  if (count == 0) {
    return {};
  }
  std::vector<double> edges(count + 1);
  const std::size_t last_anchor = kLegacySpectrumEdges.size() - 1;  // 10
  for (std::size_t i = 0; i <= count; ++i) {
    const std::size_t numerator = i * last_anchor;
    const std::size_t idx       = numerator / count;
    if (idx >= last_anchor) {
      edges[i] = kLegacySpectrumEdges[last_anchor];
      continue;
    }
    if (numerator % count == 0) {
      edges[i] = kLegacySpectrumEdges[idx];
      continue;
    }
    // Log-interpolate between the two surrounding legacy anchors.
    const double frac = static_cast<double>(numerator % count) / static_cast<double>(count);
    const double lo   = kLegacySpectrumEdges[idx];
    const double hi   = kLegacySpectrumEdges[idx + 1];
    edges[i] = std::pow(10.0, std::log10(lo) * (1 - frac) + std::log10(hi) * frac);
  }
  return edges;
}

double sample_band_linear(std::span<const float> bands, double pos) {
  switch (bands.size()) {
    case 0:
      return 0;
    case 1:
      return bands[0];
    default:
      break;
  }
  if (pos <= 0) {
    return bands[0];
  }
  const double last = static_cast<double>(bands.size() - 1);
  if (pos >= last) {
    return bands[bands.size() - 1];
  }
  const std::size_t idx  = static_cast<std::size_t>(pos);
  const double      frac = pos - static_cast<double>(idx);
  return static_cast<double>(bands[idx]) * (1 - frac) +
         static_cast<double>(bands[idx + 1]) * frac;
}

double average_spectrum_range_linear(std::span<const float> magnitudes,
                                     double lo_pos, double hi_pos) {
  if (magnitudes.empty()) {
    return 0;
  }
  // Positions are clamped to [1, len-1]: the DC bin (position 0) is never
  // averaged in (Go minPos = 1.0). Hand-rolled clamp instead of std::clamp:
  // the len == 1 case has minPos > maxPos, which std::clamp rejects.
  const double min_pos = 1.0;
  const double max_pos = static_cast<double>(magnitudes.size() - 1);
  lo_pos = std::max(min_pos, std::min(max_pos, lo_pos));
  hi_pos = std::max(min_pos, std::min(max_pos, hi_pos));
  hi_pos = std::max(lo_pos, hi_pos);

  const double span = hi_pos - lo_pos;
  if (span <= 0) {
    return sample_band_linear(magnitudes, lo_pos);
  }
  // Up-sampled sampling: 4..32 samples depending on the span width (Go
  // sampleCount = max(4, min(32, ceil(span*2)))).
  const int sample_count = static_cast<int>(std::clamp(std::ceil(span * 2.0), 4.0, 32.0));
  double    sum          = 0;
  for (int i = 0; i < sample_count; ++i) {
    const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(sample_count);
    sum += sample_band_linear(magnitudes, lo_pos + t * span);
  }
  return sum / static_cast<double>(sample_count);
}

float classic_peak_step(float current, float target, double dt) {
  // classicPeakBarRiseRate = 34/s, classicPeakBarFallRate = 10/s. Computed in
  // double like the Go original; the result is rounded to float for storage.
  const double rate = target > current ? 34.0 : 10.0;
  return static_cast<float>(current + (target - current) * (1 - std::exp(-rate * dt)));
}

VisAnalysisSpec normalize_analysis_spec(VisAnalysisSpec spec) {
  // Go: BandCount < 0 → 0 (unreachable: band_count is std::size_t; 0 is the
  // raw-sample mode) and FFTSize <= 0 → defaultFFTSize.
  if (spec.fft_size == 0) {
    spec.fft_size = kDefaultFFTSize;
  }
  return spec;
}

// ---------------------------------------------------------------------------
// SpectrumAnalyzer
// ---------------------------------------------------------------------------

SpectrumAnalyzer::SpectrumAnalyzer(double sample_rate) : sr_(sample_rate) {}

SpectrumAnalyzer::~SpectrumAnalyzer() = default;

// ensure_cache rebuilds the per-spec state (prev bands, log edges, Hann
// window, power scratch, FFT plan) whenever the analysis spec changes. Single
// shared slot — the visualizer only ever runs one spec at a time.
void SpectrumAnalyzer::ensure_cache(const VisAnalysisSpec& spec) {
  if (cache_.spec == spec && !cache_.prev.empty()) {
    return;
  }
  cache_.spec  = spec;
  cache_.prev.assign(spec.band_count, 0.0f);
  cache_.edges = build_spectrum_edges(spec.band_count);
  window_.assign(spec.fft_size, 0.0f);
  hann(window_);
  powers_.assign(spec.fft_size / 2, 0.0f);
  if (fft_ && fft_->size() != spec.fft_size) {
    fft_.reset();
  }
}

std::span<const float> SpectrumAnalyzer::analyze(std::span<const float> samples,
                                                 const VisAnalysisSpec& spec_in) {
  const VisAnalysisSpec spec = normalize_analysis_spec(spec_in);

  if (spec.band_count == 0) {
    // Raw-sample mode (wave/scope/heartbeat): no FFT output. The caller reads
    // the waveform from its own tap; band state is untouched.
    bands_.clear();
    return {};
  }

  ensure_cache(spec);
  bands_.resize(spec.band_count);

  // Silence gate: skip the FFT pipeline when the input is empty, effectively
  // silent, or the sample rate is unset. A max-abs scan is ~two orders cheaper
  // than the FFT and fires whenever playback is paused, between tracks, or
  // quiet. Silent frames decay every band by ×0.8.
  const auto decay_to_silence = [this, &spec]() -> std::span<const float> {
    for (std::size_t b = 0; b < spec.band_count; ++b) {
      bands_[b] = static_cast<float>(cache_.prev[b] * kSilentDecay);
      cache_.prev[b] = bands_[b];
    }
    return bands_;
  };

  bool silent = samples.empty();
  if (!silent) {
    float max_abs = 0.0f;
    for (const float s : samples) {
      const float a = std::fabs(s);
      if (a > max_abs) {
        max_abs = a;
      }
    }
    silent = max_abs < kSilenceGateAbs || sr_ <= 0;
  }
  if (silent) {
    return decay_to_silence();
  }

  if (!fft_) {
    fft_ = FftPlan::make(spec.fft_size);
  }
  if (!fft_) {
    // FFTW plan failed to build (e.g. non-supported size): decay like silence
    // rather than emitting garbage band levels.
    return decay_to_silence();
  }

  // Window samples into the plan's reusable real input buffer. The tail beyond
  // `have` is explicitly zeroed — the buffer persists between calls (Go does
  // the same for its complex FFT buffer).
  std::span<float>   in   = fft_->input();
  const std::size_t  have = std::min(samples.size(), spec.fft_size);
  for (std::size_t i = 0; i < have; ++i) {
    in[i] = samples[i] * window_[i];
  }
  for (std::size_t i = have; i < spec.fft_size; ++i) {
    in[i] = 0.0f;
  }

  fft_->execute();
  fft_->power_spectrum(powers_);  // |X|² for bins [0, fft_size/2), DC zeroed

  const double bin_hz = sr_ / static_cast<double>(spec.fft_size);
  for (std::size_t b = 0; b < spec.band_count; ++b) {
    const double sum = average_spectrum_range_linear(
        powers_, cache_.edges[b] / bin_hz, cache_.edges[b + 1] / bin_hz);

    // dB-like scale: 10*log10(power) == 20*log10(magnitude). Skipping the
    // sqrt per bin halves the FFT work; the log absorbs the factor of two so
    // band values stay in the same [0,1] range (Go comment). A zero sum keeps
    // the band at 0.
    double val = 0.0;
    if (sum > 0) {
      val = (10.0 * std::log10(sum) + 10.0) / 50.0;
    }
    val = std::clamp(val, 0.0, 1.0);

    // Temporal smoothing: fast attack, slow decay (Go Analyze).
    const double prev = cache_.prev[b];
    if (val > prev) {
      val = val * (1.0 - kAttackPrevWeight) + prev * kAttackPrevWeight;
    } else {
      val = val * (1.0 - kDecayPrevWeight) + prev * kDecayPrevWeight;
    }
    const float out = static_cast<float>(val);
    cache_.prev[b]  = out;
    bands_[b]       = out;
  }
  return bands_;
}

void SpectrumAnalyzer::advance_smoothing(double dt) {
  if (bands_.empty()) {
    return;
  }
  if (smoothed_.size() != bands_.size()) {
    // First frame after a spec change snaps to the current analysis output so
    // existing levels appear immediately instead of fading in from zero (Go
    // advanceSmoothing).
    smoothed_ = bands_;
    return;
  }
  // Clamp dt: long gaps (pause, sleep, stalled frame) step like ~1 frame
  // instead of easing over a huge interval (Go maxSmoothDtFrames).
  if (dt <= 0 || dt > kMaxSmoothDtFrames * kSmoothTickSeconds) {
    dt = kSmoothTickSeconds;
  }
  for (std::size_t i = 0; i < smoothed_.size(); ++i) {
    smoothed_[i] = classic_peak_step(smoothed_[i], bands_[i], dt);
  }
}

void SpectrumAnalyzer::reset_history() {
  // Port of Go resetSpectrumHistory (clears prevBySpec): the prev state
  // restarts from zero on the next analyze. Raw/smoothed bands are untouched.
  std::fill(cache_.prev.begin(), cache_.prev.end(), 0.0f);
}

}  // namespace bootamp::dsp
