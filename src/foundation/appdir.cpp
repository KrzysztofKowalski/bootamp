// foundation/appdir.cpp — bootamp config/data directory resolution.
//
// Faithful port of cliamp/internal/appdir/appdir.go. Resolution order is
// preserved 1:1; only the env-var name and the application subdirectory are
// renamed cliamp -> bootamp (project-wide convention).
#include "foundation/appdir.hpp"

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace bootamp::foundation {

namespace fs = std::filesystem;

namespace {

// env_or_empty returns the value of `name` if set and non-empty, else "".
std::string env_or_empty(const char* name) {
  if (const char* v = std::getenv(name)) {
    if (v[0] != '\0') return v;
  }
  return {};
}

// user_home_dir mirrors Go's os.UserHomeDir() on Linux: $HOME first, then the
// password database for the current uid. Returns empty on failure.
std::string user_home_dir() {
  if (std::string h = env_or_empty("HOME"); !h.empty()) return h;
  const uid_t uid = getuid();
  if (auto* pw = getpwuid(uid); pw != nullptr && pw->pw_dir != nullptr) {
    return pw->pw_dir;
  }
  return {};
}

}  // namespace

std::expected<fs::path, std::string> config_dir() {
  if (std::string d = env_or_empty("BOOTAMP_CONFIG_DIR"); !d.empty()) {
    return fs::path{d};
  }
  if (std::string xdg = env_or_empty("XDG_CONFIG_HOME"); !xdg.empty()) {
    return fs::path{xdg} / "bootamp";
  }
  if (std::string home = env_or_empty("HOME"); !home.empty()) {
    return fs::path{home} / ".config" / "bootamp";
  }
  std::string home = user_home_dir();
  if (home.empty()) {
    return std::unexpected{"could not determine home directory"};
  }
  return fs::path{home} / ".config" / "bootamp";
}

std::expected<fs::path, std::string> data_dir() {
  // Honor HOME first, matching cliamp's DataDir (keeps Dir()/DataDir()
  // consistent when HOME is set but the password DB disagrees).
  std::string home = env_or_empty("HOME");
  if (!home.empty()) {
    return fs::path{home} / ".local" / "share" / "bootamp";
  }
  home = user_home_dir();
  if (home.empty()) {
    return std::unexpected{"could not determine home directory"};
  }
  return fs::path{home} / ".local" / "share" / "bootamp";
}

std::expected<fs::path, std::string> plugin_dir() {
  auto dir = config_dir();
  if (!dir) return std::unexpected{dir.error()};
  return *dir / "plugins";
}

std::expected<fs::path, std::string> ensure_dir(const fs::path& dir) {
  std::error_code ec;
  // create_directories is a no-op if the directory already exists.
  fs::create_directories(dir, ec);
  if (ec) {
    return std::unexpected{"create directory: " + ec.message()};
  }
  // Match cliamp's 0o700 on the config directory itself.
  fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
  if (ec) {
    return std::unexpected{"secure directory: " + ec.message()};
  }
  return dir;
}

}  // namespace bootamp::foundation