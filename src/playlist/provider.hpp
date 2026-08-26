// playlist/provider.hpp — PlaylistInfo + Provider interface.
//
// Port of cliamp/playlist/provider.go. Provider is the base interface every
// playlist source implements (radio, local, Navidrome, Spotify...). Optional
// capability interfaces (Authenticator, Refresher) live here; the extended
// capabilities (Searcher/ArtistBrowser/...) live in provider/base.hpp.
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::playlist {

// PlaylistInfo describes a playlist with name + track count (cliamp
// PlaylistInfo). duration_secs==0 means "unknown"; section groups rows under a
// header (the radio provider uses SectionedList.IDPrefix instead).
struct PlaylistInfo {
  std::string id;
  std::string name;
  int         track_count   = 0;
  int         duration_secs = 0;  // 0 = unknown
  std::string section{};            // optional grouping label
};

// Provider is the base interface for playlist sources.
class Provider {
public:
  virtual ~Provider() = default;
  virtual std::string name() const = 0;
  virtual std::expected<std::vector<PlaylistInfo>, std::string> playlists() = 0;
  virtual std::expected<std::vector<Track>, std::string>
  tracks(std::string_view playlist_id) = 0;
};

// Authenticator is optionally implemented by providers requiring sign-in.
class Authenticator {
public:
  virtual ~Authenticator() = default;
  virtual std::expected<void, std::string> authenticate() = 0;
};

// Refresher is optionally implemented by providers that cache data and support
// invalidating that cache.
class Refresher {
public:
  virtual ~Refresher() = default;
  virtual void refresh() = 0;
};

// kErrNeedsAuth — providers that require interactive sign-in return this.
inline constexpr std::string_view kErrNeedsAuth = "sign-in required";

}  // namespace bootamp::playlist