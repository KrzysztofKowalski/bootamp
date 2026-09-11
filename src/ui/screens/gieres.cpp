// ui/screens/gieres.cpp — Gieres archive screen implementation (contract:
// ui/screens/gieres.hpp). Fetch lifecycle follows browse.cpp (a jthread
// publishes immutable results into an atomic mailbox that the render path
// pumps); the FTXUI glue renders the model state — the shell claims every
// key, the host routes them into the model.
#include "ui/screens/gieres.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

// hh_mm extracts "HH:MM" from an ISO8601 instant ("…T20:00:00Z").
std::string hh_mm(std::string_view iso) {
  return iso.size() >= 16 ? std::string{iso.substr(11, 5)} : std::string{};
}

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
  // Echelon starts on today's UTC day (the segments window is UTC).
  ech_date_ = date_string(
      std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(
          std::chrono::system_clock::now())});
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
  const auto make_hooks = [](const std::string& base) {
    GieresClient client(base);
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
  auto hooks = make_hooks(primary);
  if (fallback == primary) {
    return GieresModel(std::move(hooks.orgonity), std::move(hooks.by_date),
                       std::move(hooks.segments), std::move(hooks.now_playing),
                       base_url);
  }
  const auto active = std::make_shared<GieresActiveBase>();
  hooks = gieres_with_fallback(std::move(hooks), make_hooks(fallback), active);
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
      // The Echelon date index (d): one row per date with recordings; with
      // no dates loaded yet a single hint row (the index self-loads via
      // fetch_dates(), so the hint shows only while that fetch is in flight
      // — or after it failed, the error surfaces in the footer status).
      if (ech_dates_) {
        return dates_.empty() ? 1 : static_cast<int>(dates_.size());
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
        // UTC day window (MVP): start=dayT00:00:00Z, end=next-dayT00:00:00Z —
        // the archive slices the live timeline into 30-minute segments.
        const auto ymd = parse_ymd(key->date);
        if (!ymd.ok()) {
          result->fetch_error = "gieres: bad echelon date " + key->date;
          break;
        }
        const std::string start = key->date + "T00:00:00Z";
        const std::string end =
            date_string(std::chrono::year_month_day{
                std::chrono::sys_days{ymd} + std::chrono::days{1}}) +
            "T00:00:00Z";
        auto segs = segments_fn_(start, end);
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

// fetch_dates loads the shared date index (dates_) behind the Echelon `d`
// toggle: one page-1 /orgonity.json fetch — recording_dates is complete on
// every page (gieres_client.hpp), so no paging is involved. It runs on its
// own thread + mailbox (dates_inbox_) so it never cancels an in-flight view
// fetch and never clobbers one in the single-slot inbox_; NOT touching
// requested_page_ keeps the staleness key of the user's current tab intact.
void GieresModel::fetch_dates() {
  bool expected = false;
  if (!dates_loading_.compare_exchange_strong(expected, true)) {
    return;  // a date-index fetch is already in flight
  }
  // Snapshot the request key (same shape as fetch() — though the fields are
  // no staleness key here: pump applies a for_dates result wherever the user
  // has navigated in the meantime).
  auto key = std::make_shared<FetchResult>();
  key->view      = View::Orgonity;
  key->org       = OrgView::List;
  key->page      = 1;
  key->query     = "";
  key->sort      = "date";
  key->for_dates = true;
  status_ = "fetching dates…";
  if (dates_thread_.joinable()) {
    dates_thread_.request_stop();
    dates_thread_.join();
  }
  dates_thread_ = std::jthread([this, key](std::stop_token stoken) {
    if (stoken.stop_requested()) {
      return;
    }
    auto result = std::make_shared<FetchResult>(*key);
    auto listing = orgonity_fn_(key->page, key->query, key->sort);
    if (listing) {
      result->ok      = true;
      result->listing = std::move(*listing);
    } else {
      result->fetch_error = std::move(listing).error();
    }
    dates_inbox_.store(std::move(result), std::memory_order_release);
  });
}

void GieresModel::pump() {
  // The date-index mailbox first: a for_dates result bypasses the staleness
  // guard on purpose (fetch_dates) — the dates are identical on every
  // /orgonity.json page, so a late result is harmless, while the guard would
  // reject it because the user sits on another tab (typically View::Echelon)
  // when the fetch was launched.
  if (const auto dates =
          dates_inbox_.exchange(nullptr, std::memory_order_acquire)) {
    dates_loading_.store(false, std::memory_order_release);
    if (dates->ok) {
      dates_ = std::move(dates->listing.recording_dates);
      status_.clear();
    } else if (dates_.empty()) {
      status_ = dates->fetch_error;  // the hint row stays; the error shows
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
  // Sticky endpoint adoption (UI thread only): a fallback fetch recorded the
  // endpoint that answered — the footer and the playback URLs follow it even
  // when this particular result turns out stale below.
  if (active_base_) {
    auto base = active_base_->get();
    if (!base.empty() && base != base_url_) {
      base_url_ = std::move(base);
    }
  }
  // Stale fetch (the view moved on): drop silently. The page compares
  // against requested_page_ (the in-flight fetch's target), not page_ — a
  // lazy append load targets page_+1 while page_ still shows the current one.
  if (result->view != view_ || result->org != org_view_ ||
      result->page != requested_page_ || result->query != query_ ||
      result->sort != sort_ || result->date != ech_date_) {
    // The Day sub-view keys on day_.date, not ech_date_.
    if (!(result->view == View::Orgonity && org_view_ == OrgView::Day &&
          result->date == day_.date)) {
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
        // Lazy page load: merge the broadcasts; page_/total_pages_/dates_
        // refresh from the listing (dates_ is complete on every page).
        recordings_.insert(recordings_.end(),
                           result->listing.broadcasts.begin(),
                           result->listing.broadcasts.end());
        page_        = result->listing.page;
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
  // one fixed day window and the date index is complete, so neither lazy-
  // loads (a near-bottom refetch there would just re-fetch the same day).
  if (view_ != View::Orgonity) {
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
      // The Echelon date index (d): plain date rows like the Orgonity Dates
      // list; until the self-load lands dates_ is empty and one dim hint row
      // shows (an error surfaces in the footer status instead).
      if (ech_dates_) {
        if (dates_.empty()) {
          return prefix + "(loading the date index…)";
        }
        const std::size_t idx = static_cast<std::size_t>(i);
        if (idx >= dates_.size()) {
          return prefix;
        }
        return prefix + dates_[idx];
      }
      break;
  }
  // Echelon flat rows: every segment plus the expanded segment's programs
  // (indented under its row; the "> " cursor prefix stays the only marker).
  int row = 0;
  for (std::size_t s = 0; s < segments_.size(); ++s) {
    const GieresSegment& seg = segments_[s];
    if (row == i) {
      std::string label = hh_mm(seg.aired_at) + "  " + seg.title + "  (" +
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
  // The Echelon date-index hint row (no dates loaded yet) renders dimmed.
  if (view_ == View::Echelon && ech_dates_ && dates_.empty()) {
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
    head += ech_dates_ ? "  — dates with recordings (up/down, enter)"
                       : "  — " + ech_date_ + "  (d dates)";
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
        if (dates_.empty()) {
          return;
        }
        const std::size_t idx = static_cast<std::size_t>(cursor_);
        if (idx >= dates_.size()) {
          return;
        }
        ech_date_     = dates_[idx];
        ech_dates_    = false;
        cursor_       = 0;
        scroll_       = 0;
        expanded_seg_ = -1;
        fetch();
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
      actions_.on_play_track(
          archive_track(base_url_, seg.url, seg.title, seg.duration));
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
    // segment list / the same date index (a selected date becomes the shown
    // Echelon day — the index self-loads on the first d when dates_ is empty,
    // no Orgonity visit required). The Orgonity branch fetches once if dates_
    // never arrived; the Live view falls through to the global device picker.
    if (view_ == View::Echelon) {
      ech_dates_ = !ech_dates_;
      cursor_    = 0;
      scroll_    = 0;
      normalize();
      if (ech_dates_ && dates_.empty()) {
        fetch_dates();  // the date index self-loads; Orgonity visit not required
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
      cursor_   = 0;
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