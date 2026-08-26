// config/config.hpp — full user configuration loaded from
// ~/.config/bootamp/config.toml.
//
// Port of cliamp/config/config.go. The Go config is ~39 fields plus nested
// provider structs (Navidrome/Spotify/Qobuz/Tidal/YouTubeMusic/SoundCloud/
// NetEase/Plex/Jellyfin/Emby/Audiobookshelf). bootamp adds Provider, BufferMs,
// ytmusic.cookies_from, soundcloud.cookies_from per the plan, and keeps the
// same defaults, clamping, and $ENV interpolation rules.
#pragma once

#include <array>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::config {

// NavidromeConfig — Navidrome/Subsonic server credentials.
// All three of url/user/password must be non-empty for a client to construct.
struct NavidromeConfig {
  std::string url;             // e.g. "https://music.example.com"
  std::string user;
  std::string password;
  std::string browse_sort;      // album browse sort order
  bool        scrobble_disabled = false;  // true only when scrobble=false set
  bool is_set() const { return !url.empty() && !user.empty() && !password.empty(); }
};

// SpotifyConfig — requires Spotify Premium. If client_id is empty, a built-in
// fallback (librespot keymaster ID) is used so search/catalog work for users
// who never registered their own dev app.
struct SpotifyConfig {
  bool        disabled = false;  // true only when enabled=false explicitly
  bool        enabled  = false;   // true when [spotify] section exists
  std::string client_id;
  int         bitrate  = 320;     // preferred stream bitrate (96/160/320)
  bool is_set() const { return !disabled && enabled; }
  std::string resolve_client_id(std::string_view fallback_id) const {
    return client_id.empty() ? std::string(fallback_id) : client_id;
  }
};

// QobuzConfig — requires paid Qobuz subscription (Studio/Sublime).
struct QobuzConfig {
  bool disabled = false;
  bool enabled  = false;
  int  quality  = 6;  // format_id: 5 MP3, 6 FLAC CD, 7 HiRes<=96k, 27 HiRes<=192k
  bool is_set() const { return !disabled && enabled; }
};

// TidalConfig — requires paid Tidal subscription (all plans include lossless).
struct TidalConfig {
  bool        disabled      = false;
  bool        enabled       = false;
  std::string client_id;     // overrides built-in fallback
  std::string client_secret;
  std::string quality;       // "low"/"high"/"lossless"/"hires"
  bool is_set() const { return !disabled && enabled; }
};

// YouTubeMusicConfig — cookies_from drives both player-side and resolve-side
// --cookies-from-browser. expand_playlist is std::optional<bool>: nullopt =
// default (true), controls whether list= URLs expand the full playlist.
struct YouTubeMusicConfig {
  bool                 disabled        = false;
  bool                 enabled         = false;
  std::string          client_id;
  std::string          client_secret;
  std::string          cookies_from;    // browser name for yt-dlp
  std::optional<bool>  expand_playlist;
  bool is_set_or_fallback(/*fallback_fn*/ std::string_view fb_id,
                          std::string_view fb_secret) const {
    if (disabled) return false;
    if (enabled || !cookies_from.empty()) return true;
    return !fb_id.empty() && !fb_secret.empty();
  }
};

// SoundCloudConfig — opt-in (requires enabled=true). User exposes profile
// Tracks/Likes/Reposts in browse; CookiesFrom lets yt-dlp use the signed-in
// session for subscriber-gated tracks.
struct SoundCloudConfig {
  bool        enabled      = false;
  std::string user;
  std::string cookies_from;
  bool is_set() const { return enabled; }
};

// NetEaseConfig — opt-in, reuses an existing browser session via yt-dlp.
struct NetEaseConfig {
  bool        enabled      = false;
  std::string cookies_from;
  std::string user_id;
  bool is_set() const { return enabled; }
};

// PlexConfig — Plex Media Server. Both url+token required.
struct PlexConfig {
  std::string              url;
  std::string              token;
  std::vector<std::string> libraries;  // restrict to these music library names
  bool is_set() const { return !url.empty() && !token.empty(); }
};

// JellyfinConfig — url required; auth via token OR user+password.
struct JellyfinConfig {
  std::string url;
  std::string token;
  std::string user;
  std::string password;
  std::string user_id;
  bool is_set() const {
    return !url.empty() && (!token.empty() || (!user.empty() && !password.empty()));
  }
};

// EmbyConfig — same shape as Jellyfin.
struct EmbyConfig {
  std::string url;
  std::string token;
  std::string user;
  std::string password;
  std::string user_id;
  bool is_set() const {
    return !url.empty() && (!token.empty() || (!user.empty() && !password.empty()));
  }
};

// AudiobookshelfConfig — url required; auth via token OR user+password.
struct AudiobookshelfConfig {
  std::string              url;
  std::string              token;
  std::string              user;
  std::string              password;
  std::vector<std::string> libraries;
  bool is_set() const {
    return !url.empty() && (!token.empty() || (!user.empty() && !password.empty()));
  }
};

// Config — the full user-preference struct (~39 fields). Field order and
// semantics match cliamp's Config 1:1; added: Provider, BufferMs,
// YouTubeMusic.CookiesFrom, SoundCloud.CookiesFrom.
struct Config {
  // Audio controls
  double                 volume            = 0.0;    // dB, clamped [VolumeMin, +24]
  double                 volume_min        = -50.0;  // dB floor [-90, 0]
  bool                   vis_volume_linked = true;
  std::array<double, 10> eq                = {};
  std::string            eq_preset;
  std::string            repeat;                      // "off"/"all"/"one"
  bool                   shuffle           = false;
  bool                   mono              = false;
  double                 speed             = 1.0;    // 0.25-2.0
  bool                   auto_play         = false;
  int                    seek_step_large   = 30;      // seconds, [6, 600]

  // Provider / UI
  std::string provider;          // "radio"/"navidrome"/"spotify"/... (default "radio")
  std::string theme;
  std::string visualizer;        // "" = default (Bars)

  // Output device / resampling
  int          sample_rate      = 0;     // 0 = auto-detect; else 22050/44100/48000/96000/192000
  int          buffer_ms        = 250;    // speaker buffer ms [50, 5000]
  int          resample_quality = 4;      // 1-4
  int          bit_depth        = 16;     // 16 or 32
  bool         simplified       = false;
  int          padding_h        = 3;       // [0, 10]
  int          padding_v        = 1;       // [0, 5]
  std::string  audio_device;                // empty = system default
  std::string  playlist;                    // local TOML playlist name to load
  std::string  initial_directory;

  // Provider credentials
  NavidromeConfig        navidrome;
  SpotifyConfig          spotify;
  QobuzConfig            qobuz;
  TidalConfig            tidal;
  YouTubeMusicConfig     ytmusic;
  PlexConfig             plex;
  JellyfinConfig         jellyfin;
  EmbyConfig             emby;
  AudiobookshelfConfig   audiobookshelf;
  SoundCloudConfig       soundcloud;
  NetEaseConfig          netease;

  // Misc
  std::map<std::string, std::map<std::string, std::string>> plugins;  // [plugins.*]
  std::string log_level = "info";       // debug/info/warn/error
  bool        low_power = false;        // disables visualization when true

  // clamp() constrains all fields to their valid ranges. Idempotent.
  void clamp();

  // seek_step_large_duration() returns the configured Shift+Left/Right jump in
  // seconds (matches Go's SeekStepLargeDuration()).
  int seek_step_large_duration() const { return seek_step_large; }
};

// default_config returns a Config with sensible defaults (cliamp defaultConfig).
Config default_config();

// config_path returns <config_dir>/config.toml.
std::expected<std::string, std::string> config_path();

// load reads the config file from <config_dir>/config.toml. Returns defaults
// if the file does not exist (matches Go: fs.ErrNotExist → defaults, no error).
std::expected<Config, std::string> load();

// save updates only the given top-level key in the existing config file,
// preserving all other content/comments/formatting. If the key doesn't exist
// it is appended before the first section header; if no file exists one is
// created. Port of cliamp config.Save.
std::expected<void, std::string> save(std::string_view key, std::string_view value);

// save_navidrome_sort persists the album browse sort type to [navidrome].
// Port of cliamp config.SaveNavidromeSort.
std::expected<void, std::string> save_navidrome_sort(std::string_view sort_type);

// Overrides carries CLI flag values that override the loaded config. Fields
// are std::optional; only set values are applied. Port of cliamp's
// config.Overrides (in config/flags.go). Applied via apply_overrides().
struct Overrides {
  std::optional<double>                 volume;
  std::optional<double>                 speed;
  std::optional<std::array<double, 10>> eq;
  std::optional<std::string>            visualizer;
  std::optional<bool>                   shuffle;
  std::optional<std::string>            repeat;
  std::optional<std::string>            cookies_from;  // yt-dlp --cookies-from-browser
  std::optional<bool>                   mono;
};

// apply_overrides mutates `cfg` in place, setting each provided field. The eq
// array replaces cfg.eq wholesale. cookies_from is routed to ytmusic (and, if
// the provider is soundcloud, to soundcloud) to match cliamp's flag handling.
void apply_overrides(Config& cfg, const Overrides& ov);

// PlayerConfig is the subset of player controls needed to apply config live
// (cliamp config.PlayerConfig). The engine implements it; tests mock it.
class PlayerConfig {
public:
  virtual ~PlayerConfig() = default;
  virtual void set_volume_min(double db) = 0;
  virtual void set_volume(double db)     = 0;
  virtual void set_speed(double ratio)   = 0;
  virtual void set_eq_band(int band, double db) = 0;
  virtual void toggle_mono()             = 0;
};

// PlaylistConfig is the subset of playlist controls needed to apply config.
class PlaylistConfig {
public:
  virtual ~PlaylistConfig() = default;
  virtual void cycle_repeat()  = 0;
  virtual void toggle_shuffle() = 0;
};

// apply_player applies audio-engine settings from the config (cliamp
// ApplyPlayer): volume_min, volume, speed, custom EQ bands, mono.
void apply_player(const Config& cfg, PlayerConfig& p);

// apply_playlist applies playlist-state settings (repeat/shuffle) by cycling
// from the default off state to the configured value, matching cliamp.
void apply_playlist(const Config& cfg, PlaylistConfig& pl);

}  // namespace bootamp::config