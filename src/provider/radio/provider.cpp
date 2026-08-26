// provider/radio/provider.cpp — radio playlist Provider (cliamp provider.go).
//
// Faithful port of cliamp/external/radio/provider.go: the unified radio list
// of built-in + user-defined stations (radios.toml, prefix "l:"), favorites
// (radio_favorites.toml, "★", prefix "f:"), lazy-loaded Radio Browser catalog
// ("c:") and API search results ("s:"). Tracks(id) yields a single
// stream+realtime Track per station. Legacy numeric IDs (no colon) map to
// "l:" local stations exactly like cliamp parseStationID.
#include "provider/radio/radio.hpp"
#include "provider/radio/radio_internal.hpp"

#include "foundation/appdir.hpp"
#include "foundation/fileutil.hpp"
#include "foundation/tomlutil.hpp"

#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::provider::radio {

namespace fs = std::filesystem;

namespace {

// load_stations parses a TOML file with [[station]] sections, keeping only
// entries with both name and url (cliamp provider.go loadStations). A missing
// file yields an empty, successful result; other read errors are reported.
std::expected<std::vector<Station>, std::string> load_stations(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return std::vector<Station>{};
  }
  auto data = foundation::read_file(path);
  if (!data) return std::unexpected{std::move(data).error()};

  std::vector<Station> out;
  foundation::parse_sections(*data, "station", [&out](const foundation::TomlFields& f) {
    auto field = [&f](std::string_view key) {
      auto it = f.find(std::string{key});
      return it != f.end() ? it->second : std::string{};
    };
    Station s{field("name"), field("url")};
    if (!s.name.empty() && !s.url.empty()) {
      out.push_back(std::move(s));
    }
  });
  return out;
}

}  // namespace

Provider::Provider() {
  stations_.push_back(Station{std::string{kBuiltinName}, std::string{kBuiltinURL}});
  // User-defined stations from ~/.config/bootamp/radios.toml (errors ignored,
  // matching cliamp New(): `if extra, err := loadStations(...); err == nil`).
  if (auto dir = foundation::config_dir()) {
    if (auto extra = load_stations(*dir / "radios.toml")) {
      stations_.insert(stations_.end(), extra->begin(), extra->end());
    }
  }
  favorites_ = Favorites::load();
}

std::expected<std::vector<playlist::PlaylistInfo>, std::string>
Provider::playlists() {
  std::lock_guard lk(mu_);

  std::vector<playlist::PlaylistInfo> out;

  // When search is active, show only search results.
  if (searching_) {
    out.reserve(search_results_.size());
    for (std::size_t i = 0; i < search_results_.size(); ++i) {
      out.push_back(catalog_entry("s", static_cast<int>(i), search_results_[i]));
    }
    return out;
  }

  // Local stations (built-in + radios.toml).
  out.reserve(stations_.size() + catalog_.size() + 1);
  for (std::size_t i = 0; i < stations_.size(); ++i) {
    out.push_back(playlist::PlaylistInfo{
        .id = "l:" + std::to_string(i), .name = stations_[i].name});
  }

  // Favorites (★ prefixed).
  const auto& favs = favorites_->stations();
  for (std::size_t i = 0; i < favs.size(); ++i) {
    out.push_back(playlist::PlaylistInfo{
        .id = "f:" + std::to_string(i),
        .name = "★ " + format_catalog_name(favs[i])});
  }

  // Catalog stations.
  for (std::size_t i = 0; i < catalog_.size(); ++i) {
    out.push_back(catalog_entry("c", static_cast<int>(i), catalog_[i]));
  }
  return out;
}

// catalog_entry builds a PlaylistInfo for a CatalogStation, marking favorites
// with ★ (cliamp provider.go catalogEntry).
playlist::PlaylistInfo Provider::catalog_entry(std::string_view prefix, int idx,
                                               const CatalogStation& s) const {
  std::string name = format_catalog_name(s);
  if (favorites_->contains(s.url)) {
    name = "★ " + name;
  }
  return playlist::PlaylistInfo{.id = std::string{prefix} + ":" + std::to_string(idx),
                                .name = std::move(name)};
}

std::expected<std::vector<playlist::Track>, std::string>
Provider::tracks(std::string_view id) {
  std::lock_guard lk(mu_);

  auto parsed = parse_id(id);
  if (!parsed) return std::unexpected{std::move(parsed).error()};

  std::string url, title;
  switch (parsed->prefix) {
    case 'l':
      if (parsed->idx < 0 || static_cast<std::size_t>(parsed->idx) >= stations_.size()) {
        return std::unexpected{"invalid local station index"};
      }
      url = stations_[parsed->idx].url;
      title = stations_[parsed->idx].name;
      break;
    case 'f': {
      const auto& favs = favorites_->stations();
      if (parsed->idx < 0 || static_cast<std::size_t>(parsed->idx) >= favs.size()) {
        return std::unexpected{"invalid favorite index"};
      }
      url = favs[parsed->idx].url;
      title = favs[parsed->idx].name;
      break;
    }
    case 'c':
      if (parsed->idx < 0 || static_cast<std::size_t>(parsed->idx) >= catalog_.size()) {
        return std::unexpected{"invalid catalog station index"};
      }
      url = catalog_[parsed->idx].url;
      title = catalog_[parsed->idx].name;
      break;
    case 's':
      if (!searching_ || parsed->idx < 0 ||
          static_cast<std::size_t>(parsed->idx) >= search_results_.size()) {
        return std::unexpected{"invalid search result index"};
      }
      url = search_results_[parsed->idx].url;
      title = search_results_[parsed->idx].name;
      break;
    default:
      return std::unexpected{"unknown station type"};
  }

  // cliamp Tracks: []Track{{Path: url, Title: title, Stream: true, Realtime: true}}.
  playlist::Track t;
  t.path = std::move(url);
  t.title = std::move(title);
  t.stream = true;
  t.realtime = true;
  return std::vector<playlist::Track>{std::move(t)};
}

// append_catalog merges stations deduplicated by URL (cliamp AppendCatalog).
void Provider::append_catalog(const std::vector<CatalogStation>& stations) {
  std::lock_guard lk(mu_);
  std::map<std::string, bool> seen;
  for (const auto& s : catalog_) {
    seen.emplace(s.url, true);
  }
  for (const auto& s : stations) {
    if (seen.emplace(s.url, true).second) {
      catalog_.push_back(s);
    }
  }
}

std::expected<std::pair<bool, std::string>, std::string>
Provider::toggle_favorite(std::string_view id) {
  std::lock_guard lk(mu_);

  auto parsed = parse_id(id);
  if (!parsed) return std::unexpected{std::move(parsed).error()};

  CatalogStation s;
  switch (parsed->prefix) {
    case 'c':
      if (parsed->idx < 0 || static_cast<std::size_t>(parsed->idx) >= catalog_.size()) {
        return std::unexpected{"invalid catalog index"};
      }
      s = catalog_[parsed->idx];
      break;
    case 's':
      if (!searching_ || parsed->idx < 0 ||
          static_cast<std::size_t>(parsed->idx) >= search_results_.size()) {
        return std::unexpected{"invalid search result index"};
      }
      s = search_results_[parsed->idx];
      break;
    case 'f': {
      const auto& favs = favorites_->stations();
      if (parsed->idx < 0 || static_cast<std::size_t>(parsed->idx) >= favs.size()) {
        return std::unexpected{"invalid favorite index"};
      }
      s = favs[parsed->idx];
      break;
    }
    default:
      return std::unexpected{"cannot favorite local stations"};
  }

  if (favorites_->contains(s.url)) {
    auto r = favorites_->remove(s.url);
    if (!r) return std::unexpected{std::move(r).error()};
    return std::pair{false, s.name};
  }
  auto r = favorites_->add(s);
  if (!r) return std::unexpected{std::move(r).error()};
  return std::pair{true, s.name};
}

std::expected<int, std::string> Provider::load_catalog_page(int offset, int limit) {
  auto stations = detail::top_stations_offset(offset, limit);
  if (!stations) return std::unexpected{std::move(stations).error()};
  const int n = static_cast<int>(stations->size());
  append_catalog(*stations);
  return n;
}

std::expected<int, std::string> Provider::search_catalog(std::string_view query) {
  auto stations = detail::search_stations(query, 200);
  if (!stations) return std::unexpected{std::move(stations).error()};
  const int n = static_cast<int>(stations->size());
  {
    std::lock_guard lk(mu_);
    search_results_ = std::move(*stations);
    searching_ = true;
  }
  return n;
}

void Provider::clear_search() {
  std::lock_guard lk(mu_);
  search_results_.clear();
  searching_ = false;
}

bool Provider::is_searching() const {
  std::lock_guard lk(mu_);
  return searching_;
}

// id_prefix returns the section prefix for a playlist ID ("l", "f", "c", "s"
// or "" when the ID has no colon) — cliamp provider.go idPrefix.
std::string Provider::id_prefix(std::string_view id) const {
  const auto colon = id.find(':');
  if (colon == std::string_view::npos) return "";
  return std::string{id.substr(0, colon)};
}

// is_favoritable_id — cliamp IsCatalogOrFavID: "c:", "f:" or "s:" prefixed IDs.
bool Provider::is_favoritable_id(std::string_view id) const {
  return id.starts_with("c:") || id.starts_with("f:") || id.starts_with("s:");
}

// parse_id splits a prefixed ID like "c:42" into prefix + index. Legacy
// numeric IDs (no colon) are treated as "l:" local station indices, and an
// empty prefix (e.g. ":5") yields '\0' which falls through to the "unknown
// station type" error in the callers — matching cliamp parseStationID.
std::expected<Provider::ParsedID, std::string> Provider::parse_id(std::string_view id) {
  std::string_view idx_str = id;
  char prefix = 'l';
  const auto colon = id.find(':');
  if (colon != std::string_view::npos) {
    prefix = (colon == 0) ? '\0' : id[0];
    idx_str = id.substr(colon + 1);
  }
  auto idx = detail::strict_atoi(idx_str);
  if (!idx) return std::unexpected{"invalid station ID"};
  return ParsedID{prefix, *idx};
}

// format_catalog_name builds a display name from a CatalogStation with
// bitrate/country suffix (cliamp provider.go formatCatalogName).
std::string format_catalog_name(const CatalogStation& s) {
  std::string name = s.name;
  if (s.bitrate > 0) {
    name += " [" + std::to_string(s.bitrate) + "k]";
  }
  if (!s.country.empty()) {
    name += " · " + s.country;
  }
  return name;
}

}  // namespace bootamp::provider::radio
