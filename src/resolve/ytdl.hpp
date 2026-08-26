// resolve/ytdl.hpp — resolve yt-dlp URLs via --flat-playlist -j.
//
// Per the plan: drop kkdai/youtube. resolve_ytdl runs
//   yt-dlp --flat-playlist -j --socket-timeout 15 [--cookies-from-browser B]
//          [--playlist-start S+1 --playlist-end E] <url>
// (30s timeout), parses newline-JSON into Track{path,title,artist,stream=true,
// duration_secs} via nlohmann::json. Honors expand_playlist config. Port of
// cliamp resolve/resolve.go:525-754 (resolveYTDL).
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::resolve {

// resolve_ytdl resolves a yt-dlp URL (YT/YTM/SC/Bilibili/Bandcamp/ytsearch) into
// a flat track list via `yt-dlp --flat-playlist -j`. Returns an error message
// on failure (timeout, non-zero exit, unparseable JSON).
std::expected<std::vector<playlist::Track>, std::string>
resolve_ytdl(std::string_view url);

// resolve_ytdl_with_bounds is the bounded variant used when expanding a
// playlist range (start/end are 1-based; 0 = unbounded). Port of cliamp
// resolveYTDL playlist-range handling.
std::expected<std::vector<playlist::Track>, std::string>
resolve_ytdl_with_bounds(std::string_view url, int start, int end);

}  // namespace bootamp::resolve