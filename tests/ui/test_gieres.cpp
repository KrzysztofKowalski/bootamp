// tests/ui/test_gieres.cpp — GieresModel + client parser tests.
//
// The parsers run against captured JSON fixtures (docs/orgonity-api.md
// shapes); the screen model runs with injected fake fetch hooks (no network,
// no FTXUI) and asserts the playback mapping — full URLs joined with the
// test base, stream tracks for recordings/segments, realtime for live.
#include "ui/screens/gieres.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace bootamp::ui;
using namespace bootamp::ui::screens;
using bootamp::playlist::Track;

namespace {

// iso_days renders a sys_days as YYYY-MM-DD (the model's date_string twin).
std::string iso_days(std::chrono::sys_days d) {
  const std::chrono::year_month_day ymd{d};
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                static_cast<unsigned>(ymd.day()));
  return buf;
}

// parse_iso / format_iso convert the server's "YYYY-MM-DDTHH:MM:SSZ" shape —
// the chain tests stamp the fake's aired_at values with the requested window
// so the segment chronology holds across days.
std::chrono::sys_seconds parse_iso(const std::string& s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (s.size() != 20 ||
      std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi,
                  &sec) != 6) {
    return std::chrono::sys_seconds{};
  }
  const std::chrono::year_month_day ymd{
      std::chrono::year{y} / std::chrono::month{static_cast<unsigned>(mo)} /
      std::chrono::day{static_cast<unsigned>(d)}};
  return std::chrono::sys_seconds{std::chrono::sys_days{ymd}} +
         std::chrono::hours{h} + std::chrono::minutes{mi} +
         std::chrono::seconds{sec};
}

std::string format_iso(std::chrono::sys_seconds tp) {
  const auto day = std::chrono::floor<std::chrono::days>(tp);
  const auto tod = tp - day;
  const int h = static_cast<int>(
      std::chrono::duration_cast<std::chrono::hours>(tod).count());
  const int mi = static_cast<int>(
                     std::chrono::duration_cast<std::chrono::minutes>(tod)
                         .count()) %
                 60;
  const int sec = static_cast<int>(tod.count()) % 60;
  const std::chrono::year_month_day ymd{day};
  char buf[48];  // the real shape is 21 bytes; sized for GCC's int-range bound
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()),
                static_cast<unsigned>(ymd.day()), h, mi, sec);
  return buf;
}

// iso_days_back is n days before the model's "today" — the model and the
// test share gieres_local_today() as the single source of truth, so the
// expected calendar is exact regardless of the machine's timezone or the
// hour the suite runs at.
std::string iso_days_back(const int n) {
  int y = 0, m = 0, d = 0;
  std::sscanf(gieres_local_today().c_str(), "%d-%d-%d", &y, &m, &d);
  const std::chrono::year_month_day ymd{
      std::chrono::year{y} / std::chrono::month{static_cast<unsigned>(m)} /
      std::chrono::day{static_cast<unsigned>(d)}};
  return iso_days(std::chrono::sys_days{ymd} - std::chrono::days{n});
}

// kPumpBudget bounds every wait loop below. It is deliberately large: the
// suite runs ~8 binaries at once (-j8 in scripts/test.sh), and a machine
// oversubscribed with build/test processes can take a while to schedule a
// freshly-spawned fetch thread for its first run slice — a tight budget on a
// loading()-only wait then flakes (the fetch never visibly lands before the
// helper gives up, and the keyed stale-drop in pump() never retries it).
constexpr int kPumpBudget = 100000;

// pump_until_done spins pump() until the background fetch landed (the
// test_screens.cpp lazy-catalog pattern: yield until loading clears, then one
// final pump in case the result landed between checks).
void pump_until_done(GieresModel& m) {
  for (int i = 0; i < kPumpBudget && m.loading(); ++i) {
    m.pump();
    std::this_thread::yield();
  }
  m.pump();
}

// pump_until spins pump() until an observable, already-applied model state
// holds (e.g. the Orgonity List rows or the Echelon segment rows that a
// landed fetch produced) — the same yield-until-visible pattern as below.
// Waiting on a visible signal instead of raw loading() makes a test robust to
// fetch-thread scheduling delays AND to a stale drop (loading() would clear
// while the requested view never loads); the final pump covers the result
// that landed between the check and the exit.
template <typename Pred>
void pump_until(GieresModel& m, Pred pred) {
  for (int i = 0; i < kPumpBudget && !pred(); ++i) {
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
  for (int i = 0; i < kPumpBudget && m.row_count() != 3; ++i) {
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
// results; `fail` switches each hook to an error return. coverage_from is
// the timeline's first covered WINDOW START (an ISO instant — the calendar
// bisection probes the day windows, which compare chronologically as
// strings): requested windows below it return no segments. Empty = every
// window is covered (the all-or-nothing fake the other tests use).
struct FakeArchive {
  GieresListing listing;
  GieresDay     day;
  std::vector<GieresSegment> segments;
  std::string   now_playing = "Radio Radio ..::.. Pink Floyd \"Hey You\"";
  std::string   coverage_from;
  // extra_when_calls_gt appends an extra slice (id 999999) to a covered
  // window response once seg_calls exceeds it — the chain's live-edge test
  // sets it to the call count reached so far, so only the refresh (the next
  // call) sees the timeline growing. Negative = never.
  int           extra_when_calls_gt = -1;
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
    auto segs = [this](std::string_view start, std::string_view) {
      ++seg_calls;
      if (fail) {
        return std::expected<std::vector<GieresSegment>, std::string>(
            std::unexpected("gieres: connection refused"));
      }
      // The calendar bisection probes the local day's first hour; window
      // starts below the timeline's first day are dark (the real server has
      // no segments there either). ISO instants of fixed width compare
      // chronologically as strings.
      if (!coverage_from.empty() && std::string{start} < coverage_from) {
        return std::expected<std::vector<GieresSegment>, std::string>(
            std::vector<GieresSegment>{});
      }
      // The canned segments stamped with the REQUESTED window start
      // (30-minute slices from it), so the chain's aired_at chronology holds
      // across days and the live-edge refresh filters the played prefix.
      auto out = segments;
      if (extra_when_calls_gt >= 0 && seg_calls > extra_when_calls_gt &&
          !out.empty()) {
        GieresSegment extra = out.back();
        extra.id  = 999999;
        extra.url = "/broadcasts/999999/audio.mp3";
        out.push_back(std::move(extra));
      }
      const auto win = parse_iso(std::string{start});
      for (std::size_t i = 0; i < out.size(); ++i) {
        out[i].aired_at =
            format_iso(win + std::chrono::minutes{30} *
                                 static_cast<long long>(i));
      }
      return std::expected<std::vector<GieresSegment>, std::string>(out);
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
  // ł (non-ASCII) dropped → z-<drop>-o-d-z-i-e-j = "zodziej" (the second z is
  // ASCII and survives; docs/orgonity-api.md §7: q is cleaned to
  // [A-Za-z0-9 _-]).
  REQUIRE(GieresClient::sanitize_query("złodziej") == "zodziej");
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
  // Wait for the two recording rows to be VISIBLE (drained into the model),
  // not just for loading() to clear: the latter can clear on a stale drop
  // that never populates this view (a -j8 scheduling delay can make the tab
  // land while the Live fetch is still in flight, skipping the Orgonity
  // fetch and leaving recordings_/dates_ empty forever).
  pump_until(m, [&] { return m.row_count() == 2; });
  m.handle_key("y");
  REQUIRE(appended.size() == 1);
  REQUIRE(appended[0].path == "http://gieres.test/broadcasts/1900/audio.mp3");

  // → Echelon; y appends every segment so the queue chains them gapless.
  m.handle_key("tab");
  // The Echelon segment rows are the visible signal that the segmented day
  // fetch landed (fake.seg_calls becomes 1 only in that worker thread).
  pump_until(m, [&] { return m.row_count() == 2; });
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
  // Wait for the recording rows to be visible before pressing d. This is the
  // crux of the flake this tested: with loading() still true at the tab (the
  // Live fetch scheduled late under -j8) the Orgonity fetch is skipped, "d"
  // then sees dates_ empty and — because it also skips its fetch while
  // loading() — the in-flight listing lands as stale and is dropped, so the
  // date index stays at row_count 0 forever.
  pump_until(m, [&] { return m.row_count() == 2; });
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
  // esc walks back a level and finally closes the screen: the program
  // listing is still expanded (enter played a program, it did not collapse),
  // so the first esc collapses it and the second closes the screen.
  m.handle_key("esc");
  REQUIRE(m.visible());
  REQUIRE(m.row_count() == 2);  // the expansion collapsed back to the segment rows
  m.handle_key("esc");
  REQUIRE_FALSE(m.visible());
}

TEST_CASE("echelon d opens the timeline calendar; enter fetches that day",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing  = fixture_listing();  // dates_ ride the /orgonity.json fetch
  fake.segments = fixture_segments();
  fake.coverage_from =
      gieres_day_window(iso_days_back(2)).first;  // the last 3 local days
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);   // settle Live first
  m.handle_key("tab");  // → Orgonity — this loads dates_ (recording_dates)
  pump_until_done(m);
  m.handle_key("tab");  // → Echelon
  pump_until_done(m);
  const int org_calls_after_orgonity = fake.org_calls;

  m.handle_key("d");
  pump_until_dates(m);
  // The calendar is not orgonity data: no extra /orgonity.json call fired.
  REQUIRE(fake.org_calls == org_calls_after_orgonity);
  REQUIRE(m.row_count() == 3);  // the calendar: [t-2, t-1, t]
  REQUIRE(m.row_label(2, 0) == "> " + gieres_local_today());  // the shown day
  REQUIRE(m.row_label(0, 0) == "  " + iso_days_back(2));
  m.handle_key("up");
  m.handle_key("up");  // → the timeline's first day
  const int loaded_seg_calls = fake.seg_calls;
  m.handle_key("enter");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == loaded_seg_calls + 1);  // a fresh day-window fetch
  REQUIRE(m.row_count() == 2);                      // back to segments

  // esc closes the index without closing the screen.
  m.handle_key("d");
  REQUIRE(m.row_count() == 3);
  m.handle_key("esc");
  REQUIRE(m.visible());
  REQUIRE(m.row_count() == 2);
}

// The reported bug: the Echelon date index was the orgonity recording_dates
// subset (days with an uploaded recording), so days the timeline covers with
// no recording — and today, until the show is archived — were missing. The
// index is now the timeline's own calendar: fetch_dates bisects the timeline
// (1-hour /echelon/segments window probes) for its first day and emits every
// day through today; pressing d in Echelon never touches the orgonity hook.
TEST_CASE("echelon d self-loads the timeline calendar without an Orgonity visit",
          "[gieres][model]") {
  FakeArchive fake;
  fake.listing  = fixture_listing();
  fake.segments = fixture_segments();
  fake.coverage_from = gieres_day_window(iso_days_back(2)).first;
  GieresModel m = fake.make();
  m.set_actions(GieresActions{.on_play_track = {},
                              .on_append_track  = {},
                              .on_append_tracks = {}});
  m.open();
  pump_until_done(m);         // settle the Live fetch first
  m.handle_key("shift+tab");  // Live → Echelon — Orgonity is never opened
  pump_until_done(m);         // the Echelon day (segments) fetch
  REQUIRE(fake.seg_calls == 1);
  REQUIRE(fake.org_calls == 0);  // the bug's precondition: no orgonity data

  m.handle_key("d");           // the timeline calendar self-loads now
  REQUIRE_FALSE(m.loading());  // the dates fetch runs on its own thread
  pump_until_dates(m);
  REQUIRE(fake.org_calls == 0);
  REQUIRE(m.row_count() == 3);  // the hint row became the timeline calendar
  REQUIRE(m.row_label(2, 0) == "> " + gieres_local_today());  // the shown day
  REQUIRE(m.row_label(0, 0) == "  " + iso_days_back(2));
  REQUIRE_FALSE(m.row_dim(0));

  // The calendar is loaded: retoggling neither refetches nor reshapes.
  m.handle_key("esc");
  REQUIRE(m.row_count() == 2);  // back to the segment list
  m.handle_key("d");
  REQUIRE(m.row_count() == 3);
  const int loaded_seg_calls = fake.seg_calls;
  // enter on a day still selects the shown Echelon day (segments fetch).
  m.handle_key("enter");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == loaded_seg_calls + 1);
  REQUIRE(m.row_count() == 2);

  // ';' steps back a day (a day-window fetch), "'" steps forward, and at
  // today "'" is clamped — the timeline has no future to fetch.
  const int stepped_seg_calls = fake.seg_calls;
  m.handle_key(";");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == stepped_seg_calls + 1);
  REQUIRE(m.row_count() == 2);
  m.handle_key("'");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == stepped_seg_calls + 2);
  m.handle_key("'");
  pump_until_done(m);
  REQUIRE(fake.seg_calls == stepped_seg_calls + 2);  // clamped at today
}

// The local-day window: Europe/Warsaw fetches 2026-09-12 as
// "2026-09-11T22:00:00Z"…"2026-09-12T22:00:00Z", so a 2-AM local show
// (00:00Z) lands on its own day at its own wall-clock time — the window
// must be exactly 24h starting at the local midnight.
TEST_CASE("gieres_day_window is the local day's 24h UTC midnight pair",
          "[gieres][model]") {
  const auto w = gieres_day_window("2026-09-12");
  REQUIRE(w.first.size() == 20);
  REQUIRE(w.first.back() == 'Z');
  REQUIRE(w.second.size() == 20);
  REQUIRE(w.second.back() == 'Z');
  REQUIRE(gieres_day_window("nonsense").first.empty());
  REQUIRE(gieres_local_today().size() == 10);

  const auto secs = [](std::string_view s) -> long long {
    if (s.size() != 20) {
      return -1;
    }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(std::string{s}.c_str(), "%d-%d-%dT%d:%d:%d",
                    &y, &mo, &d, &h, &mi, &sec) != 6) {
      return -1;
    }
    const std::chrono::year_month_day ymd{
        std::chrono::year{y} / std::chrono::month{static_cast<unsigned>(mo)} /
        std::chrono::day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) {
      return -1;
    }
    return static_cast<long long>(
               std::chrono::sys_days{ymd}.time_since_epoch().count()) *
               86400LL +
           h * 3600LL + mi * 60LL + sec;
  };
  const long long a = secs(w.first);
  const long long b = secs(w.second);
  REQUIRE(a >= 0);
  REQUIRE(b - a == 86400);
  // The window starts at the day's local midnight (with a tz database;
  // the no-tzdb fallback is the plain UTC midnight).
  const auto* zone = [] -> const std::chrono::time_zone* {
    try {
      return std::chrono::current_zone();
    } catch (const std::exception&) {
      return nullptr;
    }
  }();
  if (zone) {
    const auto lt =
        zone->to_local(std::chrono::sys_seconds{std::chrono::seconds{a}});
    const auto day = std::chrono::floor<std::chrono::days>(lt);
    REQUIRE(std::chrono::year_month_day{std::chrono::local_days{day}} ==
            std::chrono::year{2026} / 9 / 12);
    const auto tod = std::chrono::hh_mm_ss{lt - day};
    REQUIRE(tod.hours().count() == 0);
    REQUIRE(tod.minutes().count() == 0);
    REQUIRE(tod.seconds().count() == 0);
  } else {
    REQUIRE(w.first == "2026-09-12T00:00:00Z");
    REQUIRE(w.second == "2026-09-13T00:00:00Z");
  }
}

// ---------------------------------------------------------------------------
// Echelon auto-continue chain (the user's spec: a finished segment rolls to
// the next one, a day boundary fetches the next day's first, and today's
// last segment gets ONE refresh — nothing new stops the chain)
// ---------------------------------------------------------------------------

// resolve_chain drives the watchdog pattern: the chain resolves through
// repeated echelon_resolve_end calls (each watchdog tick drains a landed
// chain fetch), so the test loops the same way until a non-Async outcome.
GieresModel::EchelonEnd resolve_chain(GieresModel& m, const std::string& path,
                                      Track* out) {
  auto r = GieresModel::EchelonEnd::Async;
  for (int i = 0; i < kPumpBudget; ++i) {
    r = m.echelon_resolve_end(path, out);
    if (r != GieresModel::EchelonEnd::Async) {
      return r;
    }
    std::this_thread::yield();
  }
  return r;
}

TEST_CASE("echelon chain rolls the day's segments and stops at the live edge",
          "[gieres][model]") {
  FakeArchive fake;
  fake.segments = fixture_segments();
  GieresModel m = fake.make();
  std::vector<Track> played;
  m.set_actions(GieresActions{
      .on_play_track    = [&](const Track& t) { played.push_back(t); },
      .on_append_track  = {},
      .on_append_tracks = {}});
  m.open();
  pump_until_done(m);          // the Live fetch
  m.handle_key("shift+tab");   // → Echelon — the today window fetch
  pump_until_done(m);
  REQUIRE(fake.seg_calls == 1);
  m.handle_key("enter");       // segment 0 plays and arms the chain
  REQUIRE(played.size() == 1);

  // A natural end rolls to the day's next segment — no fetch involved.
  Track out;
  REQUIRE(m.echelon_resolve_end(played.back().path, &out) ==
          GieresModel::EchelonEnd::Track);
  REQUIRE(out.path ==
          "http://gieres.test/broadcasts/282596/audio.mp3");  // segment 1
  // Segment 1 is the day's last: the live edge. One refresh of today's
  // window brings nothing new (the fake repeats the same two slices) — the
  // chain stops; afterwards the resolve is inert and a foreign track never
  // rolls anything.
  const int seg_calls_before = fake.seg_calls;
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Async);
  // The live-edge refresh runs on the chain's OWN thread (the watchdog
  // pattern), so the spawned fetch lands asynchronously — wait for it to
  // actually fire before counting (resolve_chain's drain loop below yields,
  // but the count must be observed before the result is consumed).
  for (int i = 0; i < kPumpBudget && fake.seg_calls == seg_calls_before; ++i) {
    std::this_thread::yield();
  }
  REQUIRE(fake.seg_calls == seg_calls_before + 1);  // the refresh spawned
  REQUIRE(resolve_chain(m, out.path, &out) ==
          GieresModel::EchelonEnd::Stopped);
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Inactive);
  REQUIRE(m.echelon_resolve_end("http://gieres.test/other.mp3", &out) ==
          GieresModel::EchelonEnd::Inactive);
}

TEST_CASE("echelon chain crosses day boundaries to the next day's first",
          "[gieres][model]") {
  FakeArchive fake;
  fake.segments      = fixture_segments();
  fake.coverage_from = gieres_day_window(iso_days_back(2)).first;
  GieresModel m = fake.make();
  std::vector<Track> played;
  m.set_actions(GieresActions{
      .on_play_track    = [&](const Track& t) { played.push_back(t); },
      .on_append_track  = {},
      .on_append_tracks = {}});
  m.open();
  pump_until_done(m);
  m.handle_key("shift+tab");   // → Echelon
  pump_until_done(m);
  m.handle_key("d");           // the calendar [t-2, t-1, t], cursor on today
  pump_until_dates(m);
  m.handle_key("up");
  m.handle_key("up");          // → t-2
  m.handle_key("enter");       // the t-2 day fetch
  pump_until_done(m);
  m.handle_key("enter");       // segment 0 of t-2 → the chain armed
  REQUIRE(played.size() == 1);

  Track out;
  // t-2's segment 1 (same day, immediate).
  REQUIRE(m.echelon_resolve_end(played.back().path, &out) ==
          GieresModel::EchelonEnd::Track);
  REQUIRE(out.path == "http://gieres.test/broadcasts/282596/audio.mp3");
  // t-2's day end → the t-1 fetch resolves to its first segment.
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Async);
  REQUIRE(resolve_chain(m, out.path, &out) == GieresModel::EchelonEnd::Track);
  REQUIRE(out.path == "http://gieres.test/broadcasts/280905/audio.mp3");
  // t-1's segment 1 (same day) → t's first segment (another boundary).
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Track);
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Async);
  REQUIRE(resolve_chain(m, out.path, &out) == GieresModel::EchelonEnd::Track);
  // t's FIRST segment (the same "next day's first" roll as the t-2→t-1
  // boundary above — the earlier "282596" here was a copy-paste of the
  // same-day roll's expectation).
  REQUIRE(out.path == "http://gieres.test/broadcasts/280905/audio.mp3");
  // t's segment 1 is the day's last: the live edge — one refresh, nothing
  // new (the fake repeats the same slices) → the chain stops.
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Track);
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Async);
  REQUIRE(resolve_chain(m, out.path, &out) ==
          GieresModel::EchelonEnd::Stopped);
}

TEST_CASE("echelon chain plays a segment that appears at the live edge",
          "[gieres][model]") {
  FakeArchive fake;
  fake.segments = fixture_segments();
  GieresModel m = fake.make();
  std::vector<Track> played;
  m.set_actions(GieresActions{
      .on_play_track    = [&](const Track& t) { played.push_back(t); },
      .on_append_track  = {},
      .on_append_tracks = {}});
  m.open();
  pump_until_done(m);
  m.handle_key("shift+tab");   // → Echelon
  pump_until_done(m);
  m.handle_key("down");        // → segment 1 (the day's last)
  m.handle_key("enter");
  REQUIRE(played.size() == 1);
  // The timeline grows before the refresh lands: the fake starts serving an
  // extra slice from the next window call on.
  fake.extra_when_calls_gt = fake.seg_calls;
  Track out;
  REQUIRE(m.echelon_resolve_end(played.back().path, &out) ==
          GieresModel::EchelonEnd::Async);
  // The previous resolve was Async and left *out untouched — out.path is
  // still the default "". resolve_chain must be fed the path of the track
  // that just finished (the chain's armed chain_track_), or the very first
  // echelon_resolve_end inside the loop bails into EchelonEnd::Inactive on
  // the "" != chain_track_ mismatch.
  REQUIRE(resolve_chain(m, played.back().path, &out) ==
          GieresModel::EchelonEnd::Track);
  REQUIRE(out.path == "http://gieres.test/broadcasts/999999/audio.mp3");
  // The chain is at the new last slice: one more refresh — still nothing
  // newer → stop.
  REQUIRE(m.echelon_resolve_end(out.path, &out) ==
          GieresModel::EchelonEnd::Async);
  REQUIRE(resolve_chain(m, out.path, &out) ==
          GieresModel::EchelonEnd::Stopped);
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