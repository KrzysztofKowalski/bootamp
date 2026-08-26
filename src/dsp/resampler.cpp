// dsp/resampler.cpp — libswresample wrapper (src-sr → device-sr).
//
// Per the plan: the engine resamples source-rate audio to the device rate and
// miniaudio is opened at the device rate with its own resampling OFF (no
// double-resample). This is the classic swr_alloc → swr_alloc_set_opts →
// swr_init → swr_convert lifecycle in RAII. Note: on the installed ffmpeg 9.0
// (libswresample 7) the legacy swr_alloc_set_opts (int64 channel mask) has
// been REMOVED — we use its documented replacement swr_alloc_set_opts2 with
// an AVChannelLayout, which allocates the context itself. Semantics are
// identical: set in/out layout, sample format and rates, then swr_init().
//
// Conversion is interleaved float32 stereo (the engine's std::array<float,2>
// frame layout, libsndfile-style). swr_convert consumes as much input as it
// can map to output; unconsumed input stays buffered inside the context and
// flush() drains it at end-of-stream. Library-driven — no hand SIMD here.
// NEVER -ffast-math (libswresample is an external lib; float in/out unchanged).
#include "dsp/resampler.hpp"

// Arch's libswresample header has no extern "C" guard (and pulls in avutil
// headers that also lack one) — without this wrap the C functions get C++
// mangling and the link fails with undefined swr_* symbols.
extern "C" {
#include <libswresample/swresample.h>
}

#include <cstddef>
#include <cstdint>

namespace bootamp::dsp {

Resampler::Resampler() = default;

Resampler::~Resampler() { swr_free(&ctx_); }

bool Resampler::configure(int src_rate, int dst_rate, int channels) {
  // reconfigure: drop any previous context (frees its buffered state too)
  swr_free(&ctx_);

  src_ = src_rate;
  dst_ = dst_rate;
  ch_  = channels;

  // Channel layout: stereo (the engine's default) and mono use the static
  // layouts — plain brace initializers, C++-compatible (no designated
  // initializers in the AV_CHANNEL_LAYOUT_MASK macro). Anything else defers
  // to av_channel_layout_default.
  AVChannelLayout layout{};
  if (channels == 2) {
    layout = AV_CHANNEL_LAYOUT_STEREO;
  } else if (channels == 1) {
    layout = AV_CHANNEL_LAYOUT_MONO;
  } else {
    av_channel_layout_default(&layout, channels);
  }

  // swr_alloc_set_opts2 allocates the context when *ps == NULL (we just
  // freed it above). Interleaved float32 on both sides — the channel layout
  // is the only transform beyond pure rate conversion. On error the lib
  // frees the context and nulls *ps, so no cleanup needed on this branch.
  const int ret = swr_alloc_set_opts2(
      &ctx_, &layout, AV_SAMPLE_FMT_FLT, dst_rate, &layout, AV_SAMPLE_FMT_FLT,
      src_rate, /*log_offset=*/0, /*log_ctx=*/nullptr);
  if (ret < 0) {
    return false;
  }
  if (swr_init(ctx_) < 0) {
    swr_free(&ctx_);
    return false;
  }
  return true;
}

std::size_t Resampler::process(std::span<const float> in,
                               std::span<float> out_dst) {
  if (ctx_ == nullptr || in.size() < 2 || out_dst.size() < 2) {
    return 0;
  }
  // Interleaved stereo: 2 floats per frame. Counts are in frames.
  const std::size_t in_frames  = in.size() / 2;
  const std::size_t out_frames = out_dst.size() / 2;

  uint8_t*       out_ptr = reinterpret_cast<uint8_t*>(out_dst.data());
  const uint8_t* in_ptr  = reinterpret_cast<const uint8_t*>(in.data());

  // For packed (interleaved) formats only the first pointer is used. Excess
  // input that cannot yet be mapped to output stays buffered inside ctx_
  // (returned as 0 output frames when the buffer is too small or the filter
  // needs more input); flush() drains it at end-of-stream.
  const int produced = swr_convert(ctx_, &out_ptr, static_cast<int>(out_frames),
                                   &in_ptr, static_cast<int>(in_frames));
  return produced > 0 ? static_cast<std::size_t>(produced) : 0;
}

std::size_t Resampler::flush(std::span<float> out_dst) {
  if (ctx_ == nullptr || out_dst.size() < 2) {
    return 0;
  }
  const std::size_t cap = out_dst.size() / 2;
  std::size_t       total = 0;
  // swr_convert with NULL input and 0 in_count flushes the internal buffer
  // (filter delay tail). Repeat until it reports no more output or the
  // caller's buffer is full.
  while (total < cap) {
    uint8_t* out_ptr =
        reinterpret_cast<uint8_t*>(out_dst.data() + total * 2);
    const int produced =
        swr_convert(ctx_, &out_ptr, static_cast<int>(cap - total), nullptr, 0);
    if (produced <= 0) {
      break;
    }
    total += static_cast<std::size_t>(produced);
  }
  return total;
}

}  // namespace bootamp::dsp
