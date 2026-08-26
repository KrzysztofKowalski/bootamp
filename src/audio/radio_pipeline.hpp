// audio/radio_pipeline.hpp — M6 live-radio pipeline.
//
// Port of the radio parts of cliamp player/pipeline.go + openSource
// (player/decode.go:190-250) + applyStreamTitle (daemon.go:1099-1114).
//
// open_source builds the complete radio chain for a stream URL:
//   HttpClient (raw-socket HTTP/1.1, ICY-prefix rewrite, Icy-MetaData: 1)
//   → icy_reader (strip interleaved metadata, parse StreamTitle → on_meta)
//   → stall_reader (10s per-read timeout, cancel closes the socket)
//   → ffmpeg stdin pipe (pcm s16le|f32le, cliamp ffmpeg.go args)
//   → live_prefetch wrap (4s SPSC ring, decode off the audio thread)
// HLS URLs (.m3u8) and OGG radio streams are opened by ffmpeg directly from
// the URL (-i <url>): ffmpeg must resolve relative chunklist/segment URIs
// itself, and the chained-ogg decoder is deferred (plan M2 — take Go's
// buildChainedOggPipeline failure fallback).
//
// Classification (cliamp openSource): live = any icy-* response header;
// prefetch = live || content_length < 0; a Content-Type overrides a ".mp3"
// URL extension when recognized (ext_from_content_type).
//
// apply_stream_title folds the ICY StreamTitle into the display state
// ("Artist - Title" split, raw fallback, Station = the entry's own title when
// the ICY title differs) — the TUI status-line contract (daemon.go).
#pragma once

#include "audio/pipeline.hpp"     // TrackPipeline
#include "playlist/playlist.hpp"  // Track

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::audio {

// open_source opens a stream URL (http:// or https://) and returns a
// ready-to-play TrackPipeline: the reader chain (socket → [icy] → stall) fed
// into a stdin-pipe ffmpeg decoder, wrapped in LivePrefetch when the source is
// live/unknown-length; HLS/.ogg take the by-URL ffmpeg path. `on_meta` is
// invoked with each parsed ICY StreamTitle (the engine publishes it to the
// stream-title atomic). Errors: "open source: <http error>", "open hls: <err>",
// "decode: <err>" (cliamp buildPipeline wording). Port of pipeline.go
// 237-248 + 273-320.
std::expected<std::unique_ptr<TrackPipeline>, std::string>
open_source(const std::string& url, int sample_rate, int bit_depth,
            std::function<void(std::string)> on_meta = {});

// StreamTitleInfo is the folded display state for a stream's live ICY title
// (the ipc.TrackInfo fields set by cliamp applyStreamTitle).
struct StreamTitleInfo {
  std::string title;         // split "Title", the raw value, or cur.title
  std::string artist;        // split "Artist", or cur.artist
  std::string station;       // cur.title when the ICY title differs
  std::string stream_title;  // the raw ICY StreamTitle value
};

// apply_stream_title folds a stream's live ICY metadata into the display
// fields, mirroring the TUI's resolveTrackDisplay: split "Artist - Title" on
// the first " - " (title part must be non-empty), fall back to the raw value,
// keep the entry's own title when the tag carries no song, and set Station to
// the entry title when the ICY title differs. Non-stream tracks and empty
// titles leave the fields untouched. Port of daemon.go:1099-1114.
StreamTitleInfo apply_stream_title(const playlist::Track& cur,
                                   std::string_view stream_title);

// detail — classification helpers ported 1:1 from cliamp openSource. Extracted
// pure so tests can pin them without sockets; not part of the public API.
namespace detail {

// Any response header key starting with "icy-" ⇒ live radio
// (cliamp decode.go:236-242). HttpResponse headers are lowercased.
bool has_icy_header(const std::vector<std::pair<std::string, std::string>>& headers);

// icy-metaint value: strict integer parse (strconv.Atoi parity — trailing
// garbage is invalid); returns the value (callers require > 0), 0 when
// absent/invalid. cliamp decode.go:230-234.
int parse_icy_metaint(std::string_view v);

// prefetch = live || content_length < 0 (cliamp decode.go:247).
bool stream_prefetch(bool live, std::int64_t content_length);

// Content-Type extension override: only when the URL extension is ".mp3" and
// the Content-Type is non-empty and recognized; "" = no override
// (cliamp pipeline.go:263-268).
std::string ext_override_from_content_type(std::string_view ext, std::string_view ct);

}  // namespace detail
}  // namespace bootamp::audio
