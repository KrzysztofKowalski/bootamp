// resolve/ytdl.cpp — resolve yt-dlp URLs via `yt-dlp --flat-playlist -j`.
//
// Port of cliamp resolve/resolve.go:525-754 (ytdlFlatEntry / resolveYTDL /
// resolveYTDLRange / resolveYTDLRangePageContext / parseYTDLTracks /
// humanizeBasename) and of the IsYTDL classifier from
// cliamp/playlist/playlist.go:188-225.
//
// The kkdai/youtube native client is dropped entirely; every YT family URL is
// resolved through yt-dlp. yt-dlp is spawned via posix_spawn + pipe() with the
// exact Go argument list:
//
//   yt-dlp --flat-playlist -j --socket-timeout 15 [--cookies-from-browser B]
//          [--playlist-start S+1 --playlist-end E] <url>
//
// bounded by a 30s timeout (context.WithTimeout in Go). stdout is parsed as
// newline-delimited JSON into Track{path,title,artist,stream=true,
// duration_secs} with nlohmann::json.

#include "resolve/ytdl.hpp"
#include "resolve/resolve.hpp"  // ytdl_cookies_from()
#include "resolve/ytdl_internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bootamp::resolve::detail {
namespace {

// ---------------------------------------------------------------- helpers --

std::string lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// strings.TrimSpace for ASCII whitespace (Go trims Unicode; yt-dlp stderr and
// browser names are ASCII in practice).
std::string trim_space(std::string_view s) {
  size_t b = 0;
  size_t e = s.size();
  auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
  };
  while (b < e && is_space(s[b])) ++b;
  while (e > b && is_space(s[e - 1])) --e;
  return std::string(s.substr(b, e - b));
}

// filepath.Ext: suffix beginning at the final dot of the final path element.
std::string_view path_ext(std::string_view s) {
  std::string_view base = s;
  const size_t slash = s.find_last_of('/');
  if (slash != std::string_view::npos) base = s.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  if (dot == std::string_view::npos) return {};
  return base.substr(dot);
}

// Audio extensions bootamp can decode — port of cliamp player/decode.go
// SupportedExts (kept local so this TU does not depend on audio/decode.cpp).
const std::set<std::string>& supported_exts() {
  static const std::set<std::string> kExts = {
      ".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac",
      ".aacp", ".m4b", ".alac", ".wma", ".opus", ".webm",
  };
  return kExts;
}

// JSON field read with Go encoding/json semantics: a missing or null field
// yields the zero value ("" / 0.0); any other type mismatch throws, which
// makes the whole entry skip — exactly like Go's all-or-nothing Unmarshal.
std::string jstr(const nlohmann::json& j, std::string_view key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return {};
  return it->get<std::string>();
}

double jdbl(const nlohmann::json& j, std::string_view key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return 0.0;
  return it->get<double>();
}

}  // namespace

// ------------------------------------------------------- parse_ytdl_tracks --

std::expected<ytdl_parse_result, std::string> parse_ytdl_tracks(std::string_view data) {
  // Go bufio.Scanner limits (resolve/m3u.go): 64KB initial buffer, 1MB max
  // line. A longer line is a hard error ("bufio.Scanner: token too long").
  constexpr size_t kMaxLine = 1024 * 1024;

  ytdl_parse_result result;
  size_t pos = 0;
  while (pos <= data.size()) {
    const size_t nl = data.find('\n', pos);
    std::string_view line = data.substr(pos, nl == std::string_view::npos
                                                ? std::string_view::npos
                                                : nl - pos);
    pos = nl == std::string_view::npos ? data.size() + 1 : nl + 1;
    if (line.size() > kMaxLine) {
      return std::unexpected(std::string("bufio.Scanner: token too long"));
    }
    line = trim_space(line);
    if (line.empty()) continue;  // Go: blank lines skipped before counting
    result.entries++;
    try {
      const nlohmann::json j = nlohmann::json::parse(line);
      std::string track_url = jstr(j, "webpage_url");
      if (track_url.empty()) track_url = jstr(j, "url");
      if (track_url.empty()) continue;
      std::string title = jstr(j, "title");
      if (title.empty()) title = humanize_basename(jstr(j, "webpage_url_basename"));
      if (title.empty()) title = track_url;
      std::string artist = jstr(j, "uploader");
      if (artist.empty()) artist = jstr(j, "playlist_uploader");

      playlist::Track t;
      t.path = std::move(track_url);
      t.title = std::move(title);
      t.artist = std::move(artist);
      t.stream = true;
      t.duration_secs = static_cast<int>(jdbl(j, "duration"));
      result.tracks.push_back(std::move(t));
    } catch (const nlohmann::json::exception&) {
      continue;  // Go: json.Unmarshal error → entry skipped, count kept
    }
  }
  return result;
}

// -------------------------------------------------------- humanize_basename --

std::string humanize_basename(std::string_view s) {
  const std::string_view ext = path_ext(s);
  if (!ext.empty() && supported_exts().count(lower(ext))) {
    s.remove_suffix(ext.size());
  }
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(c == '-' ? ' ' : c);
  return out;
}

// -------------------------------------------------------- URL host parsing --
// Minimal equivalent of Go net/url.Parse().Hostname(): for http(s) URLs
// returns the lowercased host, port stripped, IPv6 brackets removed.

std::string url_hostname(std::string_view url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) return {};
  std::string_view auth = url.substr(scheme + 3);
  const size_t end = auth.find_first_of("/?#");
  if (end != std::string_view::npos) auth = auth.substr(0, end);
  const size_t at = auth.find_last_of('@');
  if (at != std::string_view::npos) auth = auth.substr(at + 1);  // userinfo
  if (!auth.empty() && auth.front() == '[') {                    // IPv6
    const size_t close = auth.find(']');
    if (close == std::string_view::npos) return {};
    return lower(auth.substr(1, close - 1));
  }
  const size_t colon = auth.find_last_of(':');
  if (colon != std::string_view::npos) auth = auth.substr(0, colon);  // port
  return lower(auth);
}

// strings.HasPrefix-style single strip (Go trims "www." then "m.", once).
std::string_view strip_prefix(std::string_view s, std::string_view prefix) {
  if (s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix) {
    return s.substr(prefix.size());
  }
  return s;
}

// cliamp playlist.go matchSearchPrefix: name followed by digits then ':'.
bool match_search_prefix(std::string_view path, std::string_view name) {
  if (path.size() < name.size() || path.substr(0, name.size()) != name) return false;
  const std::string_view rest = path.substr(name.size());
  const size_t colon = rest.find(':');
  if (colon == std::string_view::npos) return false;
  for (size_t i = 0; i < colon; ++i) {
    if (rest[i] < '0' || rest[i] > '9') return false;
  }
  return true;
}

// cliamp playlist.go IsYTSearch (ytsearch:, ytsearchN:, scsearch:,
// scsearchN:).
bool is_ytsearch(std::string_view path) {
  return match_search_prefix(path, "ytsearch") || match_search_prefix(path, "scsearch");
}

// cliamp playlist.go IsURL: case-sensitive http:// or https:// prefix, or a
// yt-dlp search protocol string.
bool is_url(std::string_view path) {
  return path.starts_with("http://") || path.starts_with("https://") || is_ytsearch(path);
}

// cliamp playlist.go IsYTDL:188-225, ported 1:1 (including the music.163.com
// host and the .bilibili.com/.bandcamp.com suffix rules).
bool is_ytdl(std::string_view path) {
  if (!is_url(path)) return false;
  // YouTube / YouTube Music (IsYouTubeURL || IsYouTubeMusicURL).
  std::string host = url_hostname(path);
  host = std::string(strip_prefix(host, "www."));
  host = std::string(strip_prefix(host, "m."));
  if (host == "youtube.com" || host == "youtu.be" || host == "music.youtube.com") {
    return true;
  }
  if (is_ytsearch(path)) return true;
  if (host == "soundcloud.com" || host == "bandcamp.com" || host == "music.163.com" ||
      host == "bilibili.com" || host == "b23.tv") {
    return true;
  }
  if (host.ends_with(".bilibili.com")) return true;  // space.bilibili.com, ...
  if (host.ends_with(".bandcamp.com")) return true;  // artist.bandcamp.com, ...
  return false;
}

}  // namespace bootamp::resolve::detail

// NOTE: bootamp::playlist::is_ytdl is defined in playlist/playlist.cpp (the
// canonical Go IsYTDL port). resolve::detail::is_ytdl above is the resolve-
// module-internal copy used by resolve_ytdl; the two must stay in sync.

namespace bootamp::resolve {
namespace {

// -------------------------------------------------------------- process IO --

// exec.LookPath equivalent (Go os/exec.LookPath): search PATH for an
// executable named `name`; empty PATH elements mean the current directory.
std::optional<std::string> look_path(std::string_view name) {
  auto executable = [](const std::string& cand) {
    struct stat st;
    return stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           access(cand.c_str(), X_OK) == 0;
  };
  if (name.find('/') != std::string_view::npos) {
    const std::string cand(name);
    if (executable(cand)) return cand;
    return std::nullopt;
  }
  const char* path = std::getenv("PATH");
  if (path == nullptr) return std::nullopt;
  std::string_view pv(path);
  while (true) {
    const size_t sep = pv.find(':');
    const std::string_view dir = pv.substr(0, sep);
    std::string cand;
    if (dir.empty()) {
      cand = std::string(name);  // Go: empty PATH element = current directory
    } else {
      cand = std::string(dir) + (dir.back() == '/' ? "" : "/") + std::string(name);
    }
    if (executable(cand)) return cand;
    if (sep == std::string_view::npos) break;
    pv.remove_prefix(sep + 1);
  }
  return std::nullopt;
}

struct YtdlRun {
  std::string out;
  std::string err;
  int         exit_code = -1;  // valid when signal == 0
  int         signal    = 0;   // non-zero when killed by a signal
  bool        timed_out = false;
};

// Spawns `exe` with `args` (args[0] is the command name) via posix_spawn,
// collecting stdout/stderr through pipes. Bounded by a 30s deadline: on
// expiry the child is SIGKILLed and timed_out is set (Go context timeout,
// WaitDelay 3s omitted — see report). Returns nullopt on spawn failure.
std::optional<YtdlRun> run_ytdl(const std::string& exe, const std::vector<std::string>& args) {
  constexpr int kTimeoutMs = 30 * 1000;
  const auto start = std::chrono::steady_clock::now();
  auto elapsed_ms = [&]() -> int {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
  };

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0) {
    if (out_pipe[0] >= 0) {
      close(out_pipe[0]);
      close(out_pipe[1]);
    }
    if (err_pipe[0] >= 0) {
      close(err_pipe[0]);
      close(err_pipe[1]);
    }
    return std::nullopt;
  }
  // Non-blocking reads so poll() governs the collection loop.
  for (int fd : {out_pipe[0], err_pipe[0]}) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0 ||
      posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO) != 0 ||
      posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO) != 0 ||
      posix_spawn_file_actions_addclose(&actions, out_pipe[1]) != 0 ||
      posix_spawn_file_actions_addclose(&actions, err_pipe[1]) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return std::nullopt;
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(out_pipe[1]);
  close(err_pipe[1]);
  if (rc != 0) {
    close(out_pipe[0]);
    close(err_pipe[0]);
    return std::nullopt;
  }

  // Read both pipes until EOF or deadline (both must be drained: yt-dlp can
  // fill stderr while stdout is still being read).
  YtdlRun run;
  bool out_eof = false;
  bool err_eof = false;
  const auto drain = [](int fd, std::string& buf) -> bool {
    for (;;) {
      char tmp[16384];
      const ssize_t n = read(fd, tmp, sizeof tmp);
      if (n > 0) {
        buf.append(tmp, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) return true;  // EOF
      if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
      if (errno == EINTR) continue;
      return true;  // hard read error: treat as EOF
    }
  };
  while ((!out_eof || !err_eof) && elapsed_ms() < kTimeoutMs) {
    struct pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    const int prc = poll(fds, 2, kTimeoutMs - elapsed_ms());
    if (prc == 0) {
      run.timed_out = true;
      break;
    }
    if (prc < 0) {
      if (errno == EINTR) continue;
      break;  // poll error: stop collecting
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) out_eof = drain(out_pipe[0], run.out) || out_eof;
    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) err_eof = drain(err_pipe[0], run.err) || err_eof;
  }
  close(out_pipe[0]);
  close(err_pipe[0]);

  // Reap the child, bounded by the remaining deadline (a child can exit after
  // closing its pipes; Go waits for the process, not just the pipes).
  int status = 0;
  bool reaped = false;
  while (!reaped && elapsed_ms() < kTimeoutMs) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      break;  // ECHILD: already reaped
    }
    poll(nullptr, 0, 25);  // interruptible sleep slice
  }
  if (!reaped) {
    run.timed_out = true;
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  if (run.timed_out) return run;
  if (WIFEXITED(status)) {
    run.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    run.signal = WTERMSIG(status);
  }
  return run;
}

// Go syscall.Signal.String() for the common signals.
const char* signal_name(int sig) {
  switch (sig) {
    case SIGHUP: return "hangup";
    case SIGINT: return "interrupt";
    case SIGQUIT: return "quit";
    case SIGILL: return "illegal instruction";
    case SIGABRT: return "aborted";
    case SIGFPE: return "floating point exception";
    case SIGKILL: return "killed";
    case SIGSEGV: return "segmentation fault";
    case SIGPIPE: return "broken pipe";
    case SIGALRM: return "alarm clock";
    case SIGTERM: return "terminated";
    case SIGUSR1: return "user defined signal 1";
    case SIGUSR2: return "user defined signal 2";
    case SIGCHLD: return "child exited";
    case SIGCONT: return "continued";
    case SIGSTOP: return "stopped (signal)";
    case SIGTSTP: return "stopped";
    case SIGBUS: return "bus error";
    default: return "signal";
  }
}

// ------------------------------------------------------------ entry points --

}  // namespace

std::expected<std::vector<playlist::Track>, std::string>
resolve_ytdl(std::string_view url) {
  return resolve_ytdl_with_bounds(url, 0, 0);
}

std::expected<std::vector<playlist::Track>, std::string>
resolve_ytdl_with_bounds(std::string_view url, int start, int end) {
  // Go: exec.LookPath("yt-dlp") gate.
  const std::optional<std::string> exe = look_path("yt-dlp");
  if (!exe) {
    return std::unexpected(
        std::string("yt-dlp not found in PATH — see https://github.com/yt-dlp/yt-dlp#installation"));
  }

  // Exact Go argument order (resolve.go:674-691):
  // --flat-playlist -j --socket-timeout 15 [--cookies-from-browser B]
  // [--playlist-start S+1 --playlist-end E] <url>
  std::vector<std::string> args;
  args.emplace_back("yt-dlp");
  args.emplace_back("--flat-playlist");
  args.emplace_back("-j");
  args.emplace_back("--socket-timeout");
  args.emplace_back("15");
  std::string browser = detail::trim_space(ytdl_cookies_from());
  if (!browser.empty()) {
    args.emplace_back("--cookies-from-browser");
    args.push_back(std::move(browser));
  }
  if (start > 0) {
    args.emplace_back("--playlist-start");
    args.push_back(std::to_string(start + 1));  // yt-dlp is 1-based
  }
  if (end > 0) {
    args.emplace_back("--playlist-end");
    args.push_back(std::to_string(end));
  }
  args.push_back(std::string(url));

  const std::optional<YtdlRun> run = run_ytdl(*exe, args);
  if (!run) {
    return std::unexpected(std::string("yt-dlp: fork/exec failed"));
  }
  if (run->timed_out) {
    // Go: ctx.Err() checked first → context deadline exceeded.
    return std::unexpected("yt-dlp: resolve " + std::string(url) + ": context deadline exceeded");
  }
  if (run->exit_code != 0 || run->signal != 0) {
    const std::string msg = detail::trim_space(run->err);
    if (!msg.empty()) return std::unexpected("yt-dlp: " + msg);
    if (run->signal != 0) {
      return std::unexpected("yt-dlp: signal: " + std::string(signal_name(run->signal)));
    }
    return std::unexpected("yt-dlp: exit status " + std::to_string(run->exit_code));
  }
  const std::expected<detail::ytdl_parse_result, std::string> parsed =
      detail::parse_ytdl_tracks(run->out);
  if (!parsed) return std::unexpected(parsed.error());
  return parsed->tracks;
}

}  // namespace bootamp::resolve
