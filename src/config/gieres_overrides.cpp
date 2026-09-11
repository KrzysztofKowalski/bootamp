// config/gieres_overrides.cpp — gieres.ini parsing + endpoint resolution.
#include "config/gieres_overrides.hpp"

#include "foundation/appdir.hpp"
#include "foundation/applog.hpp"

#include <fstream>
#include <iterator>
#include <string>

namespace bootamp::config {

namespace {

// trim drops ASCII whitespace from both ends (ini values are user-written).
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

// unquote strips one optional pair of surrounding quotes (values like
// base_url = "https://gieres.cytr.us" read the same as bare ones).
std::string unquote(std::string_view s) {
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
      s.back() == s.front()) {
    return std::string{s.substr(1, s.size() - 2)};
  }
  return std::string{s};
}

}  // namespace

std::string parse_gieres_ini_base_url(const std::string_view text) {
  std::size_t pos = 0;
  const std::size_t n = text.size();
  while (pos < n) {
    const std::size_t eol = text.find('\n', pos);
    const std::string_view line =
        trim(eol == std::string_view::npos
                 ? text.substr(pos)
                 : text.substr(pos, eol - pos));
    pos = eol == std::string_view::npos ? n : eol + 1;
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;  // stray word — ignore
    }
    if (trim(line.substr(0, eq)) != "base_url") {
      continue;  // unknown key — ignored by design
    }
    const std::string value = unquote(trim(line.substr(eq + 1)));
    if (!value.empty()) {
      return value;  // first usable value wins
    }
  }
  return "";
}

std::string gieres_ini_base_url() {
  const auto dir = foundation::config_dir();
  if (!dir) {
    return "";
  }
  std::ifstream in(*dir / "gieres.ini");
  if (!in) {
    return "";  // missing file: the config value stands
  }
  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  return parse_gieres_ini_base_url(text);
}

std::string gieres_base_url_for(std::string_view config_value) {
  const std::string ini = gieres_ini_base_url();
  if (ini.empty()) {
    return std::string{config_value};
  }
  if (ini != config_value) {
    // Startup-only path (main() builds the model once) — one ring entry.
    foundation::applog::status("gieres endpoint override (gieres.ini): {}", ini);
  }
  return ini;
}

}  // namespace bootamp::config