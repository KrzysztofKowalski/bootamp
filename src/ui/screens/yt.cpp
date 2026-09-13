// ui/screens/yt.cpp — YouTube screen implementation (contract:
// ui/screens/yt.hpp). Fetch lifecycle follows gieres.cpp / browse.cpp
// (a jthread publishes immutable results into an atomic mailbox that the
// render path pumps); the FTXUI glue renders the model state — the shell
// claims every key, the host routes them into the model. The resolve hook
// hands the target straight to resolve::resolve_ytdl (already the browse
// net-search path), so no URL-resolution logic is duplicated here.
#include "ui/screens/yt.hpp"

#include "resolve/ytdl.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <utility>

// The on-disk list cache is foundation::DiskCache (foundation/disk_cache.hpp,
// a parallel agent's module): put(key, data, ttl_seconds) with a string-
// payload overload, get(key) -> std::optional<std::string>, evict_namespace
// (directory-granular, per first path segment), rooted at
// <XDG_CACHE_HOME>/bootamp (fallback ~/.cache/bootamp), TTL'd, atomic writes.
// for_host wires ONE shared instance; ctrl+r needs no eviction — it latches
// refetch_force_ so the next fetch skips the cache read for its target and
// re-stores a fresh copy under the same key.
#include "foundation/disk_cache.hpp"

#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

namespace {

// view_name is the resume/screen_tab spelling of a view.
std::string_view view_name(YtModel::View v) {
  switch (v) {
    case YtModel::View::Search:    return "search";
    case YtModel::View::Playlists: return "playlists";
    case YtModel::View::Feeds:     return "feeds";
  }
  return "search";
}

// view_label is the header-tab label of a view.
std::string_view view_label(YtModel::View v) {
  switch (v) {
    case YtModel::View::Search:    return "Search";
    case YtModel::View::Playlists: return "Playlists";
    case YtModel::View::Feeds:     return "Feeds";
  }
  return "Search";
}

// fmt_duration renders seconds as "3h 6m" / "12m" / "45s" (mirrors the gieres
// row style; durations are 0/unknown for some flat entries).
std::string fmt_duration(int secs) {
  if (secs <= 0) {
    return "?";
  }
  char buf[24];
  if (secs >= 3600) {
    std::snprintf(buf, sizeof buf, "%dh %dm", secs / 3600, (secs % 3600) / 60);
  } else if (secs >= 60) {
    std::snprintf(buf, sizeof buf, "%dm", secs / 60);
  } else {
    std::snprintf(buf, sizeof buf, "%ds", secs);
  }
  return buf;
}

// fnv1a64 is a small string digest used to disambiguate truncated cache keys
// (a collision is harmless: it only serves a stale list for one TTL).
std::uint64_t fnv1a64(std::string_view s) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

// hex12 renders a 12-hex-digit lowercase suffix (64-bit digest, disambiguation
// margin).
std::string hex12(std::uint64_t v) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(12, '0');
  for (int i = 11; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[v & 0xF];
    v >>= 4;
  }
  return out;
}

// thumbprint turns a resolve target into a filesystem-safe cache-key tail:
// allowed chars stay, everything else (including '/', which would read as a
// directory separator to a path-based cache) becomes '_' and the whole key is
// truncated to 96 chars with a hash suffix so truncation never collides.
// "yt-<view>/<thumbprint>" (cache_key) is the key for DiskCache::put/get.
std::string thumbprint(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const bool keep =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~' || c == '@' || c == ':' || c == '!' || c == '$' ||
        c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' ||
        c == '+' || c == ',' || c == '=' || c == '?';
    out.push_back(keep ? c : '_');
  }
  if (out.size() > 96) {
    out.erase(96);
  }
  out += "-";
  out += hex12(fnv1a64(s));
  return out;
}

// trim_space trims ASCII whitespace from a string (url.go's TrimSpace for the
// pasted targets, which are URLs/queries — ASCII in practice).
std::string trim_space(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' ||
                   s[b] == '\r' || s[b] == '\v' || s[b] == '\f')) {
    ++b;
  }
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                   s[e - 1] == '\r' || s[e - 1] == '\v' || s[e - 1] == '\f')) {
    --e;
  }
  return std::string(s.substr(b, e - b));
}

// ---------------------------------------------------------------------------
// Cache serialization
// ---------------------------------------------------------------------------
// The cache stores the RESOLVED list as JSON: one {"v":1,"tracks":[…]}
// document whose entries carry exactly the fields resolve_ytdl fills
// (path/title/artist/duration_secs/stream). The build side uses the real
// nlohmann json API (basic_json::array()'s absence aside, push_back and
// assignment from std::vector<json> are the installed header's surface —
// verified in /usr/include/nlohmann/json.hpp); the parse side uses only the
// in-tree patterns (catalog.cpp / gieres): parse → is_object/is_array →
// iterate with value(key, default). A missing "v" (or a parse error) makes
// the read a NO-OP (the caller refetches).

std::string serialize_tracks(const std::vector<playlist::Track>& tracks) {
  nlohmann::json doc;
  doc["v"] = 1;
  std::vector<nlohmann::json> arr;
  arr.reserve(tracks.size());
  for (const playlist::Track& t : tracks) {
    nlohmann::json e;
    e["path"] = t.path;
    if (!t.title.empty()) {
      e["title"] = t.title;
    }
    if (!t.artist.empty()) {
      e["artist"] = t.artist;
    }
    e["duration_secs"] = t.duration_secs;
    e["stream"]        = t.stream;
    arr.push_back(std::move(e));
  }
  doc["tracks"] = std::move(arr);
  return doc.dump();
}

std::expected<std::vector<playlist::Track>, std::string> parse_cached_tracks(
    std::string_view data) {
  try {
    const nlohmann::json j = nlohmann::json::parse(std::string{data});
    if (!j.is_object() || j.value("v", 0) != 1 || !j.contains("tracks")) {
      return std::unexpected{"yt cache: unknown format"};
    }
    const nlohmann::json& list = j.at("tracks");
    if (!list.is_array()) {
      return std::unexpected{"yt cache: tracks is not an array"};
    }
    std::vector<playlist::Track> out;
    for (const nlohmann::json& el : list) {
      if (!el.is_object()) {
        return std::unexpected{"yt cache: entry is not an object"};
      }
      const std::string path = el.value("path", std::string{});
      if (path.empty()) {
        continue;  // a defensively-skipped entry, like parse_ytdl_tracks'
      }
      playlist::Track t;
      t.path         = path;
      t.title        = el.value("title", std::string{});
      t.artist       = el.value("artist", std::string{});
      t.duration_secs = el.value("duration_secs", 0);
      t.stream       = el.value("stream", false);
      out.push_back(std::move(t));
    }
    return out;  // expected converts the value (resolve/ytdl.cpp idiom)
  } catch (const nlohmann::json::exception&) {
    return std::unexpected{"yt cache: bad JSON"};
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

YtModel::YtModel(ResolveFn resolve, Actions actions)
    : resolve_fn_(std::move(resolve)), actions_(std::move(actions)) {}

YtModel::YtModel(ResolveFn resolve, CacheGetFn get, CachePutFn put,
                 Actions actions)
    : resolve_fn_(std::move(resolve)),
      cache_get_fn_(std::move(get)),
      cache_put_fn_(std::move(put)),
      actions_(std::move(actions)) {}

YtModel::~YtModel() {
  if (fetch_thread_.joinable()) {
    fetch_thread_.request_stop();
    fetch_thread_.join();
  }
}

YtModel YtModel::for_host() {
  // ONE shared DiskCache for the screen's list cache (thread-safe internally;
  // the fetch worker reads/writes it while the UI thread only latches ctrl+r).
  // The model is non-copyable (the gieres shape), so it is built as a prvalue
  // with the cache hooks already capturing the shared cache.
  const auto cache = std::make_shared<foundation::DiskCache>();
  return YtModel(
      [](std::string_view target) { return resolve::resolve_ytdl(target); },
      [cache](std::string_view key) -> std::optional<std::string> {
        return cache->get(key);
      },
      [cache](std::string_view key, std::string_view data,
              std::int64_t ttl_seconds) {
        (void)cache->put(key, data, ttl_seconds);
      });
}

void YtModel::set_cache_hooks(const CacheGetFn get, const CachePutFn put) {
  cache_get_fn_ = std::move(get);
  cache_put_fn_ = std::move(put);
}

void YtModel::open() {
  visible_ = true;
  // Kick the pending first fetch (idempotent — a loaded list stays put;
  // ctrl+r is the explicit refetch). A restored target (resume) fetches on
  // the first open; the disk cache serves it back when the TTL survived.
  if (!query_.empty() && tracks_.empty() && !loading()) {
    fetch();
  }
}

void YtModel::normalize() {
  const int n = row_count();
  if (n <= 0) {
    cursor_ = 0;
    scroll_ = 0;
    return;
  }
  cursor_ = std::clamp(cursor_, 0, n - 1);
  if (visible_rows_ > 0) {
    if (cursor_ < scroll_) {
      scroll_ = cursor_;
    } else if (cursor_ >= scroll_ + visible_rows_) {
      scroll_ = cursor_ - visible_rows_ + 1;
    }
    scroll_ = std::clamp(scroll_, 0, std::max(0, n - visible_rows_));
  } else {
    scroll_ = 0;
  }
}

void YtModel::set_visible_rows(const int rows) {
  visible_rows_ = std::max(rows, 0);
  normalize();
}

// view_index maps a View to its saved_ slot (the enum values are 0..2; the
// explicit mapping keeps the array indexing independent of the enum layout).
std::size_t YtModel::view_index(const View v) {
  switch (v) {
    case View::Search:    return 0;
    case View::Playlists: return 1;
    case View::Feeds:     return 2;
  }
  return 0;
}

void YtModel::park_view(const View v) {
  ViewData& s = saved_[view_index(v)];
  s.query      = std::move(query_);
  s.tracks     = std::move(tracks_);
  s.from_cache = from_cache_;
  s.cursor     = cursor_;
  s.scroll     = scroll_;
}

void YtModel::restore_view(const View v) {
  const ViewData& s = saved_[view_index(v)];
  if (!s.query.empty() || !s.tracks.empty()) {
    query_      = s.query;
    tracks_     = s.tracks;
    from_cache_ = s.from_cache;
    cursor_     = s.cursor;
    scroll_     = s.scroll;
    return;
  }
  // First visit: a fresh view with no target and no list.
  query_.clear();
  tracks_.clear();
  from_cache_ = false;
  cursor_     = 0;
  scroll_     = 0;
}

std::string YtModel::current_target() const {
  if (view_ == View::Search) {
    return "ytsearch:" + std::to_string(kSearchCount) + ":" + query_;
  }
  return query_;  // Playlists / Feeds: query_ IS the pasted URL
}

std::string YtModel::cache_key() const {
  // The first segment is the DiskCache namespace (one subdirectory) — one per
  // VIEW so a future evict_namespace("yt-search") purges exactly that view;
  // the tail (thumbprint) disambiguates the targets within it.
  const std::string_view ns =
      view_ == View::Search      ? "yt-search"
      : view_ == View::Playlists ? "yt-playlist"
                                 : "yt-feed";
  return std::string{ns} + "/" + thumbprint(current_target());
}

void YtModel::fetch() {
  bool expected = false;
  if (!loading_.compare_exchange_strong(expected, true)) {
    return;  // single-flight: a fetch is already in flight
  }
  // Snapshot the request key; pump() drops results whose key no longer
  // matches (the user submitted a different target meanwhile). The resolve
  // string (ytsearch prefix applied for Search) is snapshotted too — the
  // worker must not re-derive it from the live members.
  auto key = std::make_shared<FetchResult>();
  key->view   = view_;
  key->query  = query_;
  key->target = current_target();
  key->cache  = cache_key();
  // ctrl+r's force-refetch latch (consumed by THIS fetch — a fetch skipped by
  // the single-flight CAS leaves it set for the fetch the stale drop launches).
  key->force = refetch_force_.exchange(false);

  status_ = "fetching…";
  if (fetch_thread_.joinable()) {
    fetch_thread_.request_stop();
    fetch_thread_.join();
  }
  fetch_thread_ = std::jthread([this, key](std::stop_token stoken) {
    if (stoken.stop_requested()) {
      // A stop before the body ran means no one will publish into inbox_, so
      // pump() never clears loading_ — release the single-flight latch here
      // or the model stays "loading" forever (the gieres latch-release rule).
      loading_.store(false, std::memory_order_release);
      return;
    }
    auto result = std::make_shared<FetchResult>(*key);
    // 1. On-disk cache (short TTL): repeated opens of the same list don't
    //    refetch. A cache hit is served as-is; a miss (or a store we cannot
    //    parse) falls through to a real resolve. ctrl+r's force latch skips
    //    the read so the resolve always re-runs.
    if (!key->force && cache_get_fn_ && !key->query.empty()) {
      if (const auto hit = cache_get_fn_(key->cache)) {
        auto cached = parse_cached_tracks(std::string_view{*hit});
        if (cached && !cached->empty()) {
          result->ok         = true;
          result->tracks     = std::move(*cached);
          result->from_cache = true;
          inbox_.store(std::move(result), std::memory_order_release);
          return;
        }
      }
    }
    // 2. Resolve (resolve::resolve_ytdl in for_host — the browse net-search
    //    path; no new yt-dlp flags, just the target string). A clean run that
    //    returned zero tracks is an error, mirroring main.cpp resolve_pending.
    auto got = resolve_fn_(key->target);
    if (got) {
      if (got->empty()) {
        result->fetch_error = "yt-dlp resolved no tracks for " + key->target;
      } else {
        result->ok = true;
        result->tracks = std::move(*got);
        if (cache_put_fn_ && !key->query.empty()) {
          // Store the resolved list (a forced refetch re-stores it under the
          // same key, refreshing the TTL).
          cache_put_fn_(key->cache, serialize_tracks(result->tracks),
                        kCacheTtlSeconds);
        }
      }
    } else {
      result->fetch_error = std::move(got).error();
    }
    inbox_.store(std::move(result), std::memory_order_release);
  });
}

void YtModel::pump() {
  const auto result = inbox_.exchange(nullptr, std::memory_order_acquire);
  if (!result) {
    return;
  }
  // The worker finished — clear the single-flight flag before any staleness
  // decision (a dropped result must not wedge loading_ on).
  loading_.store(false, std::memory_order_release);
  if (result->view != view_ || result->query != query_) {
    // Stale fetch (the user submitted a different target): drop silently —
    // but a stale drop must not leave a pending target waiting forever: when
    // the current target still has nothing shown and nothing is in flight,
    // launch it (the gieres stale-drop refetch, generalized to every view).
    if (!query_.empty() && tracks_.empty() && !loading()) {
      fetch();
    }
    return;
  }
  if (!result->ok) {
    error_  = result->fetch_error;
    status_ = error_;
    return;
  }
  status_.clear();
  error_.clear();
  tracks_     = std::move(result->tracks);
  from_cache_ = result->from_cache;
  normalize();
}

// ---------------------------------------------------------------------------
// Prompt
// ---------------------------------------------------------------------------

void YtModel::open_prompt() {
  // '/' keeps the previous entry in the buffer so it can be re-edited (the
  // gieres search-prompt rule).
  prompt_active_ = true;
  buf_.clear();
}

void YtModel::submit_prompt() {
  const std::string entry = trim_space(buf_);
  if (entry.empty()) {
    return;  // empty submit stays open (the url.go rule)
  }
  query_        = std::move(entry);
  prompt_active_ = false;
  cursor_       = 0;
  scroll_       = 0;
  tracks_.clear();
  normalize();
  fetch();
}

// ---------------------------------------------------------------------------
// Playback mapping
// ---------------------------------------------------------------------------

void YtModel::play_cursor() {
  const std::size_t idx = static_cast<std::size_t>(cursor_);
  if (idx >= tracks_.size() || !actions_.on_play_track) {
    return;
  }
  actions_.on_play_track(tracks_[idx]);
}

void YtModel::append_cursor() {
  const std::size_t idx = static_cast<std::size_t>(cursor_);
  if (idx >= tracks_.size() || !actions_.on_append_track) {
    return;
  }
  actions_.on_append_track(tracks_[idx]);
}

// ---------------------------------------------------------------------------
// Resume
// ---------------------------------------------------------------------------

std::string YtModel::resume_tab() const {
  // "<view>" — the same shape the gieres screen's screen_tab uses; for a
  // target the query + cursor ride along, record-separator framed.
  std::string out{view_name(view_)};
  if (query_.empty()) {
    return out;
  }
  out += '\x1e';
  out += query_;
  out += '\x1e';
  out += std::to_string(cursor_);
  return out;
}

void YtModel::restore_tab(const std::string_view tab) {
  const std::size_t sep1 = tab.find('\x1e');
  const std::string_view view_s =
      sep1 == std::string_view::npos ? tab : tab.substr(0, sep1);
  if (view_s == "playlists") {
    view_ = View::Playlists;
  } else if (view_s == "feeds") {
    view_ = View::Feeds;
  } else if (view_s == "search" || view_s.empty()) {
    view_ = View::Search;
  } else {
    return;  // unknown payload — leave the defaults alone
  }
  if (sep1 == std::string_view::npos) {
    return;
  }
  const std::size_t sep2 = tab.find('\x1e', sep1 + 1);
  query_           = std::string{tab.substr(sep1 + 1, sep2 == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : sep2 - sep1 - 1)};
  prompt_active_   = false;
  if (sep2 != std::string_view::npos) {
    const std::string_view c = tab.substr(sep2 + 1);
    int row = -1;
    if (std::sscanf(std::string{c}.c_str(), "%d", &row) == 1 && row >= 0) {
      cursor_ = row;  // normalize() clamps it when the resumed list lands
    }
  }
  scroll_ = 0;
}

// ---------------------------------------------------------------------------
// Row rendering
// ---------------------------------------------------------------------------

std::string YtModel::row_label(const int i, const int /*panel_width*/) const {
  const std::string prefix = i == cursor_ ? "> " : "  ";
  const std::size_t idx    = static_cast<std::size_t>(i);
  if (idx >= tracks_.size()) {
    return prefix;
  }
  const playlist::Track& t = tracks_[idx];
  std::string label = t.artist.empty() ? t.title
                                       : t.artist + " — " + t.title;
  if (label.empty()) {
    label = t.path;
  }
  if (t.duration_secs > 0) {
    label += "  (" + fmt_duration(t.duration_secs) + ")";
  }
  return prefix + label;
}

std::string YtModel::header_label() const {
  const auto tab = [this](View v, std::string_view name) {
    return view_ == v ? "[" + std::string{name} + "]"
                      : " " + std::string{name} + " ";
  };
  std::string head = std::string{"YOUTUBE"} + "  " +
                     tab(View::Search, "Search") + " " +
                     tab(View::Playlists, "Playlists") + " " +
                     tab(View::Feeds, "Feeds");
  if (!query_.empty()) {
    if (view_ == View::Search) {
      head += "  — search: " + query_;
    } else {
      head += "  — " + query_;
    }
  } else {
    head += "  ( / opens the " + std::string{view_label(view_)} + " prompt)";
  }
  return head;
}

std::string YtModel::footer_label() const {
  std::string foot = from_cache_ ? "cached list" : "yt-dlp";
  if (query_.empty()) {
    foot = "no target yet";
  }
  if (!status_.empty()) {
    foot += "  " + status_;
  }
  return foot;
}

std::string YtModel::prompt_line(const int /*panel_width*/) const {
  std::string label =
      view_ == View::Search    ? "/ search: "
      : view_ == View::Playlists ? "playlist URL: "
                                 : "list URL: ";
  return label + buf_;
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

bool YtModel::handle_key(const std::string_view key) {
  // The prompt swallows every key while active (the gieres search-prompt
  // rule) — including y/p/space, which only fall through when NOT typing.
  if (prompt_active_) {
    if (key == "enter") {
      submit_prompt();
      return true;
    }
    if (key == "esc") {
      prompt_active_ = false;
      return true;
    }
    if (key == "backspace") {
      if (!buf_.empty()) {
        buf_.pop_back();
      }
      return true;
    }
    if (key == "ctrl+u") {
      buf_.clear();
      return true;
    }
    if (key == "space") {
      buf_ += " ";
      return true;
    }
    if (key.size() == 1 && key[0] >= 0x21 && key[0] <= 0x7E) {
      buf_ += key;
    }
    return true;  // modal while typing
  }

  // Explicit fallthroughs (the gieres rule): y/p toggle this screen and the
  // Gieres screen at the host (the same key that opened a screen closes it,
  // DevicePicker-style), space is the global play/pause, and the volume keys
  // - / = / + are the global ones.
  if (key == "y" || key == "p" || key == "space") {
    return false;
  }
  if (key == "-" || key == "=" || key == "+") {
    return false;
  }

  if (key == "/") {
    open_prompt();
    return true;
  }

  if (key == "tab" || key == "shift+tab") {
    const int delta = key == "tab" ? 1 : -1;
    const View old  = view_;
    view_ = static_cast<View>((static_cast<int>(view_) + delta + 3) % 3);
    // Park the old view's target/list/cursor and reload the new view's (the
    // gieres per-view-data rule — a tab never shows another tab's list).
    park_view(old);
    restore_view(view_);
    prompt_active_ = false;
    normalize();
    // A view with a target but nothing shown fetches on the spot (the
    // tab-equivalent of open()'s pending check).
    if (!query_.empty() && tracks_.empty() && !loading()) {
      fetch();
    }
    return true;
  }
  if (key == "esc") {
    visible_ = false;  // the only level left once the prompt is closed
    return true;
  }
  if (key == "up" || key == "k") {
    const int n = row_count();
    cursor_ = n > 0 ? (cursor_ + n - 1) % n : 0;  // wrap (Go providerMoveUp)
    normalize();
    return true;
  }
  if (key == "down" || key == "j") {
    const int n = row_count();
    cursor_ = n > 0 ? (cursor_ + 1) % n : 0;
    normalize();
    return true;
  }
  if (key == "pageup" || key == "pgup") {
    cursor_ -= std::min(cursor_, std::max(1, visible_rows_));
    normalize();
    return true;
  }
  if (key == "pagedown" || key == "pgdown") {
    const int n = row_count();
    cursor_ += std::min(n - 1 - cursor_, std::max(1, visible_rows_));
    normalize();
    return true;
  }
  if (key == "home" || key == "g") {
    cursor_ = 0;
    normalize();
    return true;
  }
  if (key == "end" || key == "G") {
    cursor_ = std::max(0, row_count() - 1);
    normalize();
    return true;
  }
  if (key == "enter") {
    play_cursor();
    return true;
  }
  if (key == "a") {
    append_cursor();
    return true;
  }
  if (key == "ctrl+r") {
    // Force-refetch: latch the skip-cache flag and fetch — the next launched
    // fetch skips the cache read for this target and re-stores a fresh copy
    // under the same key (no eviction needed; DiskCache's evict_namespace is
    // directory-granular and would purge the whole view's lists).
    refetch_force_.store(true, std::memory_order_release);
    error_.clear();
    status_.clear();
    if (!loading()) {
      fetch();
    }
    return true;
  }
  // left/right are never consumed — the global player seek (the gieres
  // archive rule). Everything else falls through to the global player table
  // (volume/skip/repeat/shuffle/stop/mono/speed/queue/remove/esc-to-main...).
  return false;
}

// ---------------------------------------------------------------------------
// FTXUI component
// ---------------------------------------------------------------------------

#if BOOTAMP_HAS_FTXUI

std::shared_ptr<ftxui::ComponentBase> make_yt_component(YtModel& model) {
  return ftxui::Renderer([&model] {
    model.pump();  // landed background fetches apply before this render
    using namespace ftxui;
    std::vector<Element> lines;
    lines.push_back(text(model.header_label()));
    if (model.prompt_active()) {
      lines.push_back(text(model.prompt_line(0)));  // display-only prompt
    }
    lines.push_back(separator());
    const int count = model.row_count();
    const int shown = model.visible_rows() > 0
                          ? std::min(count - model.scroll(), model.visible_rows())
                          : count;
    for (int i = 0; i < shown; ++i) {
      const int idx = model.scroll() + i;
      auto line = text(model.row_label(idx, 0));
      if (model.row_dim(idx)) {
        line = dim(std::move(line));
      }
      lines.push_back(std::move(line));
    }
    if (model.loading()) {
      lines.push_back(dim(text("  fetching…")));
    } else if (count == 0) {
      lines.push_back(dim(text("  (nothing here — / opens the prompt, "
                               "ctrl+r refetches)")));
    }
    lines.push_back(separator());
    lines.push_back(dim(text(model.footer_label())));
    return vbox(std::move(lines));
  });
}

#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens