// provider/local/provider.cpp — local TOML-playlist Provider.
//
// Port of cliamp/external/local/provider.go + dirs.go. Reads/writes
// TOML-based playlists stored under <config_dir()>/playlists/. Implements the
// contract in provider/local/provider.hpp: playlist::Provider
// (playlists/tracks), Searcher, and the playlist-management methods
// (add_track_to_playlist, add_tracks_to_playlist, save_playlist,
// create_playlist, delete_playlist, remove_track, rename_playlist,
// set_bookmark). Path traversal is rejected (safe_path).
//
// Deviations from cliamp (all forced by the bootamp contract header):
//   - No history store: cliamp prepends a virtual "Recently Played" playlist
//     served from history/. bootamp has no history module, so Playlists()
//     never includes it, Tracks("Recently Played") returns an empty list
//     (cliamp's nil-store behavior), and the name stays reserved against
//     mutations. ClearHistory / SetBookmarkByPath / DirSources /
//     AddDirSource(s) / CreateDirPlaylist / Exists are not in the contract
//     header and are not ported.
//   - PlaylistInfo::section stays empty (cliamp local has no sections).
#include "provider/local/provider.hpp"

#include "provider/local/internal.hpp"

#include "audio/decode.hpp"       // supported_exts() (cliamp player.SupportedExts)
#include "foundation/appdir.hpp"  // config_dir()
#include "foundation/fuzzy.hpp"   // fuzzy_match (cliamp internal/fuzzy.Match)
#include "foundation/tomlutil.hpp"  // parse_named_sections
#include "playlist/playlist.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace bootamp::provider::local {

namespace fs = std::filesystem;

namespace {

// --- small string helpers --------------------------------------------------

std::string ascii_lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  }
  return r;
}

std::string trim_space(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                        s.front() == '\n' || s.front() == '\v' || s.front() == '\f'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                        s.back() == '\n' || s.back() == '\v' || s.back() == '\f'))
    s.remove_suffix(1);
  return std::string(s);
}

// filepath.Ext — the suffix beginning at the final dot in the final element.
std::string ext_of(std::string_view path) {
  for (std::size_t i = path.size(); i-- > 0 && path[i] != '/';) {
    if (path[i] == '.') return std::string(path.substr(i));
  }
  return "";
}

// filepath.Base — the final path element; trailing slashes are ignored.
std::string path_base(std::string_view path) {
  std::size_t end = path.size();
  while (end > 0 && path[end - 1] == '/') --end;
  const std::size_t slash = path.rfind('/', end);
  if (slash == std::string_view::npos) return std::string(path.substr(0, end));
  return std::string(path.substr(slash + 1, end - slash - 1));
}

// Go %q for strings: double-quoted, escaping quotes, backslashes and control
// characters so the output round-trips through foundation::unquote.
std::string qstr(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\t': out += "\\t";  break;
      case '\r': out += "\\r";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\x%02x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

// strconv.Atoi: optional sign, decimal digits only, no whitespace.
std::optional<int> atoi_opt(std::string_view s) {
  if (s.empty()) return std::nullopt;
  int v = 0;
  const auto [p, ec] =
      std::from_chars(s.data(), s.data() + s.size(), v, 10);
  if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
  return v;
}

// mkdir_all mirrors os.MkdirAll(p.dir, 0o755); returns an error string.
std::optional<std::string> mkdir_all(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return "mkdir " + dir.string() + ": " + ec.message();
  }
  return std::nullopt;
}

// stat_exists returns (exists, error-message): Go's
//   if _, err := os.Stat(path); errors.Is(err, fs.ErrNotExist) → !exists
//   else if err != nil → error.
std::pair<bool, std::optional<std::string>> stat_exists(const fs::path& path) {
  std::error_code ec;
  const bool exists = fs::exists(path, ec);
  if (!exists && ec) {
    return {false, "stat " + path.string() + ": " + ec.message()};
  }
  return {exists, std::nullopt};
}

// directory_entries returns the sorted directory entries (os.ReadDir sorts by
// filename; hidden files are included). ErrNotExist → empty, no error.
std::expected<std::vector<fs::path>, std::string> directory_entries(const fs::path& dir) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) {
      return std::vector<fs::path>{};
    }
    return std::unexpected("open " + dir.string() + ": " + ec.message());
  }
  std::vector<fs::path> out;
  for (const auto& de : it) out.push_back(de.path());
  std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
    return a.filename().string() < b.filename().string();
  });
  return out;
}

// playlist_name_of strips the extension from a playlist filename the way
// cliamp does: name = strings.TrimSuffix(e.Name(), filepath.Ext(e.Name())).
std::string playlist_name_of(const std::string& fname) {
  return detail::trim_suffix(fname, ext_of(fname));
}

// parse_track_fields converts a parsed [[track]] section into a Track
// (cliamp parseTrackFields).
playlist::Track parse_track_fields(const foundation::TomlFields& f) {
  const auto get = [&f](const char* key) {
    const auto it = f.find(key);
    return it == f.end() ? std::string{} : it->second;
  };
  playlist::Track t;
  t.path     = get("path");
  t.title    = get("title");
  t.artist   = get("artist");
  t.album    = get("album");
  t.genre    = get("genre");
  t.feed     = get("feed") == "true";
  t.realtime = get("realtime") == "true";
  t.embedded_lyrics = get("embedded_lyrics");
  t.album_art_url   = get("album_art_url");
  t.stream = playlist::is_url(t.path);
  // "favorite" is the pre-rename alias for "bookmark"; prefer bookmark.
  std::string bookmark = f.count("bookmark") ? f.at("bookmark") : get("favorite");
  t.bookmark = bookmark == "true";
  if (auto n = atoi_opt(get("year"))) t.year = *n;
  if (auto n = atoi_opt(get("track_number"))) t.track_number = *n;
  if (auto n = atoi_opt(get("duration_secs"))) t.duration_secs = *n;
  return t;
}

}  // namespace

// ============================================================================
// detail namespace — ported internals (see internal.hpp).
// ============================================================================
namespace detail {

bool is_history_name(std::string_view name) { return name == kHistoryPlaylistName; }

std::string expand_path(std::string_view p) {
  if (p.empty()) return std::string(p);

  // os.ExpandEnv: ${var} or $var replaced by the environment value; undefined
  // variables become the empty string. Malformed "${" is consumed (Go eats
  // both characters); "$" followed by anything else is copied verbatim
  // (together with the rest of the string, matching Go's early return).
  std::string out;
  std::size_t i = 0;
  while (i < p.size()) {
    if (p[i] != '$' || i + 1 >= p.size()) {
      out.push_back(p[i]);
      ++i;
      continue;
    }
    const char n = p[i + 1];
    if (n == '{') {
      const std::size_t close = p.find('}', i + 2);
      if (close == std::string_view::npos) {
        i += 2;  // eat "$" and "{"
        continue;
      }
      const std::string name(p.substr(i + 2, close - i - 2));
      const char* v = std::getenv(name.c_str());
      out += v ? v : "";
      i = close + 1;
      continue;
    }
    const bool special = n == '*' || n == '#' || n == '$' || n == '@' || n == '!' ||
                         n == '?' || (n >= '0' && n <= '9');
    const bool alnum = (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
                       (n >= '0' && n <= '9');
    if (!special && !alnum) {
      out += std::string(p.substr(i));  // remainder verbatim (Go early return)
      break;
    }
    if (special) {
      const std::string name(1, n);
      const char* v = std::getenv(name.c_str());
      out += v ? v : "";
      i += 2;
    } else {
      std::size_t k = i + 1;
      while (k < p.size() && ((p[k] >= 'a' && p[k] <= 'z') || (p[k] >= 'A' && p[k] <= 'Z') ||
                              (p[k] >= '0' && p[k] <= '9')))
        ++k;
      const std::string name(p.substr(i + 1, k - i - 1));
      const char* v = std::getenv(name.c_str());
      out += v ? v : "";
      i = k;
    }
  }

  // os.UserHomeDir() equivalent for the ~ expansion (cliamp ExpandPath).
  if (out == "~" || out.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      const std::string trimmed = out == "~" ? "" : std::string(out.substr(2));
      return (fs::path(home) / trimmed).lexically_normal().string();
    }
  }
  return out;
}

std::string trim_space(std::string_view s) { return ::bootamp::provider::local::trim_space(s); }

std::string trim_suffix(std::string_view s, std::string_view suffix) {
  if (!suffix.empty() && s.size() >= suffix.size() &&
      s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return std::string(s.substr(0, s.size() - suffix.size()));
  }
  return std::string(s);
}

std::string ascii_lower(std::string_view s) { return ::bootamp::provider::local::ascii_lower(s); }

std::string ext_of(std::string_view path) { return ::bootamp::provider::local::ext_of(path); }

PlaylistDoc parse_playlist_doc(std::string_view data) {
  PlaylistDoc doc;
  const std::array<std::string, 2> sections{"track", "dir"};
  foundation::parse_named_sections(data, sections,
      [&doc](std::string_view section, const foundation::TomlFields& f) {
    if (section == "track") {
      doc.tracks.push_back(parse_track_fields(f));
      doc.order.push_back(Item::track);
      return;
    }
    // "dir": a section without a path is dropped entirely.
    const auto pit = f.find("path");
    if (pit == f.end() || pit->second.empty()) return;
    bool recursive = true;
    if (const auto rit = f.find("recursive"); rit != f.end()) {
      recursive = rit->second != "false";
    }
    doc.dirs.push_back(DirSource{pit->second, recursive});
    doc.order.push_back(Item::dir);
  });
  return doc;
}

std::vector<playlist::Track> expand(const PlaylistDoc& doc, bool with_tags) {
  std::set<std::string> explicit_paths;
  for (const auto& t : doc.tracks) explicit_paths.insert(t.path);

  std::size_t ti = 0, di = 0;
  std::vector<playlist::Track> out;
  for (const Item kind : doc.order) {
    if (kind == Item::track) {
      out.push_back(doc.tracks[ti]);
      ++ti;
      continue;
    }
    const DirSource& src = doc.dirs[di];
    ++di;
    const auto files = audio_files(expand_path(src.path), src.recursive);
    if (!files) continue;
    std::vector<playlist::Track> dir_tracks;
    if (with_tags) {
      dir_tracks = tracks_from_paths(*files);
    } else {
      dir_tracks.reserve(files->size());
      for (const auto& f : *files) dir_tracks.push_back(track_from_filename(f));
    }
    for (auto& t : dir_tracks) {
      if (explicit_paths.count(t.path)) continue;
      explicit_paths.insert(t.path);
      t.dir_sourced = true;
      out.push_back(t);
    }
  }
  return out;
}

std::string write_track(const playlist::Track& t) {
  std::string out;
  out += "[[track]]\n";
  out += "path = " + qstr(t.path) + "\n";
  out += "title = " + qstr(t.title) + "\n";
  if (t.feed) out += "feed = true\n";
  if (t.realtime) out += "realtime = true\n";
  if (!t.artist.empty()) out += "artist = " + qstr(t.artist) + "\n";
  if (!t.album.empty()) out += "album = " + qstr(t.album) + "\n";
  if (!t.genre.empty()) out += "genre = " + qstr(t.genre) + "\n";
  if (t.year != 0) out += "year = " + std::to_string(t.year) + "\n";
  if (t.track_number != 0) out += "track_number = " + std::to_string(t.track_number) + "\n";
  if (t.duration_secs != 0) out += "duration_secs = " + std::to_string(t.duration_secs) + "\n";
  if (!t.embedded_lyrics.empty()) out += "embedded_lyrics = " + qstr(t.embedded_lyrics) + "\n";
  if (!t.album_art_url.empty()) out += "album_art_url = " + qstr(t.album_art_url) + "\n";
  if (t.bookmark) out += "bookmark = true\n";
  return out;
}

std::string write_dir(const DirSource& src) {
  std::string out = "[[dir]]\npath = " + qstr(src.path) + "\n";
  if (!src.recursive) out += "recursive = false\n";
  return out;
}

bool is_subsequence(const std::vector<std::string>& orig,
                    const std::vector<std::string>& sub) {
  std::size_t i = 0;
  for (const auto& p : sub) {
    while (i < orig.size() && orig[i] != p) ++i;
    if (i == orig.size()) return false;
    ++i;
  }
  return true;
}

bool dir_supplies_file(const DirSource& dir, std::string_view file) {
  const std::string ext = ascii_lower(ext_of(file));
  if (!audio::supported_exts().count(ext)) return false;
  std::error_code ec;
  const fs::path rel = fs::relative(fs::path(file), fs::path(expand_path(dir.path)), ec);
  if (ec) return false;
  const std::string r = rel.string();
  if (r == "." || r == "..") return false;
  if (r.rfind("../", 0) == 0) return false;
  if (!dir.recursive && r.find('/') != std::string::npos) return false;
  return true;
}

std::tuple<std::vector<playlist::Track>, std::vector<DirSource>, std::vector<Item>>
rebuild_doc(const PlaylistDoc& existing, const std::vector<playlist::Track>& explicit_tracks) {
  // One rewritten [[track]] or [[dir]] section (cliamp playlistSection).
  struct Section {
    Item              kind  = Item::track;
    playlist::Track   track;
    DirSource         dir;
  };

  std::vector<std::string> orig_paths;
  orig_paths.reserve(existing.tracks.size());
  for (const auto& t : existing.tracks) orig_paths.push_back(t.path);
  std::set<std::string> orig_set(orig_paths.begin(), orig_paths.end());

  std::vector<std::string> caller_subseq;
  for (const auto& t : explicit_tracks) {
    if (orig_set.count(t.path)) caller_subseq.push_back(t.path);
  }
  const bool reordered = !is_subsequence(orig_paths, caller_subseq);

  std::map<std::string, playlist::Track> by_path;
  std::set<std::string> placed;
  for (const auto& t : explicit_tracks) by_path[t.path] = t;

  std::size_t ti = 0, di = 0, used = 0;
  std::vector<Section> sections;
  for (const Item kind : existing.order) {
    if (kind == Item::dir) {
      sections.push_back({Item::dir, {}, existing.dirs[di]});
      ++di;
      continue;
    }
    const playlist::Track& orig = existing.tracks[ti];
    ++ti;
    if (reordered) {
      if (used < explicit_tracks.size()) {
        const playlist::Track& t = explicit_tracks[used];
        sections.push_back({Item::track, t, {}});
        placed.insert(t.path);
        ++used;
      }
      continue;
    }
    const auto it = by_path.find(orig.path);
    if (it != by_path.end()) {
      sections.push_back({Item::track, it->second, {}});
      placed.insert(orig.path);
      by_path.erase(it);
    }
  }

  std::vector<playlist::Track> leftovers;
  for (const auto& t : explicit_tracks) {
    if (!placed.count(t.path)) leftovers.push_back(t);
  }
  if (!leftovers.empty()) {
    // dir_pos maps each dir index to the section position its [[dir]] slot
    // currently occupies.
    std::map<std::size_t, std::size_t> dir_pos;
    {
      std::size_t dd = 0;
      for (std::size_t si = 0; si < sections.size(); ++si) {
        if (sections[si].kind == Item::dir) {
          dir_pos[dd] = si;
          ++dd;
        }
      }
    }
    // supplierOf returns the first directory (in document order) that would
    // supply file in a scan — a pure path check (cliamp comment: saves never
    // re-walk the filesystem that the load already scanned).
    const auto supplier_of = [&existing](std::string_view file) -> std::pair<std::size_t, bool> {
      for (std::size_t dd = 0; dd < existing.dirs.size(); ++dd) {
        if (dir_supplies_file(existing.dirs[dd], file)) return {dd, true};
      }
      return {0, false};
    };
    // Insert before-dir leftovers in reverse so several targeting the same
    // directory keep their caller order.
    for (std::size_t i = leftovers.size(); i-- > 0;) {
      const playlist::Track& t = leftovers[i];
      const auto [d, ok] = supplier_of(t.path);
      if (!ok) continue;
      const std::size_t pos = dir_pos[d];
      sections.insert(sections.begin() + static_cast<std::ptrdiff_t>(pos),
                      Section{Item::track, t, {}});
      // The insertion shifted every later section by one; re-align the map so
      // the next leftover lands in the right slot.
      for (auto& [dd, p] : dir_pos) {
        if (dd != d && p >= pos) ++p;
      }
    }
    // Leftovers no directory supplies are appended in caller order.
    for (const auto& t : leftovers) {
      if (auto [d, ok] = supplier_of(t.path); !ok) {
        sections.push_back({Item::track, t, {}});
      }
    }
  }

  std::vector<playlist::Track> tracks;
  std::vector<DirSource> dirs;
  std::vector<Item> order;
  tracks.reserve(sections.size());
  dirs.reserve(sections.size());
  order.reserve(sections.size());
  for (const auto& sec : sections) {
    if (sec.kind == Item::dir) {
      dirs.push_back(sec.dir);
      order.push_back(Item::dir);
    } else {
      tracks.push_back(sec.track);
      order.push_back(Item::track);
    }
  }
  return {std::move(tracks), std::move(dirs), std::move(order)};
}

std::expected<std::vector<std::string>, std::string>
audio_files(std::string_view dir, bool recursive) {
  std::error_code ec;
  const fs::file_status st = fs::status(fs::path(dir), ec);
  if (ec) {
    return std::unexpected("stat audio path \"" + std::string(dir) + "\": " + ec.message());
  }
  if (!fs::is_directory(st)) {
    if (audio::supported_exts().count(ascii_lower(ext_of(dir)))) {
      return std::vector<std::string>{std::string(dir)};
    }
    return std::vector<std::string>{};
  }

  std::vector<std::string> files;
  if (recursive) {
    // Go filepath.WalkDir: does not follow symlinks, skips entries it cannot
    // read (one unreadable subdirectory must not empty the whole result).
    fs::recursive_directory_iterator it(fs::path(dir), ec);
    if (ec) {
      return std::unexpected("walk audio directory \"" + std::string(dir) + "\": " +
                             ec.message());
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;  // skip unreadable entries instead of aborting the scan
      }
      std::error_code ec2;
      if (it->is_directory(ec2)) continue;
      if (audio::supported_exts().count(ascii_lower(ext_of(it->path().string()))))
        files.push_back(it->path().string());
    }
  } else {
    fs::directory_iterator it(fs::path(dir), ec);
    if (ec) {
      return std::unexpected("read audio directory \"" + std::string(dir) + "\": " +
                             ec.message());
    }
    for (const auto& de : it) {
      std::error_code ec2;
      if (de.is_directory(ec2)) continue;
      const std::string p = (fs::path(dir) / de.path().filename()).string();
      if (audio::supported_exts().count(ascii_lower(ext_of(p)))) files.push_back(p);
    }
  }

  std::sort(files.begin(), files.end());  // Go slices.Sort
  return files;
}

std::vector<playlist::Track> tracks_from_paths(const std::vector<std::string>& files) {
  if (files.empty()) return {};
  std::vector<playlist::Track> tracks(files.size());
  const std::size_t workers = std::min<std::size_t>(files.size(), 8);
  std::atomic<std::size_t> next{0};
  std::vector<std::jthread> pool;
  pool.reserve(workers);
  for (std::size_t w = 0; w < workers; ++w) {
    pool.emplace_back([&files, &tracks, &next](std::stop_token st) {
      while (!st.stop_requested()) {
        const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= files.size()) return;
        tracks[i] = playlist::track_from_path(files[i]);
      }
    });
  }
  return tracks;
}

playlist::Track track_from_filename(std::string_view path) {
  const std::string base = path_base(path);
  const std::string ext = ext_of(base);
  const std::string name = ext.empty() ? base : base.substr(0, base.size() - ext.size());
  playlist::Track t;
  t.path = std::string(path);
  const std::size_t sep = name.find(" - ");  // Go strings.SplitN(name, " - ", 2)
  if (sep != std::string::npos) {
    t.artist = trim_space(std::string_view(name).substr(0, sep));
    t.title = trim_space(std::string_view(name).substr(sep + 3));
  } else {
    t.title = name;
  }
  return t;
}

std::pair<int, bool> track_match_score(const playlist::Track& t, std::string_view query) {
  int best = 0;
  bool ok = false;
  for (const std::string_view field :
       {std::string_view(t.title), std::string_view(t.artist), std::string_view(t.album)}) {
    if (field.empty()) continue;
    const auto [s, matched] = foundation::fuzzy_match(query, field);
    if (matched && (!ok || s > best)) {
      best = s;
      ok = true;
    }
  }
  return {best, ok};
}

std::expected<std::string, std::string> read_file(const fs::path& path, int& err_out) {
  err_out = 0;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err_out = errno;
    return std::unexpected("open " + path.string() + ": " + std::strerror(errno));
  }
  std::string out;
  char buf[64 * 1024];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      const int e = errno;
      err_out = e;
      ::close(fd);
      return std::unexpected("read " + path.string() + ": " + std::strerror(e));
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return out;
}

std::expected<void, std::string> write_file(const fs::path& path, std::string_view data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return std::unexpected("open " + path.string() + ": " + std::strerror(errno));
  }
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      const int e = errno;
      ::close(fd);
      return std::unexpected("write " + path.string() + ": " + std::strerror(e));
    }
    off += static_cast<std::size_t>(n);
  }
  if (::close(fd) != 0) {
    return std::unexpected("close " + path.string() + ": " + std::strerror(errno));
  }
  return {};
}

std::expected<fs::path, std::string> safe_path(const fs::path& dir, std::string_view name) {
  if (name.find_first_of("/\\") != std::string_view::npos || trim_space(name).empty()) {
    return std::unexpected("invalid playlist name \"" + std::string(name) + "\"");
  }
  const fs::path resolved = (dir / (std::string(name) + ".toml")).lexically_normal();
  const std::string base = dir.lexically_normal().string();
  const std::string r = resolved.string();
  // Go: !strings.HasPrefix(resolved, filepath.Clean(p.dir)+sep) → escape.
  if (r.rfind(base + "/", 0) != 0) {
    return std::unexpected("playlist path escapes base directory");
  }
  return resolved;
}

std::optional<std::string> validate_new_name(std::string_view name) {
  if (name.find_first_of("/\\:<>\"|?*") != std::string_view::npos || name == ".." ||
      name == "." || trim_space(name).empty()) {
    return "invalid playlist name \"" + std::string(name) + "\"";
  }
  return std::nullopt;
}

std::expected<PlaylistDoc, std::string> load_doc_at(const fs::path& file, int* err_out) {
  int err = 0;
  auto data = read_file(file, err);
  if (!data) {
    if (err_out != nullptr) *err_out = err;
    return std::unexpected(data.error());
  }
  return parse_playlist_doc(*data);
}

std::expected<PlaylistDoc, std::string> load_doc(const fs::path& dir,
                                                 std::string_view name,
                                                 int* err_out) {
  auto path = safe_path(dir, name);
  if (!path) return std::unexpected(path.error());
  return load_doc_at(*path, err_out);
}

std::expected<PlaylistDoc, std::string> existing_doc(const fs::path& dir,
                                                     const fs::path& path) {
  int err = 0;
  auto data = read_file(path, err);
  if (!data) {
    if (err == ENOENT) return PlaylistDoc{};
    return std::unexpected("read playlist \"" + path.string() + "\": " + data.error());
  }
  return parse_playlist_doc(*data);
}

std::expected<void, std::string> save_doc(const fs::path& dir, std::string_view name,
                                          const PlaylistDoc& doc) {
  if (auto e = mkdir_all(dir)) {
    return std::unexpected("creating playlist dir: " + *e);
  }
  auto path = safe_path(dir, name);
  if (!path) return std::unexpected("resolving playlist path: " + path.error());
  const auto [exists, stat_err] = stat_exists(*path);
  if (stat_err) {
    return std::unexpected("stat playlist \"" + std::string(name) + "\": " + *stat_err);
  }
  if (!exists) {
    if (auto e = validate_new_name(name)) return std::unexpected(*e);
  }

  std::string out;
  std::size_t ti = 0, di = 0, sections = 0;
  for (const Item kind : doc.order) {
    if (sections > 0) out.push_back('\n');
    if (kind == Item::track) {
      out += write_track(doc.tracks[ti]);
      ++ti;
    } else {
      out += write_dir(doc.dirs[di]);
      ++di;
    }
    ++sections;
  }

  const fs::path tmp = path->string() + ".tmp";
  auto w = write_file(tmp, out);
  if (!w) {
    return std::unexpected("writing playlist \"" + std::string(name) + "\": " + w.error());
  }
  std::error_code ec;
  fs::rename(tmp, *path, ec);
  if (ec) {
    fs::remove(tmp, ec);  // best-effort cleanup (Go os.Remove(tmp))
    return std::unexpected("saving playlist \"" + std::string(name) + "\": " + ec.message());
  }
  return {};
}

std::expected<void, std::string> save_playlist(const fs::path& dir, std::string_view name,
                                               const std::vector<playlist::Track>& tracks) {
  if (auto e = mkdir_all(dir)) {
    return std::unexpected("creating playlist dir: " + *e);
  }
  auto path = safe_path(dir, name);
  if (!path) return std::unexpected("resolving playlist path: " + path.error());

  PlaylistDoc existing;
  const auto [exists, stat_err] = stat_exists(*path);
  if (stat_err) {
    return std::unexpected("stat playlist \"" + std::string(name) + "\": " + *stat_err);
  }
  if (!exists) {
    if (auto e = validate_new_name(name)) return std::unexpected(*e);
    existing = PlaylistDoc{};
  } else {
    auto ex = existing_doc(dir, *path);
    if (!ex) return std::unexpected(ex.error());
    existing = *ex;
  }

  std::vector<playlist::Track> explicit_tracks;
  explicit_tracks.reserve(tracks.size());
  for (const auto& t : tracks) {
    if (t.dir_sourced) continue;
    explicit_tracks.push_back(t);
  }
  auto [tr, dr, od] = rebuild_doc(existing, explicit_tracks);
  return save_doc(dir, name, PlaylistDoc{std::move(tr), std::move(dr), std::move(od)});
}

}  // namespace detail

// ============================================================================
// Provider — cliamp external/local/provider.go.
// ============================================================================

std::unique_ptr<Provider> Provider::new_provider() {
  // cliamp local.New: appdir.Dir() → Join(dir, "playlists"); nil on failure.
  auto dir = foundation::config_dir();
  if (!dir) return nullptr;
  return std::unique_ptr<Provider>(new Provider(*dir / "playlists"));
}

std::expected<std::vector<playlist::PlaylistInfo>, std::string> Provider::playlists() {
  std::vector<playlist::PlaylistInfo> lists;
  // cliamp prepends the virtual "Recently Played" entry here when history has
  // entries; bootamp has no history store (see internal.hpp), so nothing is
  // prepended.

  const auto entries = directory_entries(dir_);
  if (!entries) return std::unexpected(entries.error());

  for (const fs::path& e : *entries) {
    std::error_code ec;
    if (fs::is_directory(e, ec)) continue;
    const std::string fname = e.filename().string();
    const std::string lower = ascii_lower(fname);
    if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".toml") != 0) continue;
    const std::string name = playlist_name_of(fname);
    auto doc = detail::load_doc_at(dir_ / fname);
    if (!doc) continue;  // unreadable playlist files are skipped
    // Count without tag reads so the playlist browser stays fast; durations
    // stay unknown (0) for directory-backed playlists, which the browser
    // omits from display (cliamp Playlists).
    const auto tracks = detail::expand(*doc, false);
    lists.push_back(playlist::PlaylistInfo{
        /*id=*/name,
        /*name=*/name,
        /*track_count=*/static_cast<int>(tracks.size()),
        /*duration_secs=*/playlist::total_duration_secs(tracks),
        /*section=*/{},  // local has no sections
    });
  }
  return lists;
}

std::expected<std::vector<playlist::Track>, std::string>
Provider::tracks(std::string_view playlist_id) {
  if (detail::is_history_name(playlist_id)) {
    // cliamp: history store nil → return nil, nil (bootamp has no store).
    return std::vector<playlist::Track>{};
  }
  auto doc = detail::load_doc(dir_, playlist_id);
  if (!doc) return std::unexpected(doc.error());
  return detail::expand(*doc, true);
}

std::expected<std::vector<playlist::Track>, std::string>
Provider::search_tracks(std::string_view query, int limit) {
  std::string q = trim_space(query);
  if (q.empty()) return std::vector<playlist::Track>{};

  const auto entries = directory_entries(dir_);
  if (!entries) return std::unexpected(entries.error());

  struct Scored {
    playlist::Track track;
    int             score = 0;
  };
  std::vector<Scored> matches;
  std::set<std::string> seen;
  for (const fs::path& e : *entries) {
    std::error_code ec;
    if (fs::is_directory(e, ec)) continue;
    const std::string fname = e.filename().string();
    const std::string lower = ascii_lower(fname);
    if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".toml") != 0) continue;
    auto doc = detail::load_doc_at(dir_ / fname);
    if (!doc) continue;
    for (auto& t : detail::expand(*doc, true)) {
      if (seen.count(t.path)) continue;
      const auto [score, ok] = detail::track_match_score(t, q);
      if (!ok) continue;
      seen.insert(t.path);
      matches.push_back({std::move(t), score});
    }
  }

  std::stable_sort(matches.begin(), matches.end(),  // Go sort.SliceStable
                   [](const Scored& a, const Scored& b) { return a.score > b.score; });
  if (limit > 0 && matches.size() > static_cast<std::size_t>(limit)) {
    matches.resize(static_cast<std::size_t>(limit));
  }

  std::vector<playlist::Track> out;
  out.reserve(matches.size());
  for (auto& m : matches) out.push_back(std::move(m.track));
  return out;
}

std::expected<void, std::string>
Provider::add_track_to_playlist(std::string_view playlist_id, const playlist::Track& t) {
  // cliamp AddTrack discards the counts.
  auto res = add_tracks_to_playlist(playlist_id, std::vector<playlist::Track>{t});
  if (!res) return std::unexpected(res.error());
  return {};
}

std::expected<std::pair<int, int>, std::string>
Provider::add_tracks_to_playlist(std::string_view playlist_id,
                                 const std::vector<playlist::Track>& tracks) {
  if (detail::is_history_name(playlist_id)) {
    return std::unexpected(std::string(detail::kReservedHistoryError));
  }
  if (auto e = mkdir_all(dir_)) {
    return std::unexpected(*e);
  }
  auto path = safe_path(playlist_id);
  if (!path) return std::unexpected(path.error());

  int err = 0;
  auto doc = detail::load_doc(dir_, playlist_id, &err);
  if (!doc) {
    if (err != ENOENT) return std::unexpected(doc.error());
    if (auto e = detail::validate_new_name(playlist_id)) {
      return std::unexpected(*e);
    }
    doc = detail::PlaylistDoc{};
  }

  // Deduplicate against explicit entries and everything a [[dir]] source
  // would supply (cliamp AddTracks `seen`).
  std::set<std::string> seen;
  for (const auto& t : doc->tracks) seen.insert(t.path);
  for (const auto& src : doc->dirs) {
    const auto files = detail::audio_files(detail::expand_path(src.path), src.recursive);
    if (!files) continue;
    for (const auto& f : *files) seen.insert(f);
  }

  std::vector<playlist::Track> existing = doc->tracks;
  int added = 0, skipped = 0;
  for (const auto& t : tracks) {
    if (seen.count(t.path)) {
      ++skipped;
      continue;
    }
    // Incoming tracks may carry dir_sourced from a directory-backed playlist.
    // Added to a different playlist here, they must persist as explicit
    // [[track]] entries; the save would otherwise drop them (cliamp comment
    // in AddTracks).
    playlist::Track tc = t;
    tc.dir_sourced = false;
    seen.insert(tc.path);
    existing.push_back(tc);
    ++added;
  }

  if (added == 0) {
    const auto [exists, stat_err] = stat_exists(*path);
    if (stat_err) return std::unexpected(*stat_err);
    if (!exists) {
      auto sv = save_playlist(std::string(playlist_id), existing);
      if (!sv) return std::unexpected(sv.error());
    }
    return std::pair<int, int>{0, skipped};
  }
  auto sv = save_playlist(std::string(playlist_id), existing);
  if (!sv) return std::unexpected(sv.error());
  return std::pair<int, int>{added, skipped};
}

std::expected<void, std::string>
Provider::save_playlist(std::string_view name, const std::vector<playlist::Track>& tracks) {
  if (detail::is_history_name(name)) {
    return std::unexpected(std::string(detail::kReservedHistoryError));
  }
  auto res = detail::save_playlist(dir_, name, tracks);
  if (!res) return std::unexpected(res.error());
  return {};
}

std::expected<std::string, std::string> Provider::create_playlist(std::string_view name) {
  if (detail::is_history_name(name)) {
    return std::unexpected(std::string(detail::kReservedHistoryError));
  }
  if (auto e = mkdir_all(dir_)) {
    return std::unexpected(*e);
  }
  if (auto e = detail::validate_new_name(name)) {
    return std::unexpected(*e);
  }
  auto path = safe_path(name);
  if (!path) return std::unexpected(path.error());

  // cliamp: os.OpenFile(path, O_WRONLY|O_CREATE|O_EXCL, 0o644).
  const int fd = ::open(path->c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    if (errno == EEXIST) {
      return std::unexpected("playlist \"" + std::string(name) + "\" already exists");
    }
    return std::unexpected("open " + path->string() + ": " + std::strerror(errno));
  }
  if (::close(fd) != 0) {
    return std::unexpected("close " + path->string() + ": " + std::strerror(errno));
  }
  return std::string(name);
}

std::expected<void, std::string> Provider::delete_playlist(std::string_view name) {
  if (detail::is_history_name(name)) {
    return std::unexpected(std::string(detail::kReservedHistoryError));
  }
  auto path = safe_path(name);
  if (!path) return std::unexpected(path.error());
  std::error_code ec;
  fs::remove(*path, ec);
  if (ec) {
    return std::unexpected("remove " + path->string() + ": " + ec.message());
  }
  return {};
}

std::expected<void, std::string> Provider::remove_track(std::string_view name, int index) {
  if (detail::is_history_name(name)) {
    return std::unexpected(std::string(detail::kReservedHistoryError));
  }
  auto doc = detail::load_doc(dir_, name);
  if (!doc) return std::unexpected(doc.error());
  const std::vector<playlist::Track> tracks = detail::expand(*doc, true);

  if (index < 0 || index >= static_cast<int>(tracks.size())) {
    return std::unexpected("track index " + std::to_string(index) + " out of range");
  }
  if (tracks[static_cast<std::size_t>(index)].dir_sourced) {
    return std::unexpected(
        "track " + std::to_string(index + 1) + " (" + tracks[static_cast<std::size_t>(index)].path +
        ") is supplied by a directory source; remove the file from the directory "
        "or edit the playlist's [[dir]] section");
  }

  std::vector<playlist::Track> kept;
  kept.reserve(tracks.size());
  bool removed = false;
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (tracks[i].dir_sourced) continue;
    if (i == static_cast<std::size_t>(index)) {
      removed = true;
      continue;
    }
    kept.push_back(tracks[i]);
  }
  if (!removed) {
    return std::unexpected("track index " + std::to_string(index) + " out of range");
  }
  auto sv = save_playlist(std::string(name), kept);
  if (!sv) return std::unexpected(sv.error());
  return {};
}

std::expected<void, std::string> Provider::rename_playlist(std::string_view old_name,
                                                           std::string_view new_name) {
  if (detail::is_history_name(old_name) || detail::is_history_name(new_name)) {
    return std::unexpected(std::string(detail::kReservedHistoryError));
  }
  auto old_path = safe_path(old_name);
  if (!old_path) {
    return std::unexpected("invalid playlist name \"" + std::string(old_name) + "\": " +
                           old_path.error());
  }
  if (auto e = detail::validate_new_name(new_name)) {
    return std::unexpected(*e);
  }
  auto new_path = safe_path(new_name);
  if (!new_path) {
    return std::unexpected("invalid playlist name \"" + std::string(new_name) + "\": " +
                           new_path.error());
  }
  const auto [dst_exists, stat_err] = stat_exists(*new_path);
  if (dst_exists) {
    return std::unexpected("playlist \"" + std::string(new_name) + "\" already exists");
  }
  if (stat_err) {
    return std::unexpected("stat destination playlist \"" + std::string(new_name) + "\": " +
                           *stat_err);
  }
  std::error_code ec;
  fs::rename(*old_path, *new_path, ec);
  if (ec) {
    return std::unexpected("rename playlist \"" + std::string(old_name) + "\" to \"" +
                           std::string(new_name) + "\": " + ec.message());
  }
  return {};
}

std::expected<void, std::string> Provider::set_bookmark(std::string_view playlist_name, int idx) {
  if (detail::is_history_name(playlist_name)) {
    return std::unexpected(std::string(detail::kReservedHistoryError));
  }
  auto doc = detail::load_doc(dir_, playlist_name);
  if (!doc) return std::unexpected(doc.error());
  std::vector<playlist::Track> tracks = detail::expand(*doc, true);

  if (idx < 0 || idx >= static_cast<int>(tracks.size())) {
    return std::unexpected("index " + std::to_string(idx) + " out of range (playlist has " +
                           std::to_string(tracks.size()) + " tracks)");
  }
  playlist::Track& t = tracks[static_cast<std::size_t>(idx)];
  t.bookmark = !t.bookmark;
  t.dir_sourced = false;  // materialize so the change persists
  auto sv = save_playlist(std::string(playlist_name), tracks);
  if (!sv) return std::unexpected(sv.error());
  return {};
}

std::expected<fs::path, std::string> Provider::safe_path(std::string_view name) const {
  return detail::safe_path(dir_, name);
}

}  // namespace bootamp::provider::local
