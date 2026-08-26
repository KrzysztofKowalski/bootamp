// resolve/resolve.cpp — expand CLI args into playable tracks.
//
// Port of cliamp/resolve/resolve.go: Args (classification), sniffFeedURL,
// CollectAudioFiles/AudioFiles, scanTracks, resolveM3U, resolvePLS,
// isHLSPlaylist (body check, internal), plus the ExpandYTPlaylist and
// ytdlCookiesFrom globals. Remote/URL/feed/youtube/yt-dlp resolution lives in
// other modules (ytdl.cpp, feeds) — this file only classifies and fetches
// .m3u/.pls playlists, per the resolve.hpp contract.
#include "resolve/resolve.hpp"

#include "resolve/m3u.hpp"
#include "resolve/pls.hpp"

#include "audio/decode.hpp"      // supported_exts()
#include "audio/http_socket.hpp" // raw-socket HTTP client (fetch_text)
#include "playlist/playlist.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bootamp::resolve {
namespace {

namespace fs = std::filesystem;

// cliamp ExpandYTPlaylist (default true).
std::atomic<bool> g_expand_yt_playlist{true};

// cliamp ytdlCookiesFromVal — atomic.Value port. Every store leaks one small
// string so readers get a stable string_view without locks (lock-free reads
// on the resolve hot path, matching the Go design).
std::atomic<const char*> g_ytdl_cookies{nullptr};

// Go maxM3UBody — caps how much of a remote playlist is read before
// classifying it. HLS/M3U playlists are tiny (a few KB); 1 MB is generous.
constexpr std::size_t kMaxM3UBody = 1 << 20;

// --- small string helpers ---------------------------------------------------

std::string ascii_lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  }
  return r;
}

std::string_view trim_ascii(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                        s.front() == '\n' || s.front() == '\v' || s.front() == '\f'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                        s.back() == '\n' || s.back() == '\v' || s.back() == '\f'))
    s.remove_suffix(1);
  return s;
}

// filepath.Ext — the suffix beginning at the final dot in the final element.
std::string ext_of(std::string_view path) {
  for (std::size_t i = path.size(); i-- > 0 && path[i] != '/';) {
    if (path[i] == '.') return std::string(path.substr(i));
  }
  return "";
}

// --- URL helpers ------------------------------------------------------------

// url_path_of returns the path component of a URL (before '?'/'#').
// Approximation of Go url.Parse().Path (percent-encoding is not decoded).
std::string_view url_path_of(std::string_view raw) {
  std::size_t scheme = raw.find("://");
  std::size_t start = scheme == std::string_view::npos ? 0 : scheme + 3;
  std::size_t slash = raw.find('/', start);
  if (slash == std::string_view::npos) return {};
  std::size_t end = raw.find_first_of("?#", slash);
  if (end == std::string_view::npos) return raw.substr(slash);
  return raw.substr(slash, end - slash);
}

// mime.ParseMediaType: media type is the lowercase token before ';', trimmed.
std::string media_type_of(std::string_view ct) {
  std::size_t semi = ct.find(';');
  if (semi != std::string_view::npos) ct = ct.substr(0, semi);
  return ascii_lower(trim_ascii(ct));
}

// sniffFeedURL — Go sends a HEAD via a 5s-timeout client; HttpClient has no
// HEAD, so this issues a GET capped at 1 KiB (headers are what matter) with
// the same 5s timeout. URLs with a known audio extension skip the network
// round-trip entirely.
bool sniff_feed_url(std::string_view raw_url) {
  const std::string ext = ascii_lower(ext_of(url_path_of(raw_url)));
  if (!ext.empty() && audio::supported_exts().count(ext)) return false;

  audio::HttpClient client;
  auto resp = client.fetch_text(raw_url, 1024, std::chrono::seconds{5});
  if (!resp) return false;
  for (const auto& [key, value] : resp->headers) {
    if (key != "content-type") continue;
    const std::string media = media_type_of(value);
    if (media == "application/rss+xml" || media == "application/atom+xml" ||
        media == "application/xml" || media == "text/xml")
      return true;
    break;
  }
  return false;
}

// --- file collection (cliamp CollectAudioFiles / AudioFiles) ----------------

// audio_files returns audio file paths for the given argument: directories
// are walked (recursively when `recursive`) collecting supported files in
// sorted order; a file with a supported extension is returned directly.
std::expected<std::vector<std::string>, std::string>
audio_files(std::string_view dir, bool recursive) {
  std::error_code ec;
  const fs::file_status st = fs::status(fs::path(dir), ec);
  if (ec) {
    return std::unexpected("stat audio path \"" + std::string(dir) + "\": " + ec.message());
  }
  if (!fs::is_directory(st)) {
    if (audio::supported_exts().count(ascii_lower(ext_of(dir))))
      return std::vector<std::string>{std::string(dir)};
    return std::vector<std::string>{};
  }

  std::vector<std::string> files;
  if (recursive) {
    // Go filepath.WalkDir: does not follow symlinks, skips entries it cannot
    // read (one unreadable subdirectory must not empty the whole result).
    fs::recursive_directory_iterator it(fs::path(dir), ec);
    if (ec) {
      return std::unexpected("walk audio directory \"" + std::string(dir) + "\": " + ec.message());
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;  // skip unreadable entries instead of aborting the scan
      }
      const fs::directory_entry& de = *it;
      std::error_code ec2;
      if (de.is_directory(ec2)) continue;
      if (audio::supported_exts().count(ascii_lower(ext_of(de.path().string()))))
        files.push_back(de.path().string());
    }
  } else {
    fs::directory_iterator it(fs::path(dir), ec);
    if (ec) {
      return std::unexpected("read audio directory \"" + std::string(dir) + "\": " + ec.message());
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

// --- scanTracks: concurrent TrackFromPath, order preserved ----------------

std::vector<playlist::Track> scan_tracks(const std::vector<std::string>& files) {
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

// --- glob (filepath.Glob) ---------------------------------------------------

bool has_meta(std::string_view p) {
  return p.find_first_of("*?[") != std::string_view::npos;
}

// path.Match for a single path segment: '*' matches any sequence, '?' one
// character, '[...]' a character class with ranges and '^'/'!' negation.
bool match_seg_at(std::string_view s, std::size_t si, std::string_view p, std::size_t pi) {
  while (pi < p.size()) {
    const char c = p[pi];
    if (c == '*') {
      while (pi < p.size() && p[pi] == '*') ++pi;
      if (pi == p.size()) return true;
      for (std::size_t k = si; k <= s.size(); ++k) {
        if (match_seg_at(s, k, p, pi)) return true;
      }
      return false;
    }
    if (si >= s.size()) return false;
    if (c == '?') {
      ++si;
      ++pi;
      continue;
    }
    if (c == '[') {
      std::size_t j = pi + 1;
      bool negate = false;
      if (j < p.size() && (p[j] == '!' || p[j] == '^')) {
        negate = true;
        ++j;
      }
      bool matched = false;
      bool first = true;
      while (j < p.size() && (p[j] != ']' || first)) {
        first = false;  // ']' right after '[' (or '[!') is a literal member
        const char lo = p[j];
        if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
          if (s[si] >= lo && s[si] <= p[j + 2]) matched = true;
          j += 3;
        } else {
          if (s[si] == lo) matched = true;
          ++j;
        }
      }
      if (j >= p.size()) return false;  // unterminated class → bad pattern
      ++j;                              // skip ']'
      if (negate) matched = !matched;
      if (!matched) return false;
      ++si;
      pi = j;
      continue;
    }
    if (s[si] != c) return false;
    ++si;
    ++pi;
  }
  return si == s.size();
}

bool match_seg(std::string_view s, std::string_view p) {
  return match_seg_at(s, 0, p, 0);
}

void glob_rec(const std::vector<std::string>& segs, std::size_t idx,
              const std::string& prefix, std::vector<std::string>& out) {
  const bool last = idx + 1 == segs.size();
  const std::string& seg = segs[idx];
  if (seg.empty()) return;  // Go: no directory entry is named ""

  if (!has_meta(seg)) {
    const std::string path = prefix + seg;
    if (last) {
      out.push_back(path);
    } else {
      glob_rec(segs, idx + 1, path + "/", out);
    }
    return;
  }

  std::error_code ec;
  fs::directory_iterator it(prefix.empty() ? "." : prefix, ec);
  if (ec) return;  // unreadable dir → no matches (Go: skip, no error)
  for (const auto& de : it) {
    const std::string name = de.path().filename().string();
    if (!match_seg(name, seg)) continue;
    const std::string path = prefix + name;
    if (last) {
      out.push_back(path);
    } else if (de.is_directory()) {
      glob_rec(segs, idx + 1, path + "/", out);
    }
  }
}

// glob replicates filepath.Glob. Patterns without meta characters return the
// pattern itself; Args falls back to the arg either way, so this matches the
// Go behavior at the call site (Glob errors and empty results both fall back
// to the raw argument).
std::vector<std::string> glob(const std::string& pattern) {
  if (!has_meta(pattern)) return {pattern};

  const bool abs = !pattern.empty() && pattern.front() == '/';
  std::vector<std::string> segs;
  std::size_t i = abs ? 1 : 0;
  while (i < pattern.size()) {
    const std::size_t j = pattern.find('/', i);
    if (j == std::string::npos) {
      segs.push_back(pattern.substr(i));
      break;
    }
    segs.push_back(pattern.substr(i, j - i));
    i = j + 1;
  }
  if (segs.empty()) return {};

  std::vector<std::string> out;
  glob_rec(segs, 0, abs ? "/" : "", out);
  std::sort(out.begin(), out.end());
  return out;
}

// --- HLS body detection (cliamp isHLSPlaylist) ------------------------------

// is_hls_body reports whether an M3U body is an HLS playlist (master or
// media) rather than a plain list of media URLs. HLS is identified by its
// #EXT-X-* tags. Internal: the public is_hls_playlist() is the URL-extension
// variant used for routing .m3u8 to ffmpeg-by-URL.
bool is_hls_body(std::string_view body) {
  return body.find("#EXT-X-STREAM-INF") != std::string_view::npos ||      // master
         body.find("#EXT-X-TARGETDURATION") != std::string_view::npos ||  // media
         body.find("#EXT-X-MEDIA-SEQUENCE") != std::string_view::npos;    // media
}

std::string http_status_error(const audio::HttpResponse& resp) {
  std::string s = "http status " + std::to_string(resp.status);
  if (!resp.status_text.empty()) s += " " + resp.status_text;
  return s;
}

}  // namespace

// --- public contract --------------------------------------------------------

bool expand_yt_playlist() { return g_expand_yt_playlist.load(std::memory_order_relaxed); }

void set_expand_yt_playlist(bool v) { g_expand_yt_playlist.store(v, std::memory_order_relaxed); }

void set_ytdl_cookies_from(std::string_view browser) {
  auto* s = new std::string(browser);
  g_ytdl_cookies.store(s->c_str(), std::memory_order_release);
}

std::string_view ytdl_cookies_from() {
  const char* p = g_ytdl_cookies.load(std::memory_order_acquire);
  return p != nullptr ? std::string_view(p) : std::string_view{};
}

std::expected<Result, std::string> args(const std::vector<std::string>& argv) {
  Result r;
  std::vector<std::string> files;

  for (const auto& arg : argv) {
    if (playlist::is_url(arg)) {
      if (playlist::is_feed(arg) || playlist::is_m3u(arg) || playlist::is_pls(arg) ||
          playlist::is_youtube_url(arg) || playlist::is_ytdl(arg) ||
          playlist::is_xiaoyuzhou_episode(arg) || sniff_feed_url(arg)) {
        r.pending.push_back(arg);
      } else {
        files.push_back(arg);
      }
      continue;
    }

    auto matches = glob(arg);
    if (matches.empty()) matches = {arg};
    for (const auto& path : matches) {
      if (playlist::is_local_m3u(path)) {
        auto tracks = resolve_local_m3u(path);
        if (!tracks) return std::unexpected("loading m3u " + path + ": " + tracks.error());
        r.tracks.insert(r.tracks.end(), tracks->begin(), tracks->end());
        continue;
      }
      if (playlist::is_local_pls(path)) {
        auto tracks = resolve_local_pls(path);
        if (!tracks) return std::unexpected("loading pls " + path + ": " + tracks.error());
        r.tracks.insert(r.tracks.end(), tracks->begin(), tracks->end());
        continue;
      }
      auto resolved = audio_files(path, true);  // CollectAudioFiles
      if (!resolved) return std::unexpected("scanning " + path + ": " + resolved.error());
      files.insert(files.end(), resolved->begin(), resolved->end());
    }
  }

  auto scanned = scan_tracks(files);
  r.tracks.insert(r.tracks.end(), scanned.begin(), scanned.end());
  return r;
}

std::expected<std::vector<playlist::Track>, std::string>
resolve_m3u(std::string_view url) {
  audio::HttpClient client;
  auto resp = client.fetch_text(url, kMaxM3UBody, std::chrono::seconds{30});
  if (!resp) return std::unexpected(resp.error());
  if (resp->status != 200) return std::unexpected(http_status_error(*resp));

  const std::string& body = resp->body;
  if (is_hls_body(body)) {
    // Parsing an HLS playlist as a track list would extract the relative
    // "chunklist_*.m3u8" URI as a bogus local-file track. The original URL is
    // handed to the player, where ffmpeg resolves the relative chunklist/
    // segment URIs and follows the live segment window.
    playlist::Track t = playlist::track_from_path(url);
    // #EXT-X-ENDLIST only appears in media playlists, so VOD behind a master
    // playlist is conservatively treated as live.
    t.realtime = body.find("#EXT-X-ENDLIST") == std::string::npos;
    return std::vector<playlist::Track>{std::move(t)};
  }

  auto entries = parse_m3u(body, "");
  if (!entries) return std::unexpected(entries.error());
  return entries_to_tracks(*entries);
}

std::expected<std::vector<playlist::Track>, std::string>
resolve_pls(std::string_view url) {
  audio::HttpClient client;
  auto resp = client.fetch_text(url, kMaxM3UBody, std::chrono::seconds{30});
  if (!resp) return std::unexpected(resp.error());
  if (resp->status != 200) return std::unexpected(http_status_error(*resp));

  auto entries = parse_pls(resp->body);
  if (!entries) return std::unexpected(entries.error());
  return pls_entries_to_tracks(*entries);
}

bool is_hls_playlist(std::string_view url) {
  // Query/fragment are not part of the path.
  std::string_view p = url.substr(0, url.find_first_of("?#"));
  return ascii_lower(p).ends_with(".m3u8");
}

}  // namespace bootamp::resolve
