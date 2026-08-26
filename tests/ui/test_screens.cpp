// tests/ui/test_screens.cpp — screen model tests (queue / browse / eq_overlay
// / help / too_small).
//
// Ports Go cases onto the plain-C++ screen models:
//   queue     — ui/model/playlist_render_state_test.go
//               TestRenderQueueBodyPreservesVisibleQueuePositions; the queue
//               key behaviors from ui/model/keys.go handleQueueKey.
//   eq_overlay — ui/model/audio_test.go (TestCycleEQPresetReturnsToCustomCurve,
//               TestBuiltInPresetSavePreservesCustomCurve,
//               TestLoadedCustomCurveSurvivesActiveBuiltInPreset,
//               TestBandChangeReplacesCustomCurve).
//   browse    — ui/model/view.go renderProviderList section logic + keys_radio.go
//               maybeLoadCatalogBatch + keys.go handleCatalogSearchKey /
//               handleNetSearchInputKey (ytsearch10:/scsearch10:).
//   help      — ui/model/keymap.go buildKeymapEntries/updateKeymapFilter.
//   too_small — ui/model/layout_test.go TestFrameLayoutTiers (the 40x10 gate)
//               + ui/model/view.go too-small message.
#include "ui/screens/browse.hpp"
#include "ui/screens/eq_overlay.hpp"
#include "ui/screens/help.hpp"
#include "ui/screens/queue.hpp"
#include "ui/screens/too_small.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace bootamp::ui::screens;
using bootamp::playlist::Playlist;
using bootamp::playlist::PlaylistInfo;
using bootamp::playlist::Track;

namespace {

// Fill `pl` with `n` titled tracks, each queued in order (Go:
// p.Add(Track{Title:...}); p.Queue(i)). Playlist is non-movable, so the
// caller owns it.
void fill_queued(Playlist& pl, int n) {
  std::vector<Track> tracks;
  for (int i = 0; i < n; ++i) {
    tracks.push_back(Track{.title = "Track " + std::to_string(i + 1)});
  }
  pl.replace(tracks);
  for (int i = 0; i < n; ++i) {
    pl.queue(i);
  }
}

// Cursor-marked render of queue row i, mirroring Go cursorLine:
// "> label" when active, "  label" otherwise.
std::string queue_render(QueueModel& m, int i, int width) {
  return (i == m.cursor() ? "> " : "  ") + m.row_label(i, width);
}

// A fake provider for BrowseModel: a fixed PlaylistInfo list + optional
// catalog pager and search. Records calls for assertions.
struct FakeProvider {
  std::vector<PlaylistInfo> lists;
  std::vector<PlaylistInfo> catalog_pages;  // appended on load_catalog_page
  int                       load_calls = 0;
  int                       search_calls = 0;
  std::vector<std::string>  search_queries;
  std::string               search_error;

  std::string id_prefix(std::string_view id) const {
    const auto colon = id.find(':');
    return colon == std::string::npos ? "" : std::string(id.substr(0, colon));
  }
  bool is_favoritable(std::string_view id) const {
    return id.size() > 2 && (id[0] == 'c' || id[0] == 'f' || id[0] == 's');
  }

  BrowseModel make_model() {
    return BrowseModel(
        [this]() -> std::expected<std::vector<PlaylistInfo>, std::string> {
          std::vector<PlaylistInfo> out = lists;
          for (const PlaylistInfo& p : catalog_pages) {
            out.push_back(p);
          }
          return out;
        },
        [this](int offset, int limit)
            -> std::expected<int, std::string> {
          ++load_calls;
          const int available = static_cast<int>(catalog_pages.size()) - offset;
          const int added = std::max(0, std::min(limit, available));
          return added;
        },
        [this](std::string_view query)
            -> std::expected<int, std::string> {
          ++search_calls;
          search_queries.emplace_back(query);
          if (!search_error.empty()) {
            return std::unexpected(search_error);
          }
          return 3;
        },
        [this](std::string_view query)
            -> std::expected<std::vector<Track>, std::string> {
          std::vector<Track> out;
          out.push_back(Track{.path = "https://example.invalid/" +
                                       std::string(query),
                              .title = "hit for " + std::string(query),
                              .artist = "artist for " + std::string(query),
                              .duration_secs = 213});
          return out;
        },
        [this](std::string_view id) { return id_prefix(id); },
        [this](std::string_view id) { return is_favoritable(id); });
  }
};

}  // namespace

// ===========================================================================
// QueueModel — port of Go TestRenderQueueBodyPreservesVisibleQueuePositions +
// handleQueueKey.
// ===========================================================================

TEST_CASE("queue window preserves absolute positions", "[screens][queue]") {
  // Go playlist_render_state_test.go: 10 queued tracks, cursor=5, scroll=4,
  // plVisible=3 → rows "  5. Track 5" / "> 6. Track 6" / "  7. Track 7".
  // Walk to that exact state: 6 downs then 1 up in a 3-row window gives
  // cursor 5 with scroll 4 (scroll follows the cursor one row behind).
  Playlist pl;
  fill_queued(pl,10);
  QueueModel m(pl);
  m.open();
  m.set_visible_rows(3);
  for (int i = 0; i < 6; ++i) {
    m.cursor_down();
  }
  m.cursor_up();

  REQUIRE(m.count() == 10);
  REQUIRE(m.cursor() == 5);
  REQUIRE(m.scroll() == 4);

  // Window rows are absolute queue positions 4..6 (labels 5./6./7.) — the
  // numbering must not restart at the window top (Go test assertion).
  REQUIRE(queue_render(m, 4, 80) == "  5. Track 5");
  REQUIRE(queue_render(m, 5, 80) == "> 6. Track 6");
  REQUIRE(queue_render(m, 6, 80) == "  7. Track 7");
}

TEST_CASE("queue cursor wraps", "[screens][queue]") {
  Playlist pl;
  fill_queued(pl,3);
  QueueModel m(pl);
  m.open();
  REQUIRE(m.cursor() == 0);
  m.cursor_up();  // wrap to last
  REQUIRE(m.cursor() == 2);
  m.cursor_down();  // wrap to first
  REQUIRE(m.cursor() == 0);
}

TEST_CASE("queue d removes and normalizes", "[screens][queue]") {
  Playlist pl;
  fill_queued(pl,3);
  QueueModel m(pl);
  int removed_pos = -1;
  m.set_actions(QueueModel::Actions{.on_remove_at = [&](int pos) {
                                      removed_pos = pos;
                                    }});
  m.open();
  m.cursor_down();
  m.cursor_down();  // cursor 2
  REQUIRE(m.handle_key("d"));
  REQUIRE(removed_pos == 2);
  REQUIRE(m.count() == 3);  // action path: the host removes; model normalized
}

TEST_CASE("queue c clears and closes", "[screens][queue]") {
  Playlist pl;
  fill_queued(pl,3);
  QueueModel m(pl);
  int cleared = 0;
  m.set_actions(QueueModel::Actions{.on_clear = [&] { ++cleared; }});
  m.open();
  REQUIRE(m.visible());
  REQUIRE(m.handle_key("c"));
  REQUIRE(cleared == 1);
  REQUIRE(!m.visible());
}

TEST_CASE("queue enter plays queued track index", "[screens][queue]") {
  // Go daemon "queue.play": SetIndex(track index) + play.
  Playlist pl;
  fill_queued(pl,5);
  QueueModel m(pl);
  int played_index = -1;
  m.set_actions(QueueModel::Actions{.on_play = [&](int idx) {
                                      played_index = idx;
                                    }});
  m.open();
  m.cursor_down();
  m.cursor_down();  // cursor 2 → queued track index 2
  REQUIRE(m.handle_key("enter"));
  REQUIRE(played_index == 2);
}

TEST_CASE("queue s/r/f dispatch actions", "[screens][queue]") {
  Playlist pl;
  fill_queued(pl,2);
  QueueModel m(pl);
  int shuffle = 0, repeat = 0, fav = 0;
  m.set_actions(QueueModel::Actions{
      .on_toggle_shuffle = [&] { ++shuffle; },
      .on_cycle_repeat = [&] { ++repeat; },
      .on_toggle_favorite = [&](int) { ++fav; },
  });
  m.open();
  REQUIRE(m.handle_key("s"));
  REQUIRE(m.handle_key("r"));
  REQUIRE(m.handle_key("f"));
  REQUIRE(shuffle == 1);
  REQUIRE(repeat == 1);
  REQUIRE(fav == 1);
}

TEST_CASE("queue esc and A close; shift+up/down reorder", "[screens][queue]") {
  Playlist pl;
  fill_queued(pl,3);
  QueueModel m(pl);
  m.open();
  REQUIRE(m.handle_key("esc"));
  REQUIRE(!m.visible());

  m.open();
  m.cursor_down();
  m.cursor_down();  // cursor 2
  REQUIRE(m.handle_key("shift+up"));  // queue entry 2 moves to position 1
  REQUIRE(m.cursor() == 1);
  REQUIRE(m.handle_key("shift+down"));
  REQUIRE(m.cursor() == 2);

  m.open();
  REQUIRE(m.handle_key("A"));
  REQUIRE(!m.visible());
}

TEST_CASE("queue normalize resets on empty queue", "[screens][queue]") {
  Playlist pl;  // empty
  QueueModel m(pl);
  m.open();
  m.set_visible_rows(5);
  REQUIRE(m.count() == 0);
  REQUIRE(m.cursor() == 0);
  REQUIRE(m.scroll() == 0);
  REQUIRE(m.header_label() == "Queue");
}

TEST_CASE("queue header shows position", "[screens][queue]") {
  Playlist pl;
  fill_queued(pl,4);
  QueueModel m(pl);
  m.open();
  m.cursor_down();
  REQUIRE(m.header_label() == "Queue  2/4");
}

// ===========================================================================
// EqModel — port of Go ui/model/audio_test.go.
// ===========================================================================

TEST_CASE("eq presets table matches Go eq_presets.go", "[screens][eq]") {
  const auto& table = eq_presets();
  REQUIRE(table.size() == 16);
  REQUIRE(table[0].name == "Flat");
  REQUIRE(table[0].bands == std::array<double, kEqBandCount>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  REQUIRE(table[1].name == "Rock");
  REQUIRE(table[1].bands == std::array<double, kEqBandCount>{5, 4, 2, -1, -2, 2, 4, 5, 5, 5});
  REQUIRE(table[5].name == "Bass Boost");
  REQUIRE(table[6].name == "Treble Boost");
  REQUIRE(table[7].name == "Vocal");
  REQUIRE(table[15].name == "Small Speakers");
  REQUIRE(table[15].bands == std::array<double, kEqBandCount>{7, 5, 4, 2, 1, 0, -1, 0, 1, 2});

  const auto [flat, found] = eq_preset_by_name("flat");  // case-insensitive
  REQUIRE(found);
  REQUIRE(flat.name == "Flat");
  REQUIRE(!eq_preset_by_name("nonexistent").second);
}

TEST_CASE("eq labels and freqs", "[screens][eq]") {
  REQUIRE(kEqBandLabels == std::array<std::string_view, 10>{
                               "70", "180", "320", "600", "1k", "3k", "6k", "12k", "14k", "16k"});
  REQUIRE(kEqBandFreqs == std::array<int, 10>{
                              70, 180, 320, 600, 1000, 3000, 6000, 12000, 14000, 16000});
}

TEST_CASE("eq cycle preset returns to custom curve", "[screens][eq]") {
  // Go TestCycleEQPresetReturnsToCustomCurve: 17 "e" presses → Custom + the
  // original custom curve.
  const std::array<double, kEqBandCount> custom{6, 4, 2, 0, -2, 1, 3, 5, 4, 2};
  EqModel m([custom] { return custom; }, {});
  for (std::size_t i = 0; i < eq_presets().size() + 1; ++i) {
    m.handle_key("e");
  }
  REQUIRE(m.preset_name() == "Custom");
  REQUIRE(m.bands() == custom);
}

TEST_CASE("eq built-in preset preserves custom curve", "[screens][eq]") {
  // Go TestBuiltInPresetSavePreservesCustomCurve: one "e" → "Flat" active,
  // but the saved custom curve is untouched.
  const std::array<double, kEqBandCount> custom{6, 4, 2, 0, -2, 1, 3, 5, 4, 2};
  EqModel m([custom] { return custom; }, {});
  m.handle_key("e");
  REQUIRE(m.preset_name() == "Flat");
  REQUIRE(m.custom_bands() == custom);
}

TEST_CASE("eq custom curve survives active built-in preset", "[screens][eq]") {
  // Go TestLoadedCustomCurveSurvivesActiveBuiltInPreset.
  const std::array<double, kEqBandCount> custom{6, 4, 2, 0, -2, 1, 3, 5, 4, 2};
  EqModel m([custom] { return custom; }, {});
  m.apply_preset_by_name("Flat");
  m.apply_preset_by_name("Custom");
  REQUIRE(m.bands() == custom);
}

TEST_CASE("eq band change replaces custom curve", "[screens][eq]") {
  // Go TestBandChangeReplacesCustomCurve: Flat, set band 2 to +5, Rock, then
  // Custom → flat with band 2 = 5.
  EqModel m([] { return std::array<double, kEqBandCount>{}; }, {});
  m.apply_preset_by_name("Flat");
  m.set_band(2, 5.0);
  m.apply_preset_by_name("Rock");
  m.apply_preset_by_name("Custom");
  std::array<double, kEqBandCount> want{0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  want[2] = 5.0;
  REQUIRE(m.bands() == want);
}

TEST_CASE("eq keys adjust bands live", "[screens][eq]") {
  std::vector<std::pair<int, double>> engine_calls;
  EqModel m({}, [&engine_calls](int band, double db) {
    engine_calls.emplace_back(band, db);
  });
  REQUIRE(m.band_db(0) == 0.0);

  m.handle_key("k");  // band up +1 on cursor 0
  REQUIRE(m.band_db(0) == 1.0);
  REQUIRE(engine_calls.back() == std::pair<int, double>(0, 1.0));

  m.handle_key("j");  // band down -1
  REQUIRE(m.band_db(0) == 0.0);

  m.handle_key("right");  // cursor → 1
  REQUIRE(m.cursor() == 1);
  m.handle_key("l");      // cursor → 2 (right and l both move)
  REQUIRE(m.cursor() == 2);
  m.handle_key("left");
  REQUIRE(m.cursor() == 1);
  m.handle_key("h");
  REQUIRE(m.cursor() == 0);
  m.handle_key("left");  // clamped at 0
  REQUIRE(m.cursor() == 0);

  m.handle_key("0");  // flat on the cursor band (already 0)
  REQUIRE(m.band_db(0) == 0.0);
  m.set_band(0, 3.0);
  m.handle_key("0");
  REQUIRE(m.band_db(0) == 0.0);
  REQUIRE(m.preset_name() == "Custom");
}

TEST_CASE("eq value_text mirrors renderControls readout", "[screens][eq]") {
  EqModel m({}, {});
  REQUIRE(m.value_text(0) == "70");   // flat → band label
  m.set_band(0, 5.0);
  REQUIRE(m.value_text(0) == "+5");
  m.set_band(1, -3.0);
  REQUIRE(m.value_text(1) == "-3");
}

TEST_CASE("eq apply_preset labels custom curve", "[screens][eq]") {
  EqModel m({}, {});
  m.apply_preset("My Curve", std::array<double, kEqBandCount>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  REQUIRE(m.preset_name() == "My Curve");
  m.apply_preset("Custom", std::array<double, kEqBandCount>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  REQUIRE(m.preset_name() == "Custom");
}

// ===========================================================================
// BrowseModel — sectioned radio list + lazy catalog + search.
// ===========================================================================

TEST_CASE("browse sections follow id prefixes", "[screens][browse]") {
  FakeProvider fp;
  fp.lists = {
      {"l:0", "cliamp radio", 0},
      {"f:0", "★ Some FM", 0},
      {"f:1", "★ Another FM", 0},
      {"c:0", "Top Station A", 0},
      {"s:0", "Search Hit", 0},
  };
  BrowseModel m = fp.make_model();
  REQUIRE(m.refresh().empty());

  REQUIRE(m.section_label(0) == "Local");
  REQUIRE(m.section_label(1) == "★ Favorites");
  REQUIRE(m.section_label(2) == "");      // same section as previous
  REQUIRE(m.section_label(3) == "Catalog");
  REQUIRE(m.section_label(4) == "Search");
  REQUIRE(m.sectioned());
}

TEST_CASE("browse cursor wraps and selects", "[screens][browse]") {
  FakeProvider fp;
  fp.lists = {{"l:0", "A", 0}, {"l:1", "B", 0}};
  BrowseModel m = fp.make_model();
  std::string selected;
  m.set_actions(BrowseModel::Actions{.on_select = [&](std::string_view id) {
                                       selected = std::string(id);
                                     }});
  REQUIRE(m.refresh().empty());
  REQUIRE(m.count() == 2);

  m.cursor_up();  // wrap to last
  REQUIRE(m.cursor() == 1);
  m.cursor_down();  // wrap to first
  REQUIRE(m.cursor() == 0);
  m.handle_key("enter");
  REQUIRE(selected == "l:0");
}

TEST_CASE("browse lazy catalog loads near bottom and stops on short page",
          "[screens][browse]") {
  FakeProvider fp;
  for (int i = 0; i < 20; ++i) {
    fp.lists.push_back({"l:" + std::to_string(i), "local " + std::to_string(i), 0});
  }
  for (int i = 0; i < 5; ++i) {  // one short page (5 < 100)
    fp.catalog_pages.push_back({"c:" + std::to_string(i), "station " + std::to_string(i), 0});
  }
  BrowseModel m = fp.make_model();
  REQUIRE(m.refresh().empty());
  REQUIRE(fp.load_calls == 0);

  // Cursor must reach within 10 of the end (Go maybeLoadCatalogBatch).
  m.cursor_down();
  REQUIRE(fp.load_calls == 0);
  while (m.cursor() < m.count() - BrowseModel::kCatalogNearBottom) {
    m.cursor_down();
  }
  // The page fetch runs on a background thread (jthread); pump the result
  // until it lands. catalog_loading_ is cleared only by pump_catalog_result,
  // so the loop exits exactly when the merge happened; the injected fake
  // returns instantly, so a few iterations suffice — the bound is generous
  // against scheduling jitter.
  for (int i = 0; i < 1000 && m.catalog_loading(); ++i) {
    m.pump_catalog_result();
    std::this_thread::yield();
  }
  m.pump_catalog_result();  // no-op if already consumed
  REQUIRE(fp.load_calls == 1);
  REQUIRE(m.catalog_done());  // short page → done
  REQUIRE(m.count() == 25);   // catalog appended + refresh
  REQUIRE(m.section_label(20) == "Catalog");

  m.cursor_down();  // no further loads once done
  REQUIRE(fp.load_calls == 1);
}

TEST_CASE("browse catalog search fires SearchStations and refreshes",
          "[screens][browse]") {
  FakeProvider fp;
  fp.lists = {{"l:0", "cliamp radio", 0}};
  BrowseModel m = fp.make_model();
  REQUIRE(m.refresh().empty());

  REQUIRE(m.handle_key("/"));
  REQUIRE(m.search_active());
  REQUIRE(m.search_mode() == BrowseModel::SearchMode::Catalog);
  m.set_search_query("jazz fm");
  REQUIRE(m.handle_key("enter"));
  REQUIRE(fp.search_calls == 1);
  REQUIRE(fp.search_queries[0] == "jazz fm");
  REQUIRE(m.search_error().empty());

  m.handle_key("esc");
  REQUIRE(!m.search_active());
}

TEST_CASE("browse youtube search uses ytsearch10 prefix", "[screens][browse]") {
  FakeProvider fp;
  fp.lists = {{"l:0", "cliamp radio", 0}};
  BrowseModel m = fp.make_model();
  std::string submitted;
  m.set_actions(BrowseModel::Actions{
      .on_search_submitted = [&](std::string_view q) { submitted = std::string(q); }});
  REQUIRE(m.refresh().empty());

  REQUIRE(m.handle_key("ctrl+f"));
  REQUIRE(m.search_mode() == BrowseModel::SearchMode::YouTube);
  m.set_search_query("lofi beats");
  REQUIRE(m.handle_key("enter"));
  REQUIRE(submitted == "ytsearch10:lofi beats");
  REQUIRE(m.net_results().size() == 1);
  REQUIRE(m.net_results()[0].title == "hit for ytsearch10:lofi beats");

  m.net_cursor_down();
  REQUIRE(m.net_cursor() == 0);  // wraps single result
  // Enter plays the full resolved track (path/title/artist/duration from
  // resolve_ytdl), not just the URL — Go handleNetSearchResultsKey enter:
  // playTrackImmediate(track). on_select is no longer called from the net
  // path (it stays for radio playlist selects). Snapshot the result first:
  // net_select_cursor() closes the search, which clears net_results_.
  const Track first = m.net_results()[0];
  Track played;
  std::string selected;
  m.set_actions(BrowseModel::Actions{
      .on_select = [&](std::string_view id) { selected = std::string(id); },
      .on_play_track = [&](const Track& t) { played = t; }});
  m.net_select_cursor();
  REQUIRE(played.path == first.path);
  REQUIRE(played.title == first.title);
  REQUIRE(played.artist == first.artist);
  REQUIRE(played.duration_secs == first.duration_secs);
  REQUIRE(selected.empty());       // on_select untouched by net selects
  REQUIRE(!m.net_results_active());  // close_search() after enter
}

TEST_CASE("browse soundcloud search uses scsearch10 prefix", "[screens][browse]") {
  FakeProvider fp;
  fp.lists = {{"l:0", "cliamp radio", 0}};
  BrowseModel m = fp.make_model();
  m.start_search(BrowseModel::SearchMode::SoundCloud);
  m.set_search_query("ambient");
  REQUIRE(m.submit_search().empty());
  REQUIRE(m.net_results().size() == 1);
  REQUIRE(m.net_results()[0].title == "hit for scsearch10:ambient");
}

TEST_CASE("browse net results a appends the current track", "[screens][browse]") {
  // Go handleNetSearchResultsKey "a": appendTrack(track) — the full resolved
  // track, not just the URL. Unlike Go (closeNetSearch before append), the
  // results stay open so several tracks can be appended in one pass.
  FakeProvider fp;
  fp.lists = {{"l:0", "cliamp radio", 0}};
  BrowseModel m = fp.make_model();
  Track appended;
  m.set_actions(BrowseModel::Actions{
      .on_append_track = [&](const Track& t) { appended = t; }});
  REQUIRE(m.refresh().empty());

  m.start_search(BrowseModel::SearchMode::YouTube);
  m.set_search_query("lofi beats");
  REQUIRE(m.submit_search().empty());
  REQUIRE(m.net_results().size() == 1);

  REQUIRE(m.handle_key("a"));
  REQUIRE(appended.path == m.net_results()[0].path);
  REQUIRE(appended.title == "hit for ytsearch10:lofi beats");
  REQUIRE(appended.artist == "artist for ytsearch10:lofi beats");
  REQUIRE(appended.duration_secs == 213);
  REQUIRE(m.net_results_active());  // append leaves the results open
  REQUIRE(m.net_cursor() == 0);
}

TEST_CASE("browse net results ignore typing and leave q global",
          "[screens][browse]") {
  // Go handleNetSearchResultsKey has no text handling: typing on the results
  // screen does nothing. Bootamp consumes printable/space/backspace so they
  // can't fall through to the host's search-input path and edit the query;
  // q is not bound on the results screen and stays the global quit key.
  FakeProvider fp;
  fp.lists = {{"l:0", "cliamp radio", 0}};
  BrowseModel m = fp.make_model();
  REQUIRE(m.refresh().empty());
  m.start_search(BrowseModel::SearchMode::YouTube);
  m.set_search_query("lofi beats");
  REQUIRE(m.submit_search().empty());

  REQUIRE(m.handle_key("x"));
  REQUIRE(m.handle_key("space"));  // host key name (ftxui_app_impl.hpp)
  REQUIRE(m.handle_key("backspace"));
  REQUIRE(m.search_query() == "lofi beats");  // typing never edited the query
  REQUIRE(!m.handle_key("q"));                // q falls through to global quit
  REQUIRE(m.net_results_active());
  REQUIRE(m.handle_key("esc"));
  REQUIRE(!m.search_active());
}

TEST_CASE("browse empty net search query errors", "[screens][browse]") {
  FakeProvider fp;
  fp.lists = {{"l:0", "cliamp radio", 0}};
  BrowseModel m = fp.make_model();
  REQUIRE(m.refresh().empty());
  m.start_search(BrowseModel::SearchMode::YouTube);
  const std::string err = m.submit_search();
  REQUIRE(err == "Enter a search query.");
  REQUIRE(m.search_error() == err);
}

TEST_CASE("browse favorite only on favoritable ids", "[screens][browse]") {
  FakeProvider fp;
  fp.lists = {
      {"l:0", "cliamp radio", 0},   // local: not favoritable (Go IsFavoritableID)
      {"f:0", "★ Fav", 0},          // favorite id: favoritable
  };
  BrowseModel m = fp.make_model();
  std::vector<std::string> favorited;
  m.set_actions(BrowseModel::Actions{
      .on_favorite = [&](std::string_view id) { favorited.emplace_back(id); }});
  REQUIRE(m.refresh().empty());

  REQUIRE(m.handle_key("f"));  // cursor on l:0 → gated, no action
  REQUIRE(favorited.empty());
  m.cursor_down();              // f:0
  REQUIRE(m.handle_key("f"));
  REQUIRE(favorited.size() == 1);
  REQUIRE(favorited[0] == "f:0");
}

// ===========================================================================
// HelpModel — keybinding table + filter (port of keymap.go).
// ===========================================================================

TEST_CASE("help table covers global commands", "[screens][help]") {
  const auto& table = help_entries();
  REQUIRE(!table.empty());
  bool space = false, quit = false, help_row = false;
  for (const HelpEntry& e : table) {
    if (e.key == "Space" && e.action == "Play / Pause") {
      space = true;
    }
    if (e.key == "q" && e.action == "Quit") {
      quit = true;
    }
    if (e.key == "Ctrl+K" && e.action == "Help") {
      help_row = true;
    }
  }
  REQUIRE(space);
  REQUIRE(quit);
  REQUIRE(help_row);
}

TEST_CASE("help filter matches key and action case-insensitively",
          "[screens][help]") {
  HelpModel m;
  m.open();
  const int total = m.count();

  m.set_filter("volume");
  REQUIRE(m.count() > 0);
  REQUIRE(m.count() < total);
  for (int i = 0; i < m.count(); ++i) {
    const HelpEntry& e = m.entry_at(i);
    REQUIRE((e.key.find("volume") != std::string::npos ||
             e.action.find("volume") != std::string::npos));
  }

  m.set_filter("QUIT");
  REQUIRE(m.count() == 1);
  REQUIRE(m.entry_at(0).action == "Quit");

  m.set_filter("zzzznomatch");
  REQUIRE(m.count() == 0);

  m.clear_filter();
  REQUIRE(m.count() == total);
}

TEST_CASE("help navigation wraps and pages", "[screens][help]") {
  HelpModel m;
  m.open();
  m.set_visible_rows(10);
  const int n = m.count();

  m.cursor_up();  // wrap to last
  REQUIRE(m.cursor() == n - 1);
  m.cursor_down();  // wrap to first
  REQUIRE(m.cursor() == 0);

  m.go_bottom();
  REQUIRE(m.cursor() == n - 1);
  m.go_top();
  REQUIRE(m.cursor() == 0);

  m.page_down();
  REQUIRE(m.cursor() == 10);
  m.page_up();
  REQUIRE(m.cursor() == 0);
}

TEST_CASE("help keys open/close and filter", "[screens][help]") {
  HelpModel m;
  m.open();
  REQUIRE(m.visible());

  REQUIRE(m.handle_key("/"));   // enters filter mode (Go openKeymap)
  REQUIRE(!m.filtering());      // empty filter = unfiltered table
  m.set_filter("speed");        // host text editor feeds typed text
  REQUIRE(m.filtering());
  REQUIRE(m.handle_key("backspace"));
  REQUIRE(!m.filtering());

  REQUIRE(m.handle_key("esc"));
  REQUIRE(!m.visible());

  m.open();
  REQUIRE(m.handle_key("enter"));
  REQUIRE(!m.visible());
}

// ===========================================================================
// TooSmallModel — port of the 40x10 gate + Go view message.
// ===========================================================================

TEST_CASE("too small gate matches Go layout tiers", "[screens][toosmall]") {
  // Go TestFrameLayoutTiers: 39x9 → too small; 40x10 → minimal (not small).
  REQUIRE(TooSmallModel::is_too_small(39, 9));
  REQUIRE(TooSmallModel::is_too_small(39, 10));  // width gate
  REQUIRE(TooSmallModel::is_too_small(40, 9));   // height gate
  REQUIRE(!TooSmallModel::is_too_small(40, 10));
  REQUIRE(!TooSmallModel::is_too_small(56, 16));
  REQUIRE(!TooSmallModel::is_too_small(80, 24));
}

TEST_CASE("too small message matches Go view text", "[screens][toosmall]") {
  REQUIRE(TooSmallModel::message(39, 9) ==
          "Terminal too small. Resize to at least 40x10 (current: 39x9).");
  REQUIRE(TooSmallModel::message(80, 24) ==
          "Terminal too small. Resize to at least 40x10 (current: 80x24).");
}
