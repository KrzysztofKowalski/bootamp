// config/flags.cpp — CLI flag overrides applied to a loaded Config.
//
// Port of cliamp/config/flags.go's Overrides.Apply, adapted to bootamp's
// reduced Overrides struct (config.hpp): the MVP CLI exposes only
//   --vol --speed --eq --vis --shuffle --repeat --cookies-from --mono
// (full CLI11 wiring lives in app/cli.cpp at M5). cookies_from routes to
// ytmusic.cookies_from, and additionally to soundcloud.cookies_from when the
// active provider is "soundcloud" — matching cliamp's flag handling.
#include "config/config.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace bootamp::config {

namespace {
std::string trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return std::string(s);
}
}  // namespace

void apply_overrides(Config& cfg, const Overrides& ov) {
  if (ov.volume)     cfg.volume     = *ov.volume;
  if (ov.speed)      cfg.speed      = *ov.speed;
  if (ov.eq)         cfg.eq         = *ov.eq;  // wholesale replace
  if (ov.visualizer) cfg.visualizer = *ov.visualizer;
  if (ov.shuffle)    cfg.shuffle    = *ov.shuffle;
  if (ov.repeat)     cfg.repeat     = *ov.repeat;
  if (ov.mono)       cfg.mono       = *ov.mono;

  if (ov.cookies_from) {
    // Trim once, mirroring the load path's strings.TrimSpace(parseString(val)).
    const std::string browser = trim(*ov.cookies_from);
    cfg.ytmusic.cookies_from = browser;
    if (cfg.provider == "soundcloud") cfg.soundcloud.cookies_from = browser;
  }

  cfg.clamp();
}

}  // namespace bootamp::config