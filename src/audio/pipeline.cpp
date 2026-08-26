// audio/pipeline.cpp — the buildPipeline branch tree + TrackPipeline lifecycle.
//
// Port of cliamp/player/pipeline.go. PipelineBuilder::build_pipeline replicates
// the branch tree: yt-dlp URL → YtdlpPipeStreamer (ytdl_seek), HLS .m3u8 URL →
// ffmpeg by-URL (+prefetch), open_source (local fd / URL reader chain), .ogg
// URL → ffmpeg fallback (the chained-ogg decoder is deferred — we take Go's
// buildChainedOggPipeline failure path directly), needs-ffmpeg → pipe
// (stdin-fed for URLs, -ss-restart for local files), native → decode_with_ext
// (+source variant), ffmpeg fallbacks on native failure, ResampleStreamer wrap
// when the source rate differs. Dropped per plan: custom URI factories, source
// resolvers, buffered (Navidrome/Subsonic) URLs, SSH.
//
// Hot path: stream() never throws, no locks on the audio thread. close() is
// the Go close ordering: livePrefetch.Close (signal) → decoder.Close (unblocks
// a fill read) → livePrefetch.Wait (join).

#include "audio/pipeline.hpp"

#include "audio/decode.hpp"
#include "audio/format.hpp"
#include "audio/ffmpeg_pipe.hpp"
#include "audio/http_socket.hpp"
#include "audio/icy.hpp"
#include "audio/live_prefetch.hpp"
#include "audio/stall_reader.hpp"
#include "audio/streamer.hpp"
#include "audio/stream_seek_closer.hpp"
#include "audio/ytdl.hpp"
#include "dsp/resampler.hpp"
#include "playlist/playlist.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace bootamp::audio {

namespace {

// pipe_format is the PCM format emitted by every ffmpeg pipe decoder
// (cliamp decodeFFmpegURLStream / decodeFFmpegPipeStream / decodeFFmpegLocal).
AudioFormat pipe_format(int sr, int bit_depth) {
  return AudioFormat{sr, 2, ffmpeg_pcm_args(bit_depth).precision, bit_depth};
}

// header_value finds a header by (lowercased) key, e.g. "icy-metaint".
std::string header_value(const std::vector<std::pair<std::string, std::string>>& headers,
                         std::string_view key) {
  for (const auto& [k, v] : headers) {
    if (k == key) return v;
  }
  return {};
}

// has_icy_header reports any header key starting with "icy-" — cliamp's live
// detection loop.
bool has_icy_header(const std::vector<std::pair<std::string, std::string>>& headers) {
  for (const auto& [k, v] : headers) {
    (void)v;
    if (k.starts_with("icy-")) return true;
  }
  return false;
}

// FdHolder owns the URL socket fd, shared between the reader chain and the
// stall-cancel callback, so cancel can close the socket without racing the
// chain's own close (a plain double-close could hit a reused fd number).
struct FdHolder {
  explicit FdHolder(int fd) : fd(fd) {}
  int                  fd;
  std::atomic<bool>    closed{false};
  void close() {
    if (!closed.exchange(true)) ::close(fd);
  }
};

// FdByteSource — a raw fd as an IcyByteSource (the URL socket body).
class FdByteSource final : public IcyByteSource {
public:
  explicit FdByteSource(std::shared_ptr<FdHolder> h) : h_(std::move(h)) {}
  ~FdByteSource() override { close(); }

  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override {
    if (h_->closed.load() || h_->fd < 0) return {0, false};
    for (;;) {
      ssize_t n = ::read(h_->fd, dst.data(), dst.size());
      if (n >= 0) return {static_cast<std::size_t>(n), n > 0};
      if (errno == EINTR) continue;
      return {0, false};
    }
  }
  void close() override { h_->close(); }

private:
  std::shared_ptr<FdHolder> h_;
};

// CountingByteSource — port of cliamp countingReader: atomically counts bytes
// read into the pipeline's network counter. `count` is a raw pointer into the
// counter owned by the TrackPipeline (cliamp holds *atomic.Int64 the same
// way); the chain is closed before the pipeline releases its counter, so the
// pointer never dangles while reads are possible.
class CountingByteSource final : public IcyByteSource {
public:
  CountingByteSource(std::unique_ptr<IcyByteSource> inner,
                     std::atomic<std::int64_t>* count)
    : inner_(std::move(inner)), count_(count) {}

  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override {
    auto [n, ok] = inner_->read(dst);
    if (count_) {
      count_->fetch_add(static_cast<std::int64_t>(n), std::memory_order_relaxed);
    }
    return {n, ok};
  }
  void close() override { inner_->close(); }

private:
  std::unique_ptr<IcyByteSource> inner_;
  std::atomic<std::int64_t>*     count_ = nullptr;
};

// DecodeSourceFromIcy adapts an IcyByteSource chain (counting → icy → stall →
// socket) to DecodeSource for the native decoders. Always non-seekable (HTTP).
class DecodeSourceFromIcy final : public DecodeSource {
public:
  explicit DecodeSourceFromIcy(std::unique_ptr<IcyByteSource> chain) : chain_(std::move(chain)) {}
  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override {
    return chain_->read(dst);
  }
  bool seekable() const noexcept override { return false; }
  bool seek(std::int64_t, int) override { return false; }
  std::int64_t tell() const noexcept override { return -1; }
  std::int64_t length() const noexcept override { return -1; }
  void close() override {
    if (chain_) {
      chain_->close();
      chain_.reset();
    }
  }

private:
  std::unique_ptr<IcyByteSource> chain_;
};

// sourceResult mirrors cliamp sourceResult. Local files carry an owned fd;
// URLs carry the reader chain. Exactly one is valid.
struct SourceResult {
  int                              fd = -1;  // local files (owned by caller)
  std::unique_ptr<IcyByteSource>   chain;    // URL reader chain (owned)
  std::string                      contentType;
  std::int64_t                     contentLength = -1;
  bool                             prefetch = false;
  bool                             live     = false;
};

// open_source — port of cliamp openSource: local fd, or the URL chain
// (socket → stall_reader → icy_reader). Consumes on_meta for URL chains.
std::expected<SourceResult, std::string> open_source(const std::string& path,
                                                     std::function<void(std::string)> on_meta) {
  if (!is_url(path)) {
    auto fd = open_local(path);
    if (!fd) return std::unexpected(fd.error());
    return SourceResult{*fd, nullptr, "", -1, false, false};
  }

  HttpClient http;
  auto resp = http.open_stream(path);
  if (!resp) return std::unexpected(resp.error());
  if (resp->status != 200) {
    std::string msg = "http status " + std::to_string(resp->status) + " " + resp->status_text;
    http.close_body(*resp);
    return std::unexpected(std::move(msg));
  }

  // Move the socket fd into the chain; from here the chain owns the connection.
  int sock = resp->body_fd;
  resp->body_fd = -1;
  auto holder = std::make_shared<FdHolder>(sock);
  std::unique_ptr<IcyByteSource> body = std::make_unique<FdByteSource>(holder);
  // Per-read stall timeout; on timeout the cancel closes the socket so the
  // blocked read returns promptly (cliamp streamStallTimeout rationale).
  auto cancel = [holder]() { holder->close(); };
  body = std::make_unique<StallReader>(std::move(body), std::move(cancel));

  std::string ct    = header_value(resp->headers, "content-type");
  std::int64_t cl   = resp->content_length;
  bool         live = has_icy_header(resp->headers);

  // Wrap in ICY reader when the server announces a metaint interval.
  if (on_meta) {
    std::string meta = header_value(resp->headers, "icy-metaint");
    if (!meta.empty()) {
      int meta_int = std::atoi(meta.c_str());
      if (meta_int > 0) {
        body = std::make_unique<IcyReader>(std::move(body), meta_int, std::move(on_meta));
      }
    }
  }

  return SourceResult{-1, std::move(body), std::move(ct), cl, live || cl < 0, live};
}

// ResampleStreamer — the beep.Resample equivalent for the pipeline: pulls
// source-rate frames, converts via dsp::Resampler, flushes at source EOF.
// (swr doesn't expose a quality knob; the quality arg is accepted for
// signature parity and ignored.)
class ResampleStreamer final : public Streamer {
public:
  ResampleStreamer(std::shared_ptr<Streamer> src, int in_rate, int out_rate)
    : src_(std::move(src)), ok_(resampler_.configure(in_rate, out_rate, 2)) {}

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override {
    if (!ok_ || !src_) {
      err_ = "resampler configure failed";
      return {0, false};
    }
    std::size_t written = 0;
    out_buf_.resize(dst.size() * 2);
    while (written < dst.size()) {
      if (in_used_ == 0) {
        auto [n, more] = src_->stream(staging_);
        if (n == 0) {
          // (0,false) EOF; a defensive (0,true) is treated as EOF too (guard
          // against a non-progressing source).
          eof_ = true;
          break;
        }
        if (!more) eof_ = true;  // drain this last chunk, then flush
        for (std::size_t i = 0; i < n; ++i) {
          in_buf_[2 * i]     = staging_[i][0];
          in_buf_[2 * i + 1] = staging_[i][1];
        }
        in_used_ = n * 2;
      }
      std::size_t produced = resampler_.process(
          std::span<const float>(in_buf_.data(), in_used_), out_buf_);
      in_used_ = 0;  // process() consumed the input given
      if (produced > 0) {
        for (std::size_t i = 0; i < produced; ++i) {
          dst[written + i][0] = out_buf_[2 * i];
          dst[written + i][1] = out_buf_[2 * i + 1];
        }
        written += produced;
      } else if (eof_) {
        break;  // input exhausted; flush below
      }
    }
    if (eof_ && written < dst.size()) {
      std::size_t flushed = resampler_.flush(out_buf_);
      for (std::size_t i = 0; i < flushed; ++i) {
        dst[written + i][0] = out_buf_[2 * i];
        dst[written + i][1] = out_buf_[2 * i + 1];
      }
      written += flushed;
    }
    return {written, !eof_ && written == dst.size()};
  }

  std::string err() const override { return err_; }

private:
  std::shared_ptr<Streamer> src_;
  dsp::Resampler            resampler_;
  bool                      ok_       = false;
  bool                      eof_      = false;
  std::vector<Frame>        staging_{std::size_t{4096}};
  std::vector<float>        in_buf_{std::size_t{4096} * 2};
  std::vector<float>        out_buf_;
  std::size_t               in_used_  = 0;
  std::string               err_;
};

}  // namespace

// ---- TrackPipeline lifecycle (pipeline.go ports) ------------------------------

void TrackPipeline::close() {
  if (live_prefetch) live_prefetch->close();  // signal the fill loop (no join)
  if (decoder) decoder->close();              // unblocks a fill read (pipe/socket)
  if (live_prefetch) live_prefetch->wait();   // join the fill thread
  owned_bytes_read.reset();                   // release the network counter
}

void TrackPipeline::interrupt() {
  if (auto* pd = dynamic_cast<PipeDecoder*>(decoder.get())) pd->interrupt();
}

void TrackPipeline::set_known_duration(std::chrono::duration<double> d) {
  known_duration = d;
  if (d.count() <= 0) return;
  if (auto* pd = dynamic_cast<PipeDecoder*>(decoder.get())) pd->known_duration_hint(d);
}

void close_pipelines(std::initializer_list<TrackPipeline*> ps) {
  for (TrackPipeline* tp : ps) {
    if (tp != nullptr) tp->close();
  }
}

// ---- PipelineBuilder ----------------------------------------------------------

std::unique_ptr<TrackPipeline>
PipelineBuilder::prefetch_network_pipeline(std::unique_ptr<TrackPipeline> tp, bool enabled) {
  if (!enabled) return tp;
  if (std::size_t n = tp->decoder->len(); n > 0) {
    tp->decoded_duration = duration_of(tp->format, n);
  }
  auto prefetch = std::make_shared<LivePrefetch>(tp->stream, sr_);
  tp->stream = prefetch;
  tp->live_prefetch = prefetch;
  return tp;
}

std::expected<std::unique_ptr<TrackPipeline>, std::string>
PipelineBuilder::build_ytdl_pipeline(const std::string& page_url, int start_sec) {
  // decode_ytdlp_pipe spawns yt-dlp | ffmpeg, pre-fills 1 byte (30s timeout),
  // and surfaces the cause via waitCause(3s) on empty-EOF — the Go
  // buildYTDLPipeline peek/waitCause block, folded into the factory.
  auto dec = YtdlpPipeStreamer::decode_ytdlp_pipe(page_url, sr_, bit_depth_, start_sec);
  if (!dec) return std::unexpected(dec.error());
  auto tp = std::make_unique<TrackPipeline>();
  tp->decoder = std::move(*dec);
  tp->stream = tp->decoder;
  tp->format = pipe_format(sr_, bit_depth_);
  tp->seekable = false;
  tp->path = page_url;
  tp->ytdl_seek = true;
  tp->stream_offset = std::chrono::seconds(start_sec);
  return tp;
}

std::expected<std::unique_ptr<TrackPipeline>, std::string>
PipelineBuilder::build_pipeline(const std::string& path, std::function<void(std::string)> on_meta) {
  // yt-dlp URLs (YT/YT Music/SoundCloud/Bilibili/Bandcamp): piped
  // yt-dlp | ffmpeg chain. cliamp routes these via PlayYTDL/buildYTDLPipeline;
  // bootamp folds the branch in so the engine has one entry point.
  if (playlist::is_ytdl(path)) {
    return build_ytdl_pipeline(path, 0);
  }

  std::string ext = format_ext(path);

  // HLS: ffmpeg must open the .m3u8 URL itself so it can resolve relative
  // chunklist/segment URIs — feeding playlist bytes via stdin would strip the
  // base URL and break relative resolution (cliamp comment at pipeline.go:233).
  if (is_url(path) && is_hls(ext)) {
    auto d = decode_ffmpeg_url_stream(path, sr_, bit_depth_);
    if (!d) return std::unexpected("open hls: " + d.error());
    auto tp = std::make_unique<TrackPipeline>();
    tp->decoder = std::move(*d);
    tp->stream = tp->decoder;
    tp->format = pipe_format(sr_, bit_depth_);
    tp->path = path;
    return prefetch_network_pipeline(std::move(tp), true);
  }

  // onMeta is passed for URLs (ICY StreamTitle callback); nil for local files.
  std::function<void(std::string)> url_meta;
  if (is_url(path)) url_meta = std::move(on_meta);
  auto src = open_source(path, std::move(url_meta));
  if (!src) return std::unexpected("open source: " + src.error());

  // Network byte counter for HTTP streams (countingReader port). Owned by the
  // pipeline (owned_bytes_read); the chain holds a raw pointer, like Go.
  std::unique_ptr<std::atomic<std::int64_t>> counter;
  if (is_url(path)) {
    counter = std::make_unique<std::atomic<std::int64_t>>(0);
    src->chain = std::make_unique<CountingByteSource>(std::move(src->chain), counter.get());
  }

  // Format: prefer the URL extension, fall back to Content-Type.
  if (is_url(path) && ext == ".mp3" && !src->contentType.empty()) {
    if (std::string ctExt = ext_from_content_type(src->contentType); !ctExt.empty()) {
      ext = ctExt;
    }
  }

  // OGG HTTP streams: cliamp first tries a chained-ogg decoder (Icecast radio
  // continuation across song boundaries); bootamp defers it (plan M2) and takes
  // Go's buildChainedOggPipeline failure fallback directly: close the chain and
  // let ffmpeg open the URL (handles OggFLAC/OggOpus too).
  if (is_url(path) && ext == ".ogg") {
    src->chain->close();
    src->chain.reset();
    auto d = decode_ffmpeg_url_stream(path, sr_, bit_depth_);
    if (!d) return std::unexpected("decode: " + d.error());
    auto tp = std::make_unique<TrackPipeline>();
    tp->decoder = std::move(*d);
    tp->stream = tp->decoder;
    tp->format = pipe_format(sr_, bit_depth_);
    tp->path = path;
    tp->live = src->live;
    return prefetch_network_pipeline(std::move(tp), src->prefetch);
  }

  // URL needs-ffmpeg (AAC/AAC+/Opus/...): stdin-fed ffmpeg pipe from the
  // existing reader chain so ICY StreamTitle parsing keeps working for
  // ffmpeg-only codecs.
  if (is_url(path) && needs_ffmpeg(ext)) {
    auto d = decode_ffmpeg_pipe_stream(std::move(src->chain), sr_, bit_depth_, src->live);
    if (!d) return std::unexpected("decode: " + d.error());
    auto tp = std::make_unique<TrackPipeline>();
    tp->decoder = std::move(*d);
    tp->stream = tp->decoder;
    tp->format = pipe_format(sr_, bit_depth_);
    tp->path = path;
    tp->bytes_read = counter.get();
    tp->owned_bytes_read = std::move(counter);
    tp->content_length = src->contentLength;
    tp->live = src->live;
    return prefetch_network_pipeline(std::move(tp), src->prefetch);
  }

  // Local needs-ffmpeg (.m4a/.opus/.webm/...): -ss-restart pipe for instant
  // start (cliamp decodeFFmpegLocal). The fd was opened by open_source but is
  // not consumed here — close it like Go's rc.Close().
  if (!is_url(path) && needs_ffmpeg(ext)) {
    ::close(src->fd);
    src->fd = -1;
    auto d = decode_ffmpeg_local(path, sr_, bit_depth_);
    if (!d) return std::unexpected("decode: " + d.error());
    auto tp = std::make_unique<TrackPipeline>();
    tp->decoder = std::move(*d);
    tp->stream = tp->decoder;
    tp->format = pipe_format(sr_, bit_depth_);
    tp->seekable = true;
    tp->path = path;
    return tp;
  }

  // Native decode (wav/flac/ogg/mp3). For URLs the reader chain feeds the
  // decoder via DecodeSourceFromIcy; for local files the fd is consumed (the
  // decoder closes it — decode_with_ext does not touch it on the needs-ffmpeg
  // path, which never reaches here).
  std::optional<DecodeResult> result;
  if (is_url(path)) {
    auto r = decode_with_ext_source(
        std::make_unique<DecodeSourceFromIcy>(std::move(src->chain)),
        ext, path, sr_, bit_depth_);
    if (!r) {
      // decode_with_ext_source closed the chain on failure (rc.Close()).
      auto d = decode_ffmpeg_url_stream(path, sr_, bit_depth_);
      if (!d) return std::unexpected("decode: " + d.error());
      auto tp = std::make_unique<TrackPipeline>();
      tp->decoder = std::move(*d);
      tp->stream = tp->decoder;
      tp->format = pipe_format(sr_, bit_depth_);
      tp->path = path;
      tp->live = src->live;
      return prefetch_network_pipeline(std::move(tp), src->prefetch);
    }
    result = std::move(*r);
  } else {
    auto r = decode_with_ext(src->fd, ext, path, sr_, bit_depth_);
    if (!r) {
      // Native local decode failed (e.g. IEEE-float WAV) — fall back to a
      // streaming ffmpeg process. The fd was closed on failure.
      src->fd = -1;
      auto d = decode_ffmpeg_local(path, sr_, bit_depth_);
      if (!d) return std::unexpected("decode: " + d.error());
      auto tp = std::make_unique<TrackPipeline>();
      tp->decoder = std::move(*d);
      tp->stream = tp->decoder;
      tp->format = pipe_format(sr_, bit_depth_);
      tp->seekable = true;
      tp->path = path;
      return tp;
    }
    result = std::move(*r);
  }

  // HTTP streams decoded natively read from a non-seekable socket body.
  bool seekable = !is_url(path);

  std::shared_ptr<StreamSeekCloser> dec = std::move(result->decoder);
  std::shared_ptr<Streamer> s = dec;
  if (result->format.sample_rate != sr_) {
    s = std::make_shared<ResampleStreamer>(dec, result->format.sample_rate, sr_);
  }

  auto tp = std::make_unique<TrackPipeline>();
  tp->decoder = std::move(dec);
  tp->stream = std::move(s);
  tp->format = result->format;
  tp->seekable = seekable;
  tp->path = path;
  tp->bytes_read = counter.get();
  tp->owned_bytes_read = std::move(counter);
  tp->content_length = src->contentLength;
  tp->live = src->live;

  return prefetch_network_pipeline(std::move(tp), src->prefetch);
}

}  // namespace bootamp::audio
