// tests/resolve/test_ytdl_resolve.cpp — resolve_ytdl module tests.
//
// Ports of cliamp playlist/url_test.go TestIsYTDL and
// resolve/resolve_test.go TestParseYTDLTracksCountsMalformedEntries,
// plus recorded --flat-playlist JSON fixtures (as string constants) covering
// the field fallbacks of cliamp resolve.go parseYTDLTracks.
//
// The subprocess path (resolve_ytdl) is not exercised here: it requires a real
// yt-dlp on PATH. JSON parsing and classification are pure and fully tested.

#include "playlist/playlist.hpp"
#include "resolve/resolve.hpp"  // set_ytdl_cookies_from / ytdl_cookies_from
#include "resolve/ytdl.hpp"
#include "resolve/ytdl_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

namespace {

using bootamp::playlist::Track;
using bootamp::playlist::is_ytdl;
using bootamp::resolve::detail::humanize_basename;
using bootamp::resolve::detail::parse_ytdl_tracks;

// Go t.Setenv equivalent: restores the variable on scope exit, also on test
// failure (REQUIRE aborts the section but not the process).
class EnvGuard {
public:
  EnvGuard(const char* name, const std::string& value) : name_(name) {
    if (const char* old = std::getenv(name); old != nullptr) old_ = old;
    setenv(name, value.c_str(), 1);
  }
  ~EnvGuard() {
    if (old_) {
      setenv(name_.c_str(), old_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }
  EnvGuard(const EnvGuard&) = delete;
  EnvGuard& operator=(const EnvGuard&) = delete;

private:
  std::string              name_;
  std::optional<std::string> old_;
};

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

// cliamp playlist/url_test.go TestIsYTDL (all 20 cases, 1:1).
TEST_CASE("IsYTDL classifies supported and unsupported URLs", "[resolve][ytdl]") {
  struct Case {
    std::string_view path;
    bool             want;
  };
  const std::vector<Case> cases = {
      // YouTube
      {"https://www.youtube.com/watch?v=abc123", true},
      {"https://youtu.be/abc123", true},
      // YouTube Music
      {"https://music.youtube.com/watch?v=abc123", true},
      // Search protocols
      {"ytsearch:lofi hip hop", true},
      {"ytsearch1:some song", true},
      {"ytsearch10:multi result query", true},
      {"scsearch:artist name", true},
      {"scsearch1:track name", true},
      {"scsearch5:multi result query", true},
      // SoundCloud
      {"https://soundcloud.com/artist/track", true},
      // Bandcamp
      {"https://bandcamp.com/album", true},
      {"https://artist.bandcamp.com/album/name", true},
      // Bilibili
      {"https://bilibili.com/video/BV123", true},
      {"https://www.bilibili.com/video/BV123", true},
      {"https://space.bilibili.com/12345", true},
      {"https://b23.tv/abc123", true},
      // NetEase
      {"https://music.163.com/song?id=12345", true},
      // Non-YTDL
      {"https://example.com/stream.mp3", false},
      {"/local/file.mp3", false},
      {"", false},
  };
  for (const Case& c : cases) {
    CAPTURE(c.path);
    CHECK(is_ytdl(c.path) == c.want);
  }
}

// Extra classifier cases: subdomain suffix rules and the www./m. stripping.
TEST_CASE("IsYTDL suffix and prefix stripping edge cases", "[resolve][ytdl]") {
  struct Case {
    std::string_view path;
    bool             want;
  };
  const std::vector<Case> cases = {
      // Subdomain suffix matching (space.bilibili.com / artist.bandcamp.com).
      {"https://www.bilibili.com/video/BV1", true},
      {"https://music.bilibili.com/xyz", true},
      {"https://m.bilibili.com/xyz", true},        // m. prefix stripped
      {"https://notbilibili.com/x", false},        // suffix must be exact
      {"https://bilibili.com.evil.com/x", false},  // trailing host junk
      {"https://soundcloud.com.evil.com/x", false},
      // www./m. handling in both orders.
      {"https://www.m.youtube.com/watch?v=x", true},
      {"https://m.youtube.com/watch?v=x", true},
      {"https://www.youtube.com/x?list=RD1", true},
      // Scheme case-sensitivity (Go strings.HasPrefix is case-sensitive).
      {"HTTPS://soundcloud.com/artist/track", false},
      {"Ytsearch:foo", false},
      // Host with port (Go url.Hostname strips it).
      {"https://soundcloud.com:8443/artist/track", true},
      {"https://www.youtube.com:443/watch?v=x", true},
      // Non-http schemes are not URLs.
      {"ftp://youtube.com/x", false},
  };
  for (const Case& c : cases) {
    CAPTURE(c.path);
    CHECK(is_ytdl(c.path) == c.want);
  }
}

// cliamp resolve/resolve_test.go TestParseYTDLTracksCountsMalformedEntries.
TEST_CASE("parse_ytdl_tracks counts malformed entries", "[resolve][ytdl]") {
  const std::string input =
      "{\"webpage_url\":\"https://example.com/one\",\"title\":\"One\"}\n"
      "{malformed}\n"
      "{\"title\":\"Missing URL\"}\n";
  const auto res = parse_ytdl_tracks(input);
  REQUIRE(res.has_value());
  CHECK(res->entries == 3);
  REQUIRE(res->tracks.size() == 1);
  CHECK(res->tracks[0].path == "https://example.com/one");
  CHECK(res->tracks[0].title == "One");
}

// Recorded yt-dlp --flat-playlist JSON lines: field mapping, fallbacks, and
// strict-skip semantics (Go all-or-nothing Unmarshal).
TEST_CASE("parse_ytdl_tracks maps flat-playlist JSON to tracks", "[resolve][ytdl]") {
  const std::string input =
      // 1. Full entry: webpage_url preferred over url, uploader, float duration.
      "{\"webpage_url\":\"https://www.youtube.com/watch?v=abc\",\"url\":"
      "\"https://redirect.example/abc\",\"title\":\"Song One\",\"uploader\":\"Artist A\","
      "\"duration\":214.6}\n"
      // 2. No webpage_url/uploader/title: url, playlist_uploader, humanized basename.
      "{\"url\":\"https://soundcloud.com/a/b\",\"webpage_url_basename\":\"clr-podcast-467\","
      "\"playlist_uploader\":\"Artist B\"}\n"
      // 3. No URL at all: skipped.
      "{\"webpage_url\":\"\",\"url\":\"\",\"title\":\"No URL at all\"}\n"
      // 4. null title/uploader are zero-valued (Go), duration int.
      "{\"webpage_url\":\"https://bandcamp.com/track\",\"title\":null,\"uploader\":null,"
      "\"duration\":3}\n"
      // 5. duration as string: Go Unmarshal fails, entry skipped.
      "{\"webpage_url\":\"https://example.com/x\",\"title\":\"X\",\"duration\":\"5:30\"}\n"
      // 6. Trailing line without newline (bufio Scanner emits it too).
      "{\"webpage_url\":\"https://example.com/two\",\"title\":\"Two\"}";
  const auto res = parse_ytdl_tracks(input);
  REQUIRE(res.has_value());
  CHECK(res->entries == 6);
  REQUIRE(res->tracks.size() == 4);

  const Track& t0 = res->tracks[0];
  CHECK(t0.path == "https://www.youtube.com/watch?v=abc");
  CHECK(t0.title == "Song One");
  CHECK(t0.artist == "Artist A");
  CHECK(t0.stream);
  CHECK(t0.duration_secs == 214);  // int(214.6) truncates

  const Track& t1 = res->tracks[1];
  CHECK(t1.path == "https://soundcloud.com/a/b");
  CHECK(t1.title == "clr podcast 467");
  CHECK(t1.artist == "Artist B");
  CHECK(t1.stream);

  const Track& t2 = res->tracks[2];
  CHECK(t2.path == "https://bandcamp.com/track");
  CHECK(t2.title == "https://bandcamp.com/track");  // title falls back to URL
  CHECK(t2.artist.empty());
  CHECK(t2.duration_secs == 3);

  const Track& t3 = res->tracks[3];
  CHECK(t3.path == "https://example.com/two");
  CHECK(t3.title == "Two");
}

TEST_CASE("parse_ytdl_tracks skips blank lines before counting", "[resolve][ytdl]") {
  const std::string input =
      "{\"webpage_url\":\"https://example.com/untitled\"}\n"
      "\n"
      "   \t \n"
      "{\"webpage_url\":\"https://example.com/also\"}";
  const auto res = parse_ytdl_tracks(input);
  REQUIRE(res.has_value());
  CHECK(res->entries == 2);
  REQUIRE(res->tracks.size() == 2);
  CHECK(res->tracks[0].path == "https://example.com/untitled");
  CHECK(res->tracks[0].title == "https://example.com/untitled");
}

TEST_CASE("parse_ytdl_tracks rejects over-long lines like bufio.Scanner", "[resolve][ytdl]") {
  std::string input = "{\"webpage_url\":\"https://example.com/long\"";
  input.append(1024 * 1024 + 1, 'x');  // 1MB + 1 > scannerMaxLineSize
  input.append("}\n");
  const auto res = parse_ytdl_tracks(input);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == "bufio.Scanner: token too long");
}

// cliamp resolve/pls_test.go TestHumanizeBasename + extension-dropping cases.
TEST_CASE("humanize_basename converts dashes and drops audio extensions", "[resolve][ytdl]") {
  struct Case {
    std::string_view input;
    std::string_view want;
  };
  const std::vector<Case> cases = {
      {"clr-podcast-467", "clr podcast 467"},
      {"no-dashes-here", "no dashes here"},
      {"nodashes", "nodashes"},
      {"", ""},
      {"track.mp3", "track"},
      {"dir/album-track.flac", "dir/album track"},
      {"3.5-remix", "3.5 remix"},  // ".5-remix" not a media ext; dashes still humanized
      {"UPPER.MP3", "UPPER"},      // extension match is case-insensitive
  };
  for (const Case& c : cases) {
    CAPTURE(c.input);
    CHECK(humanize_basename(c.input) == c.want);
  }
}

// End-to-end subprocess check with a fake yt-dlp that logs its argv — port of
// cliamp resolve_test.go TestResolveYTDLBatchCookieSelection, extended with
// the --playlist-start/--playlist-end bounds. Verifies the EXACT Go argument
// order: flags, cookies, playlist bounds, URL last.
TEST_CASE("resolve_ytdl builds the exact yt-dlp argument list", "[resolve][ytdl]") {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::temp_directory_path() / "bootamp_ytdl_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  const fs::path log = tmp / "ytdlp_args.log";
  const fs::path fake = tmp / "yt-dlp";
  {
    std::ofstream out(fake);
    out << "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" << log.string() << "'\n";
  }
  chmod(fake.c_str(), 0755);

  const char* old_path = std::getenv("PATH");
  const EnvGuard path_guard("PATH", tmp.string() + ":" + (old_path ? old_path : ""));
  // Cookies must not leak between cases (Go t.Cleanup(SetYTDLCookiesFrom(""))).
  struct CookiesRestore {
    ~CookiesRestore() { bootamp::resolve::set_ytdl_cookies_from(""); }
  } cookies_restore;

  // 1. Bounded resolve, no cookies: flags then bounds then URL.
  bootamp::resolve::set_ytdl_cookies_from("");
  const auto r1 =
      bootamp::resolve::resolve_ytdl_with_bounds("https://example.com/playlist", 2, 5);
  REQUIRE(r1.has_value());
  REQUIRE(r1->empty());  // the arg echo is not JSON → zero tracks, no error
  CHECK(read_file(log) ==
        "yt-dlp\n"
        "--flat-playlist\n"
        "-j\n"
        "--socket-timeout\n"
        "15\n"
        "--playlist-start\n"
        "3\n"  // start+1: yt-dlp is 1-based
        "--playlist-end\n"
        "5\n"
        "https://example.com/playlist\n");

  // 2. Global cookies (ytdl_cookies_from) appended after --socket-timeout.
  bootamp::resolve::set_ytdl_cookies_from("firefox");
  const auto r2 = bootamp::resolve::resolve_ytdl_with_bounds("https://example.com/playlist", 0, 0);
  REQUIRE(r2.has_value());
  CHECK(read_file(log) ==
        "yt-dlp\n"
        "--flat-playlist\n"
        "-j\n"
        "--socket-timeout\n"
        "15\n"
        "--cookies-from-browser\n"
        "firefox\n"
        "https://example.com/playlist\n");

  // 3. Bounds + cookies together: cookies come before the playlist range.
  bootamp::resolve::set_ytdl_cookies_from("brave");
  const auto r3 = bootamp::resolve::resolve_ytdl_with_bounds("https://example.com/playlist", 0, 1);
  REQUIRE(r3.has_value());
  CHECK(read_file(log) ==
        "yt-dlp\n"
        "--flat-playlist\n"
        "-j\n"
        "--socket-timeout\n"
        "15\n"
        "--cookies-from-browser\n"
        "brave\n"
        "--playlist-end\n"
        "1\n"
        "https://example.com/playlist\n");

  // 4. Default start bound: --playlist-start omitted when start == 0.
  bootamp::resolve::set_ytdl_cookies_from("");
  const auto r4 = bootamp::resolve::resolve_ytdl("https://example.com/playlist");
  REQUIRE(r4.has_value());
  CHECK(read_file(log) ==
        "yt-dlp\n"
        "--flat-playlist\n"
        "-j\n"
        "--socket-timeout\n"
        "15\n"
        "https://example.com/playlist\n");
}
