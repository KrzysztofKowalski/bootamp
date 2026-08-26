// provider/radio/favorites.cpp — persistent favorite radio stations
// (cliamp favorites.go).
//
// Faithful port of cliamp/external/radio/favorites.go: favorites live in
// ~/.config/bootamp/radio_favorites.toml as [[station]] blocks. Persistence
// uses foundation::fileutil::write_file_atomic (tmp + fsync + rename) instead
// of Go's WriteFile(".tmp")+Rename, per the project contract; the serialized
// content matches Go's fmt %q output byte for byte. The class is thread-safe
// (mutex) even though cliamp relied on the Provider mutex alone.
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

inline constexpr std::string_view kFavoritesFile = "radio_favorites.toml";

// go_quote mirrors Go's fmt %q (strconv.Quote) for TOML string values:
// surrounding double quotes, Go-style escapes for control characters, and
// printable (incl. non-ASCII) characters left literal. Uppercase forms like
// \uHHHH never appear because Go's %q writes UTF-8 bytes directly.
std::string go_quote(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\a': out += "\\a";  break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      case '\v': out += "\\v";  break;
      default:
        if (c < 0x20 || c == 0x7f) {
          out += "\\x";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0x0F]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

// save_favorites builds the TOML content in memory and writes it atomically
// so a partial or failed write can never truncate or corrupt the existing
// favorites file (cliamp favorites.go save). The layout matches Go exactly:
// a blank line between stations, %q for strings, %d for the bitrate.
std::expected<void, std::string> save_favorites(const std::string& path,
                                                const std::vector<CatalogStation>& stations) {
  std::string content;
  for (std::size_t i = 0; i < stations.size(); ++i) {
    const CatalogStation& s = stations[i];
    if (i > 0) content.push_back('\n');
    content += "[[station]]\n";
    content += "name = " + go_quote(s.name) + "\n";
    content += "url = " + go_quote(s.url) + "\n";
    if (!s.country.empty()) {
      content += "country = " + go_quote(s.country) + "\n";
    }
    if (s.bitrate > 0) {
      content += "bitrate = " + std::to_string(s.bitrate) + "\n";
    }
    if (!s.codec.empty()) {
      content += "codec = " + go_quote(s.codec) + "\n";
    }
    if (!s.tags.empty()) {
      content += "tags = " + go_quote(s.tags) + "\n";
    }
    if (!s.homepage.empty()) {
      content += "homepage = " + go_quote(s.homepage) + "\n";
    }
  }
  // Go writes the temp file with 0o644; write_file_atomic applies the mode
  // (ANDed with the existing file's bits when it already exists).
  return foundation::write_file_atomic(path, content, 0644);
}

// load_favorite_stations parses the favorites TOML file (cliamp
// loadFavoriteStations). A missing file yields an empty, successful result.
std::expected<std::vector<CatalogStation>, std::string>
load_favorite_stations(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return std::vector<CatalogStation>{};
  }
  auto data = foundation::read_file(path);
  if (!data) return std::unexpected{std::move(data).error()};

  std::vector<CatalogStation> out;
  foundation::parse_sections(*data, "station", [&out](const foundation::TomlFields& f) {
    auto field = [&f](std::string_view key) {
      auto it = f.find(std::string{key});
      return it != f.end() ? it->second : std::string{};
    };
    CatalogStation s;
    s.name     = field("name");
    s.url      = field("url");
    s.country  = field("country");
    s.codec    = field("codec");
    s.tags     = field("tags");
    s.homepage = field("homepage");
    // bitrate: only set when it parses as a number (Go's `err == nil` check).
    if (auto it = f.find("bitrate"); it != f.end()) {
      if (auto n = detail::strict_atoi(it->second); n) {
        s.bitrate = *n;
      }
    }
    if (!s.name.empty() && !s.url.empty()) {
      out.push_back(std::move(s));
    }
  });
  return out;
}

}  // namespace

std::shared_ptr<Favorites> Favorites::load() {
  // Cannot use make_shared: the constructor is private.
  auto f = std::shared_ptr<Favorites>(new Favorites());
  auto dir = foundation::config_dir();
  if (!dir) return f;
  f->path_ = (*dir / kFavoritesFile).string();
  auto stations = load_favorite_stations(f->path_);
  if (!stations) return f;  // unreadable favorites are ignored (cliamp LoadFavorites)
  f->stations_ = std::move(*stations);
  for (std::size_t i = 0; i < f->stations_.size(); ++i) {
    f->by_url_[f->stations_[i].url] = static_cast<int>(i);
  }
  return f;
}

bool Favorites::contains(std::string_view url) const {
  std::lock_guard lk(mu_);
  return by_url_.find(url) != by_url_.end();
}

std::expected<void, std::string> Favorites::add(const CatalogStation& s) {
  std::lock_guard lk(mu_);
  if (by_url_.contains(s.url)) return {};  // duplicate add is a no-op
  by_url_[s.url] = static_cast<int>(stations_.size());
  stations_.push_back(s);
  if (path_.empty()) {
    auto dir = foundation::config_dir();
    if (!dir) return std::unexpected{std::move(dir).error()};
    path_ = (*dir / kFavoritesFile).string();
  }
  return save_favorites(path_, stations_);
}

std::expected<void, std::string> Favorites::remove(std::string_view url) {
  std::lock_guard lk(mu_);
  auto it = by_url_.find(url);
  if (it == by_url_.end()) return {};  // removing a non-favorite is a no-op
  stations_.erase(stations_.begin() + it->second);
  // Indices after the removed station shift; rebuild the URL → index map.
  by_url_.clear();
  for (std::size_t i = 0; i < stations_.size(); ++i) {
    by_url_[stations_[i].url] = static_cast<int>(i);
  }
  if (path_.empty()) {
    auto dir = foundation::config_dir();
    if (!dir) return std::unexpected{std::move(dir).error()};
    path_ = (*dir / kFavoritesFile).string();
  }
  return save_favorites(path_, stations_);
}

}  // namespace bootamp::provider::radio
