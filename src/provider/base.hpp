// provider/base.hpp — Provider base + optional capability mixins.
//
// Port of cliamp/provider/{types.go, interfaces.go}. cliamp splits the
// provider contract across ~15 small Go interfaces discovered at runtime via
// type assertions. bootamp collapses them into one base Provider (playlist::
// Provider) plus optional capability mixin interfaces the UI queries with
// dynamic_cast. This keeps the v-table surface small while preserving every
// capability the UI needs. Track.hpp holds the shared Track type alias.
#pragma once

#include "playlist/playlist.hpp"
#include "playlist/provider.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace bootamp::provider {

// ArtistInfo describes an artist in a provider's catalog (cliamp ArtistInfo).
struct ArtistInfo {
  std::string id;
  std::string name;
  int         album_count = 0;
};

// AlbumInfo describes an album in a provider's catalog (cliamp AlbumInfo).
struct AlbumInfo {
  std::string id;
  std::string name;
  std::string artist;
  std::string artist_id;
  int         year        = 0;
  int         track_count = 0;
  std::string genre;
};

// SortType describes one sort option for album listing.
struct SortType {
  std::string id;     // e.g. "alphabeticalByName"
  std::string label;  // e.g. "By Name"
};

// year_from_date extracts the year from a "YYYY-MM-DD" or bare "YYYY" date
// string. Returns 0 when the string doesn't start with a 4-digit year.
int year_from_date(std::string_view date);

// ProviderMeta key constants used across providers (cliamp provider/types.go).
inline constexpr std::string_view kMetaNavidromeID = "navidrome.id";
inline constexpr std::string_view kMetaJellyfinID  = "jellyfin.id";
inline constexpr std::string_view kMetaEmbyID      = "emby.id";
inline constexpr std::string_view kMetaNetEaseID   = "netease.id";
inline constexpr std::string_view kMetaQobuzID     = "qobuz.id";
inline constexpr std::string_view kMetaTidalID     = "tidal.id";

// --- Capability mixins (optional; queried via dynamic_cast) ---------------

// Searcher — providers that support track search.
class Searcher {
public:
  virtual ~Searcher() = default;
  virtual std::expected<std::vector<playlist::Track>, std::string>
  search_tracks(std::string_view query, int limit) = 0;
};

// ArtistBrowser — providers that list artists and their albums.
class ArtistBrowser {
public:
  virtual ~ArtistBrowser() = default;
  virtual std::expected<std::vector<ArtistInfo>, std::string> artists() = 0;
  virtual std::expected<std::vector<AlbumInfo>, std::string>
  artist_albums(std::string_view artist_id) = 0;
};

// AlbumBrowser — paginated album listing with configurable sort.
class AlbumBrowser {
public:
  virtual ~AlbumBrowser() = default;
  virtual std::expected<std::vector<AlbumInfo>, std::string>
  album_list(std::string_view sort_type, int offset, int size) = 0;
  virtual std::vector<SortType> album_sort_types() const = 0;
  virtual std::string default_album_sort() const = 0;
};

// AlbumTrackLoader — providers that return an album's tracks.
class AlbumTrackLoader {
public:
  virtual ~AlbumTrackLoader() = default;
  virtual std::expected<std::vector<playlist::Track>, std::string>
  album_tracks(std::string_view album_id) = 0;
};

// PlaybackReporter — providers that accept now-playing/scrobble reports.
class PlaybackReporter {
public:
  virtual ~PlaybackReporter() = default;
  virtual bool can_report_playback(const playlist::Track& t) const = 0;
  // best-effort: report functions return the failure for logging, never block.
  virtual std::expected<void, std::string>
  report_now_playing(const playlist::Track& t, double position_secs, bool can_seek) = 0;
  virtual std::expected<void, std::string>
  report_scrobble(const playlist::Track& t, double elapsed_secs,
                  double duration_secs, bool can_seek) = 0;
};

// ProgressReporter — PlaybackReporter + interim position updates.
class ProgressReporter : public PlaybackReporter {
public:
  virtual std::expected<void, std::string>
  report_progress(const playlist::Track& t, double position_secs) = 0;
};

// FavoriteToggler — providers that mark items as favorites (radio stations).
class FavoriteToggler {
public:
  virtual ~FavoriteToggler() = default;
  // returns (added, name) — added=true if it became a favorite, false if removed.
  virtual std::expected<std::pair<bool, std::string>, std::string>
  toggle_favorite(std::string_view id) = 0;
};

// CatalogLoader — providers that lazy-load catalog pages (Radio Browser API).
class CatalogLoader {
public:
  virtual ~CatalogLoader() = default;
  // returns the number of items added.
  virtual std::expected<int, std::string>
  load_catalog_page(int offset, int limit) = 0;
};

// CatalogSearcher — providers with server-side catalog search.
class CatalogSearcher {
public:
  virtual ~CatalogSearcher() = default;
  virtual std::expected<int, std::string> search_catalog(std::string_view query) = 0;
  virtual void clear_search() = 0;
  virtual bool is_searching() const = 0;
};

// RadioStatsLoader — radio providers exposing aggregate listener stats.
struct RadioStationStats {
  int    total_sessions    = 0;
  double total_listen_hours = 0.0;
  int    peak_listeners    = 0;
  int    active_listeners  = 0;
};
struct RadioStats {
  int                                       total_sessions    = 0;
  double                                    total_listen_hours = 0.0;
  int                                       peak_listeners    = 0;
  std::map<std::string, RadioStationStats>  stations;
};
class RadioStatsLoader {
public:
  virtual ~RadioStatsLoader() = default;
  virtual std::expected<RadioStats, std::string> radio_stats() = 0;
};

// SectionedList — providers whose playlist list has logical sections.
class SectionedList {
public:
  virtual ~SectionedList() = default;
  // id_prefix returns the section prefix for a playlist ID (e.g. "f"/"c"/"s").
  virtual std::string id_prefix(std::string_view id) const = 0;
  virtual bool is_favoritable_id(std::string_view id) const = 0;
};

// Closer — providers holding resources that should be released on shutdown.
class Closer {
public:
  virtual ~Closer() = default;
  virtual void close() = 0;
};

// CustomStreamer — providers needing a custom audio decode path for
// non-standard URI schemes (e.g. spotify:track:xxx). Deferred (out of MVP).
class CustomStreamer {
public:
  virtual ~CustomStreamer() = default;
  virtual std::vector<std::string> uri_schemes() const = 0;
  // new_streamer(uri) → (decoder, format, duration, error). The decoder is an
  // audio::StreamSeekCloser; declared opaque here to avoid an audio/ dep cycle.
  virtual std::expected<std::tuple<void*, playlist::Track, double>, std::string>
  new_streamer(std::string_view uri) = 0;
};

}  // namespace bootamp::provider