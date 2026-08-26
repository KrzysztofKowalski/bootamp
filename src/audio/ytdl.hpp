// audio/ytdl.hpp — yt-dlp | ffmpeg subprocess pipe for YT/YTM/SC/Bilibili/Bandcamp.
//
// Per the plan: drop kkdai/youtube; route every YT/YTM/SoundCloud/Bilibili/
// Bandcamp URL through yt-dlp. decode_ytdlp_pipe spawns two children connected
// by pipe(): yt-dlp -f bestaudio... -o - <url> → ffmpeg [-ss S] -i pipe:0 -f
// {s16le|f32le} ... pipe:1. Seek = ffmpeg `-ss` INPUT-side restart (NOT
// --download-sections — that comment in pipeline.go is stale; the real code at
// ytdl.go:336 passes -ss to ffmpeg). Pre-fill + cause surfacing: peek 1 byte /
// 30s timeout; on empty-EOF wait_cause(3s) preferring yt-dlp stderr error over
// ffmpeg's; dual waitpid reporter jthreads. Subprocess = posix_spawn + pipe().
#pragma once

#include "audio/format.hpp"
#include "audio/ffmpeg_pipe.hpp"
#include "audio/stream_seek_closer.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace bootamp::audio {

inline constexpr std::chrono::seconds kYtdlPipeTimeout{30};
inline constexpr std::chrono::seconds kYtdlCauseGrace {3};

// set_ytdl_cookies_from configures yt-dlp to use cookies from the given browser
// for YouTube Music playback (cliamp SetYTDLCookiesFrom). Empty = disable.
void set_ytdl_cookies_from(std::string_view browser);
std::string_view ytdl_cookies_from();

// ytdlp_available reports whether yt-dlp is on PATH.
bool ytdlp_available();

// YtdlpPipeStreamer streams PCM from a yt-dlp | ffmpeg pipe chain.
class YtdlpPipeStreamer final : public StreamSeekCloser {
public:
  // decode_ytdlp_pipe starts the pipe chain for `page_url`. If start_sec > 0,
  // ffmpeg -ss skips to that position (input-side seek-by-restart).
  static std::expected<std::unique_ptr<YtdlpPipeStreamer>, std::string>
  decode_ytdlp_pipe(std::string_view page_url, int sr, int bit_depth, int start_sec);

  ~YtdlpPipeStreamer() override;
  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override;
  std::string err() const override;
  std::size_t len() const override { return 0; }
  std::size_t position() const override;
  std::string seek(std::size_t) override { return {}; }  // seek-by-restart, handled by engine
  void close() override;

  // wait_cause reports why the audio pipe closed without producing audio,
  // preferring yt-dlp's reason (bot wall, 404, DRM) over ffmpeg's. With d<=0 it
  // polls; otherwise waits up to d. Returns "" if neither reported an error.
  std::string wait_cause(std::chrono::milliseconds d) const;

private:
  YtdlpPipeStreamer() = default;
  pid_t                 ytdl_pid_   = -1;
  pid_t                 ffmpeg_pid_ = -1;
  int                   stdout_fd_  = -1;
  std::shared_ptr<PipeStreamState> state_;
  bool                  f32_        = false;
  std::vector<std::byte> pcm_buf_;
  std::atomic<bool>      closed_{false};
  LimitedBuffer          ytdl_stderr_;
  LimitedBuffer          ffmpeg_stderr_;
  std::thread            ytdl_monitor_;
  std::thread            ffmpeg_monitor_;
  std::atomic<std::string*> ytdl_err_{nullptr};   // first reported cause
  std::atomic<std::string*> ffmpeg_err_{nullptr};
  std::atomic<bool>      ytdl_done_{false};
  std::atomic<bool>      ffmpeg_done_{false};
};

// probe_ytdlp_duration runs yt-dlp --skip-download --print duration (10s
// timeout) and returns the duration in seconds (0 on failure). Concurrent with
// pipeline build; collected ≤2s. Port of cliamp probeYTDLDuration.
std::chrono::duration<double>
probe_ytdlp_duration(std::string_view page_url);

}  // namespace bootamp::audio