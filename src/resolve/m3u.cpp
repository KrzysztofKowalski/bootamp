// resolve/m3u.cpp — M3U/.m3u8 parser (local + remote).
//
// Port of cliamp/resolve/m3u.go: parseM3U, m3uEntryToTrack, entriesToTracks,
// resolveLocalM3U, resolveM3UPath and the path-semantics helpers
// (usesWindowsPathSemantics / isPOSIXAbsolutePath / isWindowsAbsolutePath /
// hasWindowsDrivePrefix). Handles UTF-8 BOM, \r\n, missing #EXTM3U header,
// bare entries; relative paths resolve against base_dir. All helpers that
// cliamp keeps unexported live in the anonymous namespace here.
#include "resolve/m3u.hpp"

#include "playlist/playlist.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::resolve {
namespace {

// strconv.Atoi: optional sign, decimal digits only, no whitespace. Returns
// std::nullopt on any malformed input (Go: error). Values are bounded to the
// int32 range, which is far beyond any real playlist duration.
std::optional<int> atoi_strict(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') {
    neg = (s[0] == '-');
    i = 1;
    if (i == s.size()) return std::nullopt;
  }
  long long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    v = v * 10 + (s[i] - '0');
    if (v > 2147483648LL) return std::nullopt;
  }
  if (neg) v = -v;
  if (v < -2147483648LL || v > 2147483647LL) return std::nullopt;
  return static_cast<int>(v);
}

// strings.TrimSpace approximation: ASCII whitespace at both ends.
std::string_view trim_ascii(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                        s.front() == '\n' || s.front() == '\v' || s.front() == '\f'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                        s.back() == '\n' || s.back() == '\v' || s.back() == '\f'))
    s.remove_suffix(1);
  return s;
}


// filepath.Clean (POSIX): lexical cleanup of the path — collapse repeated
// separators, eliminate "." segments, resolve ".." lexically.
std::string clean_path(std::string_view p) {
  const bool rooted = !p.empty() && p[0] == '/';
  std::vector<std::string> segs;
  std::size_t i = 0;
  while (i < p.size()) {
    while (i < p.size() && p[i] == '/') ++i;
    if (i >= p.size()) break;
    std::size_t j = p.find('/', i);
    std::string seg(p.substr(i, j == std::string_view::npos ? p.size() - i : j - i));
    i = (j == std::string_view::npos) ? p.size() : j + 1;
    if (seg == ".") continue;
    if (seg == "..") {
      if (!segs.empty() && segs.back() != "..") {
        segs.pop_back();
      } else if (!rooted) {
        segs.push_back(std::move(seg));
      }
      continue;
    }
    segs.push_back(std::move(seg));
  }
  if (rooted) {
    std::string out = "/";
    for (const auto& s : segs) {
      out += s;
      out += '/';
    }
    if (!segs.empty()) out.pop_back();
    return out;
  }
  if (segs.empty()) return ".";
  std::string out;
  for (const auto& s : segs) {
    if (!out.empty()) out += '/';
    out += s;
  }
  return out;
}

// filepath.Dir: everything up to and including the last separator, cleaned
// (Split + Clean in Go terms).
std::string dir_of(std::string_view p) {
  if (p.empty()) return ".";
  std::size_t i = p.size();
  while (i > 0 && p[i - 1] != '/') --i;
  return clean_path(p.substr(0, i));
}

// path.Join: join two elements with '/', cleaning the result. Empty elements
// are ignored (path.Join semantics).
std::string join_posix(std::string_view a, std::string_view b) {
  std::string base = a.empty() ? "." : std::string(a);
  return clean_path(base + "/" + std::string(b));
}

// filepath.FromSlash on POSIX is the identity — backslashes stay literal.
std::string_view from_slash(std::string_view s) { return s; }

bool has_windows_drive_prefix(std::string_view path) {
  if (path.size() < 2 || path[1] != ':') return false;
  const char c = path[0];
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_windows_absolute_path(std::string_view path) {
  return path.starts_with("\\\\") || has_windows_drive_prefix(path);
}

bool is_posix_absolute_path(std::string_view path) {
  return path.starts_with('/');
}

// filepath.IsAbs on POSIX.
bool is_abs(std::string_view path) { return !path.empty() && path[0] == '/'; }

bool uses_windows_path_semantics(std::string_view base) {
  return base.find('\\') != std::string_view::npos || has_windows_drive_prefix(base);
}

// resolveM3UPath: returns the entry unchanged for URLs or empty paths; checks
// explicit Windows or POSIX absolute paths, falling back to base-relative
// joining with either Windows or POSIX semantics.
std::string resolve_m3u_path(std::string_view base_dir, std::string_view entry) {
  if (entry.empty() || playlist::is_url(entry)) return std::string(entry);
  if (is_windows_absolute_path(entry)) return clean_path(entry);
  if (is_posix_absolute_path(entry)) return clean_path(entry);
  if (is_abs(entry)) return clean_path(entry);
  if (uses_windows_path_semantics(base_dir))
    return join_posix(base_dir, from_slash(entry));
  return join_posix(base_dir, entry);
}

}  // namespace

std::expected<std::vector<M3UEntry>, std::string>
parse_m3u(std::string_view content, std::string_view base_dir) {
  constexpr std::size_t kMaxLineSize = 1024 * 1024;  // Go scannerMaxLineSize

  std::vector<M3UEntry> entries;
  std::optional<M3UEntry> pending;  // EXTINF parsed, waiting for path line

  std::size_t pos = 0;
  while (pos < content.size()) {
    std::size_t nl = content.find('\n', pos);
    const bool last = nl == std::string_view::npos;
    std::string_view line = last ? content.substr(pos) : content.substr(pos, nl - pos);
    pos = last ? content.size() : nl + 1;

    if (line.size() > kMaxLineSize)
      return std::unexpected(std::string("bufio.Scanner: token too long"));

    // Strip UTF-8 BOM if present (common in Windows-created M3U files).
    if (line.starts_with("\xef\xbb\xbf")) line.remove_prefix(3);
    line = trim_ascii(line);
    if (line.empty()) continue;

    // Skip the #EXTM3U header.
    if (line.starts_with("#EXTM3U")) continue;

    // Parse #EXTINF:duration,title
    if (line.starts_with("#EXTINF:")) {
      std::string_view info = line.substr(8);  // len("#EXTINF:")
      int dur = -1;
      std::string title;
      std::size_t comma = info.find(',');
      if (comma != std::string_view::npos) {
        if (auto d = atoi_strict(trim_ascii(info.substr(0, comma)))) dur = *d;
        title = std::string(trim_ascii(info.substr(comma + 1)));
      }
      pending = M3UEntry{std::string(), std::move(title), dur};
      continue;
    }

    // Skip other comment/directive lines.
    if (line.starts_with('#')) continue;

    // This is a path/URL line.
    std::string path(line);
    if (!base_dir.empty() && !playlist::is_url(path)) {
      path = resolve_m3u_path(base_dir, path);
    }

    if (pending) {
      pending->path = std::move(path);
      entries.push_back(std::move(*pending));
      pending.reset();
    } else {
      entries.push_back(M3UEntry{std::move(path), std::string(), -1});
    }
  }

  return entries;
}

playlist::Track m3u_entry_to_track(const M3UEntry& e) {
  const bool is_url = playlist::is_url(e.path);
  const int duration = std::max(e.duration, 0);
  const bool realtime = is_url && e.duration <= 0;

  if (!e.title.empty()) {
    playlist::Track t;
    t.path = e.path;
    t.title = e.title;
    t.stream = is_url;
    t.realtime = realtime;
    t.duration_secs = duration;
    return t;
  }
  playlist::Track t = playlist::track_from_path(e.path);
  t.realtime = realtime;
  t.duration_secs = duration;
  return t;
}

std::vector<playlist::Track> entries_to_tracks(const std::vector<M3UEntry>& es) {
  std::vector<playlist::Track> tracks;
  tracks.reserve(es.size());
  for (const auto& e : es) tracks.push_back(m3u_entry_to_track(e));
  return tracks;
}

std::expected<std::vector<playlist::Track>, std::string>
resolve_local_m3u(std::string_view path) {
  std::ifstream f(std::string(path), std::ios::binary);
  if (!f) return std::unexpected("open " + std::string(path) + ": no such file or directory");
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (f.bad()) return std::unexpected("read " + std::string(path) + ": read error");

  auto entries = parse_m3u(content, dir_of(path));
  if (!entries) return std::unexpected(entries.error());
  return entries_to_tracks(*entries);
}

}  // namespace bootamp::resolve
