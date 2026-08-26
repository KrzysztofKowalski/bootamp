// tests/resolve/m3u_test.cpp — port of cliamp/resolve/m3u_test.go.
//
// Fixtures are inline strings; no network, no filesystem access. The Go test
// calls resolveM3UPath directly; here that helper is internal to m3u.cpp, so
// its Windows/POSIX semantics are exercised through parse_m3u with base_dir.
#include "resolve/m3u.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using bootamp::resolve::M3UEntry;
using bootamp::resolve::entries_to_tracks;
using bootamp::resolve::m3u_entry_to_track;
using bootamp::resolve::parse_m3u;

}  // namespace

TEST_CASE("parse_m3u basic EXTINF entries", "[resolve][m3u]") {
  const std::string input =
      "#EXTM3U\n"
      "#EXTINF:120,Artist - Song One\n"
      "http://example.com/song1.mp3\n"
      "#EXTINF:180,Artist - Song Two\n"
      "http://example.com/song2.mp3\n";
  auto entries = parse_m3u(input, "");
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 2);

  CHECK(entries->at(0).title == "Artist - Song One");
  CHECK(entries->at(0).duration == 120);
  CHECK(entries->at(0).path == "http://example.com/song1.mp3");
  CHECK(entries->at(1).title == "Artist - Song Two");
  CHECK(entries->at(1).duration == 180);
  CHECK(entries->at(1).path == "http://example.com/song2.mp3");
}

TEST_CASE("parse_m3u without #EXTM3U header still works", "[resolve][m3u]") {
  const std::string input =
      "http://example.com/stream1.mp3\n"
      "http://example.com/stream2.mp3\n";
  auto entries = parse_m3u(input, "");
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 2);
  CHECK(entries->at(0).duration == -1);  // bare entries have unknown duration
  CHECK(entries->at(1).duration == -1);
}

TEST_CASE("parse_m3u strips UTF-8 BOM", "[resolve][m3u]") {
  const std::string input =
      "\xef\xbb\xbf#EXTM3U\n"
      "#EXTINF:60,Song\n"
      "http://example.com/song.mp3\n";
  auto entries = parse_m3u(input, "");
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  CHECK(entries->at(0).title == "Song");
  CHECK(entries->at(0).path == "http://example.com/song.mp3");
}

TEST_CASE("parse_m3u skips comment and directive lines", "[resolve][m3u]") {
  const std::string input =
      "#EXTM3U\n"
      "# This is a comment\n"
      "#EXTINF:60,Song\n"
      "http://example.com/song.mp3\n"
      "#EXTVLCOPT:some-option\n";
  auto entries = parse_m3u(input, "");
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  CHECK(entries->at(0).title == "Song");
}

TEST_CASE("parse_m3u resolves relative paths against base_dir", "[resolve][m3u]") {
  const std::string input =
      "#EXTM3U\n"
      "#EXTINF:60,Song\n"
      "music/song.mp3\n";
  auto entries = parse_m3u(input, "/home/user");
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  CHECK(entries->at(0).path == "/home/user/music/song.mp3");
}

TEST_CASE("parse_m3u keeps absolute paths unchanged", "[resolve][m3u]") {
  const std::string input = "/absolute/path/song.mp3\n";
  auto entries = parse_m3u(input, "/home/user");
  REQUIRE(entries.has_value());
  REQUIRE(!entries->empty());
  CHECK(entries->at(0).path == "/absolute/path/song.mp3");
}

TEST_CASE("parse_m3u of empty input yields no entries", "[resolve][m3u]") {
  auto entries = parse_m3u("", "");
  REQUIRE(entries.has_value());
  CHECK(entries->empty());
}

TEST_CASE("parse_m3u radio stream keeps -1 duration", "[resolve][m3u]") {
  const std::string input =
      "#EXTM3U\n"
      "#EXTINF:-1,Radio Station\n"
      "http://radio.example.com/stream\n";
  auto entries = parse_m3u(input, "");
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  CHECK(entries->at(0).duration == -1);
  CHECK(entries->at(0).title == "Radio Station");
}

TEST_CASE("parse_m3u Windows path semantics (base dir with backslashes)", "[resolve][m3u]") {
  // Port of TestResolveM3UPathWindows, exercised via parse_m3u because
  // resolveM3UPath is internal. Expected values computed with filepath
  // semantics on POSIX (FromSlash is the identity there).

  SECTION("relative backslash entry") {
    auto entries = parse_m3u("artist\\song.mp3\n", "C:\\Music");
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    CHECK(entries->at(0).path == "C:\\Music/artist\\song.mp3");
  }
  SECTION("relative forward slash entry") {
    auto entries = parse_m3u("artist/song.mp3\n", "C:\\Music");
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    CHECK(entries->at(0).path == "C:\\Music/artist/song.mp3");
  }
  SECTION("absolute drive-letter entry") {
    auto entries = parse_m3u("D:\\Other\\track.mp3\n", "C:\\Music");
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    CHECK(entries->at(0).path == "D:\\Other\\track.mp3");
  }
  SECTION("absolute UNC base dir") {
    auto entries = parse_m3u("sub\\file.mp3\n", "\\\\server\\share");
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    CHECK(entries->at(0).path == "\\\\server\\share/sub\\file.mp3");
  }
}

TEST_CASE("m3u_entry_to_track: URL with negative duration is live", "[resolve][m3u]") {
  const M3UEntry e{"http://radio.example.com/stream", "Radio Station", -1};
  const auto tr = m3u_entry_to_track(e);
  CHECK(tr.stream);
  CHECK(tr.realtime);
  CHECK(tr.duration_secs == 0);  // negative clamped to 0
  CHECK(tr.title == "Radio Station");
}

TEST_CASE("m3u_entry_to_track: zero-duration HTTP stream is live", "[resolve][m3u]") {
  const M3UEntry e{"http://radio.example.com/stream", "Radio Station", 0};
  const auto tr = m3u_entry_to_track(e);
  REQUIRE(tr.realtime);
}

TEST_CASE("m3u_entry_to_track: local file is not a stream", "[resolve][m3u]") {
  const M3UEntry e{"/home/user/song.mp3", "My Song", 180};
  const auto tr = m3u_entry_to_track(e);
  CHECK_FALSE(tr.stream);
  CHECK_FALSE(tr.realtime);
  CHECK(tr.duration_secs == 180);
  CHECK(tr.title == "My Song");
}

TEST_CASE("m3u_entry_to_track: titleless entry goes through track_from_path",
          "[resolve][m3u]") {
  const M3UEntry e{"/home/user/song.mp3", "", 180};
  const auto tr = m3u_entry_to_track(e);
  CHECK_FALSE(tr.stream);
  CHECK_FALSE(tr.realtime);
  CHECK(tr.duration_secs == 180);
  CHECK(tr.path == "/home/user/song.mp3");
}

TEST_CASE("entries_to_tracks converts in order", "[resolve][m3u]") {
  const std::vector<M3UEntry> es = {
      {"http://a.com/s1.mp3", "One", 100},
      {"http://a.com/s2.mp3", "Two", 200},
  };
  const auto tracks = entries_to_tracks(es);
  REQUIRE(tracks.size() == 2);
  CHECK(tracks[0].title == "One");
  CHECK(tracks[1].title == "Two");
  CHECK(tracks[1].duration_secs == 200);
}
