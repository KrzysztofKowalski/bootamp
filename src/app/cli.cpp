// app/cli.cpp — the bootamp CLI (contract: app/cli.hpp).
//
// Port of cliamp's urfave/cli/v3 root flags (cliamp/main.go commands.go
// rootFlags, ~lines 28-227) plus the flag-override semantics of
// cliamp/config/flags.go (overridesFromFlags). The flag surface is the MVP set
// the plan locks in; the positional arguments are the same <paths|urls> list
// cliamp feeds resolve.Args.
//
// Parser selection: CLI11 when <CLI/CLI.hpp> is available (the CMake build
// gates the executable on CLI11_FOUND), otherwise a getopt_long fallback with
// the identical option surface. Both backends funnel into one `finalize` step
// so validation and error text are shared: --eq must be exactly 10
// comma-separated doubles; --repeat must be off/all/one with the verbatim Go
// error message. Numeric flags are parsed but NOT range-checked here — like
// Go's overridesFromFlags they pass through and config::clamp() enforces the
// ranges afterwards.
#include "app/cli.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if __has_include(<CLI/CLI.hpp>)
#include <CLI/CLI.hpp>
#endif

namespace bootamp::app {

namespace {

// Error text for a malformed --eq (no Go equivalent — the flag does not exist
// on the Go side; the plan defines the exact-10 contract).
inline constexpr std::string_view kEqError =
    "invalid --eq value: must be exactly 10 comma-separated gain values in dB";

// parse_double parses a flag value with std::from_chars (no exceptions, no
// locale). Rejects trailing junk ("1.5x") and empty values.
std::expected<double, std::string> parse_double(std::string_view s,
                                                std::string_view flag) {
  double value = 0.0;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec == std::errc() && ptr == last) {
    return value;
  }
  return std::unexpected("invalid value for " + std::string(flag) + ": \"" +
                         std::string(s) + "\"");
}

// parse_eq splits `s` on commas and requires exactly 10 numeric values.
// Whitespace around each value is tolerated ("3, 2, 0" parses fine).
std::expected<std::array<double, 10>, std::string>
parse_eq(std::string_view s) {
  std::array<double, 10> out{};
  std::size_t begin = 0;
  for (std::size_t k = 0; k < 10; ++k) {
    if (begin > s.size()) {
      return std::unexpected(std::string(kEqError));
    }
    const std::size_t comma = s.find(',', begin);
    const std::size_t end =
        (comma == std::string_view::npos) ? s.size() : comma;
    const std::string_view token = s.substr(begin, end - begin);
    std::size_t b = 0;
    std::size_t e = token.size();
    while (b < e && std::isspace(static_cast<unsigned char>(token[b]))) {
      ++b;
    }
    while (e > b &&
           std::isspace(static_cast<unsigned char>(token[e - 1]))) {
      --e;
    }
    if (b == e) {
      return std::unexpected(std::string(kEqError));
    }
    auto v = parse_double(token.substr(b, e - b), "--eq");
    if (!v) {
      return std::unexpected(v.error());
    }
    out[k] = *v;
    begin = end + 1;  // skip the comma (past-the-end when none was found)
  }
  // All 10 values consumed; anything after the 10th comma is a trailing extra.
  if (begin <= s.size()) {
    return std::unexpected(std::string(kEqError));
  }
  return out;
}

// finalize validates the parsed values and builds the CliResult. Shared by
// both parser backends so their behavior is identical.
std::expected<CliResult, std::string>
finalize(std::optional<double> vol, std::optional<double> speed,
         std::optional<std::string> eq, std::optional<std::string> vis,
         std::optional<bool> shuffle, std::optional<std::string> repeat,
         std::optional<std::string> cookies, std::optional<bool> mono,
         std::vector<std::string> positional, bool help, bool version) {
  if (help || version) {
    // Help/version win over everything, like urfave/cli's dispatch.
    return CliResult{{}, std::move(positional), help, version};
  }
  if (repeat) {
    const std::string& r = *repeat;
    if (r != "off" && r != "all" && r != "one") {
      // Verbatim cliamp config/flags.go error text.
      return std::unexpected("--repeat must be off, all, or one (got \"" + r +
                             "\")");
    }
  }
  config::Overrides ov;
  ov.volume       = std::move(vol);
  ov.speed        = std::move(speed);
  ov.shuffle      = std::move(shuffle);
  ov.repeat       = std::move(repeat);
  ov.cookies_from = std::move(cookies);
  ov.mono         = std::move(mono);
  if (eq) {
    auto bands = parse_eq(*eq);
    if (!bands) {
      return std::unexpected(bands.error());
    }
    ov.eq = *bands;
  }
  ov.visualizer = std::move(vis);
  return CliResult{std::move(ov), std::move(positional), false, false};
}

#if __has_include(<CLI/CLI.hpp>)

// --- CLI11 backend (the CMake build compiles this branch) --------------------

std::expected<CliResult, std::string>
parse_with_cli11(const std::vector<std::string>& args) {
  CLI::App cli("bootamp — retro terminal music player");
  cli.allow_extras();  // <paths|urls> positionals

  double vol = 0.0;
  CLI::Option* vol_opt =
      cli.add_option("--vol", vol, "startup volume in dB");
  double speed = 1.0;
  CLI::Option* speed_opt =
      cli.add_option("--speed", speed, "playback speed ratio [0.25, 2.0]");
  std::string eq;
  CLI::Option* eq_opt = cli.add_option(
      "--eq", eq, "ten EQ band gains in dB, comma-separated");
  std::string vis;
  CLI::Option* vis_opt = cli.add_option(
      "--vis,--visualizer", vis, "visualizer mode name (see v key cycle)");
  std::string repeat;
  CLI::Option* repeat_opt =
      cli.add_option("--repeat", repeat, "repeat mode: off, all, one");
  std::string cookies;
  CLI::Option* cookies_opt = cli.add_option(
      "--cookies-from", cookies, "yt-dlp --cookies-from-browser value");
  bool shuffle = false;
  CLI::Option* shuffle_opt =
      cli.add_flag("--shuffle", shuffle, "shuffle playback order");
  bool mono = false;
  CLI::Option* mono_opt = cli.add_flag("--mono", mono, "force mono output");
  bool version = false;
  cli.add_flag("-v,--version", version, "print version and exit");

  // CLI11 takes an argv-style list; rebuild one from the caller's args.
  std::vector<const char*> argv;
  argv.reserve(args.size() + 2);
  argv.push_back("bootamp");
  for (const std::string& a : args) {
    argv.push_back(a.c_str());
  }
  argv.push_back(nullptr);

  try {
    cli.parse(static_cast<int>(argv.size()) - 1, argv.data());
  } catch (const CLI::CallForHelp&) {
    return CliResult{{}, {}, true, false};
  } catch (const CLI::ParseError& e) {
    return std::unexpected(e.what());
  }

  return finalize(
      vol_opt->count() ? std::optional<double>(vol) : std::nullopt,
      speed_opt->count() ? std::optional<double>(speed) : std::nullopt,
      eq_opt->count() ? std::optional<std::string>(eq) : std::nullopt,
      vis_opt->count() ? std::optional<std::string>(vis) : std::nullopt,
      shuffle_opt->count() ? std::optional<bool>(shuffle) : std::nullopt,
      repeat_opt->count() ? std::optional<std::string>(repeat) : std::nullopt,
      cookies_opt->count() ? std::optional<std::string>(cookies)
                           : std::nullopt,
      mono_opt->count() ? std::optional<bool>(mono) : std::nullopt,
      cli.remaining(), false, false);
}

#else  // !__has_include(<CLI/CLI.hpp>)

// --- getopt_long fallback (same option surface as CLI11) ---------------------

#include <getopt.h>

enum : int {
  kOptVol     = 1000,
  kOptSpeed   = 1001,
  kOptEq      = 1002,
  kOptVis     = 1003,
  kOptShuffle = 1004,
  kOptRepeat  = 1005,
  kOptCookies = 1006,
  kOptMono    = 1007,
  kOptVersion = 1008,
};

const struct option kLongOpts[] = {
    {"vol",          required_argument, nullptr, kOptVol},
    {"speed",        required_argument, nullptr, kOptSpeed},
    {"eq",           required_argument, nullptr, kOptEq},
    {"vis",          required_argument, nullptr, kOptVis},
    {"visualizer",   required_argument, nullptr, kOptVis},
    {"shuffle",      no_argument,       nullptr, kOptShuffle},
    {"repeat",       required_argument, nullptr, kOptRepeat},
    {"cookies-from", required_argument, nullptr, kOptCookies},
    {"mono",         no_argument,       nullptr, kOptMono},
    {"version",      no_argument,       nullptr, kOptVersion},
    {"help",         no_argument,       nullptr, 'h'},
    {nullptr, 0, nullptr, 0},
};

std::expected<CliResult, std::string>
parse_with_getopt(const std::vector<std::string>& args) {
  // getopt_long mutates argv; feed it a private copy.
  std::vector<std::string> owned = args;
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  argv.push_back(const_cast<char*>("bootamp"));
  for (std::string& a : owned) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);

  std::optional<double> vol, speed;
  std::optional<std::string> eq, vis, repeat, cookies;
  std::optional<bool> shuffle, mono;
  bool help = false;
  bool version = false;

  optind = 1;   // reset: parse_cli may be called more than once (tests)
  opterr = 0;   // we print our own error text
  int c = 0;
  while ((c = getopt_long(static_cast<int>(argv.size()) - 1, argv.data(),
                          "hv", kLongOpts, nullptr)) != -1) {
    switch (c) {
      case kOptVol: {
        auto v = parse_double(optarg, "--vol");
        if (!v) {
          return std::unexpected(v.error());
        }
        vol = *v;
        break;
      }
      case kOptSpeed: {
        auto v = parse_double(optarg, "--speed");
        if (!v) {
          return std::unexpected(v.error());
        }
        speed = *v;
        break;
      }
      case kOptEq: eq = optarg; break;
      case kOptVis: vis = optarg; break;
      case kOptShuffle: shuffle = true; break;
      case kOptRepeat: repeat = optarg; break;
      case kOptCookies: cookies = optarg; break;
      case kOptMono: mono = true; break;
      case kOptVersion: version = true; break;
      case 'h': help = true; break;
      case '?':
      default:
        return std::unexpected(
            "unknown or incomplete option (use --help for usage)");
    }
  }
  std::vector<std::string> positional;
  for (int i = optind; i < static_cast<int>(argv.size()) - 1; ++i) {
    positional.emplace_back(argv[i]);
  }
  return finalize(std::move(vol), std::move(speed), std::move(eq),
                  std::move(vis), std::move(shuffle), std::move(repeat),
                  std::move(cookies), std::move(mono), std::move(positional),
                  help, version);
}

#endif  // __has_include(<CLI/CLI.hpp>)

}  // namespace

std::expected<CliResult, std::string>
parse_cli(const std::vector<std::string>& args) {
#if __has_include(<CLI/CLI.hpp>)
  return parse_with_cli11(args);
#else
  return parse_with_getopt(args);
#endif
}

std::string cli_usage() {
  return
      "Usage: bootamp [flags] <paths|urls>...\n"
      "\n"
      "A retro terminal music player (C++ port of cliamp). Plays local files,\n"
      "HTTP streams, M3U/PLS playlists, and YouTube/yt-dlp URLs. With no\n"
      "arguments the default radio station list is queued.\n"
      "\n"
      "Flags:\n"
      "  --vol <dB>               startup volume in dB\n"
      "  --speed <ratio>          playback speed ratio [0.25, 2.0]\n"
      "  --eq <g1,...,g10>        ten EQ band gains in dB, comma-separated\n"
      "  --vis, --visualizer <m>  visualizer mode name\n"
      "  --shuffle                shuffle playback order\n"
      "  --repeat <off|all|one>   repeat mode\n"
      "  --cookies-from <browser> yt-dlp --cookies-from-browser value\n"
      "  --mono                   force mono output\n"
      "  -h, --help               show this help and exit\n"
      "  -v, --version            print version and exit\n"
      "\n"
      "Examples:\n"
      "  bootamp play ~/Music/test.mp3 --vol -6 --speed 1.25\n"
      "  bootamp --eq 3,2,0,-1,-2,0,1,2,3,2 --shuffle ~/Music/\n"
      "  bootamp https://radio.cliamp.stream/streams.m3u\n";
}

std::string cli_version() {
  return "bootamp " + std::string(kBootampVersion);
}

}  // namespace bootamp::app
