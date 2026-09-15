// tests/ui/test_yt_screen.cpp — YtModel YouTube-browser screen tests
// (ui/screens/yt.hpp).
//
// The model is exercised with injected fake hooks (no network, no FTXUI): the
// resolve hook records the EXACT target string a fetch would hand to yt-dlp,
// which pins the regression this file exists for — the Search target must read
// "ytsearch<Count>:<query>" (Go keys.go "ytsearch10:" spelling: the count sits
// right after the key with NO separating colon, because yt-dlp's search
// extractor regex {_SEARCH_KEY}(?P<prefix>|[1-9][0-9]*):query parses a
// "ytsearch:20:lofi beats" string as a search for the literal text
// "20:lofi beats" — empty/junk results). A structural guard (no "ytsearch:"
// substring in the target) keeps the spelling right whatever kSearchCount is.
//
// Coverage: search-target spelling, playlist/feed raw-URL pass-through, the
// zero-tracks and resolve-error paths, the on-disk list cache (hit /
// miss-store / invalid-payload refetch / ctrl+r force-refetch), the stale-drop
// refetch, per-view park/restore on tab cycling, enter/a playback mapping, the
// modal prompt, and the resume roundtrip. All fetch waits use the
// yield-until-visible pattern — wait on row_count / footer text, never just
// loading() (the -j8 scheduler-starvation lesson from the gieres suite).
#include "ui/screens/yt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace bootamp::ui;
using namespace bootamp::ui::screens;
using bootamp::playlist::Track;

namespace {

// kPumpBudget bounds every wait loop below (the gieres tests' number —
// deliberately large: -j8 oversubscription can delay a freshly-spawned fetch
// thread for a long first slice).
constexpr int kPumpBudget = 100000;

// pump_until spins pump() until an observable, already-applied model state
// holds (the gieres pump_until pattern: waiting on a VISIBLE signal like
// row_count instead of raw loading() keeps a test robust to fetch-thread
// scheduling delays AND to a stale drop). The final pump covers a result that
// landed between the last check and the exit.
template <typename Pred>
void pump_until(YtModel& m, Pred pred) {
  for (int i = 0; i < kPumpBudget && !pred(); ++i) {
    m.pump();
    std::this_thread::yield();
  }
  m.pump();
}

// pump_until_done spins pump() until loading() clears, then one final pump in
// case the result landed between the check and the exit.
void pump_until_done(YtModel& m) {
  for (int i = 0; i < kPumpBudget && m.loading(); ++i) {
    m.pump();
    std::this_thread::yield();
  }
  m.pump();
}

// yt_track builds a resolve-result Track (path = video URL, stream = true).
Track yt_track(std::string url, std::string title, std::string artist = {},
               int secs = 0) {
  Track t;
  t.path          = std::move(url);
  t.title         = std::move(title);
  t.artist        = std::move(artist);
  t.duration_secs = secs;
  t.stream        = true;
  return t;
}

// A valid on-disk cache payload for two tracks (parse_cached_tracks format:
// {"v":1,"tracks":[{path,title,artist,duration_secs,stream}, ...]}).
const std::string kCachedTwo = R"json({"v":1,"tracks":[
  {"path":"https://youtu.be/aaa","title":"Hit One","artist":"Artist A","duration_secs":181,"stream":true},
  {"path":"https://youtu.be/bbb","title":"Hit Two","duration_secs":0,"stream":false}
]})json";

// YtFake drives one YtModel with recording hooks (the gieres FakeArchive
// pattern). The hook writes happen on the fetch thread; reads after pump_until
// are safe because the worker's inbox_.store(release) publishes them to the
// acquirer. Declare the fake BEFORE the model so the hook's captured `this`
// outlives the model.
struct YtFake {
  // Resolve recording.
  int                       resolve_calls = 0;
  std::vector<std::string>  targets;                  // every target handed to resolve
  std::vector<Track>        tracks;                   // canned OK result
  std::string               err;                      // non-empty → resolve returns unexpected
  bool                      empty = false;            // resolve returns an empty (OK) list
  std::chrono::milliseconds delay{0};                  // resolve sleeps before replying

  // Cache recording.
  std::string               cached;                    // non-empty → cache_get returns it
  std::vector<std::string>  gotten_keys;
  std::vector<std::string>  put_keys;
  std::vector<std::string>  put_payloads;

  YtModel make() {
    auto resolve = [this](std::string_view target)
        -> std::expected<std::vector<Track>, std::string> {
      ++resolve_calls;
      targets.emplace_back(target);
      if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
      }
      if (!err.empty()) {
        return std::unexpected(err);
      }
      if (empty) {
        return std::vector<Track>{};
      }
      return tracks;
    };
    auto get = [this](std::string_view key) -> std::optional<std::string> {
      gotten_keys.emplace_back(key);
      if (cached.empty()) {
        return std::nullopt;
      }
      return std::optional<std::string>(cached);
    };
    auto put = [this](std::string_view key, std::string_view data, std::int64_t) {
      put_keys.emplace_back(key);
      put_payloads.emplace_back(data);
    };
    // YtModel is non-copyable/non-movable; the prvalue is copy-elided.
    return YtModel(std::move(resolve), std::move(get), std::move(put));
  }
};

// type_str feeds printable characters into the active prompt (" " mapped to the
// "space" key, the one printable the prompt handles specially).
void type_str(YtModel& m, std::string_view s) {
  for (const char c : s) {
    if (c == ' ') {
      m.handle_key("space");
    } else {
      m.handle_key(std::string(1, c));
    }
  }
}

// submit_query opens the prompt, types `query` and presses enter (submit).
void submit_query(YtModel& m, std::string query) {
  m.handle_key("/");
  type_str(m, query);
  m.handle_key("enter");
}

// join_tab builds a resume-tab payload with the record separator; the strings
// are concatenated deliberately (a "\x1e2" hex-escape literal would consume '2'
// as part of the escape and fail to compile).
std::string join_tab(std::initializer_list<std::string> parts) {
  std::string out;
  bool first = true;
  for (const std::string& p : parts) {
    if (!first) {
      out += '\x1e';
    }
    out += p;
    first = false;
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Search-target spelling (the format bug this file regresses)
// ---------------------------------------------------------------------------

TEST_CASE("yt screen: search fetch hands yt-dlp the canonical ytsearchN target",
          "[yt][model]") {
  YtFake fake;
  fake.tracks = {yt_track("https://youtu.be/aaa", "Alpha")};
  YtModel m    = fake.make();

  submit_query(m, "lofi beats");
  pump_until(m, [&] { return m.row_count() == 1; });

  REQUIRE(fake.resolve_calls == 1);
  REQUIRE(fake.targets.size() == 1);
  // Go keys.go:1470 builds "ytsearch10:" — the count sits immediately after
  // "ytsearch", NO separating colon. This is the exact spelling yt-dlp's
  // search extractor regex accepts; the old "ytsearch:20:lofi beats" form
  // parsed in yt-dlp as a search for the literal text "20:lofi beats".
  REQUIRE(fake.targets[0] == "ytsearch20:lofi beats");
  // Structural guards: the key+count segment must never contain "ytsearch:",
  // whatever kSearchCount is set to — this is what re-introducing the bug
  // would look like.
  REQUIRE(fake.targets[0].find("ytsearch:") == std::string::npos);
  REQUIRE(fake.targets[0] ==
          "ytsearch" + std::to_string(YtModel::kSearchCount) + ":lofi beats");

  REQUIRE(m.row_label(0, 0).find("Alpha") != std::string::npos);
  REQUIRE(m.footer_label().find("yt-dlp") != std::string::npos);
  REQUIRE_FALSE(m.loading());
}

TEST_CASE("yt screen: playlist and feed views pass the pasted URL verbatim",
          "[yt][model]") {
  YtFake fake;
  fake.tracks = {yt_track("https://youtu.be/p1", "P")};
  YtModel m    = fake.make();
  m.set_view(YtModel::View::Playlists);
  submit_query(m, "https://www.youtube.com/playlist?list=PLabc");
  pump_until(m, [&] { return m.row_count() == 1; });
  REQUIRE(fake.targets.size() == 1);
  REQUIRE(fake.targets[0] == "https://www.youtube.com/playlist?list=PLabc");

  YtFake f2;
  f2.tracks = {yt_track("https://youtu.be/f1", "F")};
  YtModel m2 = f2.make();
  m2.set_view(YtModel::View::Feeds);
  submit_query(m2, "https://www.youtube.com/@handle/videos");
  pump_until(m2, [&] { return m2.row_count() == 1; });
  REQUIRE(f2.targets.size() == 1);
  REQUIRE(f2.targets[0] == "https://www.youtube.com/@handle/videos");
}

// ---------------------------------------------------------------------------
// Resolve outcome paths
// ---------------------------------------------------------------------------

TEST_CASE("yt screen: a clean resolve that yields zero tracks is an error",
          "[yt][model]") {
  YtFake fake;
  fake.empty = true;
  YtModel m   = fake.make();
  submit_query(m, "nothing here");
  pump_until_done(m);
  REQUIRE(m.row_count() == 0);
  REQUIRE_FALSE(m.loading());
  REQUIRE(m.footer_label().find("resolved no tracks") != std::string::npos);
}

TEST_CASE("yt screen: a resolve error surfaces in the footer status",
          "[yt][model]") {
  YtFake fake;
  fake.err = "yt-dlp: ERROR: Sign in to confirm you're not a bot";
  YtModel m = fake.make();
  submit_query(m, "some query");
  pump_until_done(m);
  REQUIRE(m.row_count() == 0);
  REQUIRE_FALSE(m.loading());
  REQUIRE(m.footer_label().find("Sign in to confirm") != std::string::npos);
}

// ---------------------------------------------------------------------------
// On-disk list cache
// ---------------------------------------------------------------------------

TEST_CASE("yt screen: a cache hit serves the list without resolving",
          "[yt][model]") {
  YtFake fake;
  fake.cached = kCachedTwo;
  fake.tracks = {yt_track("https://youtu.be/zzz", "Should Never Resolve")};
  YtModel m    = fake.make();
  submit_query(m, "cached hit");
  pump_until(m, [&] { return m.row_count() == 2; });

  REQUIRE(fake.resolve_calls == 0);                      // the cache served it
  REQUIRE(fake.gotten_keys.size() == 1);
  REQUIRE(fake.gotten_keys[0].starts_with("yt-search/"));  // per-view namespace
  REQUIRE(m.footer_label().starts_with("cached list"));
  REQUIRE(m.row_label(0, 0).find("Hit One") != std::string::npos);
  REQUIRE(m.row_label(1, 0).find("Hit Two") != std::string::npos);
}

TEST_CASE("yt screen: a cache miss resolves and stores; garbage is refetched",
          "[yt][model]") {
  // Miss → resolve → the fresh list is stored under the view's namespace.
  YtFake miss;
  miss.tracks = {yt_track("https://youtu.be/a1", "Fresh")};
  YtModel m    = miss.make();
  submit_query(m, "miss");
  pump_until(m, [&] { return m.row_count() == 1; });
  REQUIRE(miss.resolve_calls == 1);
  REQUIRE(miss.put_keys.size() == 1);
  REQUIRE(miss.put_keys[0].starts_with("yt-search/"));
  REQUIRE(miss.put_payloads[0].find("\"v\":1") != std::string::npos);

  // A cache payload that does not parse must fall through to resolve.
  YtFake garbage;
  garbage.cached = "not json at all";
  garbage.tracks = {yt_track("https://youtu.be/a2", "Refetched")};
  YtModel mg      = garbage.make();
  submit_query(mg, "garbage");
  pump_until(mg, [&] { return mg.row_count() == 1; });
  REQUIRE(garbage.resolve_calls == 1);     // the read was attempted, then resolve
  REQUIRE(garbage.gotten_keys.size() == 1);
  REQUIRE(mg.row_label(0, 0).find("Refetched") != std::string::npos);
}

TEST_CASE("yt screen: ctrl+r force-refetches past the cache read",
          "[yt][model]") {
  YtFake fake;
  fake.cached = kCachedTwo;
  fake.tracks = {yt_track("https://youtu.be/zzz", "Fresh After Force")};
  YtModel m    = fake.make();
  submit_query(m, "force me");
  pump_until(m, [&] { return m.row_count() == 2; });  // served from cache
  REQUIRE(fake.resolve_calls == 0);

  fake.cached.clear();  // even if it were read, nothing to serve
  m.handle_key("ctrl+r");
  pump_until(m, [&] {
    return m.row_count() == 1 &&
           m.row_label(0, 0).find("Fresh After Force") != std::string::npos;
  });
  REQUIRE(m.loading() == false);
  REQUIRE(fake.resolve_calls == 1);
  // The first fetch above was served FROM the cache (reads once); ctrl+r's
  // forced fetch must skip the cache read, so the count stays at 1.
  REQUIRE(fake.gotten_keys.size() == 1);  // ctrl+r added no second read
  REQUIRE(fake.put_keys.size() == 1);  // fresh copy re-stored under the key
}

// ---------------------------------------------------------------------------
// Fetch lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("yt screen: a stale fetch is dropped and the pending target refetched",
          "[yt][model]") {
  int   calls = 0;
  bool  first = true;
  auto  resolve = [&](std::string_view) -> std::expected<std::vector<Track>,
                                                          std::string> {
    ++calls;
    if (first) {
      first = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(15));  // keep A in flight
      return std::vector<Track>{yt_track("https://youtu.be/alpha", "Alpha Row")};
    }
    return std::vector<Track>{yt_track("https://youtu.be/beta", "Beta Row")};
  };
  YtModel m(std::move(resolve));

  submit_query(m, "alpha");  // A launches (slow resolve)
  submit_query(m, "beta");   // B entered while A is in flight → single-flight skip
  // Whichever order the results land in, the SHOWN list must be B's: pump()
  // drops the stale A result and refetches the pending target (beta).
  pump_until(m, [&] {
    return m.row_count() == 1 &&
           m.row_label(0, 0).find("Beta Row") != std::string::npos;
  });
  REQUIRE_FALSE(m.loading());
  REQUIRE(calls == 2);  // A once, then B via the stale-drop refetch
  REQUIRE(m.header_label().find("beta") != std::string::npos);
}

TEST_CASE("yt screen: tab cycling parks and restores each view's list",
          "[yt][model]") {
  YtFake fake;
  fake.tracks = {yt_track("https://youtu.be/s1", "Search Row")};
  YtModel m    = fake.make();
  submit_query(m, "alpha");
  pump_until(m, [&] { return m.row_count() == 1; });

  fake.tracks = {yt_track("https://youtu.be/p1", "Playlist Row")};
  m.handle_key("tab");  // → Playlists (fresh view: empty until prompted)
  REQUIRE(m.view() == YtModel::View::Playlists);
  submit_query(m, "https://youtu.be/p1");
  pump_until(m, [&] { return m.row_count() == 1; });
  REQUIRE(m.row_label(0, 0).find("Playlist Row") != std::string::npos);

  fake.tracks = {yt_track("https://youtu.be/f1", "Feed Row")};
  m.handle_key("tab");  // → Feeds
  submit_query(m, "https://youtu.be/f1");
  pump_until(m, [&] { return m.row_count() == 1; });

  m.handle_key("tab");  // → back to Search: the parked list returns untouched
  REQUIRE(m.view() == YtModel::View::Search);
  REQUIRE(m.row_count() == 1);
  REQUIRE(m.row_label(0, 0).find("Search Row") != std::string::npos);
  REQUIRE(m.header_label().find("alpha") != std::string::npos);

  m.handle_key("tab");  // → Playlists: its parked list too
  REQUIRE(m.view() == YtModel::View::Playlists);
  REQUIRE(m.row_label(0, 0).find("Playlist Row") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Playback mapping + prompt
// ---------------------------------------------------------------------------

TEST_CASE("yt screen: enter plays and a appends the cursor track; nav wraps",
          "[yt][model]") {
  YtFake fake;
  fake.tracks = {
      yt_track("https://youtu.be/r1", "Row One"),
      yt_track("https://youtu.be/r2", "Row Two"),
      yt_track("https://youtu.be/r3", "Row Three"),
  };
  YtModel m = fake.make();
  submit_query(m, "rows");
  pump_until(m, [&] { return m.row_count() == 3; });

  std::vector<Track> played;
  std::vector<Track> appended;
  m.set_actions(YtActions{
      .on_play_track   = [&](const Track& t) { played.push_back(t); },
      .on_append_track  = [&](const Track& t) { appended.push_back(t); }});

  m.handle_key("down");  // cursor 1
  m.handle_key("down");  // cursor 2
  REQUIRE(m.cursor() == 2);
  REQUIRE(m.handle_key("enter"));
  REQUIRE(played.size() == 1);
  REQUIRE(played[0].path == "https://youtu.be/r3");
  REQUIRE(played[0].stream);
  REQUIRE(played[0].title == "Row Three");

  m.handle_key("a");
  REQUIRE(appended.size() == 1);
  REQUIRE(appended[0].path == "https://youtu.be/r3");

  // down past the end wraps to 0 (Go providerMoveUp wrap).
  m.handle_key("down");
  REQUIRE(m.cursor() == 0);
  m.handle_key("enter");
  REQUIRE(played.size() == 2);
  REQUIRE(played[1].path == "https://youtu.be/r1");
}

TEST_CASE("yt screen: the prompt is modal while typing, keys fall through after",
          "[yt][model]") {
  YtFake fake;
  YtModel m = fake.make();

  REQUIRE(m.handle_key("/"));
  REQUIRE(m.prompt_active());
  // While typing EVERY key is swallowed into the buffer — y/p/space included
  // (they only fall through to the host once the prompt is closed).
  REQUIRE(m.handle_key("y"));
  REQUIRE(m.handle_key("p"));
  REQUIRE(m.handle_key("space"));
  REQUIRE(m.handle_key("x"));
  REQUIRE(m.prompt_line(0) == "/ search: yp x");
  REQUIRE(m.handle_key("ctrl+u"));
  REQUIRE(m.prompt_line(0) == "/ search: ");
  REQUIRE(m.handle_key("esc"));
  REQUIRE_FALSE(m.prompt_active());

  // With the prompt closed, the host-owned keys fall through (not consumed).
  REQUIRE_FALSE(m.handle_key("y"));
  REQUIRE_FALSE(m.handle_key("p"));
  REQUIRE_FALSE(m.handle_key("space"));
  REQUIRE_FALSE(m.handle_key("-"));
}

// ---------------------------------------------------------------------------
// Resume + header/footer
// ---------------------------------------------------------------------------

TEST_CASE("yt screen: resume_tab / restore_tab roundtrip and clamp",
          "[yt][model]") {
  YtFake fake;
  fake.tracks = {yt_track("https://youtu.be/r1", "R1"),
                 yt_track("https://youtu.be/r2", "R2"),
                 yt_track("https://youtu.be/r3", "R3")};
  YtModel m = fake.make();
  submit_query(m, "hello");
  pump_until(m, [&] { return m.row_count() == 3; });
  m.handle_key("down");
  m.handle_key("down");
  REQUIRE(m.resume_tab() == join_tab({"search", "hello", "2"}));

  // Restored model: the same target refetches and the cursor is clamped back.
  YtFake f2;
  f2.tracks = fake.tracks;
  YtModel m2 = f2.make();
  m2.restore_tab(join_tab({"search", "hello", "2"}));
  REQUIRE(m2.view() == YtModel::View::Search);
  m2.open();  // a pending restored target fetches on the first open
  pump_until(m2, [&] { return m2.row_count() == 3; });
  REQUIRE(m2.cursor() == 2);  // normalize clamps the restored row (3 rows valid)

  // A malformed payload leaves the defaults alone.
  YtFake f3;
  f3.tracks = fake.tracks;
  YtModel m3 = f3.make();
  m3.restore_tab("bogus payload");
  REQUIRE(m3.view() == YtModel::View::Search);
  REQUIRE(m3.row_count() == 0);
  REQUIRE(m3.resume_tab() == "search");

  // View + target without a cursor: the view switches, the target lands.
  YtFake f4;
  f4.tracks = {yt_track("https://youtu.be/f1", "Feed Row")};
  YtModel m4 = f4.make();
  m4.restore_tab(join_tab({"feeds", "https://youtu.be/f1"}));
  REQUIRE(m4.view() == YtModel::View::Feeds);
}

TEST_CASE("yt screen: header and footer announce the screen and source",
          "[yt][model]") {
  YtFake fake;
  fake.tracks = {yt_track("https://youtu.be/s1", "Song", "Artist", 125)};
  YtModel m    = fake.make();
  REQUIRE(m.header_label().find("YOUTUBE") != std::string::npos);
  REQUIRE(m.header_label().find("[Search]") != std::string::npos);
  REQUIRE(m.footer_label() == "no target yet");

  submit_query(m, "song");
  pump_until(m, [&] { return m.row_count() == 1; });
  REQUIRE(m.footer_label().find("yt-dlp") != std::string::npos);
  REQUIRE(m.row_label(0, 0).find("Artist — Song") != std::string::npos);
  REQUIRE(m.row_label(0, 0).find("(2m)") != std::string::npos);
}
