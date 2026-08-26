// audio/radio_pipeline.cpp — M6 live-radio pipeline.
//
// Port of the radio parts of cliamp player/pipeline.go (HLS branch 237-248,
// .ogg fallback + needs-ffmpeg stdin-pipe branch 273-320) + openSource
// (player/decode.go:190-250) + applyStreamTitle (daemon.go:1099-1114).
//
// Chain (URL path): HttpClient (raw socket, HTTP/1.1 + ICY rewrite) →
// stall_reader (10s per-read) → [icy_reader when the server announces
// icy-metaint] → countingReader → ffmpeg stdin pipe (s16le|f32le, ffmpeg.go
// args) → live_prefetch wrap. HLS (.m3u8) and .ogg radio open ffmpeg by URL
// (-i <url>). Classification: live = any icy-* header; prefetch =
// live || content_length < 0; content-type extension override.
//
// Audio thread: stream() is LivePrefetch → never blocks on network I/O; the
// chain is drained by the decoder's stdin pump jthread. No locks on the hot
// path; the byte counter is atomic.

#include "audio/radio_pipeline.hpp"

#include "audio/decode.hpp"
#include "audio/format.hpp"
#include "audio/http_socket.hpp"
#include "audio/icy.hpp"
#include "audio/live_prefetch.hpp"
#include "audio/stall_reader.hpp"
#include "audio/streamer.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace bootamp::audio {

// ---- detail classification helpers (declared in radio_pipeline.hpp) ---------

namespace detail {

bool has_icy_header(const std::vector<std::pair<std::string, std::string>>& headers) {
  for (const auto& [k, v] : headers) {
    (void)v;
    if (k.starts_with("icy-")) return true;
  }
  return false;
}

int parse_icy_metaint(std::string_view v) {
  if (v.empty()) return 0;
  int value = 0;
  const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), value);
  // Strict like strconv.Atoi: garbage or trailing characters ⇒ invalid.
  if (ec != std::errc{} || p != v.data() + v.size()) return 0;
  return value;
}

bool stream_prefetch(bool live, std::int64_t content_length) {
  return live || content_length < 0;
}

std::string ext_override_from_content_type(std::string_view ext, std::string_view ct) {
  if (ext == ".mp3" && !ct.empty()) return ext_from_content_type(ct);
  return {};
}

}  // namespace detail

namespace {

// pipe_format is the PCM format every ffmpeg pipe decoder emits (cliamp
// startFFmpegPipe): stereo at the target rate, precision from the bit depth
// (2 = s16le, 4 = f32le).
AudioFormat pipe_format(int sr, int bit_depth) {
  return AudioFormat{sr, 2, ffmpeg_pcm_args(bit_depth).precision, bit_depth};
}

// header_value finds a header by (lowercased) key; HttpResponse keys are
// lowercased (http_socket.hpp).
std::string header_value(const std::vector<std::pair<std::string, std::string>>& headers,
                         std::string_view key) {
  for (const auto& [k, v] : headers) {
    if (k == key) return v;
  }
  return {};
}

// FdHolder owns the URL socket fd, shared between the reader chain and the
// stall-cancel callback, so cancel can close the socket without racing the
// chain's own close (a plain double-close could hit a reused fd number).
struct FdHolder {
  explicit FdHolder(int fd) : fd(fd) {}
  int               fd;
  std::atomic<bool> closed{false};
  void close() {
    if (!closed.exchange(true)) ::close(fd);
  }
};

// FdByteSource — the raw socket body as an IcyByteSource.
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
// emitted by the chain into the pipeline's network counter. `count` is a raw
// pointer into the counter owned by the TrackPipeline (cliamp holds
// *atomic.Int64 the same way); the chain is closed before the pipeline
// releases its counter, so the pointer never dangles while reads are possible.
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

// RadioSource mirrors cliamp sourceResult (decode.go:142-149): the URL reader
// chain plus the HTTP classification.
struct RadioSource {
  std::unique_ptr<IcyByteSource> chain;  // counting → icy → stall → socket
  std::string                    contentType;
  std::int64_t                   contentLength = -1;
  bool                           prefetch      = false;
  bool                           live          = false;
};

// open_radio_url — cliamp openSource URL path (decode.go:198-249): GET via the
// raw-socket client, stall-guard the body (10s), attach the ICY reader when
// the server announces a metaint, classify live/prefetch. The returned chain
// owns the socket fd.
std::expected<RadioSource, std::string>
open_radio_url(const std::string& url, std::function<void(std::string)> on_meta) {
  HttpClient http;
  auto resp = http.open_stream(url);
  if (!resp) return std::unexpected("http get: " + resp.error());
  if (resp->status != 200) {
    std::string msg = "http status " + std::to_string(resp->status);
    if (!resp->status_text.empty()) msg += " " + resp->status_text;
    http.close_body(*resp);
    return std::unexpected(std::move(msg));
  }

  // Move the socket fd into the chain; from here the chain owns the
  // connection (cliamp: the stallReader's cancel owns the request context).
  int sock = resp->body_fd;
  resp->body_fd = -1;
  auto holder = std::make_shared<FdHolder>(sock);
  std::unique_ptr<IcyByteSource> body = std::make_unique<FdByteSource>(holder);
  // Per-read stall timeout; on timeout the cancel closes the socket so the
  // blocked read returns promptly (cliamp streamStallTimeout rationale).
  auto cancel = [holder]() { holder->close(); };
  body = std::make_unique<StallReader>(std::move(body), std::move(cancel));

  std::string ct  = header_value(resp->headers, "content-type");
  std::int64_t cl = resp->content_length;
  bool         live = detail::has_icy_header(resp->headers);

  // Wrap in ICY reader when the server announces a metaint interval and an
  // on_meta callback is registered (cliamp decode.go:230-234).
  if (on_meta) {
    const int meta_int = detail::parse_icy_metaint(header_value(resp->headers, "icy-metaint"));
    if (meta_int > 0) {
      body = std::make_unique<IcyReader>(std::move(body), meta_int, std::move(on_meta));
    }
  }

  return RadioSource{std::move(body), std::move(ct), cl,
                     detail::stream_prefetch(live, cl), live};
}

}  // namespace

// ---- apply_stream_title (daemon.go:1099-1114) --------------------------------

StreamTitleInfo apply_stream_title(const playlist::Track& cur,
                                   std::string_view stream_title) {
  StreamTitleInfo info{cur.title, cur.artist, "", ""};
  // Non-stream tracks and empty titles are never rewritten; StreamTitle stays
  // "" (cliamp: the early return precedes info.StreamTitle = streamTitle).
  if (!cur.stream || stream_title.empty()) return info;
  info.stream_title = std::string(stream_title);

  // strings.Cut(streamTitle, " - "): first separator; a non-empty title part
  // wins (an empty part keeps the entry's own fields — cliamp).
  if (const std::size_t pos = stream_title.find(" - ");
      pos != std::string_view::npos) {
    const std::string_view title = stream_title.substr(pos + 3);
    if (!title.empty()) {
      info.artist = std::string(stream_title.substr(0, pos));
      info.title  = std::string(title);
    }
  } else {
    info.title = std::string(stream_title);
  }

  // The station label is the entry's own title when the ICY title differs.
  if (info.title != cur.title) info.station = cur.title;
  return info;
}

// ---- open_source (pipeline.go 237-248, 273-320 + decode.go openSource) ------

std::expected<std::unique_ptr<TrackPipeline>, std::string>
open_source(const std::string& url, int sr, int bit_depth,
            std::function<void(std::string)> on_meta) {
  if (!is_url(url)) {
    return std::unexpected("open source: not an HTTP(S) URL: " + url);
  }

  // HLS playlists must be opened by ffmpeg directly from the URL so it can
  // resolve relative chunklist/segment URIs and follow the live segment
  // window; feeding playlist bytes via stdin would strip the base URL
  // (cliamp pipeline.go:233-248). Always prefetch-wrapped.
  std::string ext = format_ext(url);
  if (is_hls(ext)) {
    auto d = decode_ffmpeg_url_stream(url, sr, bit_depth);
    if (!d) return std::unexpected("open hls: " + d.error());
    auto tp = std::make_unique<TrackPipeline>();
    tp->decoder = std::move(*d);
    tp->stream = tp->decoder;
    tp->format = pipe_format(sr, bit_depth);
    tp->path = url;
    return PipelineBuilder(sr, bit_depth).prefetch_network_pipeline(std::move(tp), true);
  }

  auto src = open_radio_url(url, std::move(on_meta));
  if (!src) return std::unexpected("open source: " + src.error());

  // Network byte counter for HTTP streams (cliamp countingReader, pipeline.go
  // 256-261). Owned by the pipeline; the chain holds a raw pointer, like Go.
  std::unique_ptr<std::atomic<std::int64_t>> counter =
      std::make_unique<std::atomic<std::int64_t>>(0);
  src->chain = std::make_unique<CountingByteSource>(std::move(src->chain), counter.get());

  // Format: prefer the URL extension, fall back to Content-Type (cliamp
  // pipeline.go:263-268: .mp3 URL extension only).
  if (std::string ctExt = detail::ext_override_from_content_type(ext, src->contentType);
      !ctExt.empty()) {
    ext = ctExt;
  }

  // OGG radio: cliamp first tries the chained-ogg decoder (Icecast radio
  // continuation across song boundaries); bootamp defers it (plan M2) and
  // takes Go's buildChainedOggPipeline failure fallback directly (pipeline.go
  // 276-288): close the chain and let ffmpeg open the URL (handles
  // OggFLAC/OggOpus too).
  if (ext == ".ogg") {
    src->chain->close();
    src->chain.reset();
    auto d = decode_ffmpeg_url_stream(url, sr, bit_depth);
    if (!d) return std::unexpected("decode: " + d.error());
    auto tp = std::make_unique<TrackPipeline>();
    tp->decoder = std::move(*d);
    tp->stream = tp->decoder;
    tp->format = pipe_format(sr, bit_depth);
    tp->path = url;
    tp->live = src->live;
    return PipelineBuilder(sr, bit_depth).prefetch_network_pipeline(std::move(tp), src->prefetch);
  }

  // Radio streams that need ffmpeg (AAC, AAC+, Opus, ...) and everything
  // else non-native here: stdin-fed ffmpeg pipe from the existing reader
  // chain so the ICY metadata reader stays attached and live StreamTitle
  // parsing works for ffmpeg-only codecs (cliamp pipeline.go:296-320).
  // The chain drains via the decoder's stdin pump jthread; decode_ffmpeg_
  // pipe_stream closes `src` on failure and waits for initial audio (15s).
  auto d = decode_ffmpeg_pipe_stream(std::move(src->chain), sr, bit_depth, src->live);
  if (!d) return std::unexpected("decode: " + d.error());
  auto tp = std::make_unique<TrackPipeline>();
  tp->decoder = std::move(*d);
  tp->stream = tp->decoder;
  tp->format = pipe_format(sr, bit_depth);
  tp->path = url;
  tp->bytes_read = counter.get();
  tp->owned_bytes_read = std::move(counter);
  tp->content_length = src->contentLength;
  tp->live = src->live;
  return PipelineBuilder(sr, bit_depth).prefetch_network_pipeline(std::move(tp), src->prefetch);
}

}  // namespace bootamp::audio
