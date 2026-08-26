// audio/ffmpeg_pipe.hpp — shared ffmpeg-subprocess PCM machinery.
//
// Port of cliamp/player/ffmpeg.go. Reused by radio, YouTube, and local-file
// fallback. decode_pcm_frame (s16le→int16/32768, f32le→bit_cast<float>),
// read_full (loop on read/EINTR), PipeStreamState (atomic<int64_t> pos +
// first_error atomic slot), limited_buffer (64KB stderr cap, mutex),
// wait_for_audio_bytes (peek jthread + condvar deadline), ffmpeg_pcm_args.
// Subprocess = posix_spawn + pipe() (NO boost).
#pragma once

#include "audio/format.hpp"
#include "audio/streamer.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bootamp::audio {

// PCM frame byte sizes (cliamp pcmFrameSize16 / pcmFrameSize32).
inline constexpr std::size_t kPcmFrameSize16 = 4;   // 2 ch × 2 bytes (s16le)
inline constexpr std::size_t kPcmFrameSize32 = 8;   // 2 ch × 4 bytes (f32le)
inline constexpr std::size_t kSubprocessStderrLimit = 64 * 1024;
inline constexpr std::chrono::seconds kFfmpegPipeTimeout{15};

// pcm_frame_size returns the byte size of one stereo frame for the bit depth.
inline constexpr std::size_t pcm_frame_size(bool f32) {
  return f32 ? kPcmFrameSize32 : kPcmFrameSize16;
}

// decode_pcm_frame decodes one stereo frame from `buf` (>= pcm_frame_size bytes)
// into a Frame (s16le: int16/32768; f32le: bit_cast<float>). Port of cliamp
// decodePCMFrame. NaN/Inf preserved (no fast-math).
Frame decode_pcm_frame(std::span<const std::byte> buf, bool f32);

// ffmpeg_pcm_args returns the (format, codec, precision) triple for ffmpeg PCM
// output at the given bit depth. 32 ⇒ (f32le, pcm_f32le, 4); 16 ⇒ (s16le,
// pcm_s16le, 2). Port of cliamp ffmpegPCMArgs.
struct PcmArgs { const char* format; const char* codec; int precision; };
PcmArgs ffmpeg_pcm_args(int bit_depth);

// FirstError publishes one immutable error without locking the audio hot path.
// publish() stores `err` only if no error has been published yet (CAS on a
// shared_ptr<error_string>). load() returns the stored error or "".
class FirstError {
public:
  void publish(std::string_view err);
  std::string load() const;
private:
  std::atomic<std::shared_ptr<const std::string>> value_{nullptr};
};

// PipeStreamState tracks the running position (atomic) and the first error.
struct PipeStreamState {
  std::atomic<std::int64_t> pos{0};
  FirstError                err;
  explicit PipeStreamState(std::int64_t initial = 0) { pos.store(initial); }
};

// LimitedBuffer captures subprocess stderr without unbounded growth. Mutex-
// guarded; truncated flag appends "[stderr truncated]". 64KB cap.
class LimitedBuffer {
public:
  void write(std::span<const std::byte> p);
  std::string str() const;  // appends "\n[stderr truncated]" if truncated
private:
  mutable std::mutex mu_;
  std::string        buf_;
  bool               truncated_ = false;
};

// FfmpegProcess is the sole owner of waitpid for one ffmpeg command. wait()
// is called exactly once (the first caller runs waitpid and caches the result);
// kill() sends SIGKILL. The destructor reaps the child if it was never waited
// on, then joins the stderr drain thread (which ends at EOF once the child's
// fd 2 closes), so no zombie or leaked drain thread can outlive the process
// handle.
struct FfmpegProcess {
  pid_t          pid        = -1;
  LimitedBuffer  stderr_buf;
  std::string    wait();          // returns formatted "ffmpeg decode: <err>: <stderr>"
  void           kill();
  ~FfmpegProcess();
  bool             wait_called_  = false;  // protected by mu_
  std::mutex       mu_;
  std::string      wait_err_;              // cached result of wait() (guarded by mu_)
  std::jthread     stderr_reader_;         // drains child stderr into stderr_buf until EOF
};

// FfmpegPipe is the shared state for a pipe-based ffmpeg streamer. Each
// concrete streamer (radio/HLS/local-fallback) embeds it and adds its Seek.
struct FfmpegPipe {
  std::unique_ptr<FfmpegProcess>  proc;
  int                             stdout_fd = -1;
  int                             stdin_fd  = -1;
  // stdin contract: stdin_fd is the READ end of the caller's stdin pipe (the
  // end dup2'd onto the child's fd 0 when >= 0; -1 = /dev/null, no stdin
  // wiring). The caller pumps the WRITE end from their reader chain (the ICY/
  // stall chain stays in the data path) and must create the pipe with
  // pipe2(O_CLOEXEC). interrupt()/EOF closes stdin_fd, which aborts the
  // caller's pump (EPIPE) and signals EOF to ffmpeg.
  std::vector<std::byte>          pcm_buf;   // reusable decode block buffer
  std::shared_ptr<PipeStreamState> state;
  bool                            f32  = false;
  bool                            live = false;   // EOF ⇒ upstream died
  std::size_t                     total = 0;       // total frames (0=unknown)

  // Buffered stdout bytes not yet consumed (Go's bufio.Reader analog):
  // wait_for_audio_bytes peeks into rbuf_ without consuming; stream() drains
  // it first. Only touched by the waiting thread, never concurrently.
  std::vector<std::byte> rbuf_;
  std::size_t            rpos_ = 0;

  // stream reads PCM from stdout, decodes frames into dst. On EOF closes the
  // input and reaps ffmpeg; if live && no error, publishes unexpected-EOF.
  std::pair<std::size_t, bool> stream(std::span<Frame> dst);
  std::string err() const { return state ? state->err.load() : std::string{}; }
  std::size_t position() const { return state ? static_cast<std::size_t>(state->pos.load()) : 0; }

  // wait_for_audio_bytes blocks until `n` bytes are available on stdout or
  // `timeout` elapses, using a peek jthread + condvar deadline. Returns "" on
  // success, an error message on timeout/EOF. Used before handing the pipe to
  // the sink so an idle live stream can't park the audio thread.
  std::string wait_for_audio_bytes(std::size_t n, std::chrono::milliseconds timeout);

  // interrupt releases blocked reads without waiting for ffmpeg; stop()
  // interrupts then reaps. close() = stop().
  void interrupt();
  std::string stop();
  void close() { stop(); }

  int bit_depth() const { return f32 ? 32 : 16; }
};

// start_ffmpeg_pipe launches ffmpeg via posix_spawn, feeding it `input_arg` as
// -i (a URL/path, or "pipe:0" when stdin_fd is wired), writing raw PCM to
// stdout at `sr`. `start_sec` > 0 inserts "-ss <start_sec>" BEFORE -i (input-
// side demuxer fast seek, cliamp localFFmpegStreamer.startPipe / decodeYTDLPipe);
// the returned PipeStreamState::pos is initialized to start_sec × sr frames.
// Returns the populated FfmpegPipe (stdout_fd open) or an error message. The
// caller owns the process lifetime.
std::expected<std::unique_ptr<FfmpegPipe>, std::string>
start_ffmpeg_pipe(std::string_view input_arg, int stdin_fd, int sr, int bit_depth,
                  double start_sec = 0.0);

}  // namespace bootamp::audio