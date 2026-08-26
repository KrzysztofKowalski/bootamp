// resolve/m3u.hpp — M3U/.m3u8 parser (local + remote).
//
// Port of cliamp/resolve/m3u.go. parse_m3u reads an M3U stream and extracts
// entries with EXTINF metadata. Handles UTF-8 BOM, \r\n, missing #EXTM3U
// header, bare entries. Relative paths resolve against base_dir (empty for
// remote). m3u_entry_to_track converts to playlist::Track with realtime flag.
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::resolve {

// M3U entry with optional EXTINF metadata (cliamp m3uEntry).
struct M3UEntry {
  std::string path;
  std::string title;
  int         duration = -1;  // seconds, -1 = unknown
};

// parse_m3u reads M3U text `content` and returns entries. `base_dir` resolves
// relative paths (empty for remote). Port of cliamp parseM3U.
std::expected<std::vector<M3UEntry>, std::string>
parse_m3u(std::string_view content, std::string_view base_dir = {});

// m3u_entry_to_track converts a parsed entry to a playlist::Track. realtime =
// isURL && duration<=0. Port of cliamp m3uEntryToTrack.
playlist::Track m3u_entry_to_track(const M3UEntry& e);

// entries_to_tracks converts a parsed M3U list to playlist tracks.
std::vector<playlist::Track> entries_to_tracks(const std::vector<M3UEntry>& es);

}  // namespace bootamp::resolve