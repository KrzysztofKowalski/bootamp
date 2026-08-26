// resolve/pls.hpp — PLS (INI-style) playlist parser.
//
// Port of cliamp/resolve/pls.go. parse_pls reads a PLS stream and returns
// entries sorted by number. pls_entries_to_tracks collapses radio mirror
// servers (radio_mirrors / strip_mirror_suffix) to the first URL, matching
// VLC/Winamp behavior. realtime = IsURL && HasLength && Length<0.
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::resolve {

// PLS entry (cliamp plsEntry).
struct PlsEntry {
  int         num        = 0;
  std::string file;
  std::string title;
  int         length     = 0;
  bool        has_length = false;
};

// parse_pls reads PLS text `content` and returns entries sorted by number.
// Port of cliamp parsePLS.
std::expected<std::vector<PlsEntry>, std::string>
parse_pls(std::string_view content);

// pls_entries_to_tracks converts parsed PLS entries to playlist tracks,
// collapsing radio mirrors to the first URL. Port of cliamp plsEntriesToTracks.
std::vector<playlist::Track> pls_entries_to_tracks(const std::vector<PlsEntry>& es);

// radio_mirrors reports whether PLS entries are all indefinite-length streams
// of the same station (mirror servers to collapse). Port of cliamp radioMirrors.
bool radio_mirrors(const std::vector<PlsEntry>& es);

}  // namespace bootamp::resolve