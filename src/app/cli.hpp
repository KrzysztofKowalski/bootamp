// app/cli.hpp — the bootamp command-line interface (contract for app/cli.cpp).
//
// Port of cliamp's urfave/cli/v3 root flag set (cliamp/main.go commands.go
// rootFlags + config/flags.go overridesFromFlags), reduced to the MVP set the
// plan locks in: --vol/--speed/--eq/--vis/--shuffle/--repeat/--cookies-from
// (plus --mono) with <paths|urls> positional arguments. Flag semantics match
// the Go side; parse_cli is implemented twice — CLI11 when the header is
// present, a getopt_long fallback otherwise — both funneling into one
// validation/finalize step so behavior is identical.
#pragma once

#include "config/config.hpp"

#include <expected>
#include <string>
#include <vector>

namespace bootamp::app {

// Version of the bootamp binary (matches the CMake project version 0.1.0).
inline constexpr std::string_view kBootampVersion = "0.1.0";

// CliResult is the parsed command line: the config Overrides derived from the
// flags plus the positional arguments (paths and/or URLs).
struct CliResult {
  config::Overrides          overrides;
  std::vector<std::string>   positional;
  bool                       show_help    = false;
  bool                       show_version = false;
};

// parse_cli parses the argument vector (excluding argv[0]) into a CliResult.
// Errors carry the Go-style message prefix (e.g. the exact --repeat text).
std::expected<CliResult, std::string> parse_cli(const std::vector<std::string>& args);

// cli_usage returns the full help text (same content for both parser
// backends; wording ports urfave/cli/v3's flag help).
std::string cli_usage();

// cli_version returns "bootamp <version>".
std::string cli_version();

}  // namespace bootamp::app
