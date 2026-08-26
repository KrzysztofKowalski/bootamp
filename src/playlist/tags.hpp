// playlist/tags.hpp — TagLib wrapper for reading embedded tags.
//
// cliamp reads ID3v2/Vorbis/MP4 tags via a tag reader; bootamp uses TagLib
// (pacman: taglib). track_tags(path) returns the embedded fields; used by
// track_from_path() for local files. Embedded art + lyrics are surfaced as
// file:// URLs by the local provider.
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace bootamp::playlist {

// TagInfo holds the embedded-tag fields the player/UI display.
struct TagInfo {
  std::string title;
  std::string artist;
  std::string album;
  std::string genre;
  int         year         = 0;
  int         track_number = 0;
  int         duration_secs = 0;
  std::string lyrics;          // embedded lyrics, if present
  // art_cache_path: file:// URL for cached embedded album art, set by caller
  // after extraction. Empty when no art is embedded.
  std::string art_cache_path;
};

// read_tags parses embedded tags from `path` via TagLib. Returns an error
// message on failure (missing file, unreadable). Port of cliamp readTags.
std::expected<TagInfo, std::string> read_tags(std::string_view path);

}  // namespace bootamp::playlist