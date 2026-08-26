// audio/decode.hpp — local-file decode dispatch + format helpers.
//
// Port of cliamp/player/decode.go (the local/native side). decode_with_ext
// routes by extension: .wav→libsndfile, .flac→libFLAC, .ogg→libvorbis, else
// mp3 (libsndfile); needs-ffmpeg formats (.m4a/.aac/.opus/.webm) go to the
// ffmpeg pipe. open_source opens a local file or HTTP source (the HTTP path
// lives in http_socket.hpp for the radio backend; here we expose the local
// file open + content-type sniffing helpers shared with the radio pipeline).
#pragma once

#include "audio/ffmpeg_pipe.hpp"
#include "audio/format.hpp"
#include "audio/icy.hpp"
#include "audio/stream_seek_closer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace bootamp::audio {

// SupportedExts — the file extensions bootamp can decode (cliamp SupportedExts).
const std::set<std::string>& supported_exts();

// is_url reports whether path is http:// or https:// (cliamp isURL).
bool is_url(std::string_view path);

// needs_ffmpeg reports whether the extension requires ffmpeg to decode
// (.m4a/.aac/.aacp/.m4b/.alac/.wma/.opus/.webm).
bool needs_ffmpeg(std::string_view ext);

// is_hls reports whether the extension is an HLS playlist (.m3u8).
bool is_hls(std::string_view ext);

// ext_from_content_type maps an HTTP Content-Type to a file extension.
// Returns "" if unrecognized. Port of cliamp extFromContentType.
std::string ext_from_content_type(std::string_view ct);

// format_ext returns the audio format extension for a path. For URLs parses
// the path component (ignoring query), checks a "format" query param as
// fallback, defaults to ".mp3". Port of cliamp formatExt.
std::string format_ext(std::string_view path);

// decode_with_ext selects a decoder by explicit extension and returns a
// StreamSeekCloser + its format. For needs-ffmpeg extensions it spawns the
// local ffmpeg pipe (decode_ffmpeg_local); native decoders use libsndfile/
// libFLAC/libvorbis. The `fd` (an opened file) is consumed by the native path
// (the decoder closes it); on the needs-ffmpeg path the fd is NOT touched
// (cliamp decodeWithExt parity — callers close it themselves). `path` is used
// by the ffmpeg path. Port of cliamp decodeWithExt.
struct DecodeResult {
  std::unique_ptr<StreamSeekCloser> decoder;
  AudioFormat                       format;
};
std::expected<DecodeResult, std::string>
decode_with_ext(int fd, std::string_view ext, std::string_view path,
                 int sample_rate, int bit_depth);

// DecodeSource abstracts the byte source handed to native decoders: a seekable
// file fd (FdSource) or a network reader chain (the URL pipeline adapts its
// IcyByteSource chain). read() returns (count, ok); ok==false at EOF/error.
// close() is idempotent. The decoder that consumes a DecodeSource owns it.
class DecodeSource {
public:
  virtual ~DecodeSource() = default;
  virtual std::pair<std::size_t, bool> read(std::span<std::byte> dst) = 0;
  virtual bool  seekable() const noexcept { return false; }
  // Seek to `offset` with SEEK_SET/SEEK_CUR/SEEK_END; false when unsupported.
  virtual bool  seek(std::int64_t offset, int whence) { return false; }
  virtual std::int64_t tell() const noexcept { return -1; }
  virtual std::int64_t length() const noexcept { return -1; }  // total bytes, -1 unknown
  virtual void  close() = 0;
};

// FdSource adapts a posix fd. Seekable only for regular files (fstat check);
// sockets/FIFOs stream (seek/tell/length report unsupported).
class FdSource final : public DecodeSource {
public:
  explicit FdSource(int fd);
  ~FdSource() override { close(); }
  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override;
  bool seekable() const noexcept override { return seekable_; }
  bool seek(std::int64_t offset, int whence) override;
  std::int64_t tell() const noexcept override;
  std::int64_t length() const noexcept override;
  void close() override;
  int fd() const noexcept { return fd_; }
private:
  int        fd_        = -1;
  bool       seekable_  = false;
};

// decode_with_ext_source is the network-chain variant of decode_with_ext:
// native decode over an abstract source (URL reader chains). TAKES OWNERSHIP
// of `rc` — on success the decoder owns and closes it; on failure it is closed
// and destroyed. Needs-ffmpeg extensions are an error here (the caller routes
// those to decode_ffmpeg_pipe_stream / decode_ffmpeg_url_stream first).
std::expected<DecodeResult, std::string>
decode_with_ext_source(std::unique_ptr<DecodeSource> rc, std::string_view ext,
                       std::string_view path, int sample_rate, int bit_depth);

// PipeDecoder — implemented by streaming ffmpeg decoders so TrackPipeline can
// interrupt blocked reads and fill known durations without coupling to concrete
// classes (cliamp trackPipeline.interrupt / setKnownDuration type switches).
class PipeDecoder {
public:
  virtual ~PipeDecoder() = default;
  // interrupt unblocks a pipe decoder's read without waiting for its process
  // (close() reaps it later).
  virtual void interrupt() = 0;
  // known_duration_hint fills missing frame totals / clears live flags when a
  // finite duration is known from metadata (cliamp setKnownDuration).
  virtual void known_duration_hint(std::chrono::duration<double> d) = 0;
};

// FfmpegPipeStreamer — generic ffmpeg pipe streamer (cliamp ffmpegPipeStreamer).
// Covers by-URL opens (HLS, .ogg radio fallback, native-decode fallback) and
// stdin-fed opens (decode_ffmpeg_pipe_stream: HTTP reader chain → ffmpeg stdin).
// The M6 radio pipeline reuses this for radio (icy → stall_reader → ffmpeg).
// Not seekable; len() is pipe->total (0 = unknown); position() is state->pos.
class FfmpegPipeStreamer final : public StreamSeekCloser, public PipeDecoder {
public:
  // Takes ownership of `pipe` (a running ffmpeg). `src` (optional) is the
  // stdin-fed reader chain — a pump jthread drains it into `stdin_write_fd`
  // (the WRITE end of the caller's stdin pipe; the read end was passed to
  // start_ffmpeg_pipe and dup2'd onto ffmpeg's fd 0). On chain EOF the pump
  // closes the write end so ffmpeg sees EOF. -1 = no stdin pump (URL/local).
  explicit FfmpegPipeStreamer(std::unique_ptr<FfmpegPipe> pipe,
                              std::unique_ptr<IcyByteSource> src = nullptr,
                              bool live = false, int stdin_write_fd = -1);
  ~FfmpegPipeStreamer() override;

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override;
  std::string err() const override;
  std::size_t len() const override { return pipe_ ? pipe_->total : 0; }
  std::size_t position() const override { return pipe_ ? pipe_->position() : 0; }
  std::string seek(std::size_t) override { return {}; }  // non-seekable (cliamp ffmpegPipeStreamer)
  void close() override;

  void interrupt() override { if (pipe_) pipe_->interrupt(); }
  void known_duration_hint(std::chrono::duration<double> d) override;

  FfmpegPipe* pipe() const noexcept { return pipe_.get(); }

private:
  void pump_loop(std::stop_token stoken);
  std::unique_ptr<FfmpegPipe>    pipe_;
  std::unique_ptr<IcyByteSource> src_;
  std::jthread                   pump_;
  int                            stdin_write_fd_ = -1;
  bool                           live_ = false;
  bool                           closed_ = false;
};

// LocalFfmpegStreamer — local-file ffmpeg pipe decoder (cliamp
// localFFmpegStreamer). Total frames come from ffprobe (probe_frames); seeking
// kills ffmpeg and restarts it with an input-side -ss (demuxer fast seek).
class LocalFfmpegStreamer final : public StreamSeekCloser, public PipeDecoder {
public:
  LocalFfmpegStreamer(std::string_view path, int sr, int bit_depth);
  ~LocalFfmpegStreamer() override;

  // start spawns ffmpeg and waits for initial audio (15s). Returns "" or error.
  std::string start();

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override;
  std::string err() const override { return pipe_ ? pipe_->err() : std::string{}; }
  std::size_t len() const override { return total_; }
  std::size_t position() const override { return pipe_ ? pipe_->position() : 0; }
  std::string seek(std::size_t frame) override;
  void close() override;

  void interrupt() override { if (pipe_) pipe_->interrupt(); }
  void known_duration_hint(std::chrono::duration<double> d) override;

  std::size_t total() const noexcept { return total_; }
  FfmpegPipe* pipe() const noexcept { return pipe_.get(); }

private:
  // spawn starts ffmpeg (with input-side -ss when seek_frames > 0), waits for
  // initial audio, and snaps the pipe position to seek_frames.
  std::expected<std::unique_ptr<FfmpegPipe>, std::string> spawn(std::size_t seek_frames);
  std::string path_;
  int         sr_       = 44100;
  int         bit_depth_ = 16;
  bool        f32_      = false;
  std::size_t total_    = 0;
  std::unique_ptr<FfmpegPipe> pipe_;
};

// decode_ffmpeg_local starts ffmpeg as a streaming pipe for local files
// (instant start, -ss restart seek). Duration probed via ffprobe. Port of
// cliamp decodeFFmpegLocal.
std::expected<std::unique_ptr<LocalFfmpegStreamer>, std::string>
decode_ffmpeg_local(std::string_view path, int sr, int bit_depth);

// decode_ffmpeg_url_stream starts ffmpeg with the URL as -i (HLS, .ogg radio
// fallback, native-decode fallback) and waits for initial audio (15s). live
// stays false: ffmpeg manages its own reconnection. Port of cliamp
// decodeFFmpegURLStream.
std::expected<std::unique_ptr<FfmpegPipeStreamer>, std::string>
decode_ffmpeg_url_stream(std::string_view path, int sr, int bit_depth);

// decode_ffmpeg_pipe_stream starts ffmpeg with -i pipe:0, feeding it bytes from
// `src` (the URL reader chain) via a stdin pump jthread, and waits for initial
// audio (15s). `live` marks infinite radio (EOF ⇒ upstream died). Takes
// ownership of `src` (closed on failure). Port of cliamp decodeFFmpegPipeStream.
std::expected<std::unique_ptr<FfmpegPipeStreamer>, std::string>
decode_ffmpeg_pipe_stream(std::unique_ptr<IcyByteSource> src, int sr, int bit_depth,
                          bool live);

// open_local opens a local file and returns its fd (posix), or an error.
std::expected<int, std::string> open_local(std::string_view path);

// probe_frames uses ffprobe to read file duration from metadata and convert
// to sample frames at `sr`. Returns 0 on failure. Port of cliamp probeFrames.
std::size_t probe_frames(std::string_view path, int sr);

}  // namespace bootamp::audio