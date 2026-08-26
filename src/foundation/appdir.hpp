// foundation/appdir.hpp — bootamp config/data directory resolution.
//
// Port of cliamp/internal/appdir/appdir.go. Resolution order:
//   1. BOOTAMP_CONFIG_DIR env (explicit override)
//   2. XDG_CONFIG_HOME/bootamp
//   3. HOME/.config/bootamp
//   4. fallback: os.UserHomeDir()/.config/bootamp
// Data dir mirrors ~/.local/share/bootamp (cliamp's DataDir).
#pragma once

#include <filesystem>
#include <expected>
#include <string>

namespace bootamp::foundation {

namespace fs = std::filesystem;

// dir() returns the bootamp configuration directory (~/.config/bootamp).
// The optional env var is BOOTAMP_CONFIG_DIR (cliamp used CLIAMP_CONFIG_DIR).
std::expected<fs::path, std::string> config_dir();

// data_dir() returns the bootamp data directory (~/.local/share/bootamp),
// used for non-user-editable state: resume, plugin stores, downloaded assets.
std::expected<fs::path, std::string> data_dir();

// plugin_dir() returns <config_dir()>/plugins.
std::expected<fs::path, std::string> plugin_dir();

// Convenience: ensure a directory exists (created with 0700 perms). Returns the
// path on success, an error string on failure. The 0700 matches Go's 0o700.
std::expected<fs::path, std::string> ensure_dir(const fs::path& dir);

}  // namespace bootamp::foundation