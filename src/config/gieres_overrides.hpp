// config/gieres_overrides.hpp — user override for the Gieres endpoint.
//
// The Giereś archive (docs/orgonity-api.md) is reachable two ways: the public
// domain (https://gieres.cytr.us — the built-in default) and the author's LAN
// base (http://192.168.1.154:13080, the fastest route on that network). The
// endpoint is chosen in priority order:
//
//   1. <config_dir>/gieres.ini  `base_url` (this file; user pins an endpoint)
//   2. bootamp.toml             `gieres_base_url`
//   3. built-in default         https://gieres.cytr.us
//
// On top of that the archive screen keeps a sticky session fallback: a
// connection-level failure on the chosen base retries the LAN base once and
// sticks to the winner for the session (ui/screens/gieres.cpp for_host).
//
// The parser is deliberately tiny — "key = value" lines, `#`/`;` comments,
// unknown keys ignored — and the testable core takes the file text, so tests
// need no filesystem.
#pragma once

#include <string>
#include <string_view>

namespace bootamp::config {

// parse_gieres_ini_base_url extracts the `base_url` value from one ini text.
// `#`/`;` comment lines, blank lines and unknown keys are skipped; values are
// trimmed and an optional pair of surrounding quotes is stripped. Returns ""
// when the key is absent or empty.
std::string parse_gieres_ini_base_url(std::string_view text);

// gieres_ini_base_url reads <config_dir()>/gieres.ini and returns its
// base_url. A missing file, an unreadable file or no usable value all yield
// "" — the config value stands.
std::string gieres_ini_base_url();

// gieres_base_url_for resolves the endpoint: the ini override when present
// (logged once via applog when it differs from the config value), else the
// config value itself.
std::string gieres_base_url_for(std::string_view config_value);

}  // namespace bootamp::config