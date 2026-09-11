// tests/ui/test_gieres.cpp — GieresModel + client parser tests.
//
// The parsers run against captured JSON fixtures (docs/orgonity-api.md
// shapes); the screen model runs with injected fake fetch hooks (no network,
// no FTXUI) and asserts the playback mapping — full URLs joined with the
// test base, stream tracks for recordings/segments, realtime for live.
#include "ui/screens/gieres.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace bootamp::ui;
using namespace bootamp::ui::screens;
using bootamp::playlist::Track;

namespace {

// pump_until_done spins pump() until the background fetch landed (the
// test_screens.cpp lazy-catalog pattern: yield until loading clears, then one
// final pump in case the result landed between checks).
void pump_until_done(GieresModel& m) {
  for (int i = 0; i < 1000 && m.loading(); ++i) {
    m.pump();
    std::this_thread::yield();
  }
  m.pump();
}

// pump_until_dates spins pump() until the date-index fetch landed: the dates
// thread publishes through its own mailbox without touching loading(), so the
// fixture's three date rows appearing (row_count: 1 hint row → 3) is the
// observable signal — the same yield-until-visible pattern as above.
void pump_until_dates(GieresModel& m) {
  for (int i = 0; i < 1000 && m.row_count() != 3; ++i) {
    m.pump();
    std::this_thread::yield();
  }
  m.pump();
}

// canned_orgonity is the /orgonity.json fixture (docs/orgonity-api.md §1):
// one page with a null duration + a no-audio entry, dates index riding along.
const std::string kOrgonityJson = R"json({
    "page": 2, "total_pages": 34, "total_count": 1688,
    "audio_count": 1688, "all_count": 1691,
    "recording_dates": ["2010-04-13", "2021-11-19", "2026-08-30"],
    "broadcasts": [
      {"id": 1900, "external_id": "ooOquVcjqg7Q",
       "title": "Teoria Chaosu Live. 2010.04.13. Wprowadzenie",
       "recording_date": "2010-04-13", "aired_at": "2010-04-12T22:00:00Z",
       "duration_seconds": 3584, "has_audio": true,
       "audio_url": "/broadcasts/1900/audio.mp3"},
      {"id": 6200, "external_id": "XXXXXXXXXXXX",
       "title": "Teoria Chaosu Live. 2021.11.19. Bez audio",
       "recording_date": "2021-11-19", "aired_at": "2021-11-19T23:00:00Z",
       "duration_seconds": null, "has_audio": false,
       "audio_url": "/broadcasts/6200/audio.mp3"}
    ]
  })json";

// canned_segments is the /echelon/segments fixture (docs/orgonity-api.md §4,
// verified 2026-08-30T20:00Z window): two 30-minute slices with listings.
const std::string kSegmentsJson = R"json({
    "segments": [
      {"id": 280905, "title": "Muzyka w Radiu",
       "aired_at": "2026-08-30T20:00:00Z", "duration": 1803,
       "url": "/broadcasts/280905/audio.mp3",
       "tracks": [
         {"title": "Rockett Radio — Most Racist Song Ever (Parody)", "position": 0},
         {"title": "Thievery Corporation — 33 Degree", "position": 180}
       ]},
      {"id": 282596, "title": "Muzyka w Radiu",
       "aired_at": "2026-08-30T20:30:00Z", "duration": 1804,
       "url": "/broadcasts/282596/audio.mp3",
       "tracks": [
         {"title": "Teoria Chaosu \"Koniec wakacji...\"", "position": 0}
       ]}
    ]
  })json";

// FakeArchive captures every fetch call for assertions and hands back canned
// results; `fail` switches each hook to an error return.
struct FakeArchive {
  GieresListing listing;
  GieresDay     day;
  std::vector<GieresSegment> segments;
  std::string   now_playing = "Radio Radio ..::.. Pink Floyd \"Hey You\"";
  bool          fail = false;

  int org_calls = 0;
  int day_calls = 0;
  int seg_calls = 0;
  int np_calls  = 0;
  std::vector<std::tuple<int, std::string, std::string>> org_args;  // page,q,sort
  std::vector<std::string> day_args;

  GieresModel make() {
    auto org = [this](int page, std::string_view q, std::string_view sort) {
      ++org_calls;
      org_args.emplace_back(page, std::string{q}, std::string{sort});
      if (fail) {
        return std::expected<GieresListing, std::string>(
            std::unexpected("gieres: connection refused"));
      }
      return std::expected<GieresListing, std::string>(listing);
    };
    auto by_date = [this](std::string_view date) {
      ++day_calls;
      day_args.push_back(std::string{date});
      if (fail) {
        return std::expected<GieresDay, std::string>(
            std::unexpected("gieres: connection refused"));
      }
      return std::expected<GieresDay, std::string>(day);
    };
    auto segs = [this](std::string_view, std::string_view) {
      ++seg_calls;
      if (fail) {
        return std::expected<std::vector<GieresSegment>, std::string>(
            std::unexpected("gieres: connection refused"));
      }
      return std::expected<std::vector<GieresSegment>, std::string>(segments);
    };
    auto np = [this]() {
      ++np_calls;
      if (fail) {
        return std::expected<std::string, std::string>(
            std::unexpected("gieres: connection refused"));
      }
      return std::expected<std::string, std::string>(now_playing);
    };
    return GieresModel(std::move(org), std::move(by_date), std::move(segs),
                       std::move(np), "http://gieres.test");
  }
};

GieresListing fixture_listing() {
  auto parsed = parse_orgonity_listing(kOrgonityJson);
  REQUIRE(parsed.has_value());
  return *parsed;
}

std::vector<GieresSegment> fixture_segments() {
  auto parsed = parse_echelon_segments(kSegmentsJson);
  REQUIRE(parsed.has_value());
  return *parsed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Query sanitizer + URL joining
// ---------------------------------------------------------------------------

TEST_CASE("sanitize_query keeps only [A-Za-z0-9 _-]", "[gieres][client]") {
  // docs/orgonity-api.md: the server silently strips non-ASCII from q —
  // sending "Most#2" would search "Most2", so sanitize client-side.
  REQUIRE(GieresClient::sanitize_query("Most#2") == "Most2");
  REQUIRE(GieresClient::sanitize_query("operacja most-2_ok") ==
          "operacja most-2_ok");
  REQUIRE(GieresClient::sanitize_query("złodziej") == "zodiej");  // ł dropped
  REQUIRE(GieresClient::sanitize_query("") == "");
}

TEST_CASE("url joins base and server-relative paths", "[gieres][client]") {
  GieresClient c("http://192.168.1.154:13080/");
  REQUIRE(c.url("/broadcasts/1900/audio.mp3") ==
          "http://192.168.1.154:13080/broadcasts/1900/audio.mp3");
  REQUIRE(c.url("orgonity.json") ==
          "http://192.168.1.154:13080/orgonity.json");
  REQUIRE(c.base_url() == "http://192.168.1.154:13080");
}

// ---------------------------------------------------------------------------
// Parsers (captured fixtures)
// ---------------------------------------------------------------------------

TEST_CASE("orgonity listing parses counts, dates and broadcasts",
          "[gieres][parse]") {
  auto got = parse_orgonity_listing(kOrgonityJson);
  REQUIRE(got.has_value());
  REQUIRE(got->page == 2);
  REQUIRE(got->total_pages == 34);
  REQUIRE(got->total_count == 1688);
  REQUIRE(got->recording_dates.size() == 3);
  REQUIRE(got->recording_dates[0] == "2010-04-13");
  REQUIRE(got->broadcasts.size() == 2);
  REQUIRE(got->broadcasts[0].id == 1900);
  REQUIRE(got->broadcasts[0].duration_seconds == 3584);
  REQUIRE(got->broadcasts[0].has_audio);
  // null duration → unknown (-1); has_audio=false gates playback
  REQUIRE(got->broadcasts[1].duration_seconds == -1);
  REQUIRE(!got->broadcasts[1].has_audio);
}

TEST_CASE("orgonity garbage is an error, not a crash", "[gieres][parse]") {
  REQUIRE_FALSE(parse_orgonity_listing("not json at all").has_value());
  REQUIRE_FALSE(parse_orgonity_listing("[1,2,3]").has_value());
}

TEST_CASE("echelon segments parse with program positions", "[gieres][parse]") {
  auto got = parse_echelon_segments(kSegmentsJson);
  REQUIRE(got.has_value());
  REQUIRE(got->size() == 2);
  REQUIRE((*got)[0].aired_at == "2026-08-30T20:00:00Z");
  REQUIRE((*got)[0].duration == 1803);
  REQUIRE((*got)[0].url == "/broadcasts/280905/audio.mp3");
  REQUIRE((*got)[0].tracks.size() == 2);
  REQUIRE((*got)[0].tracks[1].position == 180);
  REQUIRE((*got)[1].tracks.size() == 1);
}

TEST_CASE("by_date parses date and broadcasts", "[gieres][parse]") {
  const std::string json = R"json({"date": "2021-11-19", "count": 1,
    "broadcasts": [{"id": 6188, "title": "Operacja Most-2",
                    "recording_date": "2021-11-19", "duration_seconds": 11299,
                    "has_audio": true,
                    "audio_url": "/broadcasts/6188/audio.mp3"}]})json";
  auto got = parse_orgonity_day(json);
  REQUIRE(got.has_value());
  REQUIRE(got->date == "2021-11-19");
  REQUIRE(got->broadcasts.size() == 1);
  REQUIRE(got->broadcasts[0].duration_seconds == 11299);
}

TEST_CASE("now-playing parses the title", "[gieres][parse]") {
  auto got = parse_now_playing(
      R"json({"title":"Radio Radio ..::.. Pink Floyd \"Hey You\""})json");
  REQUIRE(got.has_value());
  REQUIRE(*got == "Radio Radio ..::.. Pink Floyd \"Hey You\"");
}

// ---------------------------------------------------------------------------
// Model: fetch lifecycle + playback mapping
// ---------------------------------------------------------------------------

TEST_CASE("open kicks the pending fetches and pump lands the results",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();
  GieresModel m = fake.make();
  REQUIRE(!m.visible());
  m.open();
  REQUIRE(m.visible());
  REQUIRE(m.loading());  // fetch kicked on open
  REQUIRE(m.view() == GieresModel::View::Live);
  pump_until_done(m);
  REQUIRE(!m.loading());
  REQUIRE(m.row_count() == 1);  // the live stream row
  REQUIRE(m.row_label(0, 0).find("now: " + fake.now_playing) !=
          std::string::npos);

  // Tab to Orgonity: the recording page was never fetched → it kicks now.
  m.handle_key("tab");
  REQUIRE(m.view() == GieresModel::View::Orgonity);
  pump_until_done(m);
  REQUIRE(fake.org_calls == 1);
  REQUIRE(m.row_count() == 2);
  REQUIRE(m.row_label(0, 0).find("2010-04-13") != std::string::npos);
  REQUIRE(m.row_label(1, 0).find("[no audio]") != std::string::npos);
  REQUIRE(m.row_dim(1));
  REQUIRE_FALSE(m.row_dim(0));
}

TEST_CASE("enter plays the cursor recording with a joined URL",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();
  GieresModel m = fake.make();
  std::vector<Track> played;
  m.set_actions(GieresActions{
      .on_play_track =
          [&played](const Track& t) { played.push_back(t); },
      .on_append_track  = {},
      .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle the Live fetch first
  m.handle_key("tab");  // → Orgonity (now not loading → fetch kicks)
  pump_until_done(m);
  m.handle_key("enter");
  REQUIRE(played.size() == 1);
  REQUIRE(played[0].path == "http://gieres.test/broadcasts/1900/audio.mp3");
  REQUIRE(played[0].stream);
  REQUIRE_FALSE(played[0].realtime);
  REQUIRE(played[0].title == "Teoria Chaosu Live. 2010.04.13. Wprowadzenie");
  REQUIRE(played[0].duration_secs == 3584);
  REQUIRE(played[0].artist == "Radio Radio");

  // The no-audio entry is dimmed and not playable.
  m.handle_key("down");  // → the [no audio] row
  m.handle_key("enter");
  REQUIRE(played.size() == 1);
}

TEST_CASE("enter plays the realtime live stream", "[gieres][model]") {
  FakeArchive fake;
  GieresModel m = fake.make();
  std::vector<Track> played;
  m.set_actions(GieresActions{
      .on_play_track =
          [&played](const Track& t) { played.push_back(t); },
      .on_append_track  = {},
      .on_append_tracks = {}});
  m.open();
  pump_until_done(m);
  m.handle_key("enter");
  REQUIRE(played.size() == 1);
  REQUIRE(played[0].path == "https://c16.radioboss.fm:18014/stream");
  REQUIRE(played[0].stream);
  REQUIRE(played[0].realtime);  // the ICY radio pipeline owns this one
}

TEST_CASE("y appends the cursor recording; echelon y appends the whole day",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();
  fake.segments = fixture_segments();
  GieresModel m = fake.make();
  std::vector<Track> appended;
  std::vector<std::vector<Track>> bulk;
  m.set_actions(GieresActions{
      .on_play_track = {},
      .on_append_track =
          [&appended](const Track& t) { appended.push_back(t); },
      .on_append_tracks = [&bulk](const std::vector<Track>& ts) {
        bulk.push_back(ts);
      }});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity
  pump_until_done(m);
  m.handle_key("y");
  REQUIRE(appended.size() == 1);
  REQUIRE(appended[0].path == "http://gieres.test/broadcasts/1900/audio.mp3");

  // → Echelon; y appends every segment so the queue chains them gapless.
  m.handle_key("tab");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == 1);
  REQUIRE(m.row_count() == 2);
  m.handle_key("y");
  REQUIRE(bulk.size() == 1);
  REQUIRE(bulk[0].size() == 2);
  REQUIRE(bulk[0][0].path ==
          "http://gieres.test/broadcasts/280905/audio.mp3");
  REQUIRE(bulk[0][1].path ==
          "http://gieres.test/broadcasts/282596/audio.mp3");
}

TEST_CASE("d opens the date index and enter fetches a day", "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();
  fake.day = GieresDay{"2021-11-19", {fixture_listing().broadcasts[0]}};
  GieresModel m = fake.make();
  std::vector<Track> played;
  m.set_actions(GieresActions{
      .on_play_track =
          [&played](const Track& t) { played.push_back(t); },
      .on_append_track  = {},
      .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity
  pump_until_done(m);
  m.handle_key("d");
  REQUIRE(m.org_view() == GieresModel::OrgView::Dates);
  REQUIRE(m.row_count() == 3);  // dates_ rode the /orgonity.json fetch
  m.handle_key("down");
  m.handle_key("down");  // → "2021-11-19"
  m.handle_key("enter");
  REQUIRE(m.org_view() == GieresModel::OrgView::Day);
  pump_until_done(m);
  REQUIRE(fake.day_calls == 1);
  REQUIRE(fake.day_args[0] == "2021-11-19");
  REQUIRE(m.row_count() == 1);
  m.handle_key("enter");
  REQUIRE(played.size() == 1);
  REQUIRE(played[0].path == "http://gieres.test/broadcasts/1900/audio.mp3");
  // esc walks back: Day → Dates → List
  m.handle_key("esc");
  REQUIRE(m.org_view() == GieresModel::OrgView::Dates);
  m.handle_key("esc");
  REQUIRE(m.org_view() == GieresModel::OrgView::List);
}

TEST_CASE("echelon t expands the program listing; enter plays the segment",
          "[gieres][model]") {
  FakeArchive fake;
  fake.segments = fixture_segments();
  GieresModel m = fake.make();
  std::vector<Track> played;
  m.set_actions(GieresActions{
      .on_play_track =
          [&played](const Track& t) { played.push_back(t); },
      .on_append_track  = {},
      .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity
  pump_until_done(m);
  m.handle_key("tab");  // → Echelon
  pump_until_done(m);
  REQUIRE(fake.seg_calls == 1);
  REQUIRE(m.row_count() == 2);
  m.handle_key("t");  // expand segment 0 (2 programs → 4 flat rows)
  REQUIRE(m.row_count() == 4);
  REQUIRE(m.row_label(1, 0).find("00:00") != std::string::npos);
  REQUIRE(m.row_label(2, 0).find("03:00") != std::string::npos);
  m.handle_key("down");
  m.handle_key("enter");  // program row → its segment (finite URL, seekable)
  REQUIRE(played.size() == 1);
  REQUIRE(played[0].path ==
          "http://gieres.test/broadcasts/280905/audio.mp3");
  // left/right are never consumed — they wind the playing track via the
  // global player table (seek ±5s).
  REQUIRE_FALSE(m.handle_key("left"));
  REQUIRE_FALSE(m.handle_key("right"));
  // esc at the top level closes the screen.
  m.handle_key("esc");
  REQUIRE_FALSE(m.visible());
}

TEST_CASE("echelon d opens the date index; enter fetches that day",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing  = fixture_listing();  // dates_ ride the /orgonity.json fetch
  fake.segments = fixture_segments();
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity — this loads dates_
  pump_until_done(m);
  m.handle_key("tab");  // → Echelon
  pump_until_done(m);
  const int base_seg_calls = fake.seg_calls;

  m.handle_key("d");
  REQUIRE(m.row_count() == 3);  // same date index as Orgonity
  m.handle_key("down");
  m.handle_key("down");  // → "2021-11-19"
  m.handle_key("enter");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == base_seg_calls + 1);  // a fresh day-window fetch
  REQUIRE(m.row_count() == 2);                    // back to segments

  // esc closes the index without closing the screen.
  m.handle_key("d");
  REQUIRE(m.row_count() == 3);
  m.handle_key("esc");
  REQUIRE(m.visible());
  REQUIRE(m.row_count() == 2);
}

// The reported bug: `d` in Echelon with dates_ never loaded (no Orgonity
// visit) showed only the dim hint row. The toggle now self-loads the index
// with one page-1 /orgonity.json fetch (recording_dates is complete on every
// page) on its own thread, so the shared fetch lifecycle stays untouched.
TEST_CASE("echelon d self-loads the date index without an Orgonity visit",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing  = fixture_listing();
  fake.segments = fixture_segments();
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);         // settle the Live fetch first
  m.handle_key("shift+tab");  // Live → Echelon — Orgonity is never opened
  pump_until_done(m);         // the Echelon day (segments) fetch
  REQUIRE(fake.seg_calls == 1);
  REQUIRE(fake.org_calls == 0);  // the bug's precondition: dates_ is empty
  const int base_seg_calls = fake.seg_calls;

  m.handle_key("d");           // the date index self-loads now
  REQUIRE_FALSE(m.loading());  // the dates fetch runs on its own thread
  pump_until_dates(m);
  REQUIRE(fake.org_calls == 1);  // exactly one page-1 fetch
  REQUIRE(std::get<0>(fake.org_args.back()) == 1);
  REQUIRE(std::get<1>(fake.org_args.back()) == "");  // no query
  REQUIRE(std::get<2>(fake.org_args.back()) == "date");
  REQUIRE(m.row_count() == 3);  // the hint row became the date index
  REQUIRE(m.row_label(0, 0) == "> 2010-04-13");
  REQUIRE_FALSE(m.row_dim(0));

  // dates_ is loaded now: retoggling neither refetches nor reshapes anything.
  m.handle_key("esc");
  REQUIRE(m.row_count() == 2);  // back to the segment list
  m.handle_key("d");
  REQUIRE(m.row_count() == 3);
  REQUIRE(fake.org_calls == 1);
  // enter on a date still selects the shown Echelon day (segments fetch).
  m.handle_key("enter");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == base_seg_calls + 1);
  REQUIRE(m.row_count() == 2);
}

TEST_CASE("maybe_load_more appends the next page near the bottom",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();  // page 2 / total 34, 2 broadcasts
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity (page 2 fetch kicks)
  pump_until_done(m);
  REQUIRE(fake.org_calls == 1);
  REQUIRE(m.row_count() == 2);
  // Jump to the bottom: cursor within kNearBottom → page 3 fetch.
  m.handle_key("end");
  pump_until_done(m);
  REQUIRE(fake.org_calls == 2);
  REQUIRE(std::get<0>(fake.org_args.back()) == 3);  // page 3 requested
  REQUIRE(m.row_count() == 4);  // the lazy page merged, not replaced
  // A second end press: page_ (2) < total_pages_ (34) again, but the cursor
  // is at the bottom of 4 rows → another lazy page (4).
  m.handle_key("end");
  pump_until_done(m);
  REQUIRE(fake.org_calls == 3);
  REQUIRE(std::get<0>(fake.org_args.back()) == 4);
  REQUIRE(m.row_count() == 6);
}

TEST_CASE("search prompt sanitizes and refetches; p and space fall through",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity
  pump_until_done(m);
  const int base_calls = fake.org_calls;

  m.handle_key("/");
  REQUIRE(fake.org_calls == base_calls);  // no fetch until submit
  // Typing swallows everything — including 'p' and 'space' while active.
  REQUIRE(m.handle_key("M"));
  REQUIRE(m.handle_key("o"));
  REQUIRE(m.handle_key("s"));
  REQUIRE(m.handle_key("t"));
  REQUIRE(m.handle_key("#"));
  REQUIRE(m.handle_key("2"));
  REQUIRE(m.handle_key("enter"));
  pump_until_done(m);
  REQUIRE(fake.org_calls == base_calls + 1);
  REQUIRE(std::get<1>(fake.org_args.back()) == "Most2");  // sanitized
  REQUIRE(std::get<2>(fake.org_args.back()) == "date");

  // Not typing anymore: p / space are the app-owned fallthroughs.
  REQUIRE_FALSE(m.handle_key("p"));
  REQUIRE_FALSE(m.handle_key("space"));
  // ctrl+r refetches the current view.
  const int before_r = fake.org_calls;
  REQUIRE(m.handle_key("ctrl+r"));
  pump_until_done(m);
  REQUIRE(fake.org_calls == before_r + 1);
}

TEST_CASE("volume keys fall through; typing keeps them in the buffer",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity
  pump_until_done(m);

  // Not typing: the global volume keys (- / = / +, main.cpp volume_step) fall
  // through like p and space, and so do the other global player keys.
  REQUIRE_FALSE(m.handle_key("-"));
  REQUIRE_FALSE(m.handle_key("="));
  REQUIRE_FALSE(m.handle_key("+"));
  REQUIRE_FALSE(m.handle_key("n"));
  REQUIRE_FALSE(m.handle_key("r"));
  REQUIRE_FALSE(m.handle_key("z"));
  REQUIRE_FALSE(m.handle_key("x"));

  // The screen's own keys stay consumed on Orgonity: s sorts (refetches),
  // d toggles the date index.
  REQUIRE(m.handle_key("s"));
  pump_until_done(m);
  REQUIRE(m.handle_key("d"));
  m.handle_key("esc");  // date index → list

  // While the search prompt is open '-' types into the query buffer instead
  // ('-' is a legal query char); esc closes the prompt and restores the
  // fallthrough.
  m.handle_key("/");
  REQUIRE(m.handle_key("-"));
  m.handle_key("esc");
  REQUIRE_FALSE(m.handle_key("-"));
}

TEST_CASE("unhandled keys fall through; screen keys stay consumed (Live)",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing = fixture_listing();
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);  // settle Live first

  // Global player keys are not the screen's: they fall through to the global
  // table (skip/repeat/shuffle/stop/mono/speed/queue/remove — and the
  // view-scoped s/d/t on a view where they do nothing).
  REQUIRE_FALSE(m.handle_key("n"));
  REQUIRE_FALSE(m.handle_key("r"));  // global repeat; ctrl+r is the refetch
  REQUIRE_FALSE(m.handle_key("z"));
  REQUIRE_FALSE(m.handle_key("s"));  // global stop — sort is Orgonity-only
  REQUIRE_FALSE(m.handle_key("x"));
  REQUIRE_FALSE(m.handle_key("m"));
  REQUIRE_FALSE(m.handle_key("["));
  REQUIRE_FALSE(m.handle_key("]"));
  REQUIRE_FALSE(m.handle_key("d"));  // device picker — Orgonity-only
  REQUIRE_FALSE(m.handle_key("t"));  // program listing — Echelon-only
  REQUIRE_FALSE(m.handle_key("left"));   // global seek ±5s — never consumed
  REQUIRE_FALSE(m.handle_key("right"));  // global seek ±5s — never consumed

  // The screen's own keys stay consumed.
  REQUIRE(m.handle_key("j"));  // cursor down
  REQUIRE(m.handle_key("k"));  // cursor up
  REQUIRE(m.handle_key("enter"));  // plays the live stream (null actions)
  REQUIRE(m.handle_key("tab"));    // → Orgonity (first fetch fired)
  pump_until_done(m);
  REQUIRE(m.handle_key("ctrl+r"));  // refetch the current view
  pump_until_done(m);
  // esc walks back a level and finally closes the screen (consumed).
  REQUIRE(m.handle_key("esc"));
  REQUIRE_FALSE(m.visible());
}