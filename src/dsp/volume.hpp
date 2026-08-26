// dsp/volume.hpp — gain (10^(db/20)) + mono downmix in one pass, AVX2.
//
// Port of cliamp's volume/mono application. The hot path multiplies each stereo
// sample by a per-channel gain computed from volume_db, and optionally downmixes
// to mono ((l+r)/2 on both channels). AVX2+FMA processes 8 floats (4 frames)
// per iteration; scalar fallback otherwise. NEVER -ffast-math.
#pragma once

#include <array>
#include <limits>
#include <span>

namespace bootamp::dsp {

// db_to_gain returns 10^(db/20). The volume control stores its value in dB;
// the DSP multiplies samples by this gain. NaN/Inf preserved (no fast-math).
// Not constexpr (uses expf); defined in volume.cpp.
float db_to_gain(float db);

// GainCache ports cliamp's volumeStreamer gain caching (cachedDB/cachedGain):
// the linear gain 10^(db/20) is recomputed only when the dB value changes
// (rare), never per buffer. Feed it the engine's volume_db atomic once per
// buffer, then apply_volume with the returned gain. cached_db starts NaN to
// force the first compute — mirrors Go's `cachedDB: math.NaN()` (NaN != any
// value, so the first gain_for() call always recomputes). NOT thread-safe:
// the audio thread is the sole owner.
struct GainCache {
  float cached_db   = std::numeric_limits<float>::quiet_NaN();
  float cached_gain = 1.0f;

  // gain_for returns the cached gain, recomputing only when `db` changed.
  float gain_for(float db);  // defined in volume.cpp
};

// apply_volume scales each stereo frame by `gain` (the same gain on both
// channels). Dispatches to AVX2 when supported.
void apply_volume(std::span<std::array<float, 2>> frames, float gain);

// apply_mono downmixes each frame to mono ((l+r)/2) on both channels.
void apply_mono(std::span<std::array<float, 2>> frames);

// apply_volume_mono does both in one pass: scale by gain then downmix.
// Used by the engine when both volume and mono are active. AVX2 path.
void apply_volume_mono(std::span<std::array<float, 2>> frames, float gain);

// Explicit kernels for benchmarks / golden tests.
void apply_volume_scalar(std::span<std::array<float, 2>> frames, float gain);
void apply_volume_avx2(std::span<std::array<float, 2>> frames, float gain);
void apply_mono_scalar(std::span<std::array<float, 2>> frames);
void apply_mono_avx2(std::span<std::array<float, 2>> frames);

}  // namespace bootamp::dsp