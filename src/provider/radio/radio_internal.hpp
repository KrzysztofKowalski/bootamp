// provider/radio/radio_internal.hpp — internal detail surface of the radio
// provider module. Shared between the radio TUs (provider.cpp, catalog.cpp,
// favorites.cpp) and with tests. Not part of the public module interface; do
// not depend on this from outside the provider/radio subsystem.
#pragma once

#include "provider/radio/radio.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::provider::radio::detail {

// strict_atoi mirrors Go strconv.Atoi for the small integer fields used here
// (station indices, bitrates, limits): optional sign, decimal digits only.
// Returns nullopt on empty input, non-digits, or int overflow.
std::optional<int> strict_atoi(std::string_view s);

// search_stations_url / top_stations_url build the Radio Browser API URLs
// using the exact Go templates (cliamp catalog.go:63-83). Query escaping is
// Go url.PathEscape (encodePathSegment). limit<=0 -> 50; offset<0 -> 0.
std::string search_stations_url(std::string_view query, int limit);
std::string top_stations_url(int offset, int limit);

// parse_catalog_stations decodes a Radio Browser station-list JSON array
// (fields: name, url_resolved, country, tags, codec, bitrate, votes,
// homepage). Missing fields default; type mismatches and malformed JSON
// produce an error, matching Go json.Unmarshal behavior.
std::expected<std::vector<CatalogStation>, std::string>
parse_catalog_stations(std::string_view json);

// parse_radio_stats decodes the cliamp radio statistics JSON object
// (total_sessions, total_listen_hours, peak_listeners, stations{...}).
std::expected<RadioStats, std::string> parse_radio_stats(std::string_view json);

// Network endpoints (libcurl, 10s timeout, User-Agent "cliamp/1.0"). The
// catalog client maps non-200 responses to "radio-browser: HTTP <code>" and
// the stats endpoint to "cliamp radio stats: HTTP <code>" (catalog.go).
std::expected<std::vector<CatalogStation>, std::string>
search_stations(std::string_view query, int limit);
std::expected<std::vector<CatalogStation>, std::string>
top_stations_offset(int offset, int limit);
std::expected<RadioStats, std::string> fetch_stats();

}  // namespace bootamp::provider::radio::detail
