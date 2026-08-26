// audio/format.hpp — AudioFormat describing the PCM stream layout.
//
// cliamp uses beep.Format{SampleRate, NumChannels, Precision}; bootamp's
// equivalent carries the same three fields plus an explicit bit-depth flag
// (the ffmpeg pipe emits either s16le or f32le).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace bootamp::audio {

struct AudioFormat {
  int sample_rate = 44100;  // Hz
  int channels    = 2;       // stereo (bootamp downmixes everything to 2)
  int precision   = 2;       // bytes per sample (2=s16le, 4=f32le)
  int bit_depth   = 16;       // 16 or 32 (selects ffmpeg pcm_s16le | pcm_f32le)

  bool operator==(const AudioFormat&) const = default;
};

// --- helpers (implemented in format.cpp) -------------------------------------
//
// Ports of the beep format helpers cliamp relies on (gopxl/beep Format /
// SampleRate, used in player.go speaker.Init, tap sizing, and position math).

// frame_size returns the byte width of one PCM frame: channels × precision.
// Port of beep (Format).Width().
std::size_t frame_size(const AudioFormat& f) noexcept;

// frames_in returns the number of frames that last for duration `d` at
// f.sample_rate. Port of beep SampleRate.N(d) — truncating; clamped to 0
// for d <= 0 or an invalid sample rate.
std::size_t frames_in(const AudioFormat& f, std::chrono::duration<double> d) noexcept;

// duration_of returns the duration of `frames` frames at f.sample_rate.
// Port of beep SampleRate.D(n) — exact double seconds.
std::chrono::duration<double> duration_of(const AudioFormat& f, std::size_t frames) noexcept;

}  // namespace bootamp::audio