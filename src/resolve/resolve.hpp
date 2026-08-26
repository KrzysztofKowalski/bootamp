// resolve/resolve.hpp — expand CLI args into playable tracks.
//
// Port of cliamp/resolve/resolve.go (the Args classifier + URL helpers). Args
// separates CLI arguments into immediately-resolved local tracks and pending
// remote URLs (feeds/M3U/PLS/YT) that need async HTTP/yt-dlp. resolve_m3u /
// resolve_pls fetch (≤1MB via raw-socket HTTP) and parse; is_hls_playlist routes
// .m3u8 to ffmpeg-by-URL; radio_mirrors / strip_mirror_suffix collapse PLS
// mirror servers. realtime = isURL && duration<=0.
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::resolve {

// Result holds the output of Args: instantly-resolved tracks + pending remote
// URLs (feeds/M3U/PLS/YT) that need async HTTP fetching. Port of cliamp Result.
struct Result {
  std::vector<playlist::Track> tracks;
  std::vector<std::string>     pending;  // async-fetch URLs
};

// args separates CLI arguments into local tracks and pending remote URLs.
// Port of cliamp Args (the classify branch: IsFeed/IsM3U/IsPLS/IsYouTube/IsYTDL/
// IsXiaoyuzhou/sniffFeedURL ⇒ pending; else local file/dir/glob).
std::expected<Result, std::string> args(const std::vector<std::string>& argv);

// expand_yt_playlist controls whether YouTube URLs with a list= parameter
// expand the full playlist or resolve as a single video (cliamp
// ExpandYTPlaylist, default true). Set from config ytmusic.expand_playlist.
bool expand_yt_playlist();
void set_expand_yt_playlist(bool v);

// set_ytdl_cookies_from configures yt-dlp --cookies-from-browser for resolve
// (cliamp resolve.SetYTDLCookiesFrom). Empty = disable.
void set_ytdl_cookies_from(std::string_view browser);
std::string_view ytdl_cookies_from();

// resolve_m3u fetches (≤1MB) and parses a remote .m3u/.m3u8 playlist.
std::expected<std::vector<playlist::Track>, std::string>
resolve_m3u(std::string_view url);

// resolve_pls fetches and parses a remote .pls playlist, collapsing radio
// mirrors to the first URL.
std::expected<std::vector<playlist::Track>, std::string>
resolve_pls(std::string_view url);

// resolve_local_m3u / resolve_local_pls parse a local playlist file.
std::expected<std::vector<playlist::Track>, std::string>
resolve_local_m3u(std::string_view path);
std::expected<std::vector<playlist::Track>, std::string>
resolve_local_pls(std::string_view path);

// is_hls_playlist reports whether `url` ends in .m3u8 (HLS — ffmpeg-by-URL).
bool is_hls_playlist(std::string_view url);

// strip_mirror_suffix removes a trailing " (#N)" or " #N" suffix that radio PLS
// files use to distinguish mirror servers.
std::string strip_mirror_suffix(std::string_view s);

}  // namespace bootamp::resolve