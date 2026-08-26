// resolve/pls.cpp — PLS (INI-style) playlist parser.
//
// Port of cliamp/resolve/pls.go: parsePLS, plsEntriesToTracks, radioMirrors,
// allStreams, stripMirrorSuffix, resolveLocalPLS. Radio PLS files list
// multiple mirror servers for the same stream; entries with explicit negative
// lengths or matching "(#N)" titles collapse to the first URL (VLC/Winamp
// behavior). allStreams stays internal (anonymous namespace).
#include "resolve/pls.hpp"

#include "resolve/resolve.hpp"  // strip_mirror_suffix (declared here per contract)

#include "playlist/playlist.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::resolve {
namespace {

// strconv.Atoi: optional sign, decimal digits only. Returns std::nullopt on
// malformed input. Values bounded to the int32 range.
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

std::string ascii_lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  }
  return r;
}

// allStreams reports whether every PLS entry is an HTTP stream URL.
bool all_streams(const std::vector<PlsEntry>& es) {
  for (const auto& e : es) {
    if (!playlist::is_url(e.file)) return false;
  }
  return !es.empty();
}

}  // namespace

std::expected<std::vector<PlsEntry>, std::string>
parse_pls(std::string_view content) {
  // Go parsePLS uses a default bufio.Scanner: 64 KB max token.
  constexpr std::size_t kMaxLineSize = 64 * 1024;

  std::map<int, std::string> files;
  std::map<int, std::string> titles;
  std::map<int, int> lengths;

  std::size_t pos = 0;
  while (pos < content.size()) {
    std::size_t nl = content.find('\n', pos);
    const bool last = nl == std::string_view::npos;
    std::string_view line = last ? content.substr(pos) : content.substr(pos, nl - pos);
    pos = last ? content.size() : nl + 1;

    if (line.size() > kMaxLineSize)
      return std::unexpected(std::string("bufio.Scanner: token too long"));

    line = trim_ascii(line);
    if (line.empty() || line.starts_with('[') || line.starts_with(';')) continue;

    std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view key = trim_ascii(line.substr(0, eq));
    std::string_view val = trim_ascii(line.substr(eq + 1));

    std::string lower = ascii_lower(key);
    if (lower.starts_with("file")) {
      if (auto n = atoi_strict(key.substr(4))) files[*n] = std::string(val);
    } else if (lower.starts_with("title")) {
      if (auto n = atoi_strict(key.substr(5))) titles[*n] = std::string(val);
    } else if (lower.starts_with("length")) {
      if (auto n = atoi_strict(key.substr(6))) {
        if (auto len = atoi_strict(val)) lengths[*n] = *len;
      }
    }
  }

  if (files.empty()) return std::unexpected(std::string("no entries found in PLS playlist"));

  std::vector<int> nums;
  nums.reserve(files.size());
  for (const auto& [n, f] : files) nums.push_back(n);
  std::sort(nums.begin(), nums.end());

  std::vector<PlsEntry> entries;
  entries.reserve(nums.size());
  for (int n : nums) {
    PlsEntry e;
    e.num = n;
    e.file = files[n];
    e.title = titles[n];
    auto it = lengths.find(n);
    if (it != lengths.end()) {
      e.length = it->second;
      e.has_length = true;
    }
    entries.push_back(std::move(e));
  }
  return entries;
}

std::vector<playlist::Track> pls_entries_to_tracks(const std::vector<PlsEntry>& es) {
  if (radio_mirrors(es)) {
    const PlsEntry& e = es[0];
    std::string title = strip_mirror_suffix(e.title);
    if (title.empty()) {
      playlist::Track track = playlist::track_from_path(e.file);
      track.realtime = true;
      return {std::move(track)};
    }
    playlist::Track t;
    t.path = e.file;
    t.title = std::move(title);
    t.stream = true;
    t.realtime = true;
    return {std::move(t)};
  }

  std::vector<playlist::Track> tracks;
  tracks.reserve(es.size());
  for (const auto& e : es) {
    const bool is_url = playlist::is_url(e.file);
    const bool realtime = is_url && e.has_length && e.length < 0;
    const int duration = std::max(e.length, 0);
    if (!e.title.empty()) {
      playlist::Track t;
      t.path = e.file;
      t.title = e.title;
      t.stream = is_url;
      t.realtime = realtime;
      t.duration_secs = duration;
      tracks.push_back(std::move(t));
    } else {
      playlist::Track t = playlist::track_from_path(e.file);
      t.realtime = realtime;
      t.duration_secs = duration;
      tracks.push_back(std::move(t));
    }
  }
  return tracks;
}

bool radio_mirrors(const std::vector<PlsEntry>& es) {
  if (!all_streams(es)) return false;

  bool all_indefinite = true;
  for (const auto& entry : es) {
    if (entry.has_length && entry.length >= 0) return false;
    all_indefinite = all_indefinite && entry.has_length && entry.length < 0;
  }
  if (all_indefinite) return true;

  if (es.size() < 2) return false;

  std::string name = strip_mirror_suffix(es[0].title);
  if (name.empty() || name == es[0].title) return false;
  for (std::size_t i = 1; i < es.size(); ++i) {
    const std::string& t = es[i].title;
    if (strip_mirror_suffix(t) != name || strip_mirror_suffix(t) == t) return false;
  }
  return true;
}

std::string strip_mirror_suffix(std::string_view s) {
  // Handle "(#N)" suffix.
  std::size_t i = s.rfind("(#");
  if (i != std::string_view::npos && s.ends_with(')')) {
    // strings.TrimRight(s[:i], " :")
    std::string_view head = s.substr(0, i);
    while (!head.empty() && (head.back() == ' ' || head.back() == ':')) head.remove_suffix(1);
    return std::string(head);
  }
  return std::string(s);
}

std::expected<std::vector<playlist::Track>, std::string>
resolve_local_pls(std::string_view path) {
  std::ifstream f(std::string(path), std::ios::binary);
  if (!f) return std::unexpected("open " + std::string(path) + ": no such file or directory");
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (f.bad()) return std::unexpected("read " + std::string(path) + ": read error");

  auto entries = parse_pls(content);
  if (!entries) return std::unexpected(entries.error());
  return pls_entries_to_tracks(*entries);
}

}  // namespace bootamp::resolve
