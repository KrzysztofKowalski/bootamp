// ui/screens/yt.hpp — YouTube screen: model + FTXUI component.
//
// A browse screen for YouTube data, modeled on the Giereś archive screen
// (ui/screens/gieres.hpp — fetch lifecycle, tabbed views, prompt, cursor
// handling) and the browse screen's net-search playback wiring. Three tabbed
// views, each driven through ONE prompt ("/"):
//   * Search   — yt-dlp ytsearch:N via resolve::resolve_ytdl (ytsearch:20:).
//   * Playlists — a public playlist URL ("https://www.youtube.com/playlist?list=…").
//   * Feeds    — a VIDEO LIST URL (a channel "/@handle/videos", the
//     subscriptions / history / trending feeds). With the cookies and UA that
//     reach both resolve and audio (main.cpp set_ytdl_cookies_from /
//     set_ytdl_user_agent, the K2 fix), user-gated lists work end to end.
//
// The data source is exactly the resolve side's existing shape: one URL /
// ytsearch: query string handed to resolve::resolve_ytdl (no new yt-dlp
// flags), which yields ready playlist::Track values (path = video URL, title,
// artist = uploader, duration). enter plays the cursor result and a appends it
// through host-wired actions — the same play path the browse/gieres screens
// use (append + set_index + play_track); no URL resolution is duplicated.
//
// All blocking work — the yt-dlp spawn (30s bounded inside resolve_ytdl) and
// the disk-cache reads/writes — runs on a background fetch thread (the
// gieres/browse pattern): the result lands through an atomic mailbox that the
// render path pumps, so the UI never stalls. Resolved lists are cached on disk
// (foundation::DiskCache, one shared instance per model) with a short TTL
// (kCacheTtlSeconds = 600): repeated opens of the same list (tab repeats,
// screen re-entry, the resume restore) return from cache instead of
// refetching, while 10 minutes keeps the lists fresh enough that a stale
// subscription feed / search result is never far from the network copy.
// ctrl+r forces the next fetch to skip the cache for the current target (and
// freshly re-store under the same key).
//
// The model is plain C++ and testable: the resolve hook and the cache hooks
// are injected std::functions (tests drive them with canned results, exactly
// like the gieres fetch hooks); for_host wires resolve::resolve_ytdl and the
// real foundation::disk_cache.
//
// Keys: tab / shift+tab cycle the views; / opens the active view's prompt
// (search query, playlist URL, list URL); while the prompt is open every key
// types into the query buffer; enter in the list plays the cursor result; a
// appends it (the browse net-search key — y is NOT used for append because it
// is the screen's host-level toggle below); up/down (j/k), pgup/pgdn,
// home/g, end/G navigate; ctrl+r refetches; esc walks back a level (prompt →
// close). y, p, space, -/=/+ are never consumed: y and p toggle this screen
// and the Giereś screen at the host (the key that opened a screen closes it,
// DevicePicker-style), space is play/pause and the volume keys are the
// global ones. left/right are never consumed either — they reach the global
// player table and wind the playing track (±5s), like on the archive screen.
// Everything else falls through to the global player table.
//
// Resume: the host persists the screen as "yt" with screen_tab carrying the
// view + the last target + the cursor, encoded "<view>\x1e<target>\x1e<cursor>"
// (resume_tab / restore_tab); the app's standard resume panel (foundation
// resume.json) reopens the screen at exactly that state, and the disk cache
// serves the list back without a refetch while its TTL survives the restart.
#pragma once

#include "playlist/playlist.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
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
// receives ready playlist::Track values (the resolve_ytdl output — video URL,
// title, uploader, duration) and plays/appends them through the normal play
// path (append + set_index + play_track), exactly like the browse net-search
// and gieres enter/append actions.
struct YtActions {
  // enter on a result → host starts it (Go playTrackImmediate parity).
  std::function<void(const playlist::Track&)> on_play_track{};
  // a on a result → host appends it to the queue (Go appendTrack parity).
  std::function<void(const playlist::Track&)> on_append_track{};
};

// YtModel is the YouTube browser. The injected resolve hook returns the parsed
// track list (resolve::resolve_ytdl in for_host); the injected cache hooks
// (foundation::disk_cache in for_host) back the short-TTL on-disk list cache.
// The model owns the view state (tabs, cursor, paging-free single list per
// target, the active prompt buffer) and the background fetch lifecycle.
class YtModel {
public:
  using Actions = YtActions;

  // Tabbed views (header tabs in the FTXUI glue). Feeds = VIDEO LISTS
  // (channel / subscriptions / history / trending URLs).
  enum class View : std::uint8_t { Search, Playlists, Feeds };

  // Injected I/O. All run on the background fetch thread.
  using ResolveFn = std::function<std::expected<std::vector<playlist::Track>,
                                                std::string>(
      std::string_view target)>;
  // cache_get returns the cached payload for `key` or nullopt on a miss
  // (the real foundation::DiskCache::get returns std::optional<std::string>).
  using CacheGetFn =
      std::function<std::optional<std::string>(std::string_view key)>;
  // cache_put stores `data` under `key` with a TTL in seconds (the real
  // DiskCache::put's string-payload overload).
  using CachePutFn = std::function<void(std::string_view key,
                                        std::string_view data,
                                        std::int64_t ttl_seconds)>;

  // Construct with explicit hooks (tests). The cache hooks default empty
  // (cacheless); set_cache_hooks installs them later.
  YtModel(ResolveFn resolve, Actions actions = {});
  // Cache-wired constructor (for_host): same as above with the disk-cache
  // hooks, so the non-copyable model can be returned as a prvalue with the
  // shared DiskCache already captured (the gieres for_host return shape).
  YtModel(ResolveFn resolve, CacheGetFn get, CachePutFn put,
          Actions actions = {});
  ~YtModel();  // joins the fetch thread (defined in yt.cpp)
  YtModel(const YtModel&)            = delete;
  YtModel& operator=(const YtModel&) = delete;

  // for_host wires the real resolve hook (resolve::resolve_ytdl) and a real
  // foundation::DiskCache (one shared instance, hook-captured): put(key, data,
  // ttl_seconds) / get(key) -> std::optional<std::string> / evict_namespace, on
  // <XDG_CACHE_HOME>/bootamp (fallback ~/.cache/bootamp), TTL'd, atomic writes.
  static YtModel for_host();
  // set_cache_hooks installs the disk-cache hooks (test convenience; for_host
  // uses the cache-wired constructor instead).
  void set_cache_hooks(CacheGetFn get, CachePutFn put);

  void set_actions(Actions a) { actions_ = std::move(a); }

  // --- Visibility ---------------------------------------------------------
  // open shows the screen; when a target is set but its list never landed, it
  // kicks the fetch (idempotent — reopening a loaded list is a no-op; ctrl+r
  // is the explicit refetch).
  void open();
  void close() { visible_ = false; }
  bool visible() const { return visible_; }

  // --- View state ---------------------------------------------------------
  View view() const { return view_; }
  // set_view switches the active tab directly — the startup resume path (the
  // host restores the view before open() kicks its fetch).
  void set_view(View v) { view_ = v; }

  // --- Render data --------------------------------------------------------
  // header_label renders the tab header ("YOUTUBE  [Search] [Playlists]
  // [Feeds]") plus the active target ("  — search: <q>" / "  — <url>").
  std::string header_label() const;
  // row_count is the active list's row count.
  int  row_count() const { return static_cast<int>(tracks_.size()); }
  // row_label renders "> label" / "  label" for row i ("Artist — Title
  //   (duration)").
  std::string row_label(int i, int panel_width) const;
  bool row_dim(int /*i*/) const { return false; }
  // footer_label renders the list source ("(cached)" when the shown list came
  // from the disk cache) plus the status ("fetching…" | the last error).
  std::string footer_label() const;
  // --- Prompt --------------------------------------------------------------
  // prompt_active reports whether the "/"-prompt of the active view is open.
  bool prompt_active() const { return prompt_active_; }
  // prompt_line renders " / search: <buf>" / "playlist URL: <buf>" /
  // "list URL: <buf>" from the live buffer.
  std::string prompt_line(int panel_width) const;
  int  cursor() const { return cursor_; }
  int  scroll() const { return scroll_; }
  void set_visible_rows(int rows);
  int  visible_rows() const { return visible_rows_; }

  // pump consumes a landed background fetch whose request key still matches
  // the current view state (stale results are dropped — a stale drop for a
  // still-empty current target refetches it, see pump). UI thread only; the
  // render path calls it every frame.
  void pump();
  bool loading() const { return loading_.load(std::memory_order_acquire); }

  // --- Resume --------------------------------------------------------------
  // resume_tab encodes the screen's state for the app's standard resume panel
  // (foundation resume.json, screen == "yt"): "<view>\x1e<target>\x1e<cursor>"
  // — the view name ("search" | "playlists" | "feeds"), the raw target the
  // view resolves, and the cursor row (so the data AND the selection survive
  // the restart; the disk cache serves the list back without a refetch when
  // its TTL survives). Empty target → just the "<view>" form.
  std::string resume_tab() const;
  // restore_tab reads the resume_tab payload back (set_view + target + cursor
  // restored; a malformed payload is ignored). The host calls it BEFORE the
  // first open() so the resumed list fetches (possibly from cache) on entry.
  void restore_tab(std::string_view tab);

  // handle_key dispatches a canonical key name; returns true when consumed.
  // While the prompt is active every printable key is consumed into the query
  // buffer (the gieres search-prompt pattern). y / p / space / - / = / + are
  // never consumed (see the header comment).
  bool handle_key(std::string_view key);

  // kCacheTtlSeconds — the on-disk list cache TTL (10 minutes).
  inline static constexpr int kCacheTtlSeconds = 600;
  // kSearchCount — how many ytsearch results one Search fetch returns.
  inline static constexpr int kSearchCount = 20;

private:
  // fetch kicks one background fetch for the current view state. The fetch is
  // single-flight; the request key snapshots the state it was launched for.
  void fetch();
  // refetch_target is the resolve string for the active view: the raw target
  // with the ytsearch:N prefix applied for Search.
  std::string current_target() const;
  // cache_key derives the on-disk cache key ("yt-search/<fp>" |
  // "yt-playlist/<fp>" | "yt-feed/<fp>") for the active view's target — the
  // first segment is the DiskCache namespace (one directory per view).
  std::string cache_key() const;
  void normalize();  // clamp cursor/scroll after list changes
  // Prompt submit/validate.
  void open_prompt();
  void submit_prompt();
  // park_view / restore_view swap the current view's target/list/cursor into
  // (or back from) its saved_ slot on a tab switch.
  void park_view(View v);
  void restore_view(View v);
  static std::size_t view_index(View v);

  // enter / a on the cursor result.
  void play_cursor();
  void append_cursor();

  ResolveFn      resolve_fn_;
  CacheGetFn     cache_get_fn_;
  CachePutFn     cache_put_fn_;
  Actions        actions_;

  bool   visible_ = false;
  View   view_    = View::Search;

  // The active target: query_ is the RAW user-submitted text (what the "/"
  // prompt re-edits across cycles); current_target() derives the resolve
  // string (ytsearch:N: prefix for Search). tracks_ is the shown list,
  // from_cache_ remembers where it came from. Each view parks its own
  // target/list/cursor in saved_ on a tab switch and reloads it on return —
  // the gieres per-view-data rule (a tab never shows another tab's list).
  std::string query_;
  std::vector<playlist::Track> tracks_;
  bool   from_cache_ = false;
  struct ViewData {
    std::string query;
    std::vector<playlist::Track> tracks;
    bool from_cache = false;
    int  cursor = 0;
    int  scroll = 0;
  };
  std::array<ViewData, 3> saved_;

  // Prompt state: prompt_active_ + buf_ (the live typing buffer).
  bool        prompt_active_ = false;
  std::string buf_;

  int cursor_       = 0;
  int scroll_       = 0;
  int visible_rows_ = 0;

  std::string status_;  // footer status ("" | "fetching…" | error)
  std::string error_;   // sticky last error (shown until the next success)

  // Background fetch lifecycle (browse/gieres pattern): the worker publishes
  // an immutable result into the atomic mailbox; pump() consumes it on the UI
  // thread. loading_ doubles as the single-flight guard.
  struct FetchResult {
    View        view;
    std::string query;   // the RAW target the fetch was launched for (staleness)
    std::string target;  // the resolve string snapshot (ytsearch:N: applied)
    std::string cache;   // the cache key it used
    bool        force = false;  // ctrl+r: skip the cache read on this fetch
    bool        ok = false;
    bool        from_cache = false;
    std::string fetch_error;
    std::vector<playlist::Track> tracks;
  };
  std::atomic<bool> loading_{false};
  std::atomic<std::shared_ptr<const FetchResult>> inbox_{nullptr};
  // refetch_force_ is ctrl+r's force-refetch latch: set by the key, consumed
  // by the next fetch that actually launches (the worker then skips the cache
  // read and re-stores a fresh copy under the same key). Atomic because ctrl+r
  // can arrive while a fetch is in flight (the CAS keeps it from launching;
  // the latch survives for the fetch the stale drop relaunches).
  std::atomic<bool> refetch_force_{false};
  std::jthread fetch_thread_;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see yt.cpp).
// Renders the tab header, the active list's scroll window and the footer.
std::shared_ptr<ftxui::ComponentBase> make_yt_component(YtModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens