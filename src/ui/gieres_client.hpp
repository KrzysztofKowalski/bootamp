// ui/gieres_client.hpp — HTTP+JSON client for the "Radio Radio" archive.
//
// The Giereś archive (docs/orgonity-api.md) is a small Rails app that serves
// its orgonity/echelon back catalogue as JSON over plain HTTP on the LAN. This
// client fetches the four JSON endpoints the archive browser needs and parses
// exactly the documented fields — no generic JSON DOM escapes this header, the
// parse functions below are the only surface (tests feed captured fixtures).
// Transfers ride audio::HttpClient::fetch_text (raw-socket HTTP/1.1, the same
// client the m3u/pls resolvers use); JSON parsing is nlohmann, matching
// provider/radio/catalog.cpp.
#pragma once

#include "audio/http_socket.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::ui {

// GieresBroadcast is one archive recording (orgonity entry or echelon
// segment's underlying broadcast). audio_url is server-relative
// ("/broadcasts/<id>/audio.mp3") — join with GieresClient::url before use.
struct GieresBroadcast {
  long long   id = 0;
  std::string title;
  std::string recording_date;  // YYYY-MM-DD from the title ("" when unknown)
  std::string audio_url;
  int         duration_seconds = -1;  // -1 = unknown (server sends null)
  bool        has_audio        = false;  // false → the audio file 404s
};

// GieresListing is the /orgonity.json page: one 50-recording page plus the
// full date index (recording_dates is complete on every page).
struct GieresListing {
  int                          page        = 1;
  int                          total_pages = 1;
  int                          total_count = 0;
  std::vector<std::string>     recording_dates;  // ascending YYYY-MM-DD
  std::vector<GieresBroadcast> broadcasts;
};

// GieresDay is /orgonity/by_date/:date.json — the recordings of one day.
struct GieresDay {
  std::string                  date;
  std::vector<GieresBroadcast> broadcasts;
};

// GieresTrackRef is one entry of a segment's program listing; position is
// seconds from the segment start.
struct GieresTrackRef {
  std::string title;
  int         position = 0;
};

// GieresSegment is one 30-minute slice of the echelon live timeline.
struct GieresSegment {
  long long               id = 0;
  std::string             title;
  std::string             aired_at;  // ISO8601 UTC emission time
  std::string             url;       // server-relative audio path
  int                     duration = 0;  // seconds
  std::vector<GieresTrackRef> tracks;
};

// GieresClient fetches the archive JSON endpoints. One instance per screen;
// audio::HttpClient is stateless per request, so sharing is safe.
class GieresClient {
public:
  explicit GieresClient(std::string base_url);

  // orgonity list, 1-based `page`, `q` pre-sanitized, `sort` date|title|duration.
  std::expected<GieresListing, std::string>
  orgonity(int page, std::string_view q, std::string_view sort);

  // recordings of one day ("YYYY-MM-DD", also accepts YYYY / YYYY-MM).
  std::expected<GieresDay, std::string> by_date(std::string_view date);

  // echelon timeline window; start/end are ISO8601 UTC instants.
  std::expected<std::vector<GieresSegment>, std::string>
  echelon_segments(std::string_view start_iso, std::string_view end_iso);

  // what the live stream is playing right now.
  std::expected<std::string, std::string> now_playing();

  // url joins a server-relative path ("/broadcasts/1/audio.mp3") with the
  // configured base.
  std::string url(std::string_view path) const;
  const std::string& base_url() const { return base_; }

  // sanitize_query mirrors the server's q cleanup: only [A-Za-z0-9 _-] reach
  // the search (everything else is dropped server-side anyway — sending it
  // would just change the match silently; docs/orgonity-api.md).
  static std::string sanitize_query(std::string_view q);

private:
  std::string       base_;  // no trailing slash
  audio::HttpClient http_;
};

// Parse functions, exposed for the tests (captured JSON fixtures). Network
// failures surface as std::unexpected error strings; a malformed body is an
// error too, never an exception escape.
std::expected<GieresListing, std::string>
parse_orgonity_listing(std::string_view json);
std::expected<GieresDay, std::string> parse_orgonity_day(std::string_view json);
std::expected<std::vector<GieresSegment>, std::string>
parse_echelon_segments(std::string_view json);
std::expected<std::string, std::string>
parse_now_playing(std::string_view json);

}  // namespace bootamp::ui