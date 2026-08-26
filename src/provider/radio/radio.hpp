// provider/radio/radio.hpp — radio Provider (built-in + favorites + catalog).
//
// Port of cliamp/external/radio/{provider.go, catalog.go, favorites.go}. The
// full radio provider: built-in `cliamp radio` URL (kept) +
// ~/.config/bootamp/radios.toml [[station]] (prefix `l:`) + favorites `★` from
// radio_favorites.toml (`f:`, toggle+persist) + Radio Browser catalog
// `/stations/topvote` lazy-paged via LoadCatalogPage (`c:`) + search
// `/stations/byname` via SearchCatalog (`s:`). Tracks(id) → single
// Track{url,title,stream=true,realtime=true}. Catalog/stats JSON via libcurl
// (de1.api.radio-browser.info/json); formatCatalogName (bitrate/country).
#pragma once

#include "playlist/playlist.hpp"
#include "playlist/provider.hpp"
#include "provider/base.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bootamp::provider::radio {

// Built-in cliamp radio stream (kept per plan).
inline constexpr std::string_view kBuiltinName = "cliamp radio";
inline constexpr std::string_view kBuiltinURL  = "https://radio.cliamp.stream/streams.m3u";
inline constexpr std::string_view kRadioBrowserBase = "https://de1.api.radio-browser.info/json";

// Station is a local built-in or user-defined station (radios.toml).
struct Station {
  std::string name;
  std::string url;
};

// CatalogStation is a station from the Radio Browser API (cliamp CatalogStation).
struct CatalogStation {
  std::string name{};       // json: name
  std::string url{};        // json: url_resolved
  std::string country{};    // json: country
  std::string tags{};       // json: tags
  std::string codec{};      // json: codec
  int         bitrate  = 0; // json: bitrate
  int         votes    = 0; // json: votes
  std::string homepage{};   // json: homepage
};

// format_catalog_name builds the display name for a catalog station with
// bitrate/country suffix (cliamp formatCatalogName).
std::string format_catalog_name(const CatalogStation& s);

// Favorites manages a persistent set of favorite radio stations
// (radio_favorites.toml). Thread-safe; Add/Remove persist atomically.
class Favorites {
public:
  // load reads ~/.config/bootamp/radio_favorites.toml.
  static std::shared_ptr<Favorites> load();

  const std::vector<CatalogStation>& stations() const noexcept { return stations_; }
  bool contains(std::string_view url) const;
  std::expected<void, std::string> add(const CatalogStation& s);
  std::expected<void, std::string> remove(std::string_view url);

private:
  Favorites() = default;
  std::vector<CatalogStation>     stations_;
  std::map<std::string, int, std::less<>> by_url_;  // url → index
  mutable std::mutex               mu_;
  std::string                      path_;
};

// Provider is the radio playlist provider. Implements playlist::Provider plus
// FavoriteToggler, CatalogLoader, CatalogSearcher, RadioStatsLoader,
// SectionedList (all via inheritance). IDs are prefixed: l:/f:/c:/s:.
class Provider final : public playlist::Provider,
                       public FavoriteToggler,
                       public CatalogLoader,
                       public CatalogSearcher,
                       public RadioStatsLoader,
                       public SectionedList {
public:
  Provider();

  // --- playlist::Provider ---
  std::string name() const override { return "Radio"; }
  std::expected<std::vector<playlist::PlaylistInfo>, std::string> playlists() override;
  std::expected<std::vector<playlist::Track>, std::string>
  tracks(std::string_view id) override;

  // --- FavoriteToggler ---
  std::expected<std::pair<bool, std::string>, std::string>
  toggle_favorite(std::string_view id) override;

  // --- CatalogLoader ---
  std::expected<int, std::string> load_catalog_page(int offset, int limit) override;

  // --- CatalogSearcher ---
  std::expected<int, std::string> search_catalog(std::string_view query) override;
  void        clear_search() override;
  bool        is_searching() const override;

  // --- RadioStatsLoader ---
  std::expected<RadioStats, std::string> radio_stats() override;

  // --- SectionedList ---
  std::string id_prefix(std::string_view id) const override;
  bool        is_favoritable_id(std::string_view id) const override;

  // append_catalog merges dedup-by-URL catalog stations (cliamp AppendCatalog).
  void append_catalog(const std::vector<CatalogStation>& stations);

private:
  struct ParsedID { char prefix; int idx; };
  static std::expected<ParsedID, std::string> parse_id(std::string_view id);
  playlist::PlaylistInfo catalog_entry(std::string_view prefix, int idx,
                                        const CatalogStation& s) const;

  mutable std::mutex                mu_;
  std::vector<Station>              stations_;       // built-in + radios.toml
  std::shared_ptr<Favorites>        favorites_;
  std::vector<CatalogStation>       catalog_;        // lazy-loaded
  std::vector<CatalogStation>       search_results_; // non-empty when searching
  bool                              searching_ = false;
};

}  // namespace bootamp::provider::radio