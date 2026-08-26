// audio/pipeline.hpp — TrackPipeline + PipelineBuilder (the buildPipeline tree).
//
// Port of cliamp/player/pipeline.go. A TrackPipeline bundles a decoded track's
// resources (decoder, optional resampled stream, format, seekable flag,
// known/decoded duration, byte counter, gapless token, live-prefetch handle,
// yt-dlp seek flag + stream_offset). PipelineBuilder.build_pipeline() is the
// branch tree: .ogg radio → ffmpeg fallback, needs-ffmpeg → pipe, native →
// decode_with_ext, final prefetch_network_pipeline wrap. Custom-URI/resolver/
// buffered-URL are dropped (out of MVP — see plan).
#pragma once

#include "audio/format.hpp"
#include "audio/streamer.hpp"
#include "audio/stream_seek_closer.hpp"
#include "audio/live_prefetch.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace bootamp::audio {

// TrackPipeline bundles a decoded track's resources (cliamp trackPipeline).
struct TrackPipeline {
  std::shared_ptr<StreamSeekCloser> decoder;        // raw decoder (Position/Seek)
  std::shared_ptr<Streamer>        stream;         // decoder + optional resample (fed to gapless)
  AudioFormat                       format;
  bool                              seekable       = false;
  std::chrono::duration<double>     known_duration{};   // metadata hint (0=unknown)
  std::chrono::duration<double>     decoded_duration{};  // captured before prefetch
  std::int64_t                      content_length = -1;
  std::string                       path;
  std::chrono::duration<double>     stream_offset{};    // yt-dlp seek-by-restart origin
  bool                              ytdl_seek      = false;
  std::atomic<std::int64_t>*        bytes_read     = nullptr;  // network counter (owned by caller)
  // Owned variant of bytes_read: pipelines built by PipelineBuilder own their
  // counter; close() releases it. External callers set bytes_read directly.
  std::unique_ptr<std::atomic<std::int64_t>> owned_bytes_read;
  std::uint64_t                     gapless_token  = 0;
  bool                              live           = false;
  std::shared_ptr<LivePrefetch>     live_prefetch;

  // close releases the pipeline's resources (decoder + prefetch fill thread).
  void close();
  // interrupt unblocks a pipe decoder without waiting for its process.
  void interrupt();
  // set_known_duration stores the hint and fills missing frame counts on
  // streaming ffmpeg decoders so Len/seeking keep working.
  void set_known_duration(std::chrono::duration<double> d);
};

// close_pipelines closes one or more pipelines no longer in use.
void close_pipelines(std::initializer_list<TrackPipeline*> ps);

// PipelineBuilder builds a ready-to-play TrackPipeline for `path`. This is
// the buildPipeline branch tree (minus custom-URI/resolver/buffered-URL, which
// are out of MVP). `sr` is the engine's working sample rate; `bit_depth`
// selects ffmpeg PCM output. `on_meta` is the ICY title callback (set for URLs).
class PipelineBuilder {
public:
  PipelineBuilder(int sr, int bit_depth, int resample_quality = 4)
    : sr_(sr), bit_depth_(bit_depth), resample_quality_(resample_quality) {}

  // build_pipeline opens and decodes a track: the buildPipeline branch tree —
  // yt-dlp URLs (playlist::is_ytdl) → YtdlpPipeStreamer, HLS .m3u8 URLs →
  // ffmpeg by-URL (+prefetch), generic URLs → open_source chain (native decode,
  // .ogg ffmpeg fallback, needs-ffmpeg stdin pipe), local files → native
  // decode or -ss-restart ffmpeg pipe. `on_meta` is the ICY title callback
  // (URLs only). Radio (M6) keeps its own openers.
  std::expected<std::unique_ptr<TrackPipeline>, std::string>
  build_pipeline(const std::string& path,
                 std::function<void(std::string)> on_meta = {});

  // build_ytdl_pipeline builds a yt-dlp | ffmpeg pipe pipeline for a provider
  // URL (cliamp buildYTDLPipeline). start_sec > 0 seeks by restarting the
  // chain with an input-side ffmpeg -ss. The pipeline sets ytdl_seek and
  // stream_offset = start_sec seconds; not seekable in place (engine seeks by
  // rebuilding via this entry point).
  std::expected<std::unique_ptr<TrackPipeline>, std::string>
  build_ytdl_pipeline(const std::string& page_url, int start_sec);

  // prefetch_network_pipeline wraps `tp->stream` in a LivePrefetch when
  // `enabled` (live/non-seekable HTTP sources), updating tp->live_prefetch.
  std::unique_ptr<TrackPipeline>
  prefetch_network_pipeline(std::unique_ptr<TrackPipeline> tp, bool enabled);

private:
  int sr_;
  int bit_depth_;
  int resample_quality_;
};

}  // namespace bootamp::audio