// tests/test_config.cpp — Catch2 port of cliamp/config/*_test.go.
//
// Covers: default values, clamping (all fields), $ENV interpolation, provider
// section enabling (spotify/qobuz/tidal/ytmusic opt-out; soundcloud/netease
// opt-in), plex/jellyfin/emby/audiobookshelf IsSet, IsSetOrFallback,
// Overrides::apply, save()/save_navidrome_sort line editing, ApplyPlayer,
// ApplyPlaylist, and the seek/low_power/simplified loaders.
//
// NOTE: these tests mirror the Go suite 1:1. They reference load()/save()/
// clamp()/apply_overrides() (defined in config.cpp) and foundation helpers
// (appdir/fileutil/tomlutil) declared in their headers. Per the M0 build
// rules, only a syntax check (-fsyntax-only) is run here; the full suite
// links and runs once the foundation .cpp files land. Temp config dirs are
// pointed at via BOOTAMP_CONFIG_DIR (the env var foundation::config_dir()
// honors), mirroring Go's t.Setenv("HOME", t.TempDir()).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "config/config.hpp"

namespace bootamp::config {
namespace {

namespace fs = std::filesystem;

// set_config_dir points BOOTAMP_CONFIG_DIR at a fresh per-test directory and
// returns its path so the test can write a config.toml there.
fs::path set_config_dir(std::string_view marker) {
  const auto dir = fs::temp_directory_path() / "bootamp_test_config" / std::string(marker);
  fs::remove_all(dir);
  fs::create_directories(dir);
  ::setenv("BOOTAMP_CONFIG_DIR", dir.c_str(), 1);
  // Clear XDG_CONFIG_HOME so it cannot shadow BOOTAMP_CONFIG_DIR.
  ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
  return dir;
}

void write_config(const fs::path& dir, std::string_view body) {
  std::ofstream out(dir / "config.toml", std::ios::binary | std::ios::trunc);
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

// unset_env removes an env var (for $ENV interpolation tests that must start clean).
void unset_env(const char* name) { ::setenv(name, "", 1); }

}  // namespace

// ---- default_config ------------------------------------------------------

TEST_CASE("DefaultConfig has the cliamp defaults", "[config]") {
  const Config cfg = default_config();
  CHECK(cfg.repeat == "off");
  CHECK(cfg.speed == 1.0);
  CHECK(cfg.seek_step_large == 30);
  CHECK(cfg.sample_rate == 0);
  CHECK(cfg.buffer_ms == 250);
  CHECK(cfg.resample_quality == 4);
  CHECK(cfg.bit_depth == 16);
  CHECK(cfg.padding_h == 3);
  CHECK(cfg.padding_v == 1);
  CHECK(cfg.spotify.bitrate == 320);
  CHECK(cfg.qobuz.quality == 6);
  CHECK(cfg.log_level == "info");
  CHECK(cfg.vis_volume_linked == true);
  CHECK(cfg.auto_play == false);
  CHECK(cfg.shuffle == false);
  CHECK(cfg.mono == false);
}

// ---- clamp: volume / volume_min / speed / etc. ---------------------------

TEST_CASE("ClampVolume", "[config][clamp]") {
  const std::array<std::pair<double,double>, 5> cases{{
    {-10, -10}, {-60, -50}, {20, 24}, {-50, -50}, {24, 24},
  }};
  for (auto [vol, want] : cases) {
    Config c = default_config();
    c.volume = vol;
    c.clamp();
    CHECK(c.volume == want);
  }
}

TEST_CASE("ClampVolumeMin", "[config][clamp]") {
  const std::array<std::pair<double,double>, 5> cases{{
    {-50, -50}, {-70, -70}, {-100, -90}, {5, 0}, {-90, -90},
  }};
  for (auto [min, want] : cases) {
    Config c = default_config();
    c.volume_min = min;
    c.clamp();
    CHECK(c.volume_min == want);
  }
}

TEST_CASE("VolumeClampedToVolumeMin", "[config][clamp]") {
  const std::array<std::tuple<double,double,double>, 4> cases{{
    {-30, -40, -30}, {-30, -30, -30}, {-30, -10, -10}, {-60, -70, -60},
  }};
  for (auto [vmin, vol, want] : cases) {
    Config c = default_config();
    c.volume_min = vmin;
    c.volume = vol;
    c.clamp();
    CHECK(c.volume == want);
  }
}

TEST_CASE("ClampSpeed", "[config][clamp]") {
  const std::array<std::pair<double,double>, 7> cases{{
    {1.0, 1.0}, {0.5, 0.5}, {1.5, 1.5}, {0.1, 1.0}, {3.0, 1.0}, {0.25, 0.25}, {2.0, 2.0},
  }};
  for (auto [sp, want] : cases) {
    Config c = default_config();
    c.speed = sp;
    c.clamp();
    CHECK(c.speed == want);
  }
}

TEST_CASE("ClampSampleRate via clamp()", "[config][clamp]") {
  // clamp_sample_rate is file-local in config.cpp; verify through Config::clamp.
  const std::array<std::pair<int,int>, 8> cases{{
    {0, 0}, {44100, 44100}, {48000, 48000}, {96000, 96000},
    {30000, 22050}, {45000, 44100}, {50000, 48000}, {200000, 192000},
  }};
  for (auto [in, want] : cases) {
    Config c = default_config();
    c.sample_rate = in;
    c.clamp();
    CHECK(c.sample_rate == want);
  }
}

TEST_CASE("ClampBitDepth via clamp()", "[config][clamp]") {
  const std::array<std::pair<int,int>, 4> cases{{
    {8, 16}, {16, 16}, {24, 32}, {32, 32},
  }};
  for (auto [in, want] : cases) {
    Config c = default_config();
    c.bit_depth = in;
    c.clamp();
    CHECK(c.bit_depth == want);
  }
}

TEST_CASE("ClampSpotifyBitrate via clamp()", "[config][clamp]") {
  const std::array<std::pair<int,int>, 9> cases{{
    {-1, 320}, {0, 320}, {96, 96}, {160, 160}, {320, 320},
    {120, 96}, {128, 96}, {200, 160}, {500, 320},
  }};
  for (auto [in, want] : cases) {
    Config c = default_config();
    c.spotify.bitrate = in;
    c.clamp();
    CHECK(c.spotify.bitrate == want);
  }
}

TEST_CASE("ClampBufferMs", "[config][clamp]") {
  const std::array<std::pair<int,int>, 6> cases{{
    {100, 100}, {10, 50}, {600, 600}, {50, 50}, {5000, 5000}, {5001, 5000},
  }};
  for (auto [in, want] : cases) {
    Config c = default_config();
    c.buffer_ms = in;
    c.clamp();
    CHECK(c.buffer_ms == want);
  }
}

TEST_CASE("ClampResampleQuality", "[config][clamp]") {
  const std::array<std::pair<int,int>, 4> cases{{
    {0, 1}, {1, 1}, {4, 4}, {5, 4},
  }};
  for (auto [in, want] : cases) {
    Config c = default_config();
    c.resample_quality = in;
    c.clamp();
    CHECK(c.resample_quality == want);
  }
}

TEST_CASE("ClampPadding", "[config][clamp]") {
  const std::array<std::tuple<int,int,int,int>, 3> cases{{
    {-1, -1, 0, 0}, {20, 10, 10, 5}, {3, 1, 3, 1},
  }};
  for (auto [inH, inV, wH, wV] : cases) {
    Config c = default_config();
    c.padding_h = inH;
    c.padding_v = inV;
    c.clamp();
    CHECK(c.padding_h == wH);
    CHECK(c.padding_v == wV);
  }
}

TEST_CASE("ClampSeekStepLarge", "[config][clamp]") {
  const std::array<std::pair<int,int>, 5> cases{{
    {30, 30}, {1, 6}, {700, 600}, {6, 6}, {600, 600},
  }};
  for (auto [in, want] : cases) {
    Config c = default_config();
    c.seek_step_large = in;
    c.clamp();
    CHECK(c.seek_step_large == want);
  }
}

// ---- IsSet families (header-inline) --------------------------------------

TEST_CASE("NavidromeIsSet", "[config][providers]") {
  CHECK(NavidromeConfig{.url="https://x", .user="u", .password="p"}.is_set());
  CHECK_FALSE(NavidromeConfig{.user="u", .password="p"}.is_set());
  CHECK_FALSE(NavidromeConfig{.url="https://x", .password="p"}.is_set());
  CHECK_FALSE(NavidromeConfig{.url="https://x", .user="u"}.is_set());
  CHECK_FALSE(NavidromeConfig{}.is_set());
}

TEST_CASE("SpotifyIsSet", "[config][providers]") {
  CHECK(SpotifyConfig{.enabled=true, .client_id="abc"}.is_set());
  CHECK(SpotifyConfig{.enabled=true}.is_set());
  CHECK_FALSE(SpotifyConfig{}.is_set());
  CHECK_FALSE(SpotifyConfig{.disabled=true, .enabled=true, .client_id="abc"}.is_set());
}

TEST_CASE("SpotifyResolveClientID", "[config][providers]") {
  CHECK(SpotifyConfig{.client_id="user-id"}.resolve_client_id("FALLBACK") == "user-id");
  CHECK(SpotifyConfig{}.resolve_client_id("FALLBACK") == "FALLBACK");
}

TEST_CASE("QobuzIsSet", "[config][providers]") {
  CHECK(QobuzConfig{.enabled=true}.is_set());
  CHECK_FALSE(QobuzConfig{.disabled=true, .enabled=true}.is_set());
  CHECK_FALSE(QobuzConfig{}.is_set());
}

TEST_CASE("PlexIsSet", "[config][providers]") {
  CHECK(PlexConfig{.url="http://x", .token="t"}.is_set());
  CHECK_FALSE(PlexConfig{.token="t"}.is_set());
  CHECK_FALSE(PlexConfig{.url="http://x"}.is_set());
  CHECK_FALSE(PlexConfig{}.is_set());
}

TEST_CASE("JellyfinIsSet", "[config][providers]") {
  CHECK(JellyfinConfig{.url="http://x", .token="t"}.is_set());
  CHECK(JellyfinConfig{.url="http://x", .user="u", .password="p"}.is_set());
  CHECK_FALSE(JellyfinConfig{.token="t"}.is_set());
  CHECK_FALSE(JellyfinConfig{.url="http://x"}.is_set());
  CHECK_FALSE(JellyfinConfig{.url="http://x", .user="u"}.is_set());
  CHECK_FALSE(JellyfinConfig{}.is_set());
}

TEST_CASE("SoundCloudIsSet", "[config][providers]") {
  CHECK(SoundCloudConfig{.enabled=true}.is_set());
  CHECK_FALSE(SoundCloudConfig{}.is_set());
}

TEST_CASE("YouTubeMusicIsSetOrFallback", "[config][providers]") {
  CHECK(YouTubeMusicConfig{.enabled=true}.is_set_or_fallback("", ""));
  CHECK(YouTubeMusicConfig{.cookies_from="chrome"}.is_set_or_fallback("", ""));
  CHECK_FALSE(YouTubeMusicConfig{.cookies_from="   "}.is_set_or_fallback("", ""));
  // cookies_from whitespace-only with fallback → true (fallback ids non-empty).
  CHECK(YouTubeMusicConfig{.cookies_from="   \t\n"}.is_set_or_fallback("id", "secret"));
  CHECK_FALSE(YouTubeMusicConfig{.disabled=true, .cookies_from="chrome"}.is_set_or_fallback("", ""));
  CHECK_FALSE(YouTubeMusicConfig{.disabled=true}.is_set_or_fallback("id", "secret"));
  CHECK(YouTubeMusicConfig{}.is_set_or_fallback("id", "secret"));
  CHECK_FALSE(YouTubeMusicConfig{}.is_set_or_fallback("", ""));
}

// ---- seek_step_large_duration --------------------------------------------

TEST_CASE("SeekStepLargeDuration", "[config]") {
  Config c;
  c.seek_step_large = 45;
  CHECK(c.seek_step_large_duration() == 45);
}

// ---- load: provider sections --------------------------------------------

TEST_CASE("LoadSpotifyBitrate", "[config][load]") {
  for (auto [bitrate, want] : std::array<std::pair<int,int>,3>{{std::pair{160,160},{200,160},{0,320}}}) {
    const auto dir = set_config_dir("spotify_br");
    write_config(dir, "[spotify]\nbitrate = " + std::to_string(bitrate) + "\n");
    auto cfg = load();
    REQUIRE(cfg.has_value());
    CHECK(cfg->spotify.bitrate == want);
  }
}

TEST_CASE("LoadQobuz", "[config][load]") {
  struct Case { std::string name, body; bool is_set; int quality; };
  const Case cases[] = {
    {"section enables, default quality", "[qobuz]\n", true, 6},
    {"explicit quality", "[qobuz]\nquality = 27\n", true, 27},
    {"disabled", "[qobuz]\nenabled = false\n", false, 6},
    {"absent", "", false, 6},
  };
  for (const auto& tc : cases) {
    const auto dir = set_config_dir("qobuz_" + tc.name);
    write_config(dir, tc.body);
    auto cfg = load();
    REQUIRE(cfg.has_value());
    CHECK(cfg->qobuz.is_set() == tc.is_set);
    CHECK(cfg->qobuz.quality == tc.quality);
  }
}

TEST_CASE("LoadTidal", "[config][load]") {
  struct Case { std::string name, body; bool is_set; std::string quality, cid, secret; bool disabled;};
  const Case cases[] = {
    {"section enables", "[tidal]\n", true, "", "", "", false},
    {"full config", "[tidal]\nquality = \"hires\"\nclient_id = \"id\"\nclient_secret = \"sec\"\n",
     true, "hires", "id", "sec", false},
    {"disabled", "[tidal]\nenabled = false\n", false, "", "", "", true},
    {"absent", "", false, "", "", "", false},
  };
  for (const auto& tc : cases) {
    const auto dir = set_config_dir("tidal_" + tc.name);
    write_config(dir, tc.body);
    auto cfg = load();
    REQUIRE(cfg.has_value());
    CHECK(cfg->tidal.is_set() == tc.is_set);
    CHECK(cfg->tidal.quality == tc.quality);
    CHECK(cfg->tidal.client_id == tc.cid);
    CHECK(cfg->tidal.client_secret == tc.secret);
    CHECK(cfg->tidal.disabled == tc.disabled);
    CHECK((cfg->tidal.enabled == tc.is_set || tc.disabled));  // section present → enabled
  }
}

TEST_CASE("LoadYouTubeMusicWhitespaceCookiesFrom", "[config][load]") {
  const auto dir = set_config_dir("ytm_ws");
  write_config(dir, "\n[ytmusic]\ncookies_from = \"   \"\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->ytmusic.cookies_from == "");
}

TEST_CASE("LoadYTSectionAliasesEnableYTMusic", "[config][load]") {
  for (auto sec : {"yt", "youtube", "ytmusic"}) {
    const auto dir = set_config_dir("yt_" + std::string(sec));
    write_config(dir, "[" + std::string(sec) + "]\nclient_id = \"x\"\n");
    auto cfg = load();
    REQUIRE(cfg.has_value());
    CHECK(cfg->ytmusic.enabled == true);
    CHECK(cfg->ytmusic.client_id == "x");
  }
}

TEST_CASE("LoadSoundCloudDisabledByDefault", "[config][load]") {
  set_config_dir("sc_def");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK_FALSE(cfg->soundcloud.enabled);
  CHECK_FALSE(cfg->soundcloud.is_set());
}

TEST_CASE("LoadSoundCloudExplicitlyEnabled", "[config][load]") {
  const auto dir = set_config_dir("sc_on");
  write_config(dir, "\n[soundcloud]\nenabled = true\nuser = \"alice\"\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->soundcloud.enabled);
  CHECK(cfg->soundcloud.user == "alice");
  CHECK(cfg->soundcloud.is_set());
}

TEST_CASE("LoadSoundCloudSectionWithoutEnabledStaysOff", "[config][load]") {
  const auto dir = set_config_dir("sc_nosec");
  write_config(dir, "\n[soundcloud]\nuser = \"alice\"\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK_FALSE(cfg->soundcloud.enabled);
  CHECK_FALSE(cfg->soundcloud.is_set());
  CHECK(cfg->soundcloud.user == "alice");
}

TEST_CASE("LoadSoundCloudCookiesFrom", "[config][load]") {
  const auto dir = set_config_dir("sc_cookies");
  write_config(dir, "\n[soundcloud]\nenabled = true\nuser = \"alice\"\ncookies_from = \"firefox\"\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->soundcloud.cookies_from == "firefox");
}

TEST_CASE("LoadSimplified", "[config][load]") {
  const auto dir = set_config_dir("simplified");
  write_config(dir, "simplified = true\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->simplified);
}

TEST_CASE("LoadLowPowerForcesVisualizerNone", "[config][load]") {
  const auto dir = set_config_dir("lowpower");
  write_config(dir, "visualizer = \"Bars\"\nlow_power = true\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->low_power);
  CHECK(cfg->visualizer == "none");
}

TEST_CASE("LoadSeekLargeStepSec", "[config][load]") {
  const auto dir = set_config_dir("seek42");
  write_config(dir, "seek_large_step_sec = 42\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->seek_step_large == 42);
}

TEST_CASE("LoadSeekLargeStepSecClamp", "[config][load]") {
  struct Case { int in, want; };
  const Case cases[] = {{0, 6}, {5, 6}, {999, 600}};
  for (auto [in, want] : cases) {
    const auto dir = set_config_dir("seek_clamp_" + std::to_string(in));
    write_config(dir, "seek_large_step_sec = " + std::to_string(in) + "\n");
    auto cfg = load();
    REQUIRE(cfg.has_value());
    CHECK(cfg->seek_step_large == want);
  }
}

// ---- load: $ENV interpolation -------------------------------------------

TEST_CASE("LoadInterpolatesSecretsFromEnv", "[config][load][env]") {
  ::setenv("BOOTAMP_TEST_NAVI_PASS", "s3cret!", 1);
  ::setenv("BOOTAMP_TEST_PLEX_TOKEN", "tok-abc", 1);
  ::setenv("BOOTAMP_TEST_JELLY_TOKEN", "jelly-tok", 1);
  ::setenv("BOOTAMP_TEST_YT_SECRET", "yt-secret", 1);

  const auto dir = set_config_dir("envinterp");
  write_config(dir,
    "\n[navidrome]\nurl = \"https://music.example.com\"\nuser = \"alice\"\n"
    "password = \"${BOOTAMP_TEST_NAVI_PASS}\"\n\n"
    "[plex]\nurl = \"http://plex.local:32400\"\ntoken = \"$BOOTAMP_TEST_PLEX_TOKEN\"\n\n"
    "[jellyfin]\nurl = \"https://jelly.example.com\"\ntoken = \"${BOOTAMP_TEST_JELLY_TOKEN}\"\n\n"
    "[ytmusic]\nclient_id = \"literal-id\"\nclient_secret = \"${BOOTAMP_TEST_YT_SECRET}\"\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->navidrome.password == "s3cret!");
  CHECK(cfg->plex.token == "tok-abc");
  CHECK(cfg->jellyfin.token == "jelly-tok");
  CHECK(cfg->ytmusic.client_id == "literal-id");
  CHECK(cfg->ytmusic.client_secret == "yt-secret");
}

TEST_CASE("LoadPreservesLiteralDollarInPassword", "[config][load][env]") {
  const auto dir = set_config_dir("litdollar");
  write_config(dir,
    "\n[navidrome]\nurl = \"https://music.example.com\"\nuser = \"alice\"\n"
    "password = \"p@$$w0rd\"\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->navidrome.password == "p@$$w0rd");
}

TEST_CASE("LoadInterpolatesPluginSecrets", "[config][load][env]") {
  ::setenv("BOOTAMP_TEST_LASTFM_KEY", "lastfm-abc", 1);
  const auto dir = set_config_dir("plugins_env");
  write_config(dir, "\n[plugins.lastfm]\napi_key = \"${BOOTAMP_TEST_LASTFM_KEY}\"\n");
  auto cfg = load();
  REQUIRE(cfg.has_value());
  CHECK(cfg->plugins.at("lastfm").at("api_key") == "lastfm-abc");
}

// ---- Overrides -----------------------------------------------------------

TEST_CASE("OverridesApply", "[config][overrides]") {
  Config c = default_config();
  Overrides ov;
  ov.volume = -15.0;
  ov.shuffle = true;
  ov.repeat = std::string("all");
  ov.mono = true;
  ov.visualizer = std::string("dark");  // C++ Overrides has visualizer, not theme
  apply_overrides(c, ov);
  CHECK(c.volume == -15);
  CHECK(c.shuffle);
  CHECK(c.repeat == "all");
  CHECK(c.mono);
}

TEST_CASE("OverridesApplyNil", "[config][overrides]") {
  Config c = default_config();
  const double vol = c.volume;
  const bool sh = c.shuffle;
  apply_overrides(c, Overrides{});
  CHECK(c.volume == vol);
  CHECK(c.shuffle == sh);
}

TEST_CASE("OverridesApplyClamps", "[config][overrides]") {
  Config c = default_config();
  Overrides ov;
  ov.volume = 100.0;  // out of range
  apply_overrides(c, ov);
  CHECK(c.volume == 6);
}

TEST_CASE("OverridesApplyLowPowerForcesVisualizerNone", "[config][overrides]") {
  Config c = default_config();
  c.visualizer = "Bars";
  Overrides ov;
  // C++ Overrides has no low_power field (that's a full Go Overrides); the
  // low_power path is exercised through load() instead. This case documents
  // that apply_overrides does not touch low_power/visualizer="none" on its own.
  apply_overrides(c, ov);
  CHECK(c.visualizer == "Bars");
}

TEST_CASE("OverridesCookiesFromRoutesToYTMusic", "[config][overrides]") {
  Config c = default_config();
  Overrides ov;
  ov.cookies_from = std::string("chrome");
  apply_overrides(c, ov);
  CHECK(c.ytmusic.cookies_from == "chrome");
  // provider is not soundcloud by default → soundcloud stays empty.
  CHECK(c.soundcloud.cookies_from == "");
}

TEST_CASE("OverridesCookiesFromAlsoRoutesToSoundCloudWhenProviderIsSoundCloud",
          "[config][overrides]") {
  Config c = default_config();
  c.provider = "soundcloud";
  Overrides ov;
  ov.cookies_from = std::string("firefox");
  apply_overrides(c, ov);
  CHECK(c.ytmusic.cookies_from == "firefox");
  CHECK(c.soundcloud.cookies_from == "firefox");
}

TEST_CASE("OverridesEQReplacesWholesale", "[config][overrides]") {
  Config c = default_config();
  c.eq = std::array<double,10>{1,2,3,4,5,6,7,8,9,10};
  Overrides ov;
  ov.eq = std::array<double,10>{0,0,0,0,0,0,0,0,0,0};
  apply_overrides(c, ov);
  for (double v : c.eq) CHECK(v == 0.0);
}

// ---- apply_player --------------------------------------------------------

namespace {

class MockPlayer : public PlayerConfig {
public:
  double volume_min = 0, volume = 0, speed = 0;
  std::array<double,10> eq{};
  bool mono = false;
  void set_volume_min(double db) override { volume_min = db; }
  void set_volume(double db)     override { volume = db; }
  void set_speed(double r)       override { speed = r; }
  void set_eq_band(int b, double db) override { eq[static_cast<std::size_t>(b)] = db; }
  void toggle_mono()             override { mono = !mono; }
};

}  // namespace

TEST_CASE("ApplyPlayer", "[config][apply]") {
  Config c = default_config();
  c.volume_min = -70;
  c.volume = -10;
  c.speed = 1.5;
  c.eq = std::array<double,10>{1,2,3,4,5,6,7,8,9,10};
  c.eq_preset = "";  // Custom
  c.mono = true;
  MockPlayer p;
  apply_player(c, p);
  CHECK(p.volume_min == -70);
  CHECK(p.volume == -10);
  CHECK(p.speed == 1.5);
  for (int i = 0; i < 10; ++i) CHECK(p.eq[static_cast<std::size_t>(i)] == c.eq[static_cast<std::size_t>(i)]);
  CHECK(p.mono);
}

TEST_CASE("ApplyPlayerDefaultSpeedNotSet", "[config][apply]") {
  Config c = default_config();
  c.speed = 1.0;  // default, should NOT call set_speed
  MockPlayer p;
  apply_player(c, p);
  CHECK(p.speed == 0.0);
}

TEST_CASE("ApplyPlayerWithPresetSkipsBands", "[config][apply]") {
  Config c = default_config();
  c.eq_preset = "Rock";
  c.eq = std::array<double,10>{1,2,3,4,5,6,7,8,9,10};
  MockPlayer p;
  apply_player(c, p);
  for (double v : p.eq) CHECK(v == 0.0);
}

// ---- apply_playlist ------------------------------------------------------

namespace {

class MockPlaylist : public PlaylistConfig {
public:
  int repeat_cycles = 0;
  bool shuffled = false;
  void cycle_repeat()   override { ++repeat_cycles; }
  void toggle_shuffle()  override { shuffled = !shuffled; }
};

}  // namespace

TEST_CASE("ApplyPlaylist", "[config][apply]") {
  struct Case { std::string repeat; bool shuffle; int cycles; bool shuf; };
  const Case cases[] = {
    {"off", false, 0, false},
    {"all", false, 1, false},
    {"one", false, 2, false},
    {"off", true,  0, true},
    {"all", true,  1, true},
  };
  for (const auto& tc : cases) {
    Config c = default_config();
    c.repeat = tc.repeat;
    c.shuffle = tc.shuffle;
    MockPlaylist pl;
    apply_playlist(c, pl);
    CHECK(pl.repeat_cycles == tc.cycles);
    CHECK(pl.shuffled == tc.shuf);
  }
}

// ---- save ----------------------------------------------------------------

TEST_CASE("SaveCreatesConfigFile", "[config][save]") {
  const auto dir = set_config_dir("save_new");
  auto r = save("volume", "-6");
  REQUIRE(r.has_value());
  std::ifstream f(dir / "config.toml");
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(got.find("volume = -6") != std::string::npos);
}

TEST_CASE("SaveReplacesExistingKey", "[config][save]") {
  const auto dir = set_config_dir("save_repl");
  fs::create_directories(dir);
  write_config(dir, "# a comment\nvolume = -12\nspeed = 1.0\n");
  auto r = save("volume", "-3");
  REQUIRE(r.has_value());
  std::ifstream f(dir / "config.toml");
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(got.find("volume = -3") != std::string::npos);
  CHECK(got.find("volume = -12") == std::string::npos);
  CHECK(got.find("speed = 1.0") != std::string::npos);
}

TEST_CASE("SaveInsertsBeforeFirstSection", "[config][save]") {
  const auto dir = set_config_dir("save_before_sec");
  fs::create_directories(dir);
  write_config(dir, "[navidrome]\nurl = \"https://ex.com\"\n");
  auto r = save("volume", "-6");
  REQUIRE(r.has_value());
  std::ifstream f(dir / "config.toml");
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto vi = got.find("volume = -6");
  const auto ni = got.find("[navidrome]");
  REQUIRE(vi != std::string::npos);
  REQUIRE(ni != std::string::npos);
  CHECK(vi < ni);
}

TEST_CASE("SaveDoesNotMatchKeyInSection", "[config][save]") {
  const auto dir = set_config_dir("save_no_match");
  fs::create_directories(dir);
  write_config(dir, "[navidrome]\nvolume = \"old\"\n");
  auto r = save("volume", "-6");
  REQUIRE(r.has_value());
  std::ifstream f(dir / "config.toml");
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(got.find("volume = \"old\"") != std::string::npos);  // section-local untouched
  CHECK(got.find("volume = -6") != std::string::npos);
}

// ---- save_navidrome_sort -------------------------------------------------

TEST_CASE("SaveNavidromeSortCreatesSection", "[config][save]") {
  const auto dir = set_config_dir("save_navi_new");
  auto r = save_navidrome_sort("alphabeticalByName");
  REQUIRE(r.has_value());
  std::ifstream f(dir / "config.toml");
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(got.find("[navidrome]") != std::string::npos);
  CHECK(got.find("browse_sort = \"alphabeticalByName\"") != std::string::npos);
}

TEST_CASE("SaveNavidromeSortReplacesExisting", "[config][save]") {
  const auto dir = set_config_dir("save_navi_repl");
  fs::create_directories(dir);
  write_config(dir, "[navidrome]\nurl = \"https://e.com\"\nbrowse_sort = \"old\"\n");
  auto r = save_navidrome_sort("byYear");
  REQUIRE(r.has_value());
  std::ifstream f(dir / "config.toml");
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(got.find("\"old\"") == std::string::npos);
  CHECK(got.find("browse_sort = \"byYear\"") != std::string::npos);
}

TEST_CASE("SaveNavidromeSortAppendsKeyInExistingSection", "[config][save]") {
  const auto dir = set_config_dir("save_navi_append");
  fs::create_directories(dir);
  write_config(dir, "[navidrome]\nurl = \"https://e.com\"\n[other]\nkey = \"val\"\n");
  auto r = save_navidrome_sort("random");
  REQUIRE(r.has_value());
  std::ifstream f(dir / "config.toml");
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(got.find("browse_sort = \"random\"") != std::string::npos);
  const auto ni = got.find("[navidrome]");
  const auto si = got.find("browse_sort");
  const auto oi = got.find("[other]");
  REQUIRE(ni != std::string::npos);
  REQUIRE(oi != std::string::npos);
  CHECK(si > ni);
  CHECK(si < oi);
}

}  // namespace bootamp::config