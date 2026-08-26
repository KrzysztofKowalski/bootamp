// tests/ui/test_info.cpp — InfoModel tests (track info overlay).
//
// Ports the Go track-info overlay behavior (ui/model/keys.go: "i" opens at
// keys.go:764-765 with infoScroll=0, keys.go:272-283 scroll keys;
// inline_overlays.go infoLines/renderInfoBody/infoMaybeAdjustScroll) onto the
// plain-C++ model: open/close/active, scroll clamping, and the rendered
// field lines.
#include "ui/screens/info.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace bootamp::ui::screens;
using bootamp::playlist::Track;

TEST_CASE("info open snapshots the track and activates", "[screens][info]") {
  InfoModel m;
  const Track t{.title = "Song", .artist = "Artist", .album = "Album"};
  REQUIRE(!m.active());
  m.open(t);
  REQUIRE(m.active());
  REQUIRE(m.track().title == "Song");
  REQUIRE(m.track().artist == "Artist");
  REQUIRE(m.track().album == "Album");
  m.close();
  REQUIRE(!m.active());
}

TEST_CASE("info lines cover the metadata fields", "[screens][info]") {
  // Go infoLines: Title/Artist/Album/Genre/Year(!=0)/Track(!=0)/Path with
  // empty fields skipped; bootamp adds Duration (duration_secs > 0).
  InfoModel m;
  m.open(Track{.path        = "/music/song.flac",
               .title       = "Song",
               .artist      = "Artist",
               .album       = "Album",
               .genre       = "Jazz",
               .year        = 1999,
               .track_number = 3,
               .duration_secs = 90});
  REQUIRE(m.line_count() == 8);
  REQUIRE(m.line(0) == "  Title: Song");
  REQUIRE(m.line(1) == "  Artist: Artist");
  REQUIRE(m.line(2) == "  Album: Album");
  REQUIRE(m.line(3) == "  Genre: Jazz");
  REQUIRE(m.line(4) == "  Year: 1999");
  REQUIRE(m.line(5) == "  Track: 3");
  REQUIRE(m.line(6) == "  Duration: 1:30");
  REQUIRE(m.line(7) == "  Path: /music/song.flac");
}

TEST_CASE("info empty fields skipped; fully empty track has fallback",
          "[screens][info]") {
  InfoModel m;
  m.open(Track{.title = "Song"});  // only the title set
  REQUIRE(m.line_count() == 1);
  REQUIRE(m.line(0) == "  Title: Song");

  m.open(Track{});  // no metadata (Go: "No track metadata available.")
  REQUIRE(m.line_count() == 1);
  REQUIRE(m.line(0) == "  No track metadata available.");
}

TEST_CASE("info scroll clamps to the line window", "[screens][info]") {
  // Go: up guarded at 0, down re-clamped by infoMaybeAdjustScroll; bootamp
  // clamps into [0, line_count-1] (the component frame-clips the rest).
  InfoModel m;
  m.open(Track{.path        = "E",
               .title       = "A",
               .artist      = "B",
               .album       = "C",
               .genre       = "D",
               .year        = 1,
               .track_number = 1,
               .duration_secs = 1});
  REQUIRE(m.line_count() == 8);
  REQUIRE(m.scroll() == 0);  // open resets the scroll (Go infoScroll=0)

  m.scroll(1);
  REQUIRE(m.scroll() == 1);
  m.scroll(100);  // clamp at the last line
  REQUIRE(m.scroll() == 7);
  m.scroll(-100);  // clamp at the top
  REQUIRE(m.scroll() == 0);
}

TEST_CASE("info scroll clamps on a one-line overlay", "[screens][info]") {
  InfoModel m;
  m.open(Track{.title = "Only"});
  REQUIRE(m.line_count() == 1);
  m.scroll(1);  // no room to scroll
  REQUIRE(m.scroll() == 0);
  m.scroll(-1);
  REQUIRE(m.scroll() == 0);
}

TEST_CASE("info reopen resets scroll", "[screens][info]") {
  InfoModel m;
  m.open(Track{.path        = "E",
               .title       = "A",
               .artist      = "B",
               .album       = "C",
               .genre       = "D",
               .year        = 1,
               .track_number = 1,
               .duration_secs = 1});
  m.scroll(5);
  REQUIRE(m.scroll() == 5);
  m.open(Track{.title = "New"});  // Go "i" again: infoScroll=0
  REQUIRE(m.scroll() == 0);
  REQUIRE(m.track().title == "New");
  REQUIRE(m.line_count() == 1);
}
