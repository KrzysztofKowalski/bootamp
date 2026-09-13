// ui/screens/gieres.cpp — Gieres archive screen implementation (contract:
// ui/screens/gieres.hpp). Fetch lifecycle follows browse.cpp (a jthread
// publishes immutable results into an atomic mailbox that the render path
// pumps); the FTXUI glue renders the model state — the shell claims every
// key, the host routes them into the model.
#include "ui/screens/gieres.hpp"

#include "foundation/applog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <utility>

#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

namespace {

// The archive constants from docs/orgonity-api.md (section 6): the live
// stream lives on the external RadioBoss host, not on the LAN server.
inline constexpr std::string_view kLiveStreamUrl =
    "https://c16.radioboss.fm:18014/stream";
inline constexpr std::string_view kStationName = "Radio Radio";

// date_string renders a civil date as YYYY-MM-DD.
std::string date_string(const std::chrono::year_month_day ymd) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                static_cast<unsigned>(ymd.day()));
  return buf;
}

// parse_ymd reads "YYYY-MM-DD" (ok() == false on garbage). The failure value
// is an explicit out-of-range date — the default constructor leaves the
// fields uninitialized.
std::chrono::year_month_day parse_ymd(std::string_view s) {
  using namespace std::chrono;
  const auto bad = year_month_day{year{0} / month{0} / day{0}};
  int y = 0, m = 0, d = 0;
  if (s.size() != 10 || s[4] != '-' || s[7] != '-' ||
      std::sscanf(std::string{s}.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
    return bad;
  }
  const auto ymd = year_month_day{year{y} / month{static_cast<unsigned>(m)} /
                                  day{static_cast<unsigned>(d)}};
  return ymd.ok() ? ymd : bad;
}

// hh_mm extracts "HH:MM" from an ISO8601 instant ("…T20:00:00Z") — the raw
// UTC digits, local_hh_mm's fallback.
std::string hh_mm(std::string_view iso) {
  return iso.size() >= 16 ? std::string{iso.substr(11, 5)} : std::string{};
}

// local_zone is the machine's timezone (current_zone, cached — the call
// consults TZ each time; a session-stable zone is fine). Null when the tz
// database is unavailable — everything degrades to plain UTC then.
const std::chrono::time_zone* local_zone() {
  static const std::chrono::time_zone* zone =
      [] -> const std::chrono::time_zone* {
    try {
      return std::chrono::current_zone();
    } catch (const std::exception&) {
      return nullptr;
    }
  }();
  return zone;
}

// format_instant_z renders a UTC time_point in the server's ISO8601 shape
// ("YYYY-MM-DDTHH:MM:SSZ").
std::string format_instant_z(std::chrono::sys_seconds tp) {
  const auto day = std::chrono::floor<std::chrono::days>(tp);
  const auto tod = std::chrono::hh_mm_ss{tp - day};
  char buf[16];  // "HH:MM:SS" is 8 chars + NUL; sized like fmt_clock's
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d",
                static_cast<int>(tod.hours().count()),
                static_cast<int>(tod.minutes().count()),
                static_cast<int>(tod.seconds().count()));
  return date_string(std::chrono::year_month_day{day}) + "T" + buf + "Z";
}

// parse_utc_instant reads the server's "YYYY-MM-DDTHH:MM:SSZ" shape
// (nullopt on anything else).
std::optional<std::chrono::sys_seconds> parse_utc_instant(
    std::string_view iso) {
  if (iso.size() != 20 || iso[10] != 'T' || iso[19] != 'Z') {
    return std::nullopt;
  }
  const auto ymd = parse_ymd(iso.substr(0, 10));
  if (!ymd.ok()) {
    return std::nullopt;
  }
  int hh = 0, mm = 0, ss = 0;
  if (std::sscanf(std::string{iso.substr(11, 8)}.c_str(), "%d:%d:%d",
                  &hh, &mm, &ss) != 3 ||
      hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) {
    return std::nullopt;
  }
  return std::chrono::sys_seconds{std::chrono::sys_days{ymd}} +
         std::chrono::hours{hh} + std::chrono::minutes{mm} +
         std::chrono::seconds{ss};
}

// today_date is the machine-LOCAL calendar date of "now" — the Echelon
// calendar's newest day and the next-day step's clamp (the user reads
// Warsaw time; the archive's show dates are Warsaw-local too). Plain UTC
// date without a tz database.
std::chrono::year_month_day today_date() {
  if (const auto* zone = local_zone()) {
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(
        zone->to_local(std::chrono::system_clock::now()))};
  }
  return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(
      std::chrono::system_clock::now())};
}

// local_hh_mm renders an ISO8601 UTC instant as machine-local HH:MM —
// segments' aired_at is UTC, the user reads Warsaw time (00:00Z airs at
// 02:00 local, not midnight). Raw UTC digits are the fallback (malformed
// instant or no tz database).
std::string local_hh_mm(std::string_view iso) {
  const auto* zone = local_zone();
  const auto tp = parse_utc_instant(iso);
  if (!zone || !tp) {
    return hh_mm(iso);
  }
  const auto local = zone->to_local(*tp);
  const auto tod = std::chrono::hh_mm_ss{
      local - std::chrono::floor<std::chrono::days>(local)};
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02d:%02d",
                static_cast<int>(tod.hours().count()),
                static_cast<int>(tod.minutes().count()));
  return buf;
}

// kTimelineFirstProbe is the earliest day the Echelon date-index bisection
// probes (the timeline began 2026-05; the bound only adds bisection steps of
// headroom in case older segments ever get backfilled).
inline constexpr std::chrono::year_month_day kTimelineFirstProbe{
    std::chrono::year{2009}, std::chrono::January, std::chrono::day{1}};

// fmt_duration renders seconds as "3h 6m" / "12m" / "45s" ("?" unknown).
std::string fmt_duration(int secs) {
  if (secs < 0) {
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

// fmt_clock renders seconds as mm:ss (h:mm:ss past an hour) — program-listing
// positions.
std::string fmt_clock(int secs) {
  if (secs < 0) {
    return "?";
  }
  char buf[16];
  if (secs >= 3600) {
    std::snprintf(buf, sizeof buf, "%d:%02d:%02d", secs / 3600,
                  (secs % 3600) / 60, secs % 60);
  } else {
    std::snprintf(buf, sizeof buf, "%02d:%02d", secs / 60, secs % 60);
  }
  return buf;
}

// broadcast_row renders one recording line: date, title, duration, and the
// no-audio marker for rows the audio file would 404 on.
std::string broadcast_row(const GieresBroadcast& b) {
  std::string label = b.recording_date.empty() ? std::string{"----------"}
                                               : b.recording_date;
  label += "  ";
  label += b.title;
  label += "  (";
  label += fmt_duration(b.duration_seconds);
  label += ")";
  if (!b.has_audio) {
    label += "  [no audio]";
  }
  return label;
}

}  // namespace

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

GieresModel::GieresModel(OrgonityFn orgonity, ByDateFn by_date,
                         SegmentsFn segments, NowPlayingFn now_playing,
                         std::string_view base_url, Actions actions,
                         std::shared_ptr<GieresActiveBase> active_base)
    : orgonity_fn_(std::move(orgonity)),
      by_date_fn_(std::move(by_date)),
      segments_fn_(std::move(segments)),
      now_playing_fn_(std::move(now_playing)),
      actions_(std::move(actions)),
      base_url_(base_url),
      active_base_(std::move(active_base)) {
  // Echelon starts on today's LOCAL day (the calendar and its windows are
  // local — see gieres_day_window).
  ech_date_ = date_string(today_date());
}

GieresModel::~GieresModel() {
  if (fetch_thread_.joinable()) {
    fetch_thread_.request_stop();
    fetch_thread_.join();
  }
  if (dates_thread_.joinable()) {
    dates_thread_.request_stop();
    dates_thread_.join();
  }
  if (chain_thread_.joinable()) {
    chain_thread_.request_stop();
    chain_thread_.join();
  }
}

// gieres_is_transport_error — see gieres.hpp. Transport failures (dial/DNS/
// connect/timeout/TLS) deserve the fallback retry; HTTP status errors
// ("gieres: /path HTTP 404", "http status 404 ...") and JSON-parse errors
// ("body is not ...") mean the server answered.
bool gieres_is_transport_error(const std::string_view err) {
  return err.find("HTTP") == std::string_view::npos &&
         err.find("http status") == std::string_view::npos &&
         err.find("body") == std::string_view::npos;
}

GieresFallbackHooks gieres_with_fallback(GieresFallbackHooks primary,
                                         GieresFallbackHooks secondary,
                                         const std::shared_ptr<GieresActiveBase>&
                                             active) {
  // Shared shape for all four hook kinds: try the first-tried endpoint; on a
  // transport error retry the other once; a winner is recorded into `active`
  // ("" = the primary base is authoritative again). Once the secondary has
  // won, it is tried first (sticky — the LAN being down must not cost a full
  // connect timeout on every fetch), with the primary as its own fallback.
  const std::string base1 = std::move(primary.base);
  const std::string base2 = std::move(secondary.base);

  GieresFallbackHooks hooks;
  hooks.base = base1;
  hooks.orgonity =
      [org = std::move(primary.orgonity), alt = std::move(secondary.orgonity),
        active, base2](int page, std::string_view q,
                              std::string_view sort) mutable
      -> std::expected<GieresListing, std::string> {
    if (active && active->get() == base2) {
      auto s = alt(page, q, sort);
      if (s) {
        return s;
      }
      auto r = org(page, q, sort);
      if (r && active) {
        active->set(std::string{});  // the LAN came back — primary wins again
      }
      return r;
    }
    auto r = org(page, q, sort);
    if (r || !gieres_is_transport_error(r.error())) {
      return r;
    }
    auto s = alt(page, q, sort);
    if (s && active) {
      active->set(base2);
    }
    return s ? std::move(s) : std::move(r);
  };
  hooks.by_date = [by = std::move(primary.by_date),
                   alt = std::move(secondary.by_date), active, base2](std::string_view date) mutable
      -> std::expected<GieresDay, std::string> {
    if (active && active->get() == base2) {
      auto s = alt(date);
      if (s) {
        return s;
      }
      auto r = by(date);
      if (r && active) {
        active->set(std::string{});
      }
      return r;
    }
    auto r = by(date);
    if (r || !gieres_is_transport_error(r.error())) {
      return r;
    }
    auto s = alt(date);
    if (s && active) {
      active->set(base2);
    }
    return s ? std::move(s) : std::move(r);
  };
  hooks.segments =
      [seg = std::move(primary.segments), alt = std::move(secondary.segments),
        active, base2](std::string_view start, std::string_view end) mutable
      -> std::expected<std::vector<GieresSegment>, std::string> {
    if (active && active->get() == base2) {
      auto s = alt(start, end);
      if (s) {
        return s;
      }
      auto r = seg(start, end);
      if (r && active) {
        active->set(std::string{});
      }
      return r;
    }
    auto r = seg(start, end);
    if (r || !gieres_is_transport_error(r.error())) {
      return r;
    }
    auto s = alt(start, end);
    if (s && active) {
      active->set(base2);
    }
    return s ? std::move(s) : std::move(r);
  };
  hooks.now_playing = [np = std::move(primary.now_playing),
                        alt = std::move(secondary.now_playing), active, base2]() mutable -> std::expected<std::string,
                                                          std::string> {
    if (active && active->get() == base2) {
      auto s = alt();
      if (s) {
        return s;
      }
      auto r = np();
      if (r && active) {
        active->set(std::string{});
      }
      return r;
    }
    auto r = np();
    if (r || !gieres_is_transport_error(r.error())) {
      return r;
    }
    auto s = alt();
    if (s && active) {
      active->set(base2);
    }
    return s ? std::move(s) : std::move(r);
  };
  return hooks;
}

GieresModel GieresModel::for_host(std::string_view base_url) {
  // One client per screen; audio::HttpClient is stateless per request, so
  // value-capturing it into each hook is safe. The lambdas are mutable: the
  // client's fetch methods are deliberately non-const, and a by-value capture
  // is const inside a non-mutable lambda (a mutable lambda still converts to
  // std::function).
  //
  // Both endpoint clients share ONE disk cache (the cap then accounts the
  // whole "gieres" namespace together; keys embed the endpoint base, so the
  // LAN and public URLs never alias).
  const auto make_hooks = [](const std::string& base,
                             std::shared_ptr<foundation::DiskCache> cache) {
    GieresClient client(base, std::move(cache));
    return GieresFallbackHooks{
        .base = base,
        .orgonity =
            [client](int page, std::string_view q,
                     std::string_view sort) mutable {
              return client.orgonity(page, q, sort);
            },
        .by_date =
            [client](std::string_view date) mutable {
              return client.by_date(date);
            },
        .segments =
            [client](std::string_view start, std::string_view end) mutable {
              return client.echelon_segments(start, end);
            },
        .now_playing =
            [client]() mutable { return client.now_playing(); },
    };
  };
  const std::string primary{base_url};
  // The public domain (docs/orgonity-api.md §1 — same Rails server) is the
  // sticky fallback for the LAN: off the home network the LAN base only
  // times out, so a transport error flips fetches to the public endpoint and
  // they stick there for the session (pump() adopts it into base_url_).
  const std::string fallback{"https://gieres.cytr.us"};
  const auto cache = std::make_shared<foundation::DiskCache>();
  auto hooks = make_hooks(primary, cache);
  if (fallback == primary) {
    return GieresModel(std::move(hooks.orgonity), std::move(hooks.by_date),
                       std::move(hooks.segments), std::move(hooks.now_playing),
                       base_url);
  }
  const auto active = std::make_shared<GieresActiveBase>();
  hooks = gieres_with_fallback(std::move(hooks), make_hooks(fallback, cache),
                               active);
  // Built as a prvalue: GieresModel is non-copyable (fetch thread + deleted
  // copy ctor), so the return object must be constructed in place.
  return GieresModel(std::move(hooks.orgonity), std::move(hooks.by_date),
                     std::move(hooks.segments), std::move(hooks.now_playing),
                     base_url, Actions{}, active);
}

void GieresModel::open() {
  visible_ = true;
  // Kick the pending first fetches (idempotent — reopening after the data
  // landed is a no-op; ctrl+r is the explicit refetch).
  const bool pending =
      (view_ == View::Live && now_playing_.empty()) ||
      (view_ == View::Orgonity && recordings_.empty() && dates_.empty() &&
       day_.broadcasts.empty()) ||
      (view_ == View::Echelon && segments_.empty());
  if (pending && !loading()) {
    fetch();
  }
}

int GieresModel::list_count() const {
  switch (view_) {
    case View::Live:
      return 1;  // the live stream row
    case View::Orgonity:
      switch (org_view_) {
        case OrgView::List:  return static_cast<int>(recordings_.size());
        case OrgView::Dates: return static_cast<int>(dates_.size());
        case OrgView::Day:   return static_cast<int>(day_.broadcasts.size());
      }
      return 0;
    case View::Echelon:
      // The Echelon date index (d): one row per timeline day — the calendar
      // spans the timeline's first day through today (the server has no
      // segments before its first day, probed by fetch_dates); with no
      // calendar loaded yet a single hint row (the index self-loads via
      // fetch_dates(), so the hint shows only while that fetch is in flight
      // — or after it failed, the error surfaces in the footer status).
      if (ech_dates_) {
        return ech_dates_list_.empty()
                   ? 1
                   : static_cast<int>(ech_dates_list_.size());
      }
      break;
  }
  // Echelon flat rows: every segment plus the expanded segment's programs.
  int rows = 0;
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    rows += 1;
    if (expanded_seg_ >= 0 && static_cast<int>(i) == expanded_seg_) {
      rows += static_cast<int>(segments_[i].tracks.size());
    }
  }
  return rows;
}

std::pair<int, int> GieresModel::echelon_pos(const int flat) const {
  int row = 0;
  for (std::size_t s = 0; s < segments_.size(); ++s) {
    if (row == flat) {
      return {static_cast<int>(s), -1};
    }
    row += 1;
    if (expanded_seg_ >= 0 && static_cast<int>(s) == expanded_seg_) {
      const int n = static_cast<int>(segments_[s].tracks.size());
      if (flat < row + n) {
        return {static_cast<int>(s), flat - row};
      }
      row += n;
    }
  }
  return {-1, -1};
}

int GieresModel::echelon_flat(const int segment, const int track) const {
  int row = 0;
  for (std::size_t s = 0; s < segments_.size() && static_cast<int>(s) <= segment; ++s) {
    if (static_cast<int>(s) == segment) {
      return track >= 0 ? row + 1 + track : row;
    }
    row += 1;
    if (expanded_seg_ >= 0 && static_cast<int>(s) == expanded_seg_) {
      row += static_cast<int>(segments_[s].tracks.size());
    }
  }
  return 0;
}

int GieresModel::row_count() const { return list_count(); }

void GieresModel::normalize() {
  const int n = list_count();
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

void GieresModel::set_visible_rows(const int rows) {
  visible_rows_ = std::max(rows, 0);
  normalize();
}

void GieresModel::fetch(const bool append) {
  bool expected = false;
  if (!loading_.compare_exchange_strong(expected, true)) {
    return;  // single-flight: a fetch is already in flight
  }
  // Snapshot the request key; pump() drops results whose key no longer
  // matches (the user navigated elsewhere meanwhile).
  auto key = std::make_shared<FetchResult>();
  key->view  = view_;
  key->org   = org_view_;
  key->page  = append ? page_ + 1 : page_;
  key->query = query_;
  key->sort  = sort_;
  key->date  = ech_date_;
  key->append = append;
  if (org_view_ == OrgView::Day) {
    key->date = day_.date;  // the Day sub-view fetches by day_.date
  }
  // requested_page_ records the page the IN-FLIGHT fetch targets (append
  // requests target page_+1 while page_ still shows the current one), so
  // pump() can tell stale results from lazy page loads.
  requested_page_ = key->page;

  status_ = "fetching…";
  if (fetch_thread_.joinable()) {
    fetch_thread_.request_stop();
    fetch_thread_.join();
  }
  fetch_thread_ = std::jthread([this, key](std::stop_token stoken) {
    if (stoken.stop_requested()) {
      // A stop before the body ran means no one will publish into inbox_,
      // so pump() never clears loading_ — release the single-flight latch
      // here or the model stays "loading" forever (a wedged GieresModel
      // deadlocks every later join in fetch()/~GieresModel on the futex
      // the stopped thread left behind).
      loading_.store(false, std::memory_order_release);
      return;
    }
    auto result = std::make_shared<FetchResult>(*key);
    switch (key->view) {
      case View::Live: {
        auto np = now_playing_fn_();
        if (np) {
          result->ok          = true;
          result->now_playing = std::move(*np);
        } else {
          result->fetch_error = std::move(np).error();
        }
        break;
      }
      case View::Orgonity:
        if (key->org == OrgView::Day) {
          auto d = by_date_fn_(key->date);
          if (d) {
            result->ok  = true;
            result->day = std::move(*d);
          } else {
            result->fetch_error = std::move(d).error();
          }
        } else {
          auto listing = orgonity_fn_(key->page, key->query, key->sort);
          if (listing) {
            result->ok      = true;
            result->listing = std::move(*listing);
          } else {
            result->fetch_error = std::move(listing).error();
          }
        }
        break;
      case View::Echelon: {
        // LOCAL day window (gieres_day_window): the calendar's dates are the
        // machine-local days the user thinks in — Europe/Warsaw fetches
        // 2026-09-12 as "2026-09-11T22:00:00Z"…"2026-09-12T22:00:00Z", so a
        // 2-AM show lands on its own day at its own wall-clock time. The
        // archive slices that live timeline into 30-minute segments.
        const auto window = gieres_day_window(key->date);
        if (window.first.empty()) {
          result->fetch_error = "gieres: bad echelon date " + key->date;
          break;
        }
        auto segs = segments_fn_(window.first, window.second);
        if (segs) {
          result->ok       = true;
          result->segments = std::move(*segs);
        } else {
          result->fetch_error = std::move(segs).error();
        }
        break;
      }
    }
    inbox_.store(std::move(result), std::memory_order_release);
  });
}

// fetch_dates loads the Echelon date index (ech_dates_list_) — the timeline's
// OWN calendar, not the orgonity recording_dates subset: the timeline covers
// days with no uploaded recording, and today until the show is archived.
// /echelon/segments has no range endpoint, so the first covered day is found
// by bisection (one 1-hour-window probe per step, ~log2 of the probed range)
// and the contiguous day list runs through today — gap days included
// (selecting one shows the day window's "no segments", which is exactly what
// the server returns). It runs on its own thread + mailbox (dates_inbox_) so
// it never cancels an in-flight view fetch and never clobbers one in the
// single-slot inbox_; NOT touching requested_page_ keeps the staleness key of
// the user's current tab intact.
void GieresModel::fetch_dates() {
  bool expected = false;
  if (!dates_loading_.compare_exchange_strong(expected, true)) {
    return;  // a date-index fetch is already in flight
  }
  // Snapshot the request key (same shape as fetch() — though the fields are
  // no staleness key here: pump applies a for_dates result wherever the user
  // has navigated in the meantime).
  auto key = std::make_shared<FetchResult>();
  key->view      = View::Echelon;
  key->org       = OrgView::List;
  key->page      = 1;
  key->query     = "";
  key->sort      = "date";
  key->for_dates = true;
  // Snapshot the discovered timeline start (set by the first calendar): a
  // reload skips the ~17-probe bisection burst entirely — one anchor probe
  // less matters to a small Rails server serving ffmpeg's seeks too.
  const std::string known_first = calendar_first_;
  status_ = "loading timeline days…";
  if (dates_thread_.joinable()) {
    dates_thread_.request_stop();
    dates_thread_.join();
  }
  dates_thread_ = std::jthread([this, key, known_first](std::stop_token stoken) {
    if (stoken.stop_requested()) {
      // Same as the fetch body: the stopped thread publishes nothing, so
      // pump() never sees a dates result — release dates_loading_ instead
      // of leaving the single-flight latch wedged on.
      dates_loading_.store(false, std::memory_order_release);
      return;
    }
    auto result = std::make_shared<FetchResult>(*key);
    // covered(day) — does the timeline have any segment in the local day's
    // first hour? A transport error (both fallback endpoints failed inside
    // the hook) aborts the whole index fetch; an empty window is just "that
    // day is dark".
    const auto covered = [this](std::chrono::sys_days d)
        -> std::expected<bool, std::string> {
      const auto window = gieres_day_window(
          date_string(std::chrono::year_month_day{d}));
      if (window.first.empty()) {
        return std::unexpected("gieres: bad calendar day");
      }
      const auto start_tp = parse_utc_instant(window.first);
      if (!start_tp) {
        return std::unexpected("gieres: bad window for " + window.first);
      }
      auto segs = segments_fn_(window.first,
                               format_instant_z(*start_tp +
                                                std::chrono::hours{1}));
      if (!segs) {
        return std::unexpected(std::move(segs).error());
      }
      return !segs->empty();
    };
    const auto fail = [&](std::string err) {
      result->fetch_error = std::move(err);
      dates_inbox_.store(std::move(result), std::memory_order_release);
    };
    std::chrono::sys_days hi{today_date()};
    if (!known_first.empty()) {
      // The cached timeline start from an earlier calendar load — skip the
      // probe burst (the bisection's ~17 connections) entirely; a backfilled
      // earlier start is picked up on the next full reload... the cache is
      // only cleared when the probes are, i.e. never within a session.
      const auto ymd = parse_ymd(known_first);
      if (ymd.ok()) {
        hi = std::chrono::sys_days{ymd};
      }
    } else {
    // Anchor the search top (hi must be covered): usually today's first hour
    // has segments; a server hiccup right now walks a few days back instead
    // of failing the whole calendar.
    auto anchor = covered(hi);
    for (int back = 0; back < 7; ++back) {
      if (!anchor) {
        fail(std::move(anchor).error());
        return;
      }
      if (*anchor) {
        break;
      }
      if (stoken.stop_requested()) {
        dates_loading_.store(false, std::memory_order_release);
        return;
      }
      hi -= std::chrono::days{1};
      anchor = covered(hi);
    }
    if (!anchor) {
      fail(std::move(anchor).error());
      return;
    }
    if (!*anchor) {
      fail("echelon timeline: no segments found");
      return;
    }
    // Bisection below the anchor: lo uncovered, hi covered, converge to the
    // first covered day (probes stay inside [kTimelineFirstProbe, anchor]).
    std::chrono::sys_days lo{kTimelineFirstProbe};
    const auto floor_probe = covered(lo);
    if (!floor_probe) {
      fail(std::move(floor_probe).error());
      return;
    }
    if (*floor_probe) {
      hi = lo;  // the timeline reaches the probe floor — bisect nothing
    } else {
      while (hi - lo > std::chrono::days{1}) {
        if (stoken.stop_requested()) {
          dates_loading_.store(false, std::memory_order_release);
          return;
        }
        const auto mid = lo + (hi - lo) / 2;
        const auto hit = covered(mid);
        if (!hit) {
          fail(std::move(hit).error());
          return;
        }
        if (*hit) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      // The bisection may have parked one day above the real first day when
      // a timeline gap sat in between — probe two days below to recover it
      // (one gap step, then stop; two consecutive gaps lose nothing that
      // matters, the calendar grows down to the first covered day at worst
      // one day late).
      if (hi - std::chrono::days{1} >= lo) {
        const auto below = covered(hi - std::chrono::days{1});
        if (!below) {
          fail(std::move(below).error());
          return;
        }
        if (!*below && hi - std::chrono::days{2} >= lo) {
          const auto below2 = covered(hi - std::chrono::days{2});
          if (!below2) {
            fail(std::move(below2).error());
            return;
          }
          if (*below2) {
            hi -= std::chrono::days{2};
          }
        }
      }
    }
    }  // the probe path (skipped when the timeline start is cached)
    // The calendar: every day from the first covered day through today —
    // gap days included (the day window fetch reports them as empty).
    std::vector<std::string> days;
    const std::chrono::sys_days today{today_date()};
    for (auto d = hi; d <= today; d += std::chrono::days{1}) {
      days.push_back(date_string(std::chrono::year_month_day{d}));
    }
    result->ok        = true;
    result->ech_dates = std::move(days);
    dates_inbox_.store(std::move(result), std::memory_order_release);
  });
}

void GieresModel::pump() {
  // Sticky endpoint adoption (UI thread only): a fallback fetch — a view
  // fetch or a date-index probe — recorded the endpoint that answered; the
  // footer and the playback URLs follow it even when the result turns out
  // stale below.
  if (active_base_) {
    auto base = active_base_->get();
    if (!base.empty() && base != base_url_) {
      base_url_ = std::move(base);
    }
  }
  // The date-index mailbox first: a for_dates result bypasses the staleness
  // guard on purpose (fetch_dates) — the timeline calendar is
  // view-independent, so a late result is harmless, while the guard would
  // reject it because the user sits on another tab (typically View::Echelon)
  // when the fetch was launched.
  if (const auto dates =
          dates_inbox_.exchange(nullptr, std::memory_order_acquire)) {
    dates_loading_.store(false, std::memory_order_release);
    if (dates->ok) {
      ech_dates_list_ = std::move(dates->ech_dates);
      if (!ech_dates_list_.empty()) {
        calendar_first_ = ech_dates_list_.front();
      }
      status_.clear();
      // Land the index cursor on the shown day (today at first open) — the
      // calendar's newest row is the useful one, the oldest is scroll-far.
      if (view_ == View::Echelon && ech_dates_ && !ech_dates_list_.empty()) {
        cursor_ = ech_index_row(ech_date_);
        normalize();
      }
    } else if (ech_dates_list_.empty()) {
      status_ = dates->fetch_error;  // the hint row stays; the error shows
    } else {
      status_.clear();  // a refetch failed, but the loaded calendar still shows
    }
    normalize();
  }
  const auto result = inbox_.exchange(nullptr, std::memory_order_acquire);
  if (!result) {
    return;
  }
  // The worker finished — clear the single-flight flag before any staleness
  // decision (a dropped result must not wedge loading_ on).
  loading_.store(false, std::memory_order_release);
  // Stale fetch (the view moved on): drop silently. The page compares
  // against requested_page_ (the in-flight fetch's target), not page_ — a
  // lazy append load targets page_+1 while page_ still shows the current one.
  if (result->view != view_ || result->org != org_view_ ||
      result->page != requested_page_ || result->query != query_ ||
      result->sort != sort_ || result->date != ech_date_) {
    // The Day sub-view keys on day_.date, not ech_date_.
    if (!(result->view == View::Orgonity && org_view_ == OrgView::Day &&
          result->date == day_.date)) {
      // A day step (; / ') or a date-index enter that landed while another
      // fetch was in flight would otherwise never fire: fetch() is
      // single-flight (it skipped), and this result just dropped as stale —
      // refetch the now-current day so the last selection still loads.
      if (result->view == View::Echelon && view_ == View::Echelon &&
          !ech_dates_) {
        fetch();
      }
      return;
    }
  }
  if (!result->ok) {
    error_  = result->fetch_error;
    status_ = error_;
    return;
  }
  status_.clear();
  error_.clear();
  switch (view_) {
    case View::Live:
      now_playing_ = result->now_playing;
      return;
    case View::Orgonity:
      if (org_view_ == OrgView::Day) {
        day_ = result->day;
      } else if (result->append) {
        // Lazy page load: merge the broadcasts. page_ advances by the
        // REQUESTED page (the request defines the next lazy page — the
        // response's page field may echo the server's own count, and tests
        // feed canned fixtures that never advance it); the rest refreshes
        // from the listing (dates_ is complete on every page).
        recordings_.insert(recordings_.end(),
                           result->listing.broadcasts.begin(),
                           result->listing.broadcasts.end());
        page_        = requested_page_;
        total_pages_ = result->listing.total_pages;
        total_count_ = result->listing.total_count;
        if (dates_.empty()) {
          dates_ = result->listing.recording_dates;
        }
      } else {
        page_        = result->listing.page;
        total_pages_ = result->listing.total_pages;
        total_count_ = result->listing.total_count;
        dates_       = std::move(result->listing.recording_dates);
        recordings_  = std::move(result->listing.broadcasts);
      }
      break;
    case View::Echelon:
      segments_ = result->segments;
      break;
  }
  normalize();
}

void GieresModel::maybe_load_more() {
  // Only the Orgonity recording list is paginated: the Echelon segments are
  // one fixed day window, and the date index and its Day sub-view are
  // complete, so none of those lazy-load (a near-bottom refetch there would
  // just re-fetch the same day or, in Dates, race the enter-on-a-date fetch).
  if (view_ != View::Orgonity || org_view_ != OrgView::List) {
    return;
  }
  const int n = list_count();
  if (n <= 0 || loading() || page_ >= total_pages_) {
    return;
  }
  if (n - 1 - cursor_ <= kNearBottom) {
    fetch(true);  // page_+1, appended on apply
  }
}

// ---------------------------------------------------------------------------
// Row rendering
// ---------------------------------------------------------------------------

std::string GieresModel::row_label(const int i, const int /*panel_width*/) const {
  const std::string prefix = i == cursor_ ? "> " : "  ";
  switch (view_) {
    case View::Live: {
      std::string label = std::string{kStationName} + " — live stream";
      if (!now_playing_.empty()) {
        label += "  (now: " + now_playing_ + ")";
      }
      return prefix + label;
    }
    case View::Orgonity:
      switch (org_view_) {
        case OrgView::List: {
          const std::size_t idx = static_cast<std::size_t>(i);
          if (idx >= recordings_.size()) {
            return prefix;
          }
          const GieresBroadcast& b = recordings_[idx];
          return prefix + broadcast_row(b);
        }
        case OrgView::Dates: {
          const std::size_t idx = static_cast<std::size_t>(i);
          if (idx >= dates_.size()) {
            return prefix;
          }
          return prefix + dates_[idx];
        }
        case OrgView::Day: {
          const std::size_t idx = static_cast<std::size_t>(i);
          if (idx >= day_.broadcasts.size()) {
            return prefix;
          }
          const GieresBroadcast& b = day_.broadcasts[idx];
          return prefix + broadcast_row(b);
        }
      }
      return prefix;
    case View::Echelon:
      // The Echelon date index (d): plain date rows of the timeline calendar;
      // until the self-load lands the list is empty and one dim hint row
      // shows (an error surfaces in the footer status instead).
      if (ech_dates_) {
        if (ech_dates_list_.empty()) {
          return prefix + "(loading the timeline days…)";
        }
        const std::size_t idx = static_cast<std::size_t>(i);
        if (idx >= ech_dates_list_.size()) {
          return prefix;
        }
        return prefix + ech_dates_list_[idx];
      }
      break;
  }
  // Echelon flat rows: every segment plus the expanded segment's programs
  // (indented under its row; the "> " cursor prefix stays the only marker).
  int row = 0;
  for (std::size_t s = 0; s < segments_.size(); ++s) {
    const GieresSegment& seg = segments_[s];
    if (row == i) {
      std::string label = local_hh_mm(seg.aired_at) + "  " + seg.title + "  (" +
                          fmt_duration(seg.duration) + ")";
      if (expanded_seg_ >= 0 && static_cast<int>(s) == expanded_seg_) {
        label += "  [tracks shown]";
      } else if (!seg.tracks.empty()) {
        label += "  [t " + std::to_string(seg.tracks.size()) + "]";
      }
      return prefix + label;
    }
    row += 1;
    if (expanded_seg_ >= 0 && static_cast<int>(s) == expanded_seg_) {
      for (const GieresTrackRef& t : seg.tracks) {
        if (row == i) {
          return prefix + "    " + fmt_clock(t.position) + "  " + t.title;
        }
        row += 1;
      }
    }
  }
  return prefix;
}

bool GieresModel::row_dim(const int i) const {
  // The Echelon date-index hint row (no calendar loaded yet) renders dimmed.
  if (view_ == View::Echelon && ech_dates_ && ech_dates_list_.empty()) {
    return true;
  }
  // Only no-audio recordings render dimmed; dates/segments are always live.
  if (view_ != View::Orgonity || org_view_ == OrgView::Dates) {
    return false;
  }
  const std::vector<GieresBroadcast>& items =
      org_view_ == OrgView::Day ? day_.broadcasts : recordings_;
  const std::size_t idx = static_cast<std::size_t>(i);
  return idx < items.size() && !items[idx].has_audio;
}

std::string GieresModel::header_label() const {
  const auto tab = [this](View v, std::string_view name) {
    return view_ == v ? "[" + std::string{name} + "]"
                      : " " + std::string{name} + " ";
  };
  std::string head = std::string{"GIERES ARCHIVE"} + "  " +
                     tab(View::Live, "Live") + " " + tab(View::Orgonity, "Orgonity") +
                     " " + tab(View::Echelon, "Echelon");
  if (view_ == View::Orgonity) {
    if (org_view_ == OrgView::List) {
      head += "  — recordings  page " + std::to_string(page_) + "/" +
              std::to_string(total_pages_) + "  (" + std::to_string(total_count_) +
              " total)";
    } else if (org_view_ == OrgView::Dates) {
      head += "  — dates with recordings (up/down, enter)";
    } else {
      head += "  — " + day_.date + "  (esc back)";
    }
  } else if (view_ == View::Echelon) {
    head += ech_dates_ ? "  — timeline days (up/down, enter)"
                       : "  — " + ech_date_ + "  (d dates, ; ' prev/next day)";
  }
  return head;
}

std::string GieresModel::footer_label() const {
  std::string foot = base_url_;
  if (!status_.empty()) {
    foot += "  " + status_;
  }
  return foot;
}

std::string GieresModel::search_prompt(const int /*panel_width*/) const {
  return "/ " + query_buf_;
}

// ---------------------------------------------------------------------------
// Playback mapping
// ---------------------------------------------------------------------------

namespace {

// archive_track builds the playlist Track for an archive item. stream=true
// arrives via track_from_url; the station name goes into artist so the
// status line reads "Radio Radio - <title>".
playlist::Track archive_track(std::string_view base_url, std::string_view path,
                              std::string_view title, int duration_secs) {
  playlist::Track t = playlist::track_from_path(
      std::string{base_url} + (path.empty() || path.front() == '/' ? "" : "/") +
      std::string{path});
  t.title         = std::string{title};
  t.artist        = std::string{kStationName};
  t.duration_secs = std::max(0, duration_secs);
  return t;
}

// live_track builds the realtime Track for the live stream (the ICY radio
// pipeline owns the chain — the archive adds nothing audio-side).
playlist::Track live_track() {
  playlist::Track t = playlist::track_from_path(std::string{kLiveStreamUrl});
  t.realtime = true;
  t.title    = std::string{kStationName} + " — live";
  t.artist   = std::string{kStationName};
  return t;
}

}  // namespace

// gieres_day_window maps a LOCAL calendar day ("YYYY-MM-DD") to the UTC
// instant window the archive server sees for it (docs/orgonity-api.md §4):
// local midnight to local midnight via the machine's timezone —
// Europe/Warsaw turns 2026-09-12 into "2026-09-11T22:00:00Z"…
// "2026-09-12T22:00:00Z". Without a tz database the window degrades to the
// plain UTC day. Empty strings on a bad date. Exposed for the tests (the
// fakes key the timeline coverage on these window strings).
std::pair<std::string, std::string> gieres_day_window(
    std::string_view date) {
  const auto ymd = parse_ymd(date);
  if (!ymd.ok()) {
    return {"", ""};
  }
  if (const auto* zone = local_zone()) {
    return {format_instant_z(zone->to_sys(
                std::chrono::local_days{ymd} + std::chrono::seconds{0},
                std::chrono::choose::earliest)),
            format_instant_z(zone->to_sys(
                std::chrono::local_days{ymd} + std::chrono::days{1},
                std::chrono::choose::earliest))};
  }
  return {date_string(ymd) + "T00:00:00Z",
          date_string(std::chrono::year_month_day{
              std::chrono::sys_days{ymd} + std::chrono::days{1}}) +
              "T00:00:00Z"};
}

// gieres_local_today is the machine-local calendar date of now
// ("YYYY-MM-DD") — the Echelon calendar's newest day and the next-day
// step's clamp. Exposed for the tests (they mirror the model's "today").
std::string gieres_local_today() {
  return date_string(today_date());
}

// select_echelon_day switches the shown Echelon day and fetches its segments
// — the date index's enter and the ;/' day step both land here. The program
// expansion resets with the list it expanded into.
void GieresModel::select_echelon_day(std::string date) {
  if (date.empty()) {
    return;
  }
  ech_date_     = std::move(date);
  ech_dates_    = false;
  cursor_       = 0;
  scroll_       = 0;
  expanded_seg_ = -1;
  fetch();
}

// ech_index_row is the calendar row of `date` (the newest row when absent —
// the index toggle lands the cursor there).
int GieresModel::ech_index_row(const std::string& date) const {
  if (ech_dates_list_.empty()) {
    return 0;
  }
  for (std::size_t i = 0; i < ech_dates_list_.size(); ++i) {
    if (ech_dates_list_[i] == date) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(ech_dates_list_.size()) - 1;
}

// next_day is the calendar day after `date` ("" on a malformed date).
std::string GieresModel::next_day(const std::string& date) const {
  const auto ymd = parse_ymd(date);
  if (!ymd.ok()) {
    return "";
  }
  return date_string(std::chrono::year_month_day{
      std::chrono::sys_days{ymd} + std::chrono::days{1}});
}

// spawn_chain_fetch fetches `date`'s segment window for the auto-continue
// chain (the next day's segments, or today's window at the live edge) on
// its own thread + mailbox — the dates-fetch pattern: it must never clobber
// (or wait on) a view fetch, and echelon_resolve_end (the host's watchdog
// thread) drains it. Single-flight via chain_loading_; the CAS succeeded
// means the previous result was consumed, so the respawn join is instant.
void GieresModel::spawn_chain_fetch(std::string date) {
  bool expected = false;
  if (!chain_loading_.compare_exchange_strong(expected, true)) {
    return;  // a successor fetch is already in flight
  }
  auto key = std::make_shared<FetchResult>();
  key->view      = View::Echelon;
  key->org       = OrgView::List;
  key->page      = 1;
  key->query     = "";
  key->sort      = "date";
  key->date      = std::move(date);
  key->for_chain = true;
  key->chain_gen = chain_gen_;
  if (chain_thread_.joinable()) {
    chain_thread_.request_stop();
    chain_thread_.join();
  }
  chain_thread_ = std::jthread([this, key](std::stop_token stoken) {
    if (stoken.stop_requested()) {
      // Same as the fetch body: nothing lands in chain_inbox_, so
      // echelon_resolve_end would report Async forever on a latch no one
      // clears — release chain_loading_ before leaving.
      chain_loading_.store(false, std::memory_order_release);
      return;
    }
    auto result = std::make_shared<FetchResult>(*key);
    const auto window = gieres_day_window(key->date);
    if (window.first.empty()) {
      result->fetch_error = "gieres: bad chain day " + key->date;
    } else {
      auto segs = segments_fn_(window.first, window.second);
      if (segs) {
        result->ok       = true;
        result->segments = std::move(*segs);
      } else {
        result->fetch_error = std::move(segs).error();
      }
    }
    chain_inbox_.store(std::move(result), std::memory_order_release);
  });
}

// echelon_resolve_end — see gieres.hpp. The host's watchdog calls this every
// tick while the engine is drained: the same-day successor resolves
// immediately; a day boundary and the live edge spawn a chain fetch whose
// result the NEXT resolve consumes (that is why the drain lives here, on
// the watchdog thread, and not in pump()).
GieresModel::EchelonEnd GieresModel::echelon_resolve_end(
    const std::string_view ended_path, playlist::Track* out) {
  std::lock_guard lk(chain_mu_);
  if (!chain_active_ || ended_path != chain_track_) {
    foundation::applog::info(
        "echelon chain: inactive (armed={} track='{}' ended='{}')",
        chain_active_, chain_track_, ended_path);
    return EchelonEnd::Inactive;  // not ours — the normal end path applies
  }
  // A landed successor fetch first (the watchdog re-enters until the chain
  // resolves, so this is where the async resolution completes).
  if (const auto chain =
          chain_inbox_.exchange(nullptr, std::memory_order_acquire)) {
    chain_loading_.store(false, std::memory_order_release);
    if (chain->chain_gen != chain_gen_) {
      // A stale result from an earlier arm — drop it; the current chain's
      // successor need re-derives below.
    } else if (!chain->ok) {
      // Transport failure: the network is gone, stop rolling (the user's
      // playback stopped anyway; the screen surfaces errors on its own
      // fetches).
      chain_active_ = false;
      foundation::applog::info("echelon chain: stopped (fetch error: {})",
                               chain->fetch_error);
      return EchelonEnd::Stopped;
    } else {
      // The first segment strictly after the one that just finished — a
      // next-day fetch's first segment qualifies by construction; today's
      // live-edge refresh skips the already-played prefix of the day.
      if (chain_index_ < 0 ||
          static_cast<std::size_t>(chain_index_) >= chain_segments_.size()) {
        chain_active_ = false;  // corrupt chain state — do not roll
        return EchelonEnd::Stopped;
      }
      const GieresSegment& last =
          chain_segments_[static_cast<std::size_t>(chain_index_)];
      std::size_t pick = chain->segments.size();
      for (std::size_t i = 0; i < chain->segments.size(); ++i) {
        if (!chain->segments[i].url.empty() &&
            chain->segments[i].aired_at > last.aired_at) {
          pick = i;
          break;
        }
      }
      if (pick < chain->segments.size()) {
        if (active_base_) {
          const auto base = active_base_->get();  // mutex'd; watchdog-safe
          if (!base.empty()) {
            chain_base_ = base;
          }
        }
        *out = archive_track(chain_base_, chain->segments[pick].url,
                             chain->segments[pick].title,
                             chain->segments[pick].duration);
        chain_segments_ = chain->segments;
        chain_day_      = chain->date;
        chain_index_    = static_cast<int>(pick);
        chain_track_    = out->path;
        foundation::applog::info("echelon chain: roll -> {}", out->path);
        return EchelonEnd::Track;
      }
      // The fetched day is dark relative to the chain (a gap day) — skip
      // forward; past today the chain is done.
      const std::string advance = next_day(chain->date);
      if (advance.empty() || advance > date_string(today_date())) {
        chain_active_ = false;
        foundation::applog::info("echelon chain: stopped (dark day at edge)");
        return EchelonEnd::Stopped;
      }
      spawn_chain_fetch(advance);
      return EchelonEnd::Async;
    }
  }
  if (chain_loading_.load(std::memory_order_acquire)) {
    return EchelonEnd::Async;  // a successor fetch is still in flight
  }
  // The same day's next segment resolves immediately — the chain holds the
  // day's segments, no fetch involved (dark, URL-less slices are skipped).
  int next = chain_index_ + 1;
  while (next < static_cast<int>(chain_segments_.size()) &&
         chain_segments_[static_cast<std::size_t>(next)].url.empty()) {
    ++next;
  }
  if (next < static_cast<int>(chain_segments_.size())) {
    const GieresSegment& next_seg = chain_segments_[static_cast<std::size_t>(next)];
    *out = archive_track(chain_base_, next_seg.url, next_seg.title,
                         next_seg.duration);
    chain_index_ = next;
    chain_track_ = out->path;
    foundation::applog::info("echelon chain: roll -> {}", out->path);
    return EchelonEnd::Track;
  }
  // The day's own segments rolled over: the successor needs a fetch — the
  // next day's window, or (at the live edge) one refresh of today's.
  const std::string advance = next_day(chain_day_);
  if (advance.empty() || advance > date_string(today_date())) {
    foundation::applog::info("echelon chain: live edge, refreshing {}",
                             chain_day_);
    spawn_chain_fetch(chain_day_);  // the live edge: one refresh decides
    return EchelonEnd::Async;
  }
  foundation::applog::info("echelon chain: day end, fetching {}", advance);
  spawn_chain_fetch(advance);
  return EchelonEnd::Async;
}

void GieresModel::play_cursor() {
  switch (view_) {
    case View::Live:
      if (actions_.on_play_track) {
        actions_.on_play_track(live_track());
      }
      return;
    case View::Orgonity: {
      if (org_view_ == OrgView::Dates) {
        if (dates_.empty()) {
          return;
        }
        // enter on a date opens that day's recordings (by_date fetch).
        org_view_ = OrgView::Day;
        day_      = GieresDay{dates_[static_cast<std::size_t>(cursor_)], {}};
        cursor_   = 0;
        scroll_   = 0;
        fetch();
        return;
      }
      const std::vector<GieresBroadcast>& items =
          org_view_ == OrgView::Day ? day_.broadcasts : recordings_;
      const std::size_t idx = static_cast<std::size_t>(cursor_);
      if (idx >= items.size() || !items[idx].has_audio ||
          items[idx].audio_url.empty() || !actions_.on_play_track) {
        return;  // dimmed rows (no audio file) are not playable
      }
      actions_.on_play_track(archive_track(base_url_, items[idx].audio_url,
                                           items[idx].title,
                                           items[idx].duration_seconds));
      return;
    }
    case View::Echelon: {
      // enter on the date index selects the shown Echelon day (segments
      // fetch); it never plays anything.
      if (ech_dates_) {
        if (ech_dates_list_.empty()) {
          return;
        }
        const std::size_t idx = static_cast<std::size_t>(cursor_);
        if (idx >= ech_dates_list_.size()) {
          return;
        }
        select_echelon_day(ech_dates_list_[idx]);
        return;
      }
      const auto pos = echelon_pos(cursor_);
      if (pos.first < 0 ||
          static_cast<std::size_t>(pos.first) >= segments_.size()) {
        return;
      }
      const GieresSegment& seg = segments_[static_cast<std::size_t>(pos.first)];
      if (seg.url.empty() || !actions_.on_play_track) {
        return;
      }
      // Finite URL tracks are seekable by restart (ffmpeg -ss over HTTP
      // Range), so left/right winds inside the segment like any track.
      // Arming the auto-continue chain: the day's segments ride along so
      // echelon_resolve_end rolls to the next segment (and the next day's
      // first) when this one finishes naturally.
      playlist::Track track =
          archive_track(base_url_, seg.url, seg.title, seg.duration);
      {
        std::lock_guard lk(chain_mu_);
        chain_active_   = true;
        chain_track_    = track.path;
        chain_day_      = ech_date_;
        chain_base_     = base_url_;
        chain_segments_ = segments_;
        chain_index_    = pos.first;
        ++chain_gen_;
        foundation::applog::info("echelon chain armed: day={} idx={} track={}",
                                 chain_day_, chain_index_, chain_track_);
      }
      actions_.on_play_track(track);
      return;
    }
  }
}

void GieresModel::append_cursor() {
  switch (view_) {
    case View::Live:
      if (actions_.on_append_track) {
        actions_.on_append_track(live_track());
      }
      return;
    case View::Orgonity: {
      if (org_view_ == OrgView::Dates) {
        return;  // nothing to append on the date index
      }
      const std::vector<GieresBroadcast>& items =
          org_view_ == OrgView::Day ? day_.broadcasts : recordings_;
      const std::size_t idx = static_cast<std::size_t>(cursor_);
      if (idx >= items.size() || !items[idx].has_audio ||
          !actions_.on_append_track) {
        return;
      }
      actions_.on_append_track(archive_track(base_url_, items[idx].audio_url,
                                             items[idx].title,
                                             items[idx].duration_seconds));
      return;
    }
    case View::Echelon: {
      // y in the Echelon view appends the whole shown day so the queue
      // chains the 30-minute slices gapless. Nothing to append on the date
      // index.
      if (ech_dates_ || segments_.empty() || !actions_.on_append_tracks) {
        return;
      }
      std::vector<playlist::Track> tracks;
      tracks.reserve(segments_.size());
      for (const GieresSegment& seg : segments_) {
        if (!seg.url.empty()) {
          tracks.push_back(
              archive_track(base_url_, seg.url, seg.title, seg.duration));
        }
      }
      actions_.on_append_tracks(tracks);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

bool GieresModel::handle_key(const std::string_view key) {
  // The search prompt swallows every key while active (printable characters
  // build the live buffer) — including 'p' and 'space', which only fall
  // through when NOT typing.
  if (search_active_) {
    if (key == "enter") {
      query_         = GieresClient::sanitize_query(query_buf_);
      page_          = 1;
      cursor_        = 0;
      scroll_        = 0;
      search_active_ = false;
      fetch();
      return true;
    }
    if (key == "esc") {
      search_active_ = false;
      return true;
    }
    if (key == "backspace") {
      if (!query_buf_.empty()) {
        query_buf_.pop_back();
      }
      return true;
    }
    if (key == "ctrl+u") {
      query_buf_.clear();
      return true;
    }
    if (key == "space") {
      query_buf_ += " ";
      return true;
    }
    if (key.size() == 1 && key[0] >= 0x21 && key[0] <= 0x7E) {
      query_buf_ += key;
    }
    return true;  // modal while typing
  }

  // Consume the screen's own keys; everything unhandled falls through to the
  // global player table (the browse/queue/device/eq/help screens all end
  // handle_key on return false). Explicit fallthroughs: 'p' (the app's
  // global p closes the screen, DevicePicker-style), 'space' (global
  // play/pause) and the global volume keys - / = / + (while the search
  // prompt is active the typing block above has already consumed them —
  // '-' is a legal query char — so these only fire when not typing).
  if (key == "p" || key == "space") {
    return false;
  }
  if (key == "-" || key == "=" || key == "+") {
    return false;
  }
  if (key == "/" && view_ == View::Orgonity) {
    // Open the search prompt (Orgonity only); the live buffer keeps its old
    // content so '/' re-edits the submitted query.
    search_active_ = true;
    return true;
  }

  if (key == "tab" || key == "shift+tab") {
    const int delta = key == "tab" ? 1 : -1;
    view_   = static_cast<View>((static_cast<int>(view_) + delta + 3) % 3);
    cursor_ = 0;
    scroll_ = 0;
    ech_dates_ = false;  // tab always lands on the segment/recording list
    const bool pending =
        (view_ == View::Live && now_playing_.empty()) ||
        (view_ == View::Orgonity && recordings_.empty() && dates_.empty()) ||
        (view_ == View::Echelon && segments_.empty());
    if (pending && !loading()) {
      fetch();
    }
    return true;
  }
  if (key == "esc") {
    if (view_ == View::Echelon && ech_dates_) {
      ech_dates_ = false;  // the date index closes before the screen does
      cursor_    = 0;
      scroll_    = 0;
      normalize();
    } else if (expanded_seg_ >= 0) {
      expanded_seg_ = -1;
      normalize();
    } else if (view_ == View::Orgonity && org_view_ == OrgView::Day) {
      org_view_ = OrgView::Dates;
      normalize();
    } else if (view_ == View::Orgonity && org_view_ == OrgView::Dates) {
      org_view_ = OrgView::List;
      normalize();
    } else {
      visible_ = false;  // the last level closes the screen
    }
    return true;
  }
  if (key == "up" || key == "k") {
    const int n = list_count();
    cursor_ = n > 0 ? (cursor_ + n - 1) % n : 0;  // wrap (Go providerMoveUp)
    normalize();
    maybe_load_more();
    return true;
  }
  if (key == "down" || key == "j") {
    const int n = list_count();
    cursor_ = n > 0 ? (cursor_ + 1) % n : 0;
    normalize();
    maybe_load_more();
    return true;
  }
  if (key == "pageup" || key == "pgup") {
    cursor_ -= std::min(cursor_, std::max(1, visible_rows_));
    normalize();
    return true;
  }
  if (key == "pagedown" || key == "pgdown") {
    const int n = list_count();
    cursor_ += std::min(n - 1 - cursor_, std::max(1, visible_rows_));
    normalize();
    maybe_load_more();
    return true;
  }
  if (key == "home" || key == "g") {
    cursor_ = 0;
    normalize();
    return true;
  }
  if (key == "end" || key == "G") {
    cursor_ = std::max(0, list_count() - 1);
    normalize();
    maybe_load_more();
    return true;
  }
  if (key == "enter") {
    play_cursor();
    return true;
  }
  if (key == "y") {
    append_cursor();
    return true;
  }
  // left/right are NOT consumed anywhere: they fall through to the global
  // player table (seek ±5s; shift+left/right large seek) — the user listens
  // to archive recordings here and expects the arrows to wind the track.
  if (key == "s") {
    // Orgonity sort cycle (date → title → duration); other views fall
    // through to the global stop.
    if (view_ != View::Orgonity) {
      return false;
    }
    sort_   = sort_ == "date"   ? "title"
              : sort_ == "title" ? "duration"
                                 : "date";
    page_   = 1;
    cursor_ = 0;
    scroll_ = 0;
    fetch();
    return true;
  }
  if (key == "d") {
    // Orgonity: toggle the recording list / date index; Echelon: toggle the
    // segment list / the timeline calendar (a selected day becomes the shown
    // Echelon day — the calendar self-loads on the first d via fetch_dates,
    // no Orgonity visit required, and it opens on the shown day: today at
    // first, the calendar's newest row being the useful default). The
    // Orgonity branch fetches once if dates_ never arrived; the Live view
    // falls through to the global device picker.
    if (view_ == View::Echelon) {
      ech_dates_ = !ech_dates_;
      cursor_ = ech_dates_ && !ech_dates_list_.empty()
                    ? ech_index_row(ech_date_)
                    : 0;
      scroll_ = 0;
      normalize();
      if (ech_dates_ && ech_dates_list_.empty()) {
        fetch_dates();  // the timeline calendar self-loads; Orgonity not needed
      }
      return true;
    }
    if (view_ != View::Orgonity) {
      return false;
    }
    if (org_view_ == OrgView::List) {
      if (dates_.empty() && !loading()) {
        fetch();
      }
      org_view_ = OrgView::Dates;
      // Land the cursor on the NEWEST date: the index opens on the day the
      // archive most recently reached (the Echelon calendar's "shown day"
      // equivalent) — with the ascending recording_dates list the newest
      // date is the last row.
      cursor_   = dates_.empty() ? 0 : static_cast<int>(dates_.size()) - 1;
      scroll_   = 0;
      normalize();
    } else if (org_view_ == OrgView::Dates) {
      org_view_ = OrgView::List;
      cursor_   = 0;
      scroll_   = 0;
      normalize();
    }
    return true;
  }
  if (key == "t") {
    // Echelon program listing: expand the cursor segment; on a program row
    // (or already expanded) collapse back to the segment row. Other views
    // fall through to the global player table.
    if (view_ != View::Echelon) {
      return false;
    }
    if (ech_dates_) {
      return true;  // the date index has no program listings
    }
    const auto pos = echelon_pos(cursor_);
    if (pos.first < 0 ||
        static_cast<std::size_t>(pos.first) >= segments_.size()) {
      return false;  // nothing expandable under the cursor either
    }
    if (pos.second >= 0) {
      cursor_ = echelon_flat(pos.first, -1);
      if (expanded_seg_ == pos.first) {
        expanded_seg_ = -1;
      }
    } else if (expanded_seg_ == pos.first) {
      expanded_seg_ = -1;
    } else {
      expanded_seg_ = pos.first;
    }
    normalize();
    return true;
  }
  if ((key == ";" || key == "'") && view_ == View::Echelon) {
    // Previous / next day. Inside the date index the keys walk the cursor
    // over the day list (the list IS the day sequence); on the segment list
    // they switch the shown day and fetch it — next stops at today (the
    // timeline has no future), previous stops at the timeline's first day
    // once the calendar is loaded.
    if (ech_dates_) {
      cursor_ += key == "'" ? 1 : -1;
      normalize();
      return true;
    }
    const auto cur = parse_ymd(ech_date_);
    if (!cur.ok()) {
      return true;
    }
    auto target = std::chrono::sys_days{cur};
    target += key == "'" ? std::chrono::days{1} : -std::chrono::days{1};
    if (key == "'" && target > std::chrono::sys_days{today_date()}) {
      return true;
    }
    if (key == ";" && !ech_dates_list_.empty() &&
        date_string(std::chrono::year_month_day{target}) <
            ech_dates_list_.front()) {
      return true;
    }
    select_echelon_day(date_string(std::chrono::year_month_day{target}));
    return true;
  }
  if (key == "ctrl+r") {
    error_.clear();
    status_.clear();
    if (!loading()) {
      fetch();
    }
    return true;
  }
  // Fall through like every other screen (browse/queue/device/eq/help all
  // end handle_key on return false): unhandled keys reach the global player
  // table, so volume/skip/repeat/shuffle/stop/mono/speed/queue/remove keep
  // working on the archive. Typing mode above still swallows everything.
  return false;
}

// ---------------------------------------------------------------------------
// FTXUI component
// ---------------------------------------------------------------------------

#if BOOTAMP_HAS_FTXUI

std::shared_ptr<ftxui::ComponentBase> make_gieres_component(GieresModel& model) {
  return ftxui::Renderer([&model] {
    model.pump();  // landed background fetches apply before this render
    using namespace ftxui;
    std::vector<Element> lines;
    lines.push_back(text(model.header_label()));
    if (model.search_active()) {
      lines.push_back(text(model.search_prompt(0)));  // display-only prompt
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
      lines.push_back(dim(text("  (nothing here — ctrl+r refetches)")));
    }
    lines.push_back(separator());
    lines.push_back(dim(text(model.footer_label())));
    return vbox(std::move(lines));
  });
}

#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens