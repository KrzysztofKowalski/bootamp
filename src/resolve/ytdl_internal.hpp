// resolve/ytdl_internal.hpp — internal detail surface of the resolve_ytdl
// module. Shared with sibling resolve module TUs (pls.cpp calls
// detail::humanize_basename) and with tests. Not part of the public module
// interface; do not depend on this from outside the resolve subsystem.
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::resolve::detail {

// ytdl_flat_entry mirrors cliamp resolve.ytdlFlatEntry (resolve.go:525-533):
// the JSON fields read from yt-dlp --flat-playlist output.
struct ytdl_flat_entry {
  std::string url;
  std::string webpage_url;
  std::string title;
  std::string uploader;
  std::string playlist_uploader;
  std::string webpage_url_basename;
  double      duration = 0.0;
};

// ytdl_parse_result: parsed tracks plus the number of source lines emitted by
// yt-dlp (cliamp parseYTDLTracks returns tracks, entries, error).
struct ytdl_parse_result {
  std::vector<playlist::Track> tracks;
  int                          entries = 0;
};

// parse_ytdl_tracks parses newline-delimited --flat-playlist JSON
// (cliamp resolve.go:710-754). Malformed/type-mismatched lines are skipped
// (still counted in entries, matching Go's all-or-nothing json.Unmarshal);
// a line exceeding 1MB yields the bufio "token too long" error, mirroring
// Go's scanner limits (scannerMaxLineSize).
std::expected<ytdl_parse_result, std::string> parse_ytdl_tracks(std::string_view data);

// humanize_basename converts a URL basename like "clr-podcast-467" into
// "clr podcast 467" (cliamp resolve.go:838-846). A trailing known audio
// extension is dropped; non-media suffixes are left intact.
std::string humanize_basename(std::string_view s);

}  // namespace bootamp::resolve::detail
