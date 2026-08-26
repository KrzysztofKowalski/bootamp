// audio/ytdl.cpp — yt-dlp | ffmpeg subprocess pipe for YT/YTM/SC/Bilibili/
// Bandcamp.
//
// Port of cliamp/player/ytdl.go, 1:1 semantics, using posix_spawn + pipe()
// (no boost):
//
//   - decode_ytdlp_pipe: Go decodeYTDLPipe + the buildYTDLPipeline prefill,
//     folded into the factory. Two children connected by pipe(): yt-dlp
//     "-f bestaudio[protocol=https]/.../best -o - <url>" feeds ffmpeg
//     "[-ss S] -i pipe:0 -f {s16le|f32le} -acodec pcm_{s16le|f32le} -ar SR
//     -ac 2 -loglevel error pipe:1" (exact Go argv). The factory then
//     pre-fills 1 byte with a 30s timeout (Go bufio.Peek(1)/ytdlPipeTimeout,
//     implemented with poll() so the byte stays in the pipe for the first
//     stream()); on empty-EOF it surfaces the cause via wait_cause(3s),
//     preferring yt-dlp's stderr (bot wall, 404, DRM) over ffmpeg's.
//   - dual waitpid-reporter threads (Go monitorExit): each drains the
//     child's stderr to EOF into the 64KB-capped LimitedBuffer (Go's
//     os/exec copy goroutine), reaps (single waitpid owner), and publishes
//     the first cause.
//   - stream(): Go streamFromReader + ytdlPipeStreamer.Stream — fills dst
//     from the stdout fd, decodes every complete frame, advances the atomic
//     position, and on empty-EOF polls wait_cause(0) to surface the cause.
//   - close(): SIGKILL both children, close the pipe, join the monitors —
//     no zombies. Idempotent like Go's closeOnce.
//   - probe_ytdlp_duration: Go probeYTDLDuration — "--skip-download
//     --no-playlist --socket-timeout 10 --print duration [--cookies-from-
//     browser B] <url>" with a 10s deadline, whole-string float parse,
//     secs <= 0 ⇒ 0.
//
// Seek is ffmpeg -ss INPUT-side restart: the engine rebuilds the pipeline
// via build_ytdl_pipeline (engine.cpp seek_ytdl, generation-cancelled) —
// this streamer's own Seek is a no-op (seek-by-restart handled out-of-band).
#include "audio/ytdl.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace bootamp::audio {

namespace {

// read_eintr retries read() on EINTR.
ssize_t read_eintr(int fd, void* buf, std::size_t n) {
  for (;;) {
    const ssize_t r = ::read(fd, buf, n);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    return r;
  }
}

// read_full reads until dst is full or EOF (Go io.ReadFull). Any error
// other than EINTR is EOF-equivalent: close() unblocks a blocked reader by
// closing the fd, and Go's os.File read on a closed pipe yields EOF.
std::size_t read_full(int fd, std::span<std::byte> dst) {
  std::size_t filled = 0;
  while (filled < dst.size()) {
    const ssize_t r = read_eintr(fd, dst.data() + filled, dst.size() - filled);
    if (r <= 0) {
      break;
    }
    filled += static_cast<std::size_t>(r);
  }
  return filled;
}

// Go's strings.TrimSpace.
std::string trim_space(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) {
    ++b;
  }
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                   s[e - 1] == '\r')) {
    --e;
  }
  return std::string(s.substr(b, e - b));
}

// Renders a timeout like Go's time.Duration.String() ("30s" / "1500ms").
std::string format_duration(std::chrono::milliseconds ms) {
  const auto m = ms.count();
  if (m % 1000 == 0) {
    return std::to_string(m / 1000) + "s";
  }
  return std::to_string(m) + "ms";
}

// look_path scans PATH for an executable (Go exec.LookPath). Empty PATH
// entries mean the current directory.
bool look_path(std::string_view name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return false;
  }
  std::string_view rest(path_env);
  while (!rest.empty()) {
    const std::size_t colon = rest.find(':');
    const std::string_view dir = rest.substr(0, colon);
    const std::string cand =
        dir.empty() ? std::string(name) : std::string(dir) + "/" + std::string(name);
    if (::access(cand.c_str(), X_OK) == 0) {
      return true;
    }
    if (colon == std::string_view::npos) {
      break;
    }
    rest = rest.substr(colon + 1);
  }
  return false;
}

// ytdlp_install_hint / ffmpeg_install_hint: the linux branches of Go's
// YtdlpInstallHint / ffmpegInstallHint.
std::string ytdlp_install_hint() {
  if (look_path("apt-get")) {
    return "sudo apt install yt-dlp";
  }
  if (look_path("pacman")) {
    return "sudo pacman -S yt-dlp";
  }
  return "pip install yt-dlp";
}

std::string ffmpeg_install_hint() {
  if (look_path("apt-get")) {
    return "sudo apt install ffmpeg";
  }
  if (look_path("pacman")) {
    return "sudo pacman -S ffmpeg";
  }
  return "see https://ffmpeg.org/download.html";
}

// Global cookie-browser setting (Go: ytdlCookiesFrom, configured at startup).
std::string g_ytdl_cookies;

// Go os/exec wait-error formatting: "exit status N" | "signal: <name>".
std::string wait_status_str(int status) {
  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    if (code != 0) {
      return "exit status " + std::to_string(code);
    }
    return {};
  }
  if (WIFSIGNALED(status)) {
    return std::string("signal: ") + ::strsignal(WTERMSIG(status));
  }
  return "unknown wait status";
}

// reap_waitpid is the single waitpid owner for one child. Returns the Go
// wait-error string ("" = clean exit).
std::string reap_waitpid(pid_t pid) {
  int status = 0;
  for (;;) {
    const pid_t r = ::waitpid(pid, &status, 0);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::string("waitpid: ") + std::strerror(errno);
    }
    break;
  }
  return wait_status_str(status);
}

// publish_ptr stores the first reported cause (CAS; the loser is freed).
// The winning string is immutable and freed by ~YtdlpPipeStreamer after the
// monitors are joined — atomics only, no locks on the audio path.
void publish_ptr(std::atomic<std::string*>& slot, const std::string& err) {
  auto* fresh = new std::string(err);
  std::string* expected = nullptr;
  if (!slot.compare_exchange_strong(expected, fresh, std::memory_order_release,
                                    std::memory_order_relaxed)) {
    delete fresh;
  }
}

// to_argv converts string storage into a null-terminated argv for spawn.
std::vector<char*> to_argv(std::vector<std::string>& storage) {
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& s : storage) {
    argv.push_back(s.data());
  }
  argv.push_back(nullptr);
  return argv;
}

// add_fd_action wires src onto dst (dup2 + close src), or /dev/null when
// src < 0 (Go: cmd.Stdin/Stderr nil ⇒ /dev/null). Returns errno (0 = ok).
int add_fd_action(posix_spawn_file_actions_t& fa, int src, int dst, int oflag) {
  if (src >= 0) {
    const int e = posix_spawn_file_actions_adddup2(&fa, src, dst);
    if (e != 0) {
      return e;
    }
    return posix_spawn_file_actions_addclose(&fa, src);
  }
  const int dn = ::open("/dev/null", oflag | O_CLOEXEC);
  if (dn < 0) {
    return errno;
  }
  const int e = posix_spawn_file_actions_adddup2(&fa, dn, dst);
  ::close(dn);
  return e;
}

// spawn_with_fds runs prog (PATH search like exec.Command) with the given
// fds wired to 0/1/2. Returns errno (0 = spawned, *out = child pid).
int spawn_with_fds(const char* prog, char* const argv[], int stdin_fd, int stdout_fd,
                   int stderr_fd, pid_t* out) {
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) {
    return EIO;
  }
  const int e1 = add_fd_action(fa, stdin_fd, STDIN_FILENO, O_RDONLY);
  const int e2 = add_fd_action(fa, stdout_fd, STDOUT_FILENO, O_WRONLY);
  const int e3 = add_fd_action(fa, stderr_fd, STDERR_FILENO, O_WRONLY);
  if (e1 != 0 || e2 != 0 || e3 != 0) {
    posix_spawn_file_actions_destroy(&fa);
    return e1 != 0 ? e1 : (e2 != 0 ? e2 : e3);
  }
  const int e = ::posix_spawnp(out, prog, &fa, nullptr, argv, ::environ);
  posix_spawn_file_actions_destroy(&fa);
  return e;
}

// poll_readable blocks until >= 1 byte is available on fd (Go bufio.Peek(1)
// for a pipe: readable ⇒ >= 1 byte, write end closed ⇒ EOF). The byte is NOT
// consumed — the first stream() still sees it. Returns "" (data ready),
// "EOF", an error message, or "timeout" when the deadline elapses first.
std::string poll_readable(int fd, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remain.count() <= 0) {
      return "timeout";
    }
    struct pollfd pfd {
      fd, POLLIN, 0
    };
    const int r = ::poll(&pfd, 1,
                         static_cast<int>(std::min<std::int64_t>(remain.count(), INT_MAX)));
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::string("poll: ") + std::strerror(errno);
    }
    if (r == 0) {
      return "timeout";
    }
    if ((pfd.revents & POLLIN) != 0) {
      return {};  // >= 1 byte ready
    }
    if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      return "EOF";
    }
  }
}

// monitor_process is the waitpid reporter for one child (Go monitorExit).
// Drains the child's stderr to EOF first (continuous draining, so a stderr
// flood can never deadlock the child against the 64KB cap — Go's os/exec
// copy goroutine), reaps (the single waitpid owner), then publishes the
// first cause: "<name>: <waitErr>[: <trimmed stderr>]" or nothing on a
// clean exit, and finally sets the done flag (Go: buffered ch + close(done)).
void monitor_process(pid_t pid, int stderr_fd, LimitedBuffer& buf,
                     std::atomic<std::string*>& slot, std::atomic<bool>& done,
                     std::string_view name) {
  std::array<std::byte, 4096> chunk{};
  for (;;) {
    const ssize_t r = read_eintr(stderr_fd, chunk.data(), chunk.size());
    if (r <= 0) {
      break;
    }
    buf.write(std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(r)));
  }
  ::close(stderr_fd);

  const std::string werr = reap_waitpid(pid);
  std::string err;
  if (!werr.empty()) {
    const std::string stderr_text = trim_space(buf.str());
    if (!stderr_text.empty()) {
      err = std::string(name) + ": " + werr + ": " + stderr_text;
    } else {
      err = std::string(name) + ": " + werr;
    }
  }
  if (!err.empty()) {
    publish_ptr(slot, err);
  }
  done.store(true, std::memory_order_release);
}

}  // namespace

// ---------------------------------------------------------------------------
// yt-dlp presence + cookies
// ---------------------------------------------------------------------------

void set_ytdl_cookies_from(std::string_view browser) {
  g_ytdl_cookies = std::string(browser);
}

std::string_view ytdl_cookies_from() {
  return g_ytdl_cookies;
}

bool ytdlp_available() {
  return look_path("yt-dlp");
}

// ---------------------------------------------------------------------------
// probe_ytdlp_duration (Go probeYTDLDuration)
// ---------------------------------------------------------------------------

std::chrono::duration<double> probe_ytdlp_duration(std::string_view page_url) {
  // "--skip-download --no-playlist --socket-timeout 10 --print duration
  // [--cookies-from-browser B] <url>" — exact Go argv.
  std::vector<std::string> args = {
      "yt-dlp", "--skip-download", "--no-playlist", "--socket-timeout", "10",
      "--print", "duration"};
  if (std::string_view cb = ytdl_cookies_from(); !cb.empty()) {
    args.push_back("--cookies-from-browser");
    args.push_back(std::string(cb));
  }
  args.push_back(std::string(page_url));

  int out_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0) {
    return {};
  }
  pid_t pid = -1;
  {
    std::vector<char*> argv = to_argv(args);
    const int e = spawn_with_fds("yt-dlp", argv.data(), -1, out_pipe[1], -1, &pid);
    ::close(out_pipe[1]);
    if (e != 0) {
      ::close(out_pipe[0]);
      return {};
    }
  }

  // Read stdout to EOF with a 10s deadline (Go: 10s context timeout + 3s
  // WaitDelay). Capped so a misbehaving yt-dlp cannot exhaust memory.
  std::string out;
  out.reserve(128);
  std::array<char, 4096> chunk{};
  constexpr std::size_t kProbeOutputLimit = 64 * 1024;
  bool timed_out = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (out.size() < kProbeOutputLimit) {
    const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remain.count() <= 0) {
      timed_out = true;
      break;
    }
    struct pollfd pfd {
      out_pipe[0], POLLIN, 0
    };
    const int r = ::poll(&pfd, 1,
                         static_cast<int>(std::min<std::int64_t>(remain.count(), INT_MAX)));
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (r == 0) {
      timed_out = true;
      break;
    }
    if ((pfd.revents & POLLIN) == 0) {
      break;  // HUP/ERR/NVAL ⇒ EOF
    }
    const ssize_t n = ::read(out_pipe[0], chunk.data(), chunk.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;  // EOF
    }
    out.append(chunk.data(), static_cast<std::size_t>(n));
  }
  ::close(out_pipe[0]);
  if (timed_out && pid > 0) {
    ::kill(pid, SIGKILL);  // Go: the context deadline cancels the command
  }
  if (pid > 0) {
    (void)reap_waitpid(pid);  // no zombies
  }

  // Go: strconv.ParseFloat(strings.TrimSpace(out)); secs <= 0 ⇒ 0.
  const std::string t = trim_space(out);
  if (t.empty()) {
    return {};
  }
  char* end = nullptr;
  const double secs = std::strtod(t.c_str(), &end);
  // ParseFloat requires the whole string to be a valid float.
  if (end == t.c_str() || *end != '\0' || !std::isfinite(secs)) {
    return {};
  }
  if (!(secs > 0.0)) {
    return {};
  }
  return std::chrono::duration<double>(secs);
}

// ---------------------------------------------------------------------------
// YtdlpPipeStreamer
// ---------------------------------------------------------------------------

std::expected<std::unique_ptr<YtdlpPipeStreamer>, std::string>
YtdlpPipeStreamer::decode_ytdlp_pipe(std::string_view page_url, int sr, int bit_depth,
                                     int start_sec) {
  if (!look_path("yt-dlp")) {
    return std::unexpected(std::string("yt-dlp is required — install: ") + ytdlp_install_hint());
  }
  if (!look_path("ffmpeg")) {
    return std::unexpected(std::string("ffmpeg is required — install: ") + ffmpeg_install_hint());
  }

  // os.Pipe connects yt-dlp stdout → ffmpeg stdin.
  int mid_pipe[2] = {-1, -1};
  if (::pipe2(mid_pipe, O_CLOEXEC) != 0) {
    return std::unexpected(std::string("os.Pipe: ") + std::strerror(errno));
  }
  const int pr = mid_pipe[0];
  const int pw = mid_pipe[1];

  // yt-dlp: best audio to stdout. Prefer direct HTTPS/HTTP streams over HLS
  // (m3u8) — HLS needs segment downloading/muxing which doesn't pipe cleanly
  // to stdout; live streams expose only muxed video+audio over HLS, so fall
  // back to "best" (the ffmpeg stage drops the video). Exact Go argv.
  std::vector<std::string> ytdl_args = {
      "yt-dlp",
      "-f", "bestaudio[protocol=https]/bestaudio[protocol=http]/bestaudio[protocol!=m3u8_native][protocol!=m3u8]/bestaudio/best",
      "--no-playlist", "--quiet", "--no-warnings",
      "--socket-timeout", "15", "-o", "-"};
  if (std::string_view cb = ytdl_cookies_from(); !cb.empty()) {
    ytdl_args.push_back("--cookies-from-browser");
    ytdl_args.push_back(std::string(cb));
  }
  ytdl_args.push_back(std::string(page_url));

  int ytdl_err_pipe[2] = {-1, -1};
  if (::pipe2(ytdl_err_pipe, O_CLOEXEC) != 0) {
    ::close(pr);
    ::close(pw);
    return std::unexpected(std::string("yt-dlp stderr pipe: ") + std::strerror(errno));
  }
  pid_t ytdl_pid = -1;
  {
    std::vector<char*> argv = to_argv(ytdl_args);
    const int e =
        spawn_with_fds("yt-dlp", argv.data(), -1, pw, ytdl_err_pipe[1], &ytdl_pid);
    ::close(pw);
    ::close(ytdl_err_pipe[1]);
    if (e != 0) {
      ::close(pr);
      ::close(ytdl_err_pipe[0]);
      return std::unexpected(std::string("yt-dlp start: ") + std::strerror(e));
    }
  }

  // ffmpeg: read pipe:0, output PCM to stdout. startSec > 0 ⇒ input-side -ss.
  const PcmArgs pcm = ffmpeg_pcm_args(bit_depth);
  std::vector<std::string> ffmpeg_args = {"ffmpeg"};
  if (start_sec > 0) {
    ffmpeg_args.push_back("-ss");
    ffmpeg_args.push_back(std::to_string(start_sec));
  }
  ffmpeg_args.push_back("-i");
  ffmpeg_args.push_back("pipe:0");
  ffmpeg_args.push_back("-f");
  ffmpeg_args.push_back(pcm.format);
  ffmpeg_args.push_back("-acodec");
  ffmpeg_args.push_back(pcm.codec);
  ffmpeg_args.push_back("-ar");
  ffmpeg_args.push_back(std::to_string(sr));
  ffmpeg_args.push_back("-ac");
  ffmpeg_args.push_back("2");
  ffmpeg_args.push_back("-loglevel");
  ffmpeg_args.push_back("error");
  ffmpeg_args.push_back("pipe:1");

  int out_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0) {
    ::close(pr);
    ::kill(ytdl_pid, SIGKILL);  // Go: ytdlCmd.Process.Kill() + Wait()
    (void)reap_waitpid(ytdl_pid);
    ::close(ytdl_err_pipe[0]);
    return std::unexpected(std::string("ffmpeg stdout pipe: ") + std::strerror(errno));
  }
  int ff_err_pipe[2] = {-1, -1};
  if (::pipe2(ff_err_pipe, O_CLOEXEC) != 0) {
    ::close(pr);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::kill(ytdl_pid, SIGKILL);
    (void)reap_waitpid(ytdl_pid);
    ::close(ytdl_err_pipe[0]);
    return std::unexpected(std::string("ffmpeg stderr pipe: ") + std::strerror(errno));
  }
  pid_t ffmpeg_pid = -1;
  {
    std::vector<char*> argv = to_argv(ffmpeg_args);
    const int e = spawn_with_fds("ffmpeg", argv.data(), pr, out_pipe[1], ff_err_pipe[1],
                                 &ffmpeg_pid);
    ::close(pr);
    ::close(out_pipe[1]);
    ::close(ff_err_pipe[1]);
    if (e != 0) {
      ::close(out_pipe[0]);
      ::close(ff_err_pipe[0]);
      ::kill(ytdl_pid, SIGKILL);
      (void)reap_waitpid(ytdl_pid);
      ::close(ytdl_err_pipe[0]);
      return std::unexpected(std::string("ffmpeg start: ") + std::strerror(e));
    }
  }

  // Private ctor: make_unique can't reach it, but this member function can.
  auto s = std::unique_ptr<YtdlpPipeStreamer>(new YtdlpPipeStreamer());
  s->ytdl_pid_ = ytdl_pid;
  s->ffmpeg_pid_ = ffmpeg_pid;
  s->stdout_fd_ = out_pipe[0];
  s->state_ = std::make_shared<PipeStreamState>(0);
  s->f32_ = (bit_depth == 32);

  // Dual waitpid-reporter threads (Go monitorExit). Each owns a stderr read
  // end: drains to EOF (bounded), reaps, publishes the first cause.
  try {
    s->ytdl_monitor_ = std::thread(monitor_process, ytdl_pid, ytdl_err_pipe[0],
                                   std::ref(s->ytdl_stderr_), std::ref(s->ytdl_err_),
                                   std::ref(s->ytdl_done_), "yt-dlp");
    s->ffmpeg_monitor_ = std::thread(monitor_process, ffmpeg_pid, ff_err_pipe[0],
                                     std::ref(s->ffmpeg_stderr_), std::ref(s->ffmpeg_err_),
                                     std::ref(s->ffmpeg_done_), "ffmpeg");
  } catch (...) {
    // Thread creation failed (practically unreachable): kill both children
    // now; ~YtdlpPipeStreamer → close() joins whatever monitor started. The
    // monitor that did start owns its stderr read end and closes it after
    // draining; the other read ends are closed here so no fd leaks.
    ::close(ytdl_err_pipe[0]);
    ::close(ff_err_pipe[0]);
    if (ytdl_pid > 0) {
      ::kill(ytdl_pid, SIGKILL);
      (void)reap_waitpid(ytdl_pid);
    }
    if (ffmpeg_pid > 0) {
      ::kill(ffmpeg_pid, SIGKILL);
      (void)reap_waitpid(ffmpeg_pid);
    }
    return std::unexpected("failed to start monitor threads");
  }

  // Pre-fill: block until yt-dlp + ffmpeg produce initial audio (Go
  // buildYTDLPipeline bufio.Peek(1) / ytdlPipeTimeout=30s). poll() leaves
  // the byte in the pipe so the first stream() still sees it. On empty-EOF,
  // wait_cause(3s) prefers yt-dlp's stderr reason (bot wall, 404, DRM) over
  // ffmpeg's; otherwise the bare EOF is wrapped.
  const std::string peek = poll_readable(s->stdout_fd_, kYtdlPipeTimeout);
  if (peek == "timeout") {
    s->close();
    return std::unexpected(std::string("timed out waiting for audio data (") +
                           format_duration(kYtdlPipeTimeout) + ")");
  }
  if (!peek.empty()) {
    std::string cause = s->wait_cause(kYtdlCauseGrace);
    s->close();
    if (!cause.empty()) {
      return std::unexpected(std::move(cause));
    }
    return std::unexpected("waiting for audio data: " + peek);
  }
  return s;
}

YtdlpPipeStreamer::~YtdlpPipeStreamer() {
  close();
  // close() joined the monitors, so the atomically-published cause strings
  // (immutable, first-cause-wins) are safe to free here.
  delete ytdl_err_.load();
  delete ffmpeg_err_.load();
}

std::pair<std::size_t, bool> YtdlpPipeStreamer::stream(std::span<Frame> dst) {
  // Go streamFromReader: a published error poisons the stream immediately.
  if (!state_ || !state_->err.load().empty()) {
    return {0, false};
  }
  if (dst.empty()) {
    return {0, true};
  }

  const std::size_t fs = pcm_frame_size(f32_);
  pcm_buf_.resize(dst.size() * fs);  // reusable block buffer (Go *bufp)
  const std::size_t n_bytes = read_full(stdout_fd_, std::span<std::byte>(pcm_buf_));
  // Short reads are EOF-equivalent (io.ReadFull EOF / ErrUnexpectedEOF —
  // never published); a read on an fd closed by close() is EOF too (EBADF),
  // like Go reading a closed os.File.
  const std::size_t n = n_bytes / fs;  // partial trailing frames are dropped
  for (std::size_t i = 0; i < n; ++i) {
    dst[i] = decode_pcm_frame(std::span<const std::byte>(pcm_buf_).subspan(i * fs, fs),
                              f32_);
  }
  state_->pos.fetch_add(static_cast<std::int64_t>(n));

  const bool ok = n > 0;
  if (!ok && state_->err.load().empty()) {
    // Empty-EOF: surface why the pipe closed (yt-dlp bot wall, 404, DRM, or
    // undecodable ffmpeg input) instead of a bare EOF.
    if (const std::string cause = wait_cause(std::chrono::milliseconds(0));
        !cause.empty()) {
      state_->err.publish(cause);
    }
  }
  return {n, ok};
}

std::string YtdlpPipeStreamer::err() const {
  return state_ ? state_->err.load() : std::string{};
}

std::size_t YtdlpPipeStreamer::position() const {
  return state_ ? static_cast<std::size_t>(state_->pos.load()) : 0;
}

std::string YtdlpPipeStreamer::wait_cause(std::chrono::milliseconds d) const {
  if (d.count() <= 0) {
    // Go poll: yt-dlp's error wins; after a CLEAN yt-dlp report, ffmpeg's
    // report (if any) is returned; nothing pending ⇒ "".
    if (const std::string* p = ytdl_err_.load(std::memory_order_acquire)) {
      return *p;
    }
    if (ytdl_done_.load(std::memory_order_acquire)) {
      if (const std::string* p = ffmpeg_err_.load(std::memory_order_acquire)) {
        return *p;
      }
    }
    return {};
  }

  // Blocking path: wait up to d for the processes to report; yt-dlp's error
  // wins the moment it appears; ffmpeg's is returned at the deadline or once
  // both have reported. Polling the atomic slots (5ms) mirrors Go's select
  // loop without channels.
  const auto deadline = std::chrono::steady_clock::now() + d;
  bool ytdl_done = false;
  bool ffmpeg_done = false;
  std::string ff_err;
  while (!ytdl_done || !ffmpeg_done) {
    if (const std::string* p = ytdl_err_.load(std::memory_order_acquire)) {
      return *p;
    }
    if (ytdl_done_.load(std::memory_order_acquire)) {
      ytdl_done = true;
    }
    if (ffmpeg_done_.load(std::memory_order_acquire)) {
      ffmpeg_done = true;
      if (const std::string* p = ffmpeg_err_.load(std::memory_order_acquire)) {
        ff_err = *p;
      }
    }
    if (ytdl_done && ffmpeg_done) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return ff_err;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return ff_err;
}

void YtdlpPipeStreamer::close() {
  if (closed_.exchange(true)) {
    return;  // Go closeOnce
  }
  // Go Close(): kill yt-dlp first, then ffmpeg, close the pipe (unblocks a
  // stream() blocked in read), then wait for both monitor threads — which
  // own waitpid, so Close leaves no zombies.
  if (ytdl_pid_ > 0) {
    ::kill(ytdl_pid_, SIGKILL);
  }
  if (ffmpeg_pid_ > 0) {
    ::kill(ffmpeg_pid_, SIGKILL);
  }
  if (stdout_fd_ >= 0) {
    ::close(stdout_fd_);
    stdout_fd_ = -1;
  }
  if (ytdl_monitor_.joinable()) {
    ytdl_monitor_.join();
  }
  if (ffmpeg_monitor_.joinable()) {
    ffmpeg_monitor_.join();
  }
}

}  // namespace bootamp::audio
