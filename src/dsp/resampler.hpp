// dsp/resampler.hpp — libswresample wrapper (src-sr → device-sr).
//
// Per the plan: the engine resamples source-rate audio to the device rate;
// miniaudio is opened at the device rate with its own resampling OFF (no
// double-resample). This wraps swr_alloc/set_opts/init/convert in a small RAII
// type. Library-driven (no hand SIMD here).
#pragma once

#include <cstddef>
#include <memory>
#include <span>

struct SwrContext;

namespace bootamp::dsp {

// Resampler converts stereo float32 frames from src_rate to dst_rate using
// libswresample. Reusable across rate changes via reconfigure(). NOT
// thread-safe (the audio thread is the sole owner).
class Resampler {
public:
  Resampler();
  ~Resampler();
  Resampler(const Resampler&)            = delete;
  Resampler& operator=(const Resampler&) = delete;

  // configure sets src→dst rates and the stereo f32 in/out layout. Returns
  // false if libswresample rejected the options.
  bool configure(int src_rate, int dst_rate, int channels = 2);

  // process converts `in` (src-rate f32 interleaved) and writes up to
  // out_dst.size() frames of dst-rate f32 to out_dst. Returns the number of
  // output frames produced. May be 0 if not enough input has accumulated.
  std::size_t process(std::span<const float> in, std::span<float> out_dst);

  // flush drains any internally buffered samples at end-of-stream. Returns the
  // number of output frames written.
  std::size_t flush(std::span<float> out_dst);

  // src_rate / dst_rate accessors.
  int src_rate() const noexcept { return src_; }
  int dst_rate() const noexcept { return dst_; }

private:
  SwrContext* ctx_ = nullptr;
  int         src_ = 0;
  int         dst_ = 0;
  int         ch_  = 2;
};

}  // namespace bootamp::dsp