// ui/screens/gieres.hpp — Gieres "Radio Radio" archive screen: model + FTXUI
// component.
//
// Browse screen for Giereś's Radio Radio archive (docs/orgonity-api.md): three
// tabbed views — Live (the radioboss live stream + now-playing), Orgonity (the
// paginated BitChute recording archive with a date index and server-side
// search) and Echelon (the 30-minute live-timeline segments with their program
// listings). All blocking network work runs on a background fetch thread (the
// browse-screen pattern: the result lands through an atomic mailbox that the
// render path pumps), so the UI never stalls on the LAN server — and a dead
// server only writes an error into the footer status.
//
// The model is plain C++ and testable: fetches are injected as std::function
// hooks (tests drive them with canned results), playback is fired through the
// host-wired actions as ready playlist::Track values. The FTXUI Component glue
// is compiled only when BOOTAMP_HAS_FTXUI.
//
// Keys: tab / shift+tab cycle views; enter plays the cursor entry (on a date
// index it selects the date — Orgonity opens the day, Echelon fetches the
// day's segments); y appends (in Echelon: the whole day's segments, so the
// queue chains them gapless); / searches Orgonity; s cycles the Orgonity
// sort; d toggles the date index (Orgonity: the server's recording_dates —
// days with an uploaded recording; Echelon: the timeline's OWN calendar —
// every day from the timeline's first day through today, discovered by
// probing /echelon/segments — because the timeline covers days with no
// recording, and today until the show is archived; it self-loads on the
// first d, no Orgonity visit needed); the calendar's days and the segment
// times are LOCAL (machine timezone — the archive's show dates are
// Warsaw-local too); ; / ' step to the previous / next day
// (inside the date index they walk the cursor instead); t expands the
// Echelon program listing; ctrl+r refetches; esc walks back a level and
// finally closes the screen. ←/→ are never consumed — they
// reach the global player table and wind the playing track (seek ±5s,
// shift+left/right large seek), which is what the user expects while
// listening to archive recordings; the list navigates with up/down (j/k),
// pgup/pgdn, home/end. Everything else falls through to the global player
// table like on the other screens (browse/queue/device/eq/help all end
// handle_key on return false), so the player stays controllable on the
// archive: volume `-`/`=`/`+`, skip, repeat, shuffle, stop (outside
// Orgonity, where `s` sorts), mono, speed, queue toggle, remove,
// percent-seek — and `p` toggles the screen closed, `space` play/pause.
// While the search prompt is open all keys type into the query buffer
// instead.
#pragma once

#include "playlist/playlist.hpp"
#include "ui/gieres_client.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if BOOTAMP_HAS_FTXUI
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// Actions the host app wires (side effects outside the model). The host
// receives ready playlist::Track values (URL tracks for recordings/segments,
// a realtime track for the live stream).
struct GieresActions {
  // enter on a playable entry → host starts it (Go playTrackImmediate).
  std::function<void(const playlist::Track&)> on_play_track{};
  // y on a single entry → host appends it to the queue (Go appendTrack).
  std::function<void(const playlist::Track&)> on_append_track{};
  // y in the Echelon view → host appends every segment of the shown day so
  // the queue chains them gapless.
  std::function<void(const std::vector<playlist::Track>&)> on_append_tracks{};
};

// GieresActiveBase records the endpoint that served the last successful
// fetch when for_host's sticky LAN→public fallback engaged: the fetch hooks
// write it (fetch thread, mutex-protected) and pump() reads it on the UI
// thread, adopting the winner into the model's base_url_ — the footer and
// the playback URLs then follow the endpoint that actually answered. Empty
// string = the constructed (primary) base is still authoritative.
struct GieresActiveBase {
  void set(std::string base) {
    std::lock_guard lk(mu_);
    base_ = std::move(base);
  }
  std::string get() const {
    std::lock_guard lk(mu_);
    return base_;
  }

private:
  mutable std::mutex mu_;
  std::string        base_;
};

// GieresModel is the archive browser. Injected fetch hooks return the parsed
// client results; the model owns the view state (tabs, cursors, paging,
// search) and the background fetch lifecycle.
class GieresModel {
public:
  using Actions = GieresActions;

  // Tabbed views (header tabs in the FTXUI glue).
  enum class View : std::uint8_t { Live, Orgonity, Echelon };

  // Orgonity sub-views: the paged recording list, the full date index, or one
  // day's recordings (by_date).
  enum class OrgView : std::uint8_t { List, Dates, Day };

  // Injected archive I/O (for_host wires a real GieresClient; tests inject
  // fakes). All run on the background fetch thread.
  using OrgonityFn  = std::function<std::expected<GieresListing, std::string>(
      int page, std::string_view q, std::string_view sort)>;
  using ByDateFn    = std::function<std::expected<GieresDay, std::string>(
      std::string_view date)>;
  using SegmentsFn  = std::function<std::expected<std::vector<GieresSegment>,
      std::string>(std::string_view start_iso, std::string_view end_iso)>;
  using NowPlayingFn =
      std::function<std::expected<std::string, std::string>()>;

  GieresModel(OrgonityFn orgonity, ByDateFn by_date, SegmentsFn segments,
              NowPlayingFn now_playing, std::string_view base_url,
              Actions actions = {},
              std::shared_ptr<GieresActiveBase> active_base = {});
  ~GieresModel();  // joins the fetch thread (defined in gieres.cpp)
  GieresModel(const GieresModel&)            = delete;
  GieresModel& operator=(const GieresModel&) = delete;

  // for_host wires the real client against `base_url` (config
  // gieres_base_url) and the live stream constants from docs/orgonity-api.md.
  static GieresModel for_host(std::string_view base_url);

  void set_actions(Actions a) { actions_ = std::move(a); }

  // --- Visibility ---------------------------------------------------------
  // open shows the screen and kicks any pending first fetches (idempotent —
  // each view's data is fetched once per lifetime; ctrl+r refetches).
  void open();
  void close() { visible_ = false; }
  bool visible() const { return visible_; }

  // --- View state ---------------------------------------------------------
  View    view() const { return view_; }
  OrgView org_view() const { return org_view_; }
  // set_view switches the active tab directly — the startup resume path (the
  // host reopens the screen on the tab the last session left, before open()
  // kicks that view's first fetch).
  void set_view(View v) { view_ = v; }

  // --- Render data --------------------------------------------------------
  // header_label renders the tab header, e.g. "GIERES ARCHIVE  [Live]
  // [Orgonity] [Echelon]" plus the per-view subtitle line.
  std::string header_label() const;
  // row_count is the active list's row count (Echelon counts the expanded
  // program rows of the cursor segment).
  int  row_count() const;
  // row_label renders "> label" / "  label" for row i (plain text).
  std::string row_label(int i, int panel_width) const;
  // row_dim reports rows that must render dimmed (no-audio recordings).
  bool row_dim(int i) const;
  // footer_label renders "<base_url>  <status>" (status "" | "fetching…" |
  // the last error).
  std::string footer_label() const;
  // search_active reports whether the Orgonity search prompt is open; the
  // prompt line renders "/ <query>▏" from the live buffer.
  bool search_active() const { return search_active_; }
  std::string search_prompt(int panel_width) const;
  int  cursor() const { return cursor_; }
  int  scroll() const { return scroll_; }
  void set_visible_rows(int rows);
  int  visible_rows() const { return visible_rows_; }

  // pump consumes a landed background fetch whose request key still matches
  // the current view state (stale results are dropped). UI thread only; the
  // render path calls it every frame.
  void pump();
  bool loading() const { return loading_.load(std::memory_order_acquire); }

  // kNearBottom — cursor distance from the list end that triggers a lazy page
  // load (browse.cpp kCatalogNearBottom).
  inline static constexpr int kNearBottom = 10;
  // maybe_load_more fetches the next Orgonity page and APPENDS it to the
  // list when the cursor nears the bottom (Go maybeLoadCatalogBatch). No-op
  // while a fetch is in flight, on the last page, or away from the bottom.
  void maybe_load_more();

  // handle_key dispatches a canonical key name; returns true when consumed.
  // While the search prompt is active every printable key is consumed into
  // the query buffer. `p` and `space` are never consumed (see the header
  // comment).
  bool handle_key(std::string_view key);

private:
  // fetch kicks one background fetch for the current view state (append=true
  // → a near-bottom lazy page load that merges instead of replacing). A
  // fetch is single-flight; the request key snapshots the state it was
  // launched for.
  void fetch(bool append = false);
  // fetch_dates loads the Echelon `d` date index — the timeline's own
  // calendar: /echelon/segments has no range endpoint, so the first covered
  // day is found by bisection (one 1-hour-window probe per step) and the
  // contiguous day list runs through today. Single-flight via
  // dates_loading_, on its own thread + mailbox — see the members below.
  void fetch_dates();
  void normalize();   // clamp cursor/scroll after list changes
  // active list size per view (before expansion).
  int  list_count() const;
  // echelon_pos maps a flat Echelon row index to (segment, program) — program
  // -1 on the segment row itself; echelon_flat is the inverse (track -1 =
  // segment row).
  std::pair<int, int> echelon_pos(int flat) const;
  int  echelon_flat(int segment, int track) const;

  // enter / y on the cursor entry (view-dependent).
  void play_cursor();
  void append_cursor();
  // select_echelon_day switches the shown Echelon day and fetches its
  // segments — the date index's enter and the ;/' day step land here.
  void select_echelon_day(std::string date);
  // ech_index_row is the calendar row of `date` (the newest row when absent —
  // the index toggle lands the cursor there).
  int  ech_index_row(const std::string& date) const;

  OrgonityFn   orgonity_fn_;
  ByDateFn     by_date_fn_;
  SegmentsFn   segments_fn_;
  NowPlayingFn now_playing_fn_;
  Actions      actions_;

  bool   visible_      = false;
  View   view_         = View::Live;
  OrgView org_view_    = OrgView::List;

  // Orgonity list state. dates_ rides along every /orgonity.json fetch (the
  // Orgonity date index; the Echelon index has its own calendar below).
  std::vector<GieresBroadcast> recordings_;
  std::vector<std::string>     dates_;
  GieresDay                    day_;   // OrgView::Day content
  int    page_        = 1;
  int    total_pages_ = 1;
  int    total_count_ = 0;
  // requested_page_ is the page the in-flight fetch targets (see fetch()).
  int    requested_page_ = 1;
  std::string query_;    // submitted search ("" = none)
  std::string sort_ = "date";  // date | title | duration

  // Echelon state: the day cursor (YYYY-MM-DD, UTC window) and its segments.
  // expanded_seg_ >= 0 shows the program listing (tracks[]) of that segment
  // inline below its row; ech_dates_ swaps the segment list for the date
  // index (d) — a selected date becomes the shown Echelon day. The index list
  // (ech_dates_list_) is the timeline's OWN calendar, fetched by fetch_dates
  // (probing /echelon/segments) on the first d press — distinct from dates_
  // (the Orgonity recording_dates subset): the timeline covers days with no
  // uploaded recording, and today until the show is archived.
  std::string                 ech_date_;
  std::vector<GieresSegment>  segments_;
  int                         expanded_seg_ = -1;
  bool                        ech_dates_ = false;
  std::vector<std::string>    ech_dates_list_;

  // Live view state.
  std::string now_playing_;

  // Search prompt state: query_buf_ is the live typing buffer, query_ the
  // submitted (sanitized) search that the list is filtered by.
  bool        search_active_ = false;
  std::string query_buf_;

  int cursor_       = 0;
  int scroll_       = 0;
  int visible_rows_ = 0;

  std::string status_;  // footer status ("" | "fetching…" | error)
  std::string error_;   // sticky last error (shown until the next success)

  std::string base_url_;  // archive server base (footer + URL joining)

  // Background fetch lifecycle (browse.cpp pattern): the worker publishes an
  // immutable result into the atomic mailbox; pump() consumes it on the UI
  // thread. loading_ doubles as the single-flight guard.
  struct FetchResult {
    // The state snapshot this fetch was launched for; pump() applies the
    // result only when every field still matches.
    View    view;
    OrgView org;
    int     page;
    std::string query;
    std::string sort;
    std::string date;
    // append=true → the page was fetched for a near-bottom lazy load; pump
    // merges the broadcasts into the existing list instead of replacing it.
    bool append = false;
    // for_dates=true → the fetch fills the Echelon date index: the timeline's
    // OWN calendar (every day from the timeline's first day through today, in
    // ech_dates), not the orgonity recording_dates subset. pump applies it
    // before the staleness guard because it may arrive while the user is on
    // another tab (the calendar is view-independent, so a late result is
    // harmless). It rides the dedicated dates_inbox_ mailbox.
    bool for_dates = false;
    // Payload (one per fetch kind).
    bool                        ok = false;
    std::string                 fetch_error;
    GieresListing               listing;
    std::vector<std::string>    ech_dates;  // for_dates: the timeline calendar
    GieresDay                   day;
    std::vector<GieresSegment>  segments;
    std::string                 now_playing;
  };
  std::atomic<bool> loading_{false};
  std::atomic<std::shared_ptr<const FetchResult>> inbox_{nullptr};
  std::jthread fetch_thread_;

  // The date-index fetch (fetch_dates) runs on its own thread + mailbox:
  // independent of the shared fetch thread so it never cancels an in-flight
  // view fetch, and with its own single-slot inbox so a regular result can
  // never clobber it (or be clobbered by one) — a shared slot would wedge
  // loading_ / dates_loading_. dates_loading_ is the single-flight guard;
  // pump() resets it when the result is consumed (never in the worker, like
  // loading_).
  std::jthread      dates_thread_;
  std::atomic<bool> dates_loading_{false};
  std::atomic<std::shared_ptr<const FetchResult>> dates_inbox_{nullptr};

  // for_host's sticky-fallback state (null for test-constructed models):
  // the fetch hooks record the endpoint that answered; pump() adopts it.
  std::shared_ptr<GieresActiveBase> active_base_;
};

// gieres_is_transport_error — connection-level failures (dial/DNS/connect/
// timeout/TLS/reset) are worth retrying on the fallback endpoint; an HTTP
// status error ("gieres: /path HTTP 404" / "http status 404 ...") or a JSON
// parse error means the server ANSWERED and retrying is pointless. Exposed
// for the fallback tests.
bool gieres_is_transport_error(std::string_view err);

// GieresFallbackHooks bundles one endpoint's four fetch hooks plus the base
// it serves (the winner recording needs the URL even when a hook failed).
struct GieresFallbackHooks {
  std::string               base;
  GieresModel::OrgonityFn   orgonity;
  GieresModel::ByDateFn     by_date;
  GieresModel::SegmentsFn   segments;
  GieresModel::NowPlayingFn now_playing;
};

// gieres_with_fallback wraps a primary hook set with a sticky secondary: a
// transport error on the first-tried set retries the other once, and the
// winner is recorded into `active` ("" = primary won). Once the secondary
// has won, it is tried first (sticky) with the primary as its own fallback;
// when both fail the FIRST-tried endpoint's error surfaces. Exposed for the
// tests — for_host wires the real clients through it.
GieresFallbackHooks gieres_with_fallback(GieresFallbackHooks primary,
                                         GieresFallbackHooks secondary,
                                         const std::shared_ptr<GieresActiveBase>& active);

// gieres_day_window maps a LOCAL calendar day ("YYYY-MM-DD") to the UTC
// instant window the archive server sees for it (docs/orgonity-api.md §4):
// local midnight to local midnight via the machine's timezone —
// Europe/Warsaw turns 2026-09-12 into "2026-09-11T22:00:00Z"…
// "2026-09-12T22:00:00Z", so a 2-AM show lands on its own day at its own
// wall-clock time. Without a tz database the window degrades to the plain
// UTC day. Empty strings on a bad date. Exposed for the tests (the fakes
// key the timeline coverage on these window strings).
std::pair<std::string, std::string> gieres_day_window(std::string_view date);

// gieres_local_today is the machine-local calendar date of now
// ("YYYY-MM-DD") — the Echelon calendar's newest day and the next-day
// step's clamp. Exposed for the tests (they mirror the model's "today").
std::string gieres_local_today();

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see gieres.cpp).
// Renders the tab header, the active list's scroll window and the footer.
std::shared_ptr<ftxui::ComponentBase> make_gieres_component(GieresModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens