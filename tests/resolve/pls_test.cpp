// tests/resolve/pls_test.cpp — port of cliamp/resolve/pls_test.go.
//
// Fixtures are inline strings; no network, no filesystem access. The Go test
// exercises allStreams directly; here that helper is internal to pls.cpp and
// is covered indirectly through radio_mirrors / pls_entries_to_tracks.
#include "resolve/pls.hpp"

#include "resolve/resolve.hpp"  // strip_mirror_suffix

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using bootamp::resolve::parse_pls;
using bootamp::resolve::pls_entries_to_tracks;
using bootamp::resolve::radio_mirrors;
using bootamp::resolve::strip_mirror_suffix;
using bootamp::resolve::PlsEntry;

}  // namespace

TEST_CASE("parse_pls basic entries", "[resolve][pls]") {
  const std::string input =
      "[playlist]\n"
      "File1=http://radio.example.com/stream1\n"
      "Title1=Station One\n"
      "Length1=-1\n"
      "File2=http://radio.example.com/stream2\n"
      "Title2=Station Two\n"
      "Length2=-1\n"
      "NumberOfEntries=2\n"
      "Version=2\n";
  auto entries = parse_pls(input);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 2);

  CHECK(entries->at(0).file == "http://radio.example.com/stream1");
  CHECK(entries->at(0).title == "Station One");
  CHECK(entries->at(0).num == 1);
  CHECK(entries->at(0).length == -1);
  CHECK(entries->at(0).has_length);

  CHECK(entries->at(1).file == "http://radio.example.com/stream2");
  CHECK(entries->at(1).title == "Station Two");
}

TEST_CASE("parse_pls sorts entries by number", "[resolve][pls]") {
  const std::string input =
      "[playlist]\n"
      "File3=http://example.com/3\n"
      "Title3=Third\n"
      "File1=http://example.com/1\n"
      "Title1=First\n"
      "File2=http://example.com/2\n"
      "Title2=Second\n";
  auto entries = parse_pls(input);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 3);
  CHECK(entries->at(0).title == "First");
  CHECK(entries->at(1).title == "Second");
  CHECK(entries->at(2).title == "Third");
}

TEST_CASE("parse_pls errors on empty playlist", "[resolve][pls]") {
  const std::string input =
      "[playlist]\n"
      "NumberOfEntries=0\n"
      "Version=2\n";
  auto entries = parse_pls(input);
  REQUIRE_FALSE(entries.has_value());
  CHECK(entries.error() == "no entries found in PLS playlist");
}

TEST_CASE("parse_pls skips sections and comments", "[resolve][pls]") {
  const std::string input =
      "[playlist]\n"
      "; This is a comment\n"
      "File1=http://example.com/stream\n"
      "Title1=Station\n";
  auto entries = parse_pls(input);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  CHECK(entries->at(0).file == "http://example.com/stream");
}

TEST_CASE("strip_mirror_suffix removes (#N) suffixes", "[resolve][pls]") {
  CHECK(strip_mirror_suffix("Groove Salad (#3)") == "Groove Salad");
  CHECK(strip_mirror_suffix("Station Name (#1)") == "Station Name");
  CHECK(strip_mirror_suffix("No Mirror Suffix") == "No Mirror Suffix");
  CHECK(strip_mirror_suffix("") == "");
  CHECK(strip_mirror_suffix("Station: (#2)") == "Station");
}

TEST_CASE("radio_mirrors collapses URL entries with mirror-suffix titles",
          "[resolve][pls]") {
  const std::vector<PlsEntry> es = {
      {1, "http://a.com/stream", "Station (#1)", 0, false},
      {2, "http://b.com/stream", "Station (#2)", 0, false},
  };
  auto tracks = pls_entries_to_tracks(es);
  REQUIRE(tracks.size() == 1);
  CHECK(tracks[0].title == "Station");
  CHECK(tracks[0].stream);
  CHECK(tracks[0].realtime);
  CHECK(tracks[0].path == "http://a.com/stream");
}

TEST_CASE("radio_mirrors true when all entries are indefinite streams",
          "[resolve][pls]") {
  const std::vector<PlsEntry> es = {
      {1, "http://a.com/s", "A", -1, true},
      {2, "http://b.com/s", "B", -1, true},
  };
  CHECK(radio_mirrors(es));
}

TEST_CASE("radio_mirrors false for mixed or local entries", "[resolve][pls]") {
  SECTION("mixed URL and local file") {
    const std::vector<PlsEntry> es = {
        {1, "http://a.com/s", "A", 0, false},
        {2, "/local/file.mp3", "B", 0, false},
    };
    CHECK_FALSE(radio_mirrors(es));
  }
  SECTION("all local") {
    const std::vector<PlsEntry> es = {
        {1, "/a.mp3", "", 0, false},
        {2, "/b.mp3", "", 0, false},
    };
    CHECK_FALSE(radio_mirrors(es));
  }
  SECTION("empty") {
    CHECK_FALSE(radio_mirrors({}));
  }
}

TEST_CASE("pls_entries_to_tracks: titleless indefinite stream is live",
          "[resolve][pls]") {
  const std::vector<PlsEntry> es = {
      {1, "http://radio.example.com/stream", "", -1, true},
  };
  auto tracks = pls_entries_to_tracks(es);
  REQUIRE(tracks.size() == 1);
  CHECK(tracks[0].stream);
  CHECK(tracks[0].realtime);
}

TEST_CASE("pls_entries_to_tracks: finite HTTP entry is not live", "[resolve][pls]") {
  const std::vector<PlsEntry> es = {
      {1, "http://media.example.com/episode.mp3", "", 180, true},
  };
  auto tracks = pls_entries_to_tracks(es);
  REQUIRE(tracks.size() == 1);
  CHECK_FALSE(tracks[0].realtime);
  CHECK(tracks[0].duration_secs == 180);
}

TEST_CASE("pls_entries_to_tracks: missing lengths are not assumed live",
          "[resolve][pls]") {
  const std::vector<PlsEntry> es = {
      {1, "http://media.example.com/one.mp3", "One", 0, false},
      {2, "http://media.example.com/two.mp3", "Two", 0, false},
  };
  auto tracks = pls_entries_to_tracks(es);
  REQUIRE(tracks.size() == 2);
  CHECK_FALSE(tracks[0].realtime);
  CHECK_FALSE(tracks[1].realtime);
}

TEST_CASE("pls_entries_to_tracks: multiple local entries keep individual tracks",
          "[resolve][pls]") {
  const std::vector<PlsEntry> es = {
      {1, "/home/user/song.mp3", "Song One", 0, false},
      {2, "/home/user/song2.mp3", "Song Two", 0, false},
  };
  auto tracks = pls_entries_to_tracks(es);
  REQUIRE(tracks.size() == 2);
  CHECK(tracks[0].title == "Song One");
  CHECK_FALSE(tracks[0].stream);
  CHECK(tracks[1].title == "Song Two");
}
