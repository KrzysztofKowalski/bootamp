// provider/radio/catalog.cpp — Radio Browser API client (cliamp catalog.go).
//
// Faithful port of cliamp/external/radio/catalog.go: a libcurl client for the
// Radio Browser API (de1.api.radio-browser.info) plus the cliamp radio
// statistics endpoint. Go's net/http.Client{Timeout: 10s} is replaced by
// libcurl per-request handles with the same 10s transfer timeout and
// User-Agent "cliamp/1.0"; redirects are followed (Go client default).
#include "provider/radio/radio.hpp"
#include "provider/radio/radio_internal.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::provider::radio {

namespace {

// cliamp's stats endpoint (catalog.go cliampRadioStatsURL).
inline constexpr std::string_view kCliampRadioStatsURL =
    "https://radio.cliamp.stream/statistics";

// path_escape mirrors Go url.PathEscape (encodePathSegment, net/url/url.go):
// keeps [A-Za-z0-9] plus the RFC 3986 path-segment allowed characters
// "-_.~$&+:=@"; escapes everything else (including '/' ';' ',' '?' and
// space) as %XX with uppercase hex digits.
std::string path_escape(std::string_view s) {
  constexpr auto keep = [](unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
      return true;
    }
    switch (c) {
      case '-': case '_': case '.': case '~':
      case '$': case '&': case '+': case ':': case '=': case '@':
        return true;
      default:
        return false;
    }
  };
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    if (keep(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// HttpResult carries the response body plus the final HTTP status (after
// redirects, like Go's http.Client.Do).
struct HttpResult {
  std::string body;
  long        status = 0;
};

// write_cb appends the received chunk to the caller's std::string.
std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

// http_get performs a GET with libcurl. 10s total timeout (Go's
// catalogClient/statsClient), User-Agent "cliamp/1.0", follows up to 10
// redirects. Transport-level failures return the libcurl error message.
std::expected<HttpResult, std::string> http_get(std::string_view url) {
  const std::string url_str{url};
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return std::unexpected{"radio: libcurl failed to initialize"};
  }
  std::string body;
  char errbuf[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cliamp/1.0");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  const CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    return std::unexpected{errbuf[0] != '\0'
                               ? std::string{errbuf}
                               : std::string{curl_easy_strerror(rc)}};
  }
  return HttpResult{std::move(body), status};
}

}  // namespace

namespace detail {

std::optional<int> strict_atoi(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  bool neg = false;
  if (s[i] == '+' || s[i] == '-') {
    neg = (s[i] == '-');
    ++i;
  }
  if (i >= s.size()) return std::nullopt;  // sign alone
  long long v = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + (c - '0');
    if (v > 2147483648LL) return std::nullopt;  // int overflow
  }
  if (neg) {
    v = -v;
  } else if (v > 2147483647LL) {
    return std::nullopt;
  }
  return static_cast<int>(v);
}

std::string search_stations_url(std::string_view query, int limit) {
  if (limit <= 0) limit = 50;
  std::string out{kRadioBrowserBase};
  out += "/stations/byname/";
  out += path_escape(query);
  out += "?limit=" + std::to_string(limit);
  out += "&order=votes&reverse=true&hidebroken=true";
  return out;
}

std::string top_stations_url(int offset, int limit) {
  if (limit <= 0) limit = 50;
  if (offset < 0) offset = 0;
  std::string out{kRadioBrowserBase};
  out += "/stations/topvote/" + std::to_string(limit);
  out += "?offset=" + std::to_string(offset);
  out += "&hidebroken=true";
  return out;
}

std::expected<std::vector<CatalogStation>, std::string>
parse_catalog_stations(std::string_view json) {
  try {
    const nlohmann::json j = nlohmann::json::parse(std::string{json});
    if (!j.is_array()) {
      return std::unexpected{"radio-browser: response is not a JSON array"};
    }
    std::vector<CatalogStation> out;
    for (const auto& el : j) {
      if (!el.is_object()) {
        return std::unexpected{"radio-browser: station entry is not a JSON object"};
      }
      CatalogStation s;
      s.name     = el.value("name", std::string{});
      s.url      = el.value("url_resolved", std::string{});
      s.country  = el.value("country", std::string{});
      s.tags     = el.value("tags", std::string{});
      s.codec    = el.value("codec", std::string{});
      s.bitrate  = el.value("bitrate", 0);
      s.votes    = el.value("votes", 0);
      s.homepage = el.value("homepage", std::string{});
      out.push_back(std::move(s));
    }
    return out;
  } catch (const nlohmann::json::exception& e) {
    return std::unexpected{std::string{"radio-browser: "} + e.what()};
  }
}

std::expected<RadioStats, std::string> parse_radio_stats(std::string_view json) {
  try {
    const nlohmann::json j = nlohmann::json::parse(std::string{json});
    if (!j.is_object()) {
      return std::unexpected{"radio stats: response is not a JSON object"};
    }
    RadioStats stats;
    stats.total_sessions     = j.value("total_sessions", 0);
    stats.total_listen_hours = j.value("total_listen_hours", 0.0);
    stats.peak_listeners     = j.value("peak_listeners", 0);
    if (j.contains("stations")) {
      const nlohmann::json& st = j.at("stations");
      if (!st.is_object()) {
        return std::unexpected{"radio stats: stations is not a JSON object"};
      }
      for (auto it = st.begin(); it != st.end(); ++it) {
        const nlohmann::json& v = it.value();
        if (!v.is_object()) {
          return std::unexpected{"radio stats: station stats is not a JSON object"};
        }
        RadioStationStats ss;
        ss.total_sessions     = v.value("total_sessions", 0);
        ss.total_listen_hours = v.value("total_listen_hours", 0.0);
        ss.peak_listeners     = v.value("peak_listeners", 0);
        ss.active_listeners   = v.value("active_listeners", 0);
        stats.stations[it.key()] = ss;
      }
    }
    return stats;
  } catch (const nlohmann::json::exception& e) {
    return std::unexpected{std::string{"radio stats: "} + e.what()};
  }
}

std::expected<std::vector<CatalogStation>, std::string>
search_stations(std::string_view query, int limit) {
  auto resp = http_get(search_stations_url(query, limit));
  if (!resp) return std::unexpected{std::move(resp).error()};
  if (resp->status != 200) {
    return std::unexpected{"radio-browser: HTTP " + std::to_string(resp->status)};
  }
  return parse_catalog_stations(resp->body);
}

std::expected<std::vector<CatalogStation>, std::string>
top_stations_offset(int offset, int limit) {
  auto resp = http_get(top_stations_url(offset, limit));
  if (!resp) return std::unexpected{std::move(resp).error()};
  if (resp->status != 200) {
    return std::unexpected{"radio-browser: HTTP " + std::to_string(resp->status)};
  }
  return parse_catalog_stations(resp->body);
}

std::expected<RadioStats, std::string> fetch_stats() {
  auto resp = http_get(kCliampRadioStatsURL);
  if (!resp) return std::unexpected{std::move(resp).error()};
  if (resp->status != 200) {
    return std::unexpected{"cliamp radio stats: HTTP " + std::to_string(resp->status)};
  }
  return parse_radio_stats(resp->body);
}

}  // namespace detail

// Provider::radio_stats — port of cliamp catalog.go RadioStats.
std::expected<RadioStats, std::string> Provider::radio_stats() {
  return detail::fetch_stats();
}

}  // namespace bootamp::provider::radio
