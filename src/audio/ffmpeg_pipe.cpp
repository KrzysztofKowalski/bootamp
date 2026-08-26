// audio/ffmpeg_pipe.cpp — shared ffmpeg-subprocess PCM machinery.
//
// Port of cliamp/player/ffmpeg.go, 1:1 semantics, using posix_spawn + pipe()
// (no boost). Reused by radio, YouTube, and local-file fallback.
//
//   - decode_pcm_frame: s16le → int16/32768, f32le → std::bit_cast<float>
//     (NaN/Inf preserved — no fast-math).
//   - stream(): Go's streamFromReader + ffmpegPipe.Stream: fills dst from the
//     buffered prefix (rbuf_, Go's bufio.Reader) then the stdout fd, decodes
//     every complete frame, advances the atomic position, and on EOF closes
//     the stdin pipe and reaps ffmpeg (single waitpid owner).
//   - wait_for_audio_bytes: Go's waitForAudioBytes — a peek jthread blocks in
//     read until n bytes are available (bufio.Peek), a condvar deadline fires
//     on timeout, and stop() (close fds → kill → waitpid) unblocks the peek.
//     Peeked bytes are stashed in rbuf_ so stream() still sees them.
//   - start_ffmpeg_pipe: posix_spawn "ffmpeg -i <input> -f {s16le|f32le}
//     -acodec pcm_{s16le|f32le} -ar SR -ac 2 -loglevel error pipe:1" with the
//     stdout pipe as fd 1, the caller's stdin pipe read end (or /dev/null) as
//     fd 0, and a capped 64KB stderr drain thread.
#include "audio/ffmpeg_pipe.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace bootamp::audio {

namespace {

// read_eintr retries read() on EINTR (the "read_full EINTR loop").
ssize_t read_eintr(int fd, void* buf, std::size_t n) {
  for (;;) {
    const ssize_t r = ::read(fd, buf, n);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    return r;
  }
}

// Explicit little-endian assembly (portable, no host-endianness assumption;
// Go's binary.LittleEndian equivalents).
std::uint16_t le_u16(std::span<const std::byte> b) {
  const std::uint16_t b0 = std::to_integer<std::uint16_t>(b[0]);
  const std::uint16_t b1 = std::to_integer<std::uint16_t>(b[1]);
  return static_cast<std::uint16_t>(b0 | (b1 << 8));
}

std::uint32_t le_u32(std::span<const std::byte> b) {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) {
    v = (v << 8) | std::to_integer<std::uint32_t>(b[static_cast<std::size_t>(i)]);
  }
  return v;
}

// drain_buffered copies the unconsumed prefix of fp.rbuf_ into dst and
// advances rpos_. Returns bytes copied.
std::size_t drain_buffered(FfmpegPipe& fp, std::span<std::byte> dst) {
  if (dst.empty() || fp.rpos_ >= fp.rbuf_.size()) {
    return 0;
  }
  const std::size_t avail = fp.rbuf_.size() - fp.rpos_;
  const std::size_t n = std::min(avail, dst.size());
  std::memcpy(dst.data(), fp.rbuf_.data() + fp.rpos_, n);
  fp.rpos_ += n;
  if (fp.rpos_ == fp.rbuf_.size()) {
    fp.rbuf_.clear();
    fp.rpos_ = 0;
  }
  return n;
}

// read_chain fills dst from the buffered prefix first, then the stdout fd.
// Returns bytes read and an error string that can only be "" (complete),
// "EOF" (nothing read) or "unexpected EOF" (partial) — Go's io.ReadFull error
// set for an fd chain; EOF and unexpected-EOF are tolerated, never published.
std::pair<std::size_t, std::string> read_chain(FfmpegPipe& fp,
                                               std::span<std::byte> dst) {
  std::size_t filled = drain_buffered(fp, dst);
  while (filled < dst.size()) {
    const ssize_t r = read_eintr(fp.stdout_fd, dst.data() + filled,
                                 dst.size() - filled);
    if (r <= 0) {
      break;
    }
    filled += static_cast<std::size_t>(r);
  }
  if (filled < dst.size()) {
    return {filled, filled == 0 ? "EOF" : "unexpected EOF"};
  }
  return {filled, ""};
}

// Go's strings.TrimSpace on the captured stderr.
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

// waitpid_loop reaps pid (EINTR retried; this is the single waitpid owner) and
// formats the outcome like Go's os/exec Cmd.Wait error: "exit status N",
// "signal: <name>", ... wrapped as "ffmpeg decode: <err>[: <stderr>]"
// (cliamp ffmpegProcess.wait). "" means the process exited 0.
std::string waitpid_loop(pid_t pid, const LimitedBuffer& stderr_buf) {
  int status = 0;
  for (;;) {
    const pid_t r = ::waitpid(pid, &status, 0);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::string("ffmpeg decode: waitpid: ") + std::strerror(errno);
    }
    break;
  }
  std::string err;
  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    if (code != 0) {
      err = "exit status " + std::to_string(code);
    }
  } else if (WIFSIGNALED(status)) {
    err = std::string("signal: ") + ::strsignal(WTERMSIG(status));
  } else {
    err = "unknown wait status";
  }
  if (err.empty()) {
    return {};
  }
  const std::string stderr_text = trim_space(stderr_buf.str());
  if (!stderr_text.empty()) {
    return "ffmpeg decode: " + err + ": " + stderr_text;
  }
  return "ffmpeg decode: " + err;
}

// Renders a timeout like Go's time.Duration.String() for whole seconds
// ("15s") and plain ms otherwise.
std::string format_duration(std::chrono::milliseconds ms) {
  const auto m = ms.count();
  if (m % 1000 == 0) {
    return std::to_string(m / 1000) + "s";
  }
  return std::to_string(m) + "ms";
}

}  // namespace

// ---------------------------------------------------------------------------
// decode_pcm_frame / ffmpeg_pcm_args
// ---------------------------------------------------------------------------

Frame decode_pcm_frame(std::span<const std::byte> buf, bool f32) {
  Frame out{0.0f, 0.0f};
  if (f32) {
    // f32le: little-endian float bits, bit_cast preserved verbatim — NaN/Inf
    // must survive (no fast-math). Go: math.Float32frombits.
    if (buf.size() < kPcmFrameSize32) {
      return out;
    }
    out[0] = std::bit_cast<float>(le_u32(buf.subspan(0, 4)));
    out[1] = std::bit_cast<float>(le_u32(buf.subspan(4, 4)));
  } else {
    // s16le: int16/32768. float32(int16) is exact and /32768 is a power of
    // two, so this is bit-identical to Go's float64(left)/32768 → float32.
    if (buf.size() < kPcmFrameSize16) {
      return out;
    }
    out[0] = static_cast<float>(static_cast<std::int16_t>(le_u16(buf.subspan(0, 2)))) /
             32768.0f;
    out[1] = static_cast<float>(static_cast<std::int16_t>(le_u16(buf.subspan(2, 2)))) /
             32768.0f;
  }
  return out;
}

PcmArgs ffmpeg_pcm_args(int bit_depth) {
  if (bit_depth == 32) {
    return {"f32le", "pcm_f32le", 4};
  }
  return {"s16le", "pcm_s16le", 2};
}

// ---------------------------------------------------------------------------
// FirstError / LimitedBuffer
// ---------------------------------------------------------------------------

void FirstError::publish(std::string_view err) {
  if (err.empty()) {
    return;
  }
  std::shared_ptr<const std::string> cur = value_.load();
  if (cur != nullptr) {
    return;  // first error wins (Go: CompareAndSwap(nil, ...))
  }
  auto fresh = std::make_shared<const std::string>(err);
  if (!value_.compare_exchange_strong(cur, fresh)) {
    // Lost a race against another publisher; the winner stands.
  }
}

std::string FirstError::load() const {
  const std::shared_ptr<const std::string> cur = value_.load();
  return cur ? *cur : std::string{};
}

void LimitedBuffer::write(std::span<const std::byte> p) {
  if (p.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lk(mu_);
  const std::size_t remaining = kSubprocessStderrLimit - buf_.size();
  if (remaining > 0) {
    const std::size_t n = std::min(p.size(), remaining);
    buf_.append(reinterpret_cast<const char*>(p.data()), n);
  }
  if (p.size() > remaining) {
    truncated_ = true;
  }
}

std::string LimitedBuffer::str() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::string s = buf_;
  if (truncated_) {
    s += "\n[stderr truncated]";
  }
  return s;
}

// ---------------------------------------------------------------------------
// FfmpegProcess: single waitpid owner, capped stderr drain
// ---------------------------------------------------------------------------

void FfmpegProcess::kill() {
  if (pid > 0) {
    ::kill(pid, SIGKILL);  // Go: cmd.Process.Kill()
  }
}

std::string FfmpegProcess::wait() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!wait_called_) {
    wait_called_ = true;
    if (pid > 0) {
      wait_err_ = waitpid_loop(pid, stderr_buf);
    }
  }
  return wait_err_;
}

FfmpegProcess::~FfmpegProcess() {
  // Reap the child if the caller never waited, so the stderr drain thread
  // (which ends when the child's fd 2 closes) can be joined — no zombie, no
  // leaked thread. SIGKILL first so the reap cannot hang on a stuck child.
  std::lock_guard<std::mutex> lk(mu_);
  if (pid > 0 && !wait_called_) {
    ::kill(pid, SIGKILL);
    wait_called_ = true;
    wait_err_ = waitpid_loop(pid, stderr_buf);
  }
  if (stderr_reader_.joinable()) {
    stderr_reader_.join();
  }
}

// ---------------------------------------------------------------------------
// FfmpegPipe: stream / wait_for_audio_bytes / interrupt / stop
// ---------------------------------------------------------------------------

std::pair<std::size_t, bool> FfmpegPipe::stream(std::span<Frame> dst) {
  // Go streamFromReader: a published error poisons the stream immediately.
  if (state == nullptr || !state->err.load().empty()) {
    return {0, false};
  }
  if (dst.empty()) {
    return {0, true};
  }

  const std::size_t fs = pcm_frame_size(f32);
  const std::size_t need = dst.size() * fs;
  pcm_buf.resize(need);  // reusable block buffer (Go's *bufp)
  auto [n_bytes, read_err] =
      read_chain(*this, std::span<std::byte>(pcm_buf));
  if (!read_err.empty() && read_err != "EOF" && read_err != "unexpected EOF") {
    state->err.publish(read_err);  // first non-EOF error
  }
  const std::size_t n = n_bytes / fs;  // partial trailing frames are dropped
  for (std::size_t i = 0; i < n; ++i) {
    dst[i] = decode_pcm_frame(
        std::span<const std::byte>(pcm_buf).subspan(i * fs, fs), f32);
  }
  state->pos.fetch_add(static_cast<std::int64_t>(n));

  const bool ok = n > 0;
  if (!ok) {
    // Go ffmpegPipe.Stream: close the input (EOF for ffmpeg) and reap.
    if (stdin_fd >= 0) {
      ::close(stdin_fd);
      stdin_fd = -1;
    }
    if (proc) {
      const std::string werr = proc->wait();
      if (!werr.empty()) {
        state->err.publish(werr);
      }
    }
  }
  if (!ok && live && state->err.load().empty()) {
    // A live stream never ends cleanly: EOF means the upstream connection
    // dropped (e.g. the stall timeout cancelled it) — surface it so
    // StreamErr()/auto-reconnect fires instead of end-of-track.
    state->err.publish("unexpected EOF");
  }
  return {n, ok};
}

std::string FfmpegPipe::wait_for_audio_bytes(std::size_t n,
                                             std::chrono::milliseconds timeout) {
  if (n == 0) {
    return {};  // Go bufio.Peek(0): immediate success
  }
  std::vector<std::byte> scratch(n);
  std::size_t got_new = 0;  // bytes the peek thread read into scratch
  std::string peek_err;
  std::atomic<bool> done{false};
  std::mutex mu;
  std::condition_variable cv;

  std::jthread peek([&](std::stop_token) {
    // bufio.Peek(n) analog: serve from the buffer first, then block in read
    // until n bytes total are available or EOF/error.
    const std::size_t buffered =
        (rpos_ < rbuf_.size()) ? rbuf_.size() - rpos_ : 0;
    const std::size_t served = std::min(n, buffered);
    while (served + got_new < n) {
      const ssize_t r = read_eintr(stdout_fd, scratch.data() + got_new,
                                   n - served - got_new);
      if (r <= 0) {
        break;
      }
      got_new += static_cast<std::size_t>(r);
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      if (served + got_new < n) {
        peek_err = (served + got_new == 0) ? "EOF" : "unexpected EOF";
      }
      done.store(true, std::memory_order_release);
    }
    cv.notify_all();
  });

  // Stash bytes read by the peek thread into rbuf_ so stream() still sees
  // them (bufio.Peek leaves peeked data in the buffer). Called after the
  // thread has finished.
  const auto stash = [&] {
    if (got_new == 0) {
      return;
    }
    rbuf_.insert(rbuf_.begin() + static_cast<std::ptrdiff_t>(rpos_),
                 scratch.begin(),
                 scratch.begin() + static_cast<std::ptrdiff_t>(got_new));
  };

  std::unique_lock<std::mutex> lk(mu);
  if (cv.wait_for(lk, timeout, [&] { return done.load(std::memory_order_acquire); })) {
    if (!peek_err.empty()) {
      // Go waitForAudioBytes error path: close the input, then reap ffmpeg;
      // a wait error (already "ffmpeg decode: ...") wins, otherwise wrap the
      // peek error.
      if (stdin_fd >= 0) {
        ::close(stdin_fd);
        stdin_fd = -1;
      }
      const std::string werr = proc ? proc->wait() : "";
      stash();
      if (!werr.empty()) {
        return werr;
      }
      return "waiting for audio data: " + peek_err;
    }
    stash();
    return std::string{};
  }

  // Timeout: interrupt (close fds → SIGKILL) then reap; closing the stdout fd
  // unblocks the peek read; drain the peek before returning.
  lk.unlock();
  (void)stop();
  std::unique_lock<std::mutex> lk2(mu);
  cv.wait(lk2, [&] { return done.load(std::memory_order_acquire); });
  stash();
  return "timed out waiting for audio data (" + format_duration(timeout) + ")";
}

void FfmpegPipe::interrupt() {
  // Go ffmpegPipe.interrupt: release blocked PCM/stdin reads without waiting.
  if (stdin_fd >= 0) {
    ::close(stdin_fd);  // aborts the caller's pump (EPIPE) + EOF for ffmpeg
    stdin_fd = -1;
  }
  if (stdout_fd >= 0) {
    ::close(stdout_fd);  // unblocks any blocked stdout read
    stdout_fd = -1;
  }
  if (proc) {
    proc->kill();
  }
}

std::string FfmpegPipe::stop() {
  // Go ffmpegPipe.stop: interrupt before reaping (os/exec may otherwise wait
  // on its stdin copy to finish).
  interrupt();
  if (!proc) {
    return {};
  }
  return proc->wait();
}

// ---------------------------------------------------------------------------
// start_ffmpeg_pipe: posix_spawn ffmpeg, stdout pipe, capped stderr drain
// ---------------------------------------------------------------------------

std::expected<std::unique_ptr<FfmpegPipe>, std::string> start_ffmpeg_pipe(
    std::string_view input_arg, int stdin_fd, int sr, int bit_depth,
    double start_sec) {
  const PcmArgs args = ffmpeg_pcm_args(bit_depth);
  const std::string input(input_arg);
  const std::string sr_str = std::to_string(sr);
  std::vector<std::string> argv_storage;
  argv_storage.reserve(16);
  argv_storage.push_back("ffmpeg");
  if (start_sec > 0.0) {
    // Input-side demuxer fast seek (cliamp localFFmpegStreamer.startPipe /
    // decodeYTDLPipe): "-ss" BEFORE "-i" seeks in the demuxer, not the
    // decoder, so it is near-instant on network/yt-dlp sources.
    argv_storage.push_back("-ss");
    argv_storage.push_back(std::to_string(start_sec));
  }
  argv_storage.push_back("-i");
  argv_storage.push_back(input);
  argv_storage.push_back("-f");
  argv_storage.push_back(args.format);
  argv_storage.push_back("-acodec");
  argv_storage.push_back(args.codec);
  argv_storage.push_back("-ar");
  argv_storage.push_back(sr_str);
  argv_storage.push_back("-ac");
  argv_storage.push_back("2");
  argv_storage.push_back("-loglevel");
  argv_storage.push_back("error");
  argv_storage.push_back("pipe:1");
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (std::string& s : argv_storage) {
    argv.push_back(s.data());
  }
  argv.push_back(nullptr);

  // stdout pipe: the child writes raw PCM here (Go cmd.StdoutPipe).
  int out_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0) {
    return std::unexpected(std::string("ffmpeg stdout pipe: ") +
                           std::strerror(errno));
  }
  // stderr pipe: drained by a thread into the 64KB-capped LimitedBuffer
  // (Go's cmd.Stderr = limitedBuffer + copy goroutine).
  int err_pipe[2] = {-1, -1};
  if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return std::unexpected(std::string("ffmpeg stderr pipe: ") +
                           std::strerror(errno));
  }

  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return std::unexpected("ffmpeg start: posix_spawn_file_actions_init");
  }
  posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
  posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&fa, err_pipe[1]);
  if (stdin_fd >= 0) {
    // Caller's pipe read end becomes the child's stdin (Go cmd.Stdin = src).
    posix_spawn_file_actions_adddup2(&fa, stdin_fd, STDIN_FILENO);
    posix_spawn_file_actions_addclose(&fa, stdin_fd);
  } else {
    // Go: cmd.Stdin = nil wires /dev/null.
    const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull >= 0) {
      posix_spawn_file_actions_adddup2(&fa, devnull, STDIN_FILENO);
      posix_spawn_file_actions_addclose(&fa, devnull);
    }
  }

  // posix_spawnp searches PATH for "ffmpeg" like exec.Command in Go;
  // a failed exec surfaces in the parent as this function's return value.
  pid_t pid = -1;
  const int e = ::posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), ::environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (e != 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    return std::unexpected(std::string("ffmpeg start: ") + std::strerror(e));
  }

  auto fp = std::make_unique<FfmpegPipe>();
  fp->proc = std::make_unique<FfmpegProcess>();
  fp->proc->pid = pid;
  fp->stdout_fd = out_pipe[0];
  // Position starts at start_sec × sr frames so the engine's position clock
  // reflects the seek (cliamp startPipe initializes pos the same way).
  const std::int64_t start_frames =
      start_sec > 0.0 ? static_cast<std::int64_t>(start_sec * sr) : 0;
  fp->state = std::make_shared<PipeStreamState>(start_frames);
  fp->f32 = (bit_depth == 32);

  // stderr drain thread: the only remaining holder of the write end is the
  // child's fd 2, so the drain ends at EOF exactly when the child exits (or
  // is killed+reaped). It is joined by ~FfmpegProcess.
  FfmpegProcess* proc = fp->proc.get();
  const int stderr_read_fd = err_pipe[0];
  proc->stderr_reader_ = std::jthread([proc, stderr_read_fd](std::stop_token) {
    std::array<std::byte, 4096> chunk{};
    for (;;) {
      const ssize_t r = read_eintr(stderr_read_fd, chunk.data(), chunk.size());
      if (r <= 0) {
        break;
      }
      proc->stderr_buf.write(
          std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(r)));
    }
    ::close(stderr_read_fd);
  });
  return fp;
}

}  // namespace bootamp::audio
