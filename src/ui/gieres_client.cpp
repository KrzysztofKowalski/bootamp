// ui/gieres_client.cpp — GieresClient + fixture parsers (contract:
// ui/gieres_client.hpp). Shapes mirror docs/orgonity-api.md sections 1/2/4/5;
// the parse style (parse → type-check → value()/contains) follows
// provider/radio/catalog.cpp.
#include "ui/gieres_client.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>
#include <utility>

namespace bootamp::ui {

namespace {

namespace nl = nlohmann;  // like catalog.cpp's concise alias

// encode_query_component percent-encodes one query value. Sanitized q only
// ever contains [A-Za-z0-9 _-] and sort comes from a fixed whitelist, so this
// is defensive: space → %20, unreserved ASCII kept, anything else hex-escaped.
std::string encode_query_component(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(c);
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0x0F]);
    }
  }
  return out;
}

// parse_broadcast fills one GieresBroadcast from a broadcasts[] element. The
// server nulls duration_seconds for unknown lengths and omits nothing else
// that matters; missing audio_url is tolerated (has_audio is the gate).
GieresBroadcast parse_broadcast(const nl::json& el) {
  GieresBroadcast b;
  b.id             = el.value("id", 0LL);
  b.title          = el.value("title", std::string{});
  b.recording_date = el.value("recording_date", std::string{});
  b.audio_url      = el.value("audio_url", std::string{});
  if (el.contains("duration_seconds") && el["duration_seconds"].is_number()) {
    b.duration_seconds = el["duration_seconds"].get<int>();
  }
  b.has_audio = el.value("has_audio", false);
  return b;
}

}  // namespace

// Cache wiring: the HTTP bodies below ride a foundation::DiskCache under the
// "gieres" namespace, keyed by the full request URL. TTLs follow how stale
// each payload may be — archive listings and day windows are immutable for
// years (5 min is a debounce, not a correctness gate); now-playing is a live
// badge (30 s — an explicit ctrl+r within the TTL still sees the previous
// title). A cache failure is a miss, never an error: the LAN fetch proceeds.
inline constexpr std::string_view kCacheNamespace        = "gieres";
inline constexpr std::int64_t     kArchiveCacheTtlSeconds = 300;
inline constexpr std::int64_t     kNowPlayingCacheTtlSeconds = 30;

GieresClient::GieresClient(std::string base_url)
    : GieresClient(std::move(base_url),
                   std::make_shared<foundation::DiskCache>()) {}

GieresClient::GieresClient(std::string base_url,
                           std::shared_ptr<foundation::DiskCache> cache)
    : base_(std::move(base_url)), cache_(std::move(cache)) {
  while (!base_.empty() && base_.back() == '/') {
    base_.pop_back();
  }
}

std::string GieresClient::url(std::string_view path) const {
  std::string out = base_;
  if (!path.empty() && path.front() != '/') {
    out += '/';
  }
  out += path;
  return out;
}

std::expected<audio::HttpResponse, std::string>
GieresClient::fetch_cached(const std::string_view path,
                           const std::size_t max_bytes,
                           const std::chrono::milliseconds timeout,
                           const std::int64_t ttl_seconds) {
  const std::string full = url(path);
  // A fresh cached body (only 200s are ever stored) skips the LAN round-trip;
  // a miss, an expired/corrupt entry or an unusable cache dir falls through
  // to the real fetch. The cache accelerates reads — it never gates one.
  if (auto hit = cache_->get(std::string{kCacheNamespace} + "/" + full)) {
    audio::HttpResponse resp;
    resp.status = 200;  // stored bodies are captured 200s; parse proceeds as
    resp.body   = std::move(*hit);  // it would after a real fetch.
    return resp;
  }
  auto resp = http_.fetch_text(full, max_bytes, timeout);
  if (!resp || resp->status != 200) {
    return resp;  // errors and non-200 stay uncached (the caller reports them)
  }
  (void)cache_->put(std::string{kCacheNamespace} + "/" + full, resp->body,
                    ttl_seconds);
  return resp;
}

std::expected<GieresListing, std::string>
GieresClient::orgonity(const int page, const std::string_view q,
                       const std::string_view sort) {
  std::string path = "/orgonity.json?page=" + std::to_string(page);
  if (!q.empty()) {
    path += "&q=" + encode_query_component(q);
  }
  if (!sort.empty()) {
    path += "&sort=" + encode_query_component(sort);
  }
  auto resp = fetch_cached(path, 1024 * 1024, std::chrono::seconds{10},
                           kArchiveCacheTtlSeconds);
  if (!resp) {
    return std::unexpected(std::move(resp).error());
  }
  if (resp->status != 200) {
    return std::unexpected("gieres: /orgonity.json HTTP " +
                           std::to_string(resp->status));
  }
  return parse_orgonity_listing(resp->body);
}

std::expected<GieresDay, std::string>
GieresClient::by_date(const std::string_view date) {
  std::string path = "/orgonity/by_date/";
  path += encode_query_component(date);
  path += ".json";
  auto resp = fetch_cached(path, 1024 * 1024, std::chrono::seconds{10},
                           kArchiveCacheTtlSeconds);
  if (!resp) {
    return std::unexpected(std::move(resp).error());
  }
  if (resp->status != 200) {
    return std::unexpected("gieres: by_date HTTP " + std::to_string(resp->status));
  }
  return parse_orgonity_day(resp->body);
}

std::expected<std::vector<GieresSegment>, std::string>
GieresClient::echelon_segments(const std::string_view start_iso,
                               const std::string_view end_iso) {
  std::string path = "/echelon/segments?start=" + encode_query_component(start_iso);
  if (!end_iso.empty()) {
    path += "&end=" + encode_query_component(end_iso);
  }
  auto resp = fetch_cached(path, 1024 * 1024, std::chrono::seconds{10},
                           kArchiveCacheTtlSeconds);
  if (!resp) {
    return std::unexpected(std::move(resp).error());
  }
  if (resp->status != 200) {
    return std::unexpected("gieres: /echelon/segments HTTP " +
                           std::to_string(resp->status));
  }
  return parse_echelon_segments(resp->body);
}

std::expected<std::string, std::string> GieresClient::now_playing() {
  auto resp = fetch_cached("/now-playing.json", 64 * 1024,
                           std::chrono::seconds{10}, kNowPlayingCacheTtlSeconds);
  if (!resp) {
    return std::unexpected(std::move(resp).error());
  }
  if (resp->status != 200) {
    return std::unexpected("gieres: /now-playing.json HTTP " +
                           std::to_string(resp->status));
  }
  return parse_now_playing(resp->body);
}

std::string GieresClient::sanitize_query(const std::string_view q) {
  std::string out;
  out.reserve(q.size());
  for (const char c : q) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0 || c == ' ' || c == '_' || c == '-') {
      out.push_back(c);
    }
  }
  return out;
}

std::expected<GieresListing, std::string>
parse_orgonity_listing(const std::string_view json) {
  // parse(..., nullptr, false) returns a discarded value instead of throwing.
  const nl::json j = nl::json::parse(std::string{json}, nullptr, false);
  if (!j.is_object()) {
    return std::unexpected("gieres: /orgonity.json body is not a JSON object");
  }
  GieresListing out;
  out.page        = j.value("page", 1);
  out.total_pages = j.value("total_pages", 1);
  out.total_count = j.value("total_count", 0);
  if (j.contains("recording_dates") && j["recording_dates"].is_array()) {
    for (const nl::json& d : j["recording_dates"]) {
      if (d.is_string()) {
        out.recording_dates.push_back(d.get<std::string>());
      }
    }
  }
  if (j.contains("broadcasts") && j["broadcasts"].is_array()) {
    for (const nl::json& el : j["broadcasts"]) {
      if (el.is_object()) {
        out.broadcasts.push_back(parse_broadcast(el));
      }
    }
  }
  return out;
}

std::expected<GieresDay, std::string>
parse_orgonity_day(const std::string_view json) {
  // parse(..., nullptr, false) returns a discarded value instead of throwing.
  const nl::json j = nl::json::parse(std::string{json}, nullptr, false);
  if (!j.is_object()) {
    return std::unexpected("gieres: by_date body is not a JSON object");
  }
  GieresDay out;
  out.date = j.value("date", std::string{});
  if (j.contains("broadcasts") && j["broadcasts"].is_array()) {
    for (const nl::json& el : j["broadcasts"]) {
      if (el.is_object()) {
        out.broadcasts.push_back(parse_broadcast(el));
      }
    }
  }
  return out;
}

std::expected<std::vector<GieresSegment>, std::string>
parse_echelon_segments(const std::string_view json) {
  // parse(..., nullptr, false) returns a discarded value instead of throwing.
  const nl::json j = nl::json::parse(std::string{json}, nullptr, false);
  if (!j.is_object()) {
    return std::unexpected("gieres: /echelon/segments body is not a JSON object");
  }
  std::vector<GieresSegment> out;
  if (j.contains("segments") && j["segments"].is_array()) {
    for (const nl::json& el : j["segments"]) {
      if (!el.is_object()) {
        continue;
      }
      GieresSegment s;
      s.id       = el.value("id", 0LL);
      s.title    = el.value("title", std::string{});
      s.aired_at = el.value("aired_at", std::string{});
      s.url      = el.value("url", std::string{});
      s.duration = el.value("duration", 0);
      if (el.contains("tracks") && el["tracks"].is_array()) {
        for (const nl::json& t : el["tracks"]) {
          if (!t.is_object()) {
            continue;
          }
          s.tracks.push_back(
              GieresTrackRef{t.value("title", std::string{}),
                             t.value("position", 0)});
        }
      }
      out.push_back(std::move(s));
    }
  }
  return out;
}

std::expected<std::string, std::string>
parse_now_playing(const std::string_view json) {
  // parse(..., nullptr, false) returns a discarded value instead of throwing.
  const nl::json j = nl::json::parse(std::string{json}, nullptr, false);
  if (!j.is_object()) {
    return std::unexpected("gieres: /now-playing.json body is not a JSON object");
  }
  return j.value("title", std::string{});
}

}  // namespace bootamp::ui