// config/config.cpp — load/save the bootamp config file.
//
// Port of cliamp/config/config.go. The Go loader is a hand-rolled, line-by-line
// scanner; per the M0 plan we parse with tomlplusplus instead, which gives the
// same field semantics for valid TOML files (real configs and all Go tests are
// valid TOML). $ENV interpolation, clamping, defaults, and the provider
// section-enabling rules are ported 1:1. save()/save_navidrome_sort() stay
// line-based (matching cliamp's saver.go) so comments and formatting survive.
//
// $ENV interpolation (config.parseString in Go) is private to this package in
// cliamp, so it lives here too. The tomlutil peer module owns the [[section]]
// block reader used by the radio/favorites loaders, not the env rule.
#include "config/config.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <expected>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "foundation/appdir.hpp"
#include "foundation/fileutil.hpp"

namespace bootamp::config {

namespace fs = std::filesystem;

namespace {

// ---- $ENV interpolation (port of config.parseString) --------------------
//
// parseString in Go first strips surrounding quotes, then — only when the
// whole remaining value is exactly `$NAME` or `${NAME}` — substitutes the
// env var (or "" when unset). Mixed values keep their literal '$'.
//
// tomlplusplus already strips quotes for us, so interp_env operates on the
// already-unquoted string and applies only the env-substitution rule. Every
// case of cliamp's TestParseStringEnvInterpolation maps 1:1 to this.

bool is_env_name(std::string_view s) {
  if (s.empty()) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '_') continue;
    if (c >= 'A' && c <= 'Z') continue;
    if (c >= 'a' && c <= 'z') continue;
    if (i > 0 && c >= '0' && c <= '9') continue;
    return false;
  }
  return true;
}

std::string interp_env(std::string_view s) {
  if (s.size() < 2 || s.front() != '$') return std::string(s);
  std::string_view name = s.substr(1);
  if (!name.empty() && name.front() == '{') {
    if (name.back() != '}') return std::string(s);
    name = name.substr(1, name.size() - 2);
  }
  if (!is_env_name(name)) return std::string(s);
  // os.Getenv returns "" for unset vars (no unset-vs-empty distinction).
  if (const char* v = std::getenv(name.data())) return std::string(v);
  return std::string();
}

// str_from_node returns the string payload of a string-valued TOML node with
// $ENV interpolation applied, or std::nullopt when the node is not a string.
std::optional<std::string> str_from_node(const toml::node& n) {
  if (!n.is_string()) return std::nullopt;
  // as_string()->get() returns the raw (already unquoted) TOML string; interp_env
  // then applies the $NAME / ${NAME} env-var rule (no-op for literal values).
  return interp_env(n.as_string()->get());
}

// ---- value extraction helpers -------------------------------------------

std::string tolower_string(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// bool_from_node matches Go's `strings.ToLower(val) == "true|false"` checks,
// accepting either a TOML boolean or a string value.
std::optional<bool> bool_from_node(const toml::node& n) {
  if (n.is_boolean()) return n.as_boolean()->get();
  if (n.is_string()) {
    const auto v = tolower_string(n.as_string()->get());
    if (v == "true")  return true;
    if (v == "false") return false;
  }
  return std::nullopt;
}

// bool_true_only mirrors Go's `val == "true"` (case-sensitive) for fields where
// only an explicit true sets the flag (shuffle/mono/auto_play/simplified). For
// a TOML boolean this is just the value; for a string it must equal "true".
std::optional<bool> bool_true_only(const toml::node& n) {
  if (n.is_boolean()) return n.as_boolean()->get();
  if (n.is_string()) return n.as_string()->get() == "true";
  return std::nullopt;
}

std::optional<int> int_from_node(const toml::node& n) {
  if (n.is_integer()) return static_cast<int>(n.as_integer()->get());
  return std::nullopt;
}

// dbl_from_node reads a float, accepting either a TOML float or an integer
// (Go's strconv.ParseFloat accepts "-6"; TOML renders `-6` as int).
std::optional<double> dbl_from_node(const toml::node& n) {
  if (n.is_floating_point()) return n.as_floating_point()->get();
  if (n.is_integer()) return static_cast<double>(n.as_integer()->get());
  return std::nullopt;
}

// enabled_false_only — opt-out pattern (spotify/qobuz/tidal/ytmusic):
// `enabled = false` sets Disabled=true; any other value leaves Disabled=false.
bool enabled_false_only(const toml::node& n) {
  if (n.is_boolean()) return n.as_boolean()->get() == false;
  if (n.is_string()) return tolower_string(n.as_string()->get()) == "false";
  return false;
}

// enabled_true_only — opt-in pattern (soundcloud/netease):
// `enabled = true` sets Enabled=true; any other value leaves it false.
bool enabled_true_only(const toml::node& n) {
  if (n.is_boolean()) return n.as_boolean()->get();
  if (n.is_string()) return tolower_string(n.as_string()->get()) == "true";
  return false;
}

// parse_string_slice — port of config.parseStringSlice. Accepts a string
// payload (for `libraries = "Music, Jazz"`) or array (handled by callers).
std::vector<std::string> parse_string_slice(std::string_view val) {
  // Strip surrounding [] if present (already-unquoted TOML string may carry them).
  while (!val.empty() && val.front() == '[') val.remove_prefix(1);
  while (!val.empty() && val.back() == ']') val.remove_suffix(1);
  std::vector<std::string> result;
  std::string_view::size_type pos = 0;
  while (pos <= val.size()) {
    auto comma = val.find(',', pos);
    std::string_view tok = (comma == std::string_view::npos)
                               ? val.substr(pos)
                               : val.substr(pos, comma - pos);
    tok = trim(tok);
    // Strip a single layer of surrounding quotes (cliamp trims `"'`).
    if (tok.size() >= 1 && (tok.front() == '"' || tok.front() == '\'')) tok.remove_prefix(1);
    if (tok.size() >= 1 && (tok.back() == '"' || tok.back() == '\'')) tok.remove_suffix(1);
    if (!tok.empty()) result.emplace_back(interp_env(tok));
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return result;
}

// libraries_from_node — accepts a TOML array of strings or a single
// comma-separated string (cliamp parseStringSlice handles both shapes).
std::vector<std::string> libraries_from_node(const toml::node& n) {
  if (n.is_array()) {
    std::vector<std::string> out;
    for (const auto& el : *n.as_array()) {
      if (el.is_string()) {
        auto s = interp_env(el.as_string()->get());
        if (!s.empty()) out.push_back(std::move(s));
      }
    }
    return out;
  }
  if (n.is_string()) return parse_string_slice(n.as_string()->get());
  return {};
}

// ---- clamping helpers (ports of config.go's free functions) --------------

int abs_int(int x) { return x < 0 ? -x : x; }

int nearest_allowed(int v, std::initializer_list<int> allowed) {
  const int* best = allowed.begin();
  int best_dist = abs_int(v - *best);
  for (const int* a = allowed.begin() + 1; a != allowed.end(); ++a) {
    if (int d = abs_int(v - *a); d < best_dist) {
      best = a;
      best_dist = d;
    }
  }
  return *best;
}

int clamp_sample_rate(int v) {
  if (v == 0) return 0;  // auto-detect
  return nearest_allowed(v, {22050, 44100, 48000, 96000, 192000});
}

int clamp_bit_depth(int v) { return v >= 24 ? 32 : 16; }

int clamp_spotify_bitrate(int v) {
  if (v <= 0) return 320;
  return nearest_allowed(v, {96, 160, 320});
}

// ---- field readers --------------------------------------------------------
//
// Each read_* helper pulls a key from a table and applies it to the matching
// Config field, replicating the Go switch case. They are no-ops when the key
// is absent or the value is the wrong type (matching Go's strconv error-skip).

void read_navidrome(const toml::table& t, NavidromeConfig& n) {
  if (auto* v = t["url"].node())         n.url = str_from_node(*v).value_or(n.url);
  if (auto* v = t["user"].node())        n.user = str_from_node(*v).value_or(n.user);
  if (auto* v = t["password"].node())    n.password = str_from_node(*v).value_or(n.password);
  if (auto* v = t["browse_sort"].node()) n.browse_sort = str_from_node(*v).value_or(n.browse_sort);
  if (auto* v = t["scrobble"].node())    n.scrobble_disabled = enabled_false_only(*v);
}

void read_spotify(const toml::table& t, SpotifyConfig& s) {
  if (auto* v = t["enabled"].node())   s.disabled = enabled_false_only(*v);
  if (auto* v = t["client_id"].node()) s.client_id = str_from_node(*v).value_or(s.client_id);
  if (auto* v = t["bitrate"].node()) {
    if (auto i = int_from_node(*v)) s.bitrate = *i;
  }
}

void read_qobuz(const toml::table& t, QobuzConfig& q) {
  if (auto* v = t["enabled"].node())  q.disabled = enabled_false_only(*v);
  if (auto* v = t["quality"].node()) {
    if (auto i = int_from_node(*v)) q.quality = *i;
  }
}

void read_tidal(const toml::table& t, TidalConfig& td) {
  if (auto* v = t["enabled"].node())        td.disabled = enabled_false_only(*v);
  if (auto* v = t["client_id"].node())      td.client_id = str_from_node(*v).value_or(td.client_id);
  if (auto* v = t["client_secret"].node()) td.client_secret = str_from_node(*v).value_or(td.client_secret);
  if (auto* v = t["quality"].node())        td.quality = str_from_node(*v).value_or(td.quality);
}

void read_ytmusic(const toml::table& t, YouTubeMusicConfig& y) {
  if (auto* v = t["enabled"].node())       y.disabled = enabled_false_only(*v);
  if (auto* v = t["client_id"].node())     y.client_id = str_from_node(*v).value_or(y.client_id);
  if (auto* v = t["client_secret"].node()) y.client_secret = str_from_node(*v).value_or(y.client_secret);
  if (auto* v = t["cookies_from"].node()) {
    // Go: strings.TrimSpace(parseString(val)); whitespace-only becomes "".
    if (auto s = str_from_node(*v)) y.cookies_from = std::string(trim(*s));
  }
  if (auto* v = t["expand_playlist"].node()) {
    if (auto b = bool_from_node(*v)) y.expand_playlist = *b;
  }
}

void read_plex(const toml::table& t, PlexConfig& p) {
  if (auto* v = t["url"].node())       p.url = str_from_node(*v).value_or(p.url);
  if (auto* v = t["token"].node())     p.token = str_from_node(*v).value_or(p.token);
  if (auto* v = t["libraries"].node()) p.libraries = libraries_from_node(*v);
}

void read_soundcloud(const toml::table& t, SoundCloudConfig& s) {
  if (auto* v = t["enabled"].node())      s.enabled = enabled_true_only(*v);
  if (auto* v = t["user"].node())         s.user = str_from_node(*v).value_or(s.user);
  if (auto* v = t["cookies_from"].node()) {
    if (auto sv = str_from_node(*v)) s.cookies_from = std::string(trim(*sv));
  }
}

void read_netease(const toml::table& t, NetEaseConfig& n) {
  if (auto* v = t["enabled"].node())      n.enabled = enabled_true_only(*v);
  if (auto* v = t["cookies_from"].node()) {
    if (auto sv = str_from_node(*v)) n.cookies_from = std::string(trim(*sv));
  }
  if (auto* v = t["user_id"].node())      n.user_id = str_from_node(*v).value_or(n.user_id);
}

void read_jellyfin(const toml::table& t, JellyfinConfig& j) {
  if (auto* v = t["url"].node())      j.url = str_from_node(*v).value_or(j.url);
  if (auto* v = t["token"].node())    j.token = str_from_node(*v).value_or(j.token);
  if (auto* v = t["user"].node())     j.user = str_from_node(*v).value_or(j.user);
  if (auto* v = t["password"].node()) j.password = str_from_node(*v).value_or(j.password);
  if (auto* v = t["user_id"].node())  j.user_id = str_from_node(*v).value_or(j.user_id);
}

void read_emby(const toml::table& t, EmbyConfig& e) {
  if (auto* v = t["url"].node())      e.url = str_from_node(*v).value_or(e.url);
  if (auto* v = t["token"].node())    e.token = str_from_node(*v).value_or(e.token);
  if (auto* v = t["user"].node())     e.user = str_from_node(*v).value_or(e.user);
  if (auto* v = t["password"].node()) e.password = str_from_node(*v).value_or(e.password);
  if (auto* v = t["user_id"].node())  e.user_id = str_from_node(*v).value_or(e.user_id);
}

void read_audiobookshelf(const toml::table& t, AudiobookshelfConfig& a) {
  if (auto* v = t["url"].node())       a.url = str_from_node(*v).value_or(a.url);
  if (auto* v = t["token"].node())     a.token = str_from_node(*v).value_or(a.token);
  if (auto* v = t["user"].node())      a.user = str_from_node(*v).value_or(a.user);
  if (auto* v = t["password"].node())  a.password = str_from_node(*v).value_or(a.password);
  if (auto* v = t["libraries"].node()) a.libraries = libraries_from_node(*v);
}

// read_eq parses the `eq` array (TOML native) into 10 bands clamped to [-12,12].
// Matches cliamp's parseEQ: first 10 entries win, missing slots stay 0.
std::array<double, 10> read_eq(const toml::node& n) {
  std::array<double, 10> bands{};
  if (!n.is_array()) return bands;
  const auto* arr = n.as_array();
  for (std::size_t i = 0; i < bands.size() && i < arr->size(); ++i) {
    double v = 0.0;
    const auto& el = (*arr)[i];
    if (el.is_floating_point()) v = el.as_floating_point()->get();
    else if (el.is_integer())   v = static_cast<double>(el.as_integer()->get());
    bands[i] = std::clamp(v, -12.0, 12.0);
  }
  return bands;
}

// read_plugins walks the [plugins] table: top-level keys go into plugins[""],
// sub-tables [plugins.<name>] go into plugins["<name>"]. Matches cliamp's
// section handling (pluginName == "" for the bare [plugins] section).
void read_plugins(const toml::table& t, Config& cfg) {
  for (const auto& [k, node] : t) {
    const std::string name(k.str());
    if (node.is_table()) {
      auto& sub = cfg.plugins[name];
      for (const auto& [pk, pv] : *node.as_table()) {
        if (pv.is_string()) sub[std::string(pk.str())] = interp_env(pv.as_string()->get());
      }
    } else if (node.is_string()) {
      cfg.plugins[""][name] = interp_env(node.as_string()->get());
    }
  }
}

bool is_section_header(std::string_view line) {
  return line.size() >= 2 && line.front() == '[' && line.back() == ']';
}

std::string_view section_name(std::string_view header) {
  return header.substr(1, header.size() - 2);
}

}  // namespace

// ---- Config::clamp -------------------------------------------------------

void Config::clamp() {
  volume_min = std::clamp(volume_min, -90.0, 0.0);
  volume     = std::clamp(volume, volume_min, 6.0);
  if (speed < 0.25 || speed > 2.0) speed = 1.0;
  seek_step_large   = std::clamp(seek_step_large, 6, 600);
  sample_rate       = clamp_sample_rate(sample_rate);
  buffer_ms         = std::clamp(buffer_ms, 50, 5000);
  resample_quality  = std::clamp(resample_quality, 1, 4);
  bit_depth         = clamp_bit_depth(bit_depth);
  spotify.bitrate   = clamp_spotify_bitrate(spotify.bitrate);
  padding_h         = std::clamp(padding_h, 0, 10);
  padding_v         = std::clamp(padding_v, 0, 5);
  if (low_power) visualizer = "none";
}

// ---- default_config -------------------------------------------------------

Config default_config() {
  // Defaults are set by struct member initializers (config.hpp), mirroring
  // cliamp's defaultConfig() exactly. Return a fresh copy.
  return Config{};
}

// ---- config_path / load / save -------------------------------------------

std::expected<std::string, std::string> config_path() {
  auto dir = foundation::config_dir();
  if (!dir) return std::unexpected(std::move(dir).error());
  return (dir.value() / "config.toml").string();
}

std::expected<Config, std::string> load() {
  Config cfg = default_config();

  auto path = config_path();
  if (!path) return cfg;  // matches Go: appdir error → defaults, no error

  // Missing file → defaults (matches Go fs.ErrNotExist handling).
  std::error_code ec;
  if (!fs::exists(path.value(), ec)) return cfg;

  // tomlplusplus' parse_file returns toml::table and throws toml::parse_error
  // on failure when exceptions are enabled (the default). Config loading is
  // NOT the audio hot path, so catching here is fine; the error is surfaced as
  // a std::expected error string. This is a deliberate, documented deviation
  // from cliamp's lenient line scanner (which returns cfg + scanner.Err() and
  // skips malformed lines); tomlplusplus is all-or-nothing.
  toml::table tbl;
  try {
    tbl = toml::parse_file(path.value());
  } catch (const toml::parse_error& e) {
    std::ostringstream os;
    os << "config: failed to parse " << path.value() << ": " << e.description();
    return std::unexpected(os.str());
  }

  // Section presence enables the opt-out providers (cliamp sets Enabled=true
  // when the section header appears, regardless of keys). [yt]/[youtube]/
  // [ytmusic] all configure YouTubeMusic.
  if (tbl.contains("yt") || tbl.contains("youtube") || tbl.contains("ytmusic"))
    cfg.ytmusic.enabled = true;
  if (tbl.contains("spotify")) cfg.spotify.enabled = true;
  if (tbl.contains("qobuz"))   cfg.qobuz.enabled   = true;
  if (tbl.contains("tidal"))   cfg.tidal.enabled   = true;

  if (const auto* t = tbl["navidrome"].as_table())      read_navidrome(*t, cfg.navidrome);
  if (const auto* t = tbl["spotify"].as_table())       read_spotify(*t, cfg.spotify);
  if (const auto* t = tbl["qobuz"].as_table())         read_qobuz(*t, cfg.qobuz);
  if (const auto* t = tbl["tidal"].as_table())         read_tidal(*t, cfg.tidal);
  if (const auto* t = tbl["ytmusic"].as_table())        read_ytmusic(*t, cfg.ytmusic);
  if (const auto* t = tbl["plex"].as_table())          read_plex(*t, cfg.plex);
  if (const auto* t = tbl["soundcloud"].as_table())    read_soundcloud(*t, cfg.soundcloud);
  if (const auto* t = tbl["netease"].as_table())       read_netease(*t, cfg.netease);
  if (const auto* t = tbl["jellyfin"].as_table())      read_jellyfin(*t, cfg.jellyfin);
  if (const auto* t = tbl["emby"].as_table())          read_emby(*t, cfg.emby);
  if (const auto* t = tbl["audiobookshelf"].as_table())read_audiobookshelf(*t, cfg.audiobookshelf);
  if (const auto* t = tbl["plugins"].as_table())       read_plugins(*t, cfg);

  // Top-level keys (cliamp's `default` switch branch).
  if (auto* v = tbl["volume"].node()) {
    if (auto d = dbl_from_node(*v)) cfg.volume = *d;
  }
  if (auto* v = tbl["volume_min"].node()) {
    if (auto d = dbl_from_node(*v)) cfg.volume_min = *d;
  }
  if (auto* v = tbl["vis_volume_linked"].node()) {
    if (auto b = bool_from_node(*v)) cfg.vis_volume_linked = *b;
  }
  if (auto* v = tbl["repeat"].node()) {
    if (auto s = str_from_node(*v)) {
      auto low = tolower_string(*s);
      if (low == "all" || low == "one" || low == "off") cfg.repeat = low;
    }
  }
  if (auto* v = tbl["shuffle"].node()) {
    if (auto b = bool_true_only(*v)) cfg.shuffle = *b;
  }
  if (auto* v = tbl["mono"].node()) {
    if (auto b = bool_true_only(*v)) cfg.mono = *b;
  }
  if (auto* v = tbl["auto_play"].node()) {
    if (auto b = bool_true_only(*v)) cfg.auto_play = *b;
  }
  if (auto* v = tbl["seek_large_step_sec"].node()) {
    if (auto i = int_from_node(*v)) cfg.seek_step_large = *i;
  }
  if (auto* v = tbl["eq"].node()) cfg.eq = read_eq(*v);
  if (auto* v = tbl["eq_preset"].node()) {
    if (auto s = str_from_node(*v)) cfg.eq_preset = *s;
  }
  if (auto* v = tbl["theme"].node()) {
    if (auto s = str_from_node(*v)) cfg.theme = *s;
  }
  if (auto* v = tbl["provider"].node()) {
    if (auto s = str_from_node(*v)) cfg.provider = tolower_string(*s);
  }
  if (auto* v = tbl["visualizer"].node()) {
    if (auto s = str_from_node(*v)) cfg.visualizer = *s;
  }
  if (auto* v = tbl["sample_rate"].node()) {
    if (auto i = int_from_node(*v)) cfg.sample_rate = *i;
  }
  if (auto* v = tbl["buffer_ms"].node()) {
    if (auto i = int_from_node(*v)) cfg.buffer_ms = *i;
  }
  if (auto* v = tbl["resample_quality"].node()) {
    if (auto i = int_from_node(*v)) cfg.resample_quality = *i;
  }
  if (auto* v = tbl["bit_depth"].node()) {
    if (auto i = int_from_node(*v)) cfg.bit_depth = *i;
  }
  if (auto* v = tbl["speed"].node()) {
    if (auto d = dbl_from_node(*v)) cfg.speed = *d;
  }
  if (auto* v = tbl["simplified"].node()) {
    if (auto b = bool_true_only(*v)) cfg.simplified = *b;
  }
  if (auto* v = tbl["audio_device"].node()) {
    if (auto s = str_from_node(*v)) cfg.audio_device = *s;
  }
  if (auto* v = tbl["initial_directory"].node()) {
    if (auto s = str_from_node(*v)) cfg.initial_directory = *s;
  }
  if (auto* v = tbl["padding_horizontal"].node()) {
    if (auto i = int_from_node(*v)) cfg.padding_h = *i;
  }
  if (auto* v = tbl["padding_vertical"].node()) {
    if (auto i = int_from_node(*v)) cfg.padding_v = *i;
  }
  if (auto* v = tbl["log_level"].node()) {
    if (auto s = str_from_node(*v)) {
      auto lvl = tolower_string(*s);
      if (lvl == "debug" || lvl == "info" || lvl == "warn" ||
          lvl == "warning" || lvl == "error") cfg.log_level = lvl;
    }
  }
  if (auto* v = tbl["low_power"].node()) {
    if (auto b = bool_from_node(*v)) cfg.low_power = *b;
  }

  cfg.clamp();
  return cfg;
}

// save — port of cliamp config.Save: in-place top-level key rewrite, preserving
// comments/formatting/section blocks. Atomic write via fileutil.
std::expected<void, std::string> save(std::string_view key, std::string_view value) {
  auto path = config_path();
  if (!path) return std::unexpected(std::move(path).error());

  if (auto d = foundation::ensure_dir(fs::path(path.value()).parent_path()); !d)
    return std::unexpected(std::move(d).error());

  const std::string line = std::string(key) + " = " + std::string(value);

  std::error_code ec;
  if (!fs::exists(path.value(), ec)) {
    return foundation::write_file_atomic(path.value(), line + "\n", 0600);
  }

  auto contents = foundation::read_file(path.value());
  if (!contents) return std::unexpected(std::move(contents).error());

  std::vector<std::string> lines;
  {
    std::string data = std::move(contents).value();
    std::string cur;
    for (char c : data) {
      if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
      else cur.push_back(c);
    }
    lines.push_back(std::move(cur));  // trailing field (may be empty)
  }

  bool found = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    auto trimmed = trim(lines[i]);
    if (trimmed.empty() || trimmed.front() == '#') continue;
    if (is_section_header(trimmed)) break;  // only top-level scope
    auto eq = trimmed.find('=');
    if (eq == std::string_view::npos) continue;
    if (trim(trimmed.substr(0, eq)) == key) {
      lines[i] = line;
      found = true;
      break;
    }
  }

  if (!found) {
    bool inserted = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      auto trimmed = trim(lines[i]);
      if (is_section_header(trimmed)) {
        lines.insert(lines.begin() + static_cast<ptrdiff_t>(i), line);
        inserted = true;
        break;
      }
    }
    if (!inserted) lines.push_back(line);
  }

  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i) out.push_back('\n');
    out += lines[i];
  }
  out.push_back('\n');
  return foundation::write_file_atomic(path.value(), out, 0600);
}

// save_navidrome_sort — port of cliamp config.SaveNavidromeSort: rewrite or
// append the browse_sort key inside the [navidrome] section.
std::expected<void, std::string> save_navidrome_sort(std::string_view sort_type) {
  auto path = config_path();
  if (!path) return std::unexpected(std::move(path).error());

  if (auto d = foundation::ensure_dir(fs::path(path.value()).parent_path()); !d)
    return std::unexpected(std::move(d).error());

  // Go: fmt.Sprintf("browse_sort = %q", sortType) — TOML double-quoted string.
  const std::string line = "browse_sort = \"" + std::string(sort_type) + "\"";

  std::error_code ec;
  if (!fs::exists(path.value(), ec)) {
    return foundation::write_file_atomic(path.value(),
                                         "[navidrome]\n" + line + "\n", 0600);
  }

  auto contents = foundation::read_file(path.value());
  if (!contents) return std::unexpected(std::move(contents).error());

  std::vector<std::string> lines;
  {
    std::string data = std::move(contents).value();
    std::string cur;
    for (char c : data) {
      if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
      else cur.push_back(c);
    }
    lines.push_back(std::move(cur));
  }

  // Pass 1: replace an existing browse_sort inside [navidrome].
  bool in_navidrome = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    auto trimmed = trim(lines[i]);
    if (is_section_header(trimmed)) {
      in_navidrome = tolower_string(section_name(trimmed)) == "navidrome";
      continue;
    }
    if (in_navidrome) {
      auto eq = trimmed.find('=');
      if (eq != std::string_view::npos && trim(trimmed.substr(0, eq)) == "browse_sort") {
        lines[i] = line;
        std::string out;
        for (std::size_t j = 0; j < lines.size(); ++j) {
          if (j) out.push_back('\n');
          out += lines[j];
        }
        out.push_back('\n');
        return foundation::write_file_atomic(path.value(), out, 0600);
      }
    }
  }

  // Pass 2: key not found — append after the last line inside [navidrome],
  // or append a new [navidrome] section at the end.
  in_navidrome = false;
  int insert_at = -1;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    auto trimmed = trim(lines[i]);
    if (is_section_header(trimmed)) {
      if (in_navidrome && insert_at >= 0) break;  // moved past [navidrome]
      in_navidrome = tolower_string(section_name(trimmed)) == "navidrome";
    }
    if (in_navidrome) insert_at = static_cast<int>(i);
  }

  if (insert_at >= 0) {
    lines.insert(lines.begin() + insert_at + 1, line);
  } else {
    lines.push_back("[navidrome]");
    lines.push_back(line);
  }

  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i) out.push_back('\n');
    out += lines[i];
  }
  out.push_back('\n');
  return foundation::write_file_atomic(path.value(), out, 0600);
}

// ---- apply_player / apply_playlist ---------------------------------------

void apply_player(const Config& cfg, PlayerConfig& p) {
  p.set_volume_min(cfg.volume_min);
  p.set_volume(cfg.volume);
  if (cfg.speed != 0.0 && cfg.speed != 1.0) p.set_speed(cfg.speed);
  if (cfg.eq_preset.empty() || cfg.eq_preset == "Custom") {
    for (int i = 0; i < 10; ++i) p.set_eq_band(i, cfg.eq[static_cast<std::size_t>(i)]);
  }
  if (cfg.mono) p.toggle_mono();
}

void apply_playlist(const Config& cfg, PlaylistConfig& pl) {
  // Start state is "off"; cycle to the configured value (all=1, one=2 cycles).
  if (cfg.repeat == "all") {
    pl.cycle_repeat();  // off -> all
  } else if (cfg.repeat == "one") {
    pl.cycle_repeat();  // off -> all
    pl.cycle_repeat();  // all -> one
  }
  if (cfg.shuffle) pl.toggle_shuffle();
}

}  // namespace bootamp::config