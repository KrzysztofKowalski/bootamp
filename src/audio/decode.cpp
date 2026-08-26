// audio/decode.cpp — local-file decode dispatch + native/ffmpeg decoders.
//
// Port of cliamp/player/decode.go + the local/pipe sides of cliamp/player/
// ffmpeg.go. decode_with_ext routes by extension: .wav→libsndfile, .flac→
// libFLAC, .ogg→libvorbis, else mp3→libsndfile; needs-ffmpeg formats spawn the
// local ffmpeg pipe. The same native decoders work over an abstract
// DecodeSource so URL reader chains (radio M6) can decode natively too
// (decode_with_ext_source). probe_frames uses ffprobe.
//
// Hot path: stream() never throws, never allocates per call (staging buffers
// reused), no locks. NaN/Inf preserved (no fast-math).

#include "audio/decode.hpp"

#include "audio/ffmpeg_pipe.hpp"
#include "audio/format.hpp"
#include "audio/icy.hpp"
#include "audio/streamer.hpp"
#include "audio/stream_seek_closer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>

#include <sndfile.h>
#include <FLAC/stream_decoder.h>
#include <vorbis/vorbisfile.h>

namespace bootamp::audio {

namespace {

constexpr std::chrono::milliseconds kProbeTimeout{10'000};

// ---- small helpers ----------------------------------------------------------

bool find_in_path(std::string_view name) {
  if (name.find('/') != std::string_view::npos) {
    return ::access(std::string(name).c_str(), X_OK) == 0;
  }
  const char* p = std::getenv("PATH");
  if (!p) return false;
  std::string_view rest(p);
  while (!rest.empty()) {
    auto colon = rest.find(':');
    std::string_view dir = rest.substr(0, colon);
    std::string cand = dir.empty() ? std::string(name) : std::string(dir) + "/" + std::string(name);
    if (::access(cand.c_str(), X_OK) == 0) return true;
    if (colon == std::string_view::npos) break;
    rest = rest.substr(colon + 1);
  }
  return false;
}

// file_ext returns the lowercase extension of the final path element
// (cliamp filepath.Ext + ToLower): "" when none, "." for "file.".
std::string file_ext(std::string_view path) {
  auto slash = path.find_last_of('/');
  std::string_view last = slash == std::string_view::npos ? path : path.substr(slash + 1);
  auto dot = last.find_last_of('.');
  if (dot == std::string_view::npos) return "";
  std::string e(last.substr(dot));
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim_ascii(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// query_param finds "key=value" in a URL query string (Go url.Query().Get).
// Minimal percent-decoding of %XX; first occurrence wins.
std::string query_param(std::string_view q, std::string_view key) {
  std::size_t pos = 0;
  while (pos <= q.size()) {
    auto amp = q.find('&', pos);
    std::string_view tok = q.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
    if (tok.size() > key.size() && tok.substr(0, key.size()) == key && tok[key.size()] == '=') {
      std::string_view v = tok.substr(key.size() + 1);
      std::string out;
      out.reserve(v.size());
      for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) {
          auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          int hi = hex(v[i + 1]), lo = hex(v[i + 2]);
          if (hi >= 0 && lo >= 0) {
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
          }
        }
        out.push_back(v[i]);
      }
      return out;
    }
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return "";
}

// run_capture spawns a command (argv[0] resolved via PATH), capturing stdout,
// and reaps it within `timeout`. exit_status == 0 means a clean exit (the raw
// waitpid status, WIFEXITED-tested by the caller); -1 on spawn failure or
// timeout (the child is SIGKILLed on timeout).
struct CapturedProc {
  int         exit_status = -1;
  std::string out;
};

CapturedProc run_capture(const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
  CapturedProc r;
  int pfd[2];
  if (::pipe2(pfd, O_CLOEXEC) != 0) return r;

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, pfd[0]);

  pid_t pid = -1;
  int err = ::posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(pfd[1]);
  if (err != 0) {
    ::close(pfd[0]);
    return r;
  }

  auto deadline = std::chrono::steady_clock::now() + timeout;
  char buf[4096];
  for (;;) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) break;
    struct pollfd pf{pfd[0], POLLIN, 0};
    int pr = ::poll(&pf, 1, static_cast<int>(remaining.count()));
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) break;  // timed out
    ssize_t n = ::read(pfd[0], buf, sizeof buf);
    if (n > 0) r.out.append(buf, static_cast<std::size_t>(n));
    else if (n < 0 && errno == EINTR) continue;
    else break;  // EOF
  }
  ::close(pfd[0]);

  // Reap the child (bounded by the deadline).
  int status = 0;
  for (;;) {
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      return r;  // exit_status stays -1 (timeout)
    }
    pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (w < 0 && errno != EINTR) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  r.exit_status = status;
  return r;
}

// ffmpeg_format mirrors cliamp's beep.Format for pipe decoders:
// {SampleRate: sr, NumChannels: 2, Precision: ffmpegPCMArgs(bitDepth).precision}.
AudioFormat ffmpeg_format(int sr, int bit_depth) {
  return AudioFormat{sr, 2, ffmpeg_pcm_args(bit_depth).precision, bit_depth};
}

// ---- libsndfile decoder (wav + mp3) -----------------------------------------

sf_count_t vio_get_filelen(void* ud) {
  auto* s = static_cast<DecodeSource*>(ud);
  auto len = s->length();
  return len < 0 ? -1 : static_cast<sf_count_t>(len);
}

sf_count_t vio_seek(sf_count_t offset, int whence, void* ud) {
  auto* s = static_cast<DecodeSource*>(ud);
  if (!s->seekable()) return -1;
  return s->seek(static_cast<std::int64_t>(offset), whence) ? s->tell() : -1;
}

sf_count_t vio_read(void* ptr, sf_count_t count, void* ud) {
  auto* s = static_cast<DecodeSource*>(ud);
  auto [n, ok] = s->read(std::span(static_cast<std::byte*>(ptr), static_cast<std::size_t>(count)));
  (void)ok;  // error and EOF both surface as short reads to libsndfile
  return static_cast<sf_count_t>(n);
}

sf_count_t vio_write(const void*, sf_count_t, void*) { return -1; }

sf_count_t vio_tell(void* ud) {
  auto* s = static_cast<DecodeSource*>(ud);
  auto t = s->tell();
  return t < 0 ? -1 : static_cast<sf_count_t>(t);
}

class SndfileDecoder final : public StreamSeekCloser {
public:
  static std::expected<std::unique_ptr<StreamSeekCloser>, std::string>
  open(std::unique_ptr<DecodeSource> src) {
    static SF_VIRTUAL_IO vio{vio_get_filelen, vio_seek, vio_read, vio_write, vio_tell};
    SF_INFO info{};
    info.format = 0;
    SNDFILE* f = ::sf_open_virtual(&vio, SFM_READ, &info, src.get());
    if (f == nullptr) {
      std::string msg = ::sf_strerror(nullptr);
      src->close();
      return std::unexpected(std::move(msg));
    }
    int channels = info.channels > 0 ? info.channels : 2;
    bool seekable = src->seekable();
    std::size_t frames = (seekable && info.frames > 0) ? static_cast<std::size_t>(info.frames) : 0;
    int sample_rate = info.samplerate > 0 ? info.samplerate : 44100;
    return std::unique_ptr<StreamSeekCloser>(
        new SndfileDecoder(f, std::move(src), channels, seekable, frames, sample_rate));
  }

  ~SndfileDecoder() override { close(); }

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override {
    if (f_ == nullptr) return {0, false};
    std::size_t want = dst.size();
    staging_.resize(want * static_cast<std::size_t>(channels_));
    sf_count_t got = ::sf_read_float(f_, staging_.data(), static_cast<sf_count_t>(staging_.size()));
    if (got < 0) {
      err_ = ::sf_strerror(f_);
      return {0, false};
    }
    std::size_t nframes = static_cast<std::size_t>(got) / static_cast<std::size_t>(channels_);
    if (channels_ == 1) {
      for (std::size_t i = 0; i < nframes; ++i) {
        float v = staging_[i];
        dst[i][0] = v;
        dst[i][1] = v;
      }
    } else {
      for (std::size_t i = 0; i < nframes; ++i) {
        dst[i][0] = staging_[static_cast<std::size_t>(channels_) * i];
        dst[i][1] = staging_[static_cast<std::size_t>(channels_) * i + 1];
      }
    }
    pos_frames_ += nframes;
    bool more = nframes == want;
    return {nframes, more};
  }

  std::string err() const override { return err_; }
  std::size_t len() const override { return frames_; }
  std::size_t position() const override {
    if (!seekable_ || f_ == nullptr) return pos_frames_;
    sf_count_t p = ::sf_seek(f_, 0, SEEK_CUR);
    return p < 0 ? pos_frames_ : static_cast<std::size_t>(p);
  }
  std::string seek(std::size_t frame) override {
    if (!seekable_ || f_ == nullptr) return {};  // no-op for non-seekable
    if (frames_ > 0 && frame > frames_) frame = frames_;
    if (::sf_seek(f_, static_cast<sf_count_t>(frame), SEEK_SET) < 0) {
      return std::string("sndfile seek: ") + ::sf_strerror(f_);
    }
    pos_frames_ = frame;
    return {};
  }
  void close() override {
    if (f_ != nullptr) {
      ::sf_close(f_);
      f_ = nullptr;
    }
    src_->close();
  }

  int sample_rate() const noexcept { return sample_rate_; }

private:
  SndfileDecoder(SNDFILE* f, std::unique_ptr<DecodeSource> src, int channels, bool seekable,
                 std::size_t frames, int sample_rate)
    : f_(f), src_(std::move(src)), channels_(channels), seekable_(seekable),
      frames_(frames), sample_rate_(sample_rate) {}

  SNDFILE*                       f_ = nullptr;
  std::unique_ptr<DecodeSource>  src_;
  int                            channels_   = 2;
  bool                           seekable_   = false;
  std::size_t                    frames_     = 0;
  int                            sample_rate_ = 44100;
  std::size_t                    pos_frames_ = 0;
  std::vector<float>             staging_;
  std::string                    err_;
};

// ---- libFLAC decoder --------------------------------------------------------

class FlacDecoder final : public StreamSeekCloser {
public:
  static std::expected<std::unique_ptr<StreamSeekCloser>, std::string>
  open(std::unique_ptr<DecodeSource> src) {
    auto self = std::unique_ptr<FlacDecoder>(new FlacDecoder(std::move(src)));
    self->dec_ = ::FLAC__stream_decoder_new();
    if (self->dec_ == nullptr) {
      self->src_->close();
      return std::unexpected("flac: out of memory");
    }
    FLAC__StreamDecoderInitStatus st = ::FLAC__stream_decoder_init_stream(
        self->dec_,
        &FlacDecoder::read_cb, &FlacDecoder::seek_cb, &FlacDecoder::tell_cb,
        &FlacDecoder::length_cb, &FlacDecoder::eof_cb,
        &FlacDecoder::write_cb, &FlacDecoder::metadata_cb, &FlacDecoder::error_cb,
        self.get());
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      std::string msg = FLAC__StreamDecoderInitStatusString[st];
      self->src_->close();
      ::FLAC__stream_decoder_delete(self->dec_);
      self->dec_ = nullptr;
      return std::unexpected("flac: " + msg);
    }
    if (!::FLAC__stream_decoder_process_until_end_of_metadata(self->dec_)) {
      std::string msg = self->err_.empty() ? "metadata read failed" : self->err_;
      self->src_->close();
      ::FLAC__stream_decoder_delete(self->dec_);
      self->dec_ = nullptr;
      return std::unexpected("flac: " + msg);
    }
    return std::unique_ptr<StreamSeekCloser>(std::move(self));
  }

  ~FlacDecoder() override { close(); }

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override {
    std::size_t want = dst.size();
    std::size_t written = 0;
    while (written < want) {
      if (out_pos_ >= out_.size()) {
        if (eof_ || dec_ == nullptr) break;
        out_.clear();
        out_pos_ = 0;
        if (!::FLAC__stream_decoder_process_single(dec_)) {
          eof_ = true;
          break;
        }
        FLAC__StreamDecoderState st = ::FLAC__stream_decoder_get_state(dec_);
        if (st == FLAC__STREAM_DECODER_END_OF_STREAM) eof_ = true;
        if (out_.empty() && eof_) break;
        continue;
      }
      std::size_t n = std::min(want - written, out_.size() - out_pos_);
      std::copy_n(out_.begin() + static_cast<std::ptrdiff_t>(out_pos_), n,
                  dst.begin() + static_cast<std::ptrdiff_t>(written));
      out_pos_ += n;
      written += n;
    }
    emitted_ += written;
    return {written, written == want};
  }

  std::string err() const override { return err_; }
  std::size_t len() const override { return total_samples_; }
  std::size_t position() const override { return emitted_; }
  std::string seek(std::size_t frame) override {
    if (!seekable_ || dec_ == nullptr) return {};  // no-op for non-seekable
    if (total_samples_ > 0 && frame > total_samples_) frame = total_samples_;
    if (!::FLAC__stream_decoder_seek_absolute(dec_, static_cast<FLAC__uint64>(frame))) {
      return err_.empty() ? std::string("flac seek failed") : err_;
    }
    emitted_ = frame;
    out_.clear();
    out_pos_ = 0;
    return {};
  }
  void close() override {
    if (dec_ != nullptr) {
      ::FLAC__stream_decoder_delete(dec_);
      dec_ = nullptr;
    }
    src_->close();
  }

  int sample_rate() const noexcept { return sample_rate_; }

private:
  explicit FlacDecoder(std::unique_ptr<DecodeSource> src) : src_(std::move(src)) {}

  // ---- libFLAC callbacks -----------------------------------------------------
  static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder*, FLAC__byte buffer[],
                                               std::size_t* bytes, void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    auto [n, ok] = self->src_->read(
        std::span(reinterpret_cast<std::byte*>(buffer), *bytes));
    *bytes = n;
    if (n == 0) return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    if (!ok) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }

  static FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder*, FLAC__uint64 offset,
                                               void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    if (!self->src_->seekable()) return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;
    return self->src_->seek(static_cast<std::int64_t>(offset), SEEK_SET)
             ? FLAC__STREAM_DECODER_SEEK_STATUS_OK
             : FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
  }

  static FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder*, FLAC__uint64* offset,
                                               void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    auto t = self->src_->tell();
    if (t < 0) return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;
    *offset = static_cast<FLAC__uint64>(t);
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }

  static FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder*, FLAC__uint64* len,
                                                   void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    auto l = self->src_->length();
    if (l < 0) return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
    *len = static_cast<FLAC__uint64>(l);
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
  }

  static FLAC__bool eof_cb(const FLAC__StreamDecoder*, void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    auto len = self->src_->length();
    if (len < 0) return false;  // live streams report EOF via read_cb
    auto pos = self->src_->tell();
    return pos >= 0 && static_cast<std::int64_t>(pos) >= len;
  }

  static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder*,
                                                 const FLAC__Frame* frame,
                                                 const FLAC__int32* const buffer[], void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    std::size_t n = frame->header.blocksize;
    std::size_t ch = frame->header.channels;
    if (ch == 0 || buffer == nullptr) {
      self->err_ = "flac: zero-channel frame";
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    constexpr float kScale = 1.0f / 32768.0f;
    std::size_t base = self->out_.size();
    self->out_.resize(base + n);
    if (ch == 1) {
      for (std::size_t i = 0; i < n; ++i) {
        float v = static_cast<float>(buffer[0][i]) * kScale;
        self->out_[base + i][0] = v;
        self->out_[base + i][1] = v;
      }
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        self->out_[base + i][0] = static_cast<float>(buffer[0][i]) * kScale;
        self->out_[base + i][1] = static_cast<float>(buffer[1][i]) * kScale;
      }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  static void metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* meta, void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
      self->sample_rate_ = static_cast<int>(meta->data.stream_info.sample_rate);
      self->total_samples_ = static_cast<std::size_t>(meta->data.stream_info.total_samples);
      self->seekable_ = self->src_->seekable();
    }
  }

  static void error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void* cd) {
    auto* self = static_cast<FlacDecoder*>(cd);
    if (self->err_.empty()) self->err_ = FLAC__StreamDecoderErrorStatusString[status];
  }

  FLAC__StreamDecoder*        dec_ = nullptr;
  std::unique_ptr<DecodeSource> src_;
  std::vector<Frame>          out_;
  std::size_t                 out_pos_       = 0;
  std::size_t                 emitted_       = 0;
  std::size_t                 total_samples_ = 0;
  int                         sample_rate_   = 44100;
  bool                        seekable_      = false;
  bool                        eof_           = false;
  std::string                 err_;
};

// ---- libvorbis (vorbisfile) decoder -----------------------------------------

struct VorbisClient {
  std::unique_ptr<DecodeSource> src;
  std::string                   err;
};

std::string vorbis_error_string(int e) {
  switch (e) {
    case OV_EREAD: return "read error";
    case OV_ENOTVORBIS: return "not vorbis data";
    case OV_EVERSION: return "vorbis version mismatch";
    case OV_EBADHEADER: return "invalid vorbis header";
    case OV_EFAULT: return "internal fault";
    case OV_EIMPL: return "feature not implemented";
    default: return "error " + std::to_string(e);
  }
}

std::size_t v_read(void* ptr, std::size_t size, std::size_t nmemb, void* ud) {
  auto* c = static_cast<VorbisClient*>(ud);
  auto [n, ok] = c->src->read(std::span(static_cast<std::byte*>(ptr), size * nmemb));
  (void)ok;
  return n / size;
}

int v_seek(void* ud, ogg_int64_t offset, int whence) {
  auto* c = static_cast<VorbisClient*>(ud);
  return c->src->seek(static_cast<std::int64_t>(offset), whence) ? 0 : -1;
}

int v_close(void* ud) {
  auto* c = static_cast<VorbisClient*>(ud);
  c->src->close();
  return 0;
}

long v_tell(void* ud) {
  auto* c = static_cast<VorbisClient*>(ud);
  auto t = c->src->tell();
  return t < 0 ? -1 : static_cast<long>(t);
}

class VorbisDecoder final : public StreamSeekCloser {
public:
  static std::expected<std::unique_ptr<StreamSeekCloser>, std::string>
  open(std::unique_ptr<DecodeSource> src) {
    auto self = std::unique_ptr<VorbisDecoder>(new VorbisDecoder(std::move(src)));
    ov_callbacks cb{v_read, v_seek, v_close, v_tell};
    int err = ::ov_open_callbacks(self->client_.get(), &self->vf_, nullptr, 0, cb);
    if (err != 0) {
      std::string msg = vorbis_error_string(err);
      self->client_->src->close();
      return std::unexpected("vorbis: " + msg);
    }
    self->opened_ = true;
    if (vorbis_info* vi = ::ov_info(&self->vf_, -1); vi != nullptr && vi->rate > 0) {
      self->sample_rate_ = vi->rate;
    }
    self->seekable_ = self->client_->src->seekable();
    return std::unique_ptr<StreamSeekCloser>(std::move(self));
  }

  ~VorbisDecoder() override { close(); }

  std::pair<std::size_t, bool> stream(std::span<Frame> dst) override {
    std::size_t want = dst.size();
    if (want == 0 || client_ == nullptr) return {0, false};
    float** chans = nullptr;
    int request = static_cast<int>(std::min<std::size_t>(want, 1u << 20));
    long n = ::ov_read_float(&vf_, &chans, request, &cur_bitstream_);
    if (n < 0) {
      client_->err = "vorbis: " + vorbis_error_string(static_cast<int>(n));
      return {0, false};
    }
    if (n == 0) return {0, false};  // EOF
    int channels = 2;
    if (vorbis_info* vi = ::ov_info(&vf_, cur_bitstream_); vi != nullptr && vi->channels > 0) {
      channels = vi->channels;
    }
    if (channels >= 2) {
      for (long i = 0; i < n; ++i) {
        dst[static_cast<std::size_t>(i)][0] = chans[0][i];
        dst[static_cast<std::size_t>(i)][1] = chans[1][i];
      }
    } else {
      for (long i = 0; i < n; ++i) {
        float v = chans[0][i];
        dst[static_cast<std::size_t>(i)][0] = v;
        dst[static_cast<std::size_t>(i)][1] = v;
      }
    }
    pos_frames_ += static_cast<std::size_t>(n);
    return {static_cast<std::size_t>(n), static_cast<std::size_t>(n) == want};
  }

  std::string err() const override { return client_->err; }
  std::size_t len() const override {
    if (!seekable_) return 0;
    ogg_int64_t total = ::ov_pcm_total(&vf_, -1);
    return total < 0 ? 0 : static_cast<std::size_t>(total);
  }
  std::size_t position() const override {
    if (!seekable_) return pos_frames_;
    ogg_int64_t p = ::ov_pcm_tell(&vf_);
    return p < 0 ? pos_frames_ : static_cast<std::size_t>(p);
  }
  std::string seek(std::size_t frame) override {
    if (!seekable_) return {};  // no-op for non-seekable
    std::size_t total = len();
    if (total > 0 && frame > total) frame = total;
    if (::ov_pcm_seek(&vf_, static_cast<ogg_int64_t>(frame)) != 0) {
      return "vorbis seek failed";
    }
    pos_frames_ = frame;
    return {};
  }
  void close() override {
    if (client_ == nullptr) return;
    if (opened_) {
      if (!vf_closed_) {
        ::ov_clear(&vf_);  // calls v_close (src->close())
        vf_closed_ = true;
      }
    } else {
      client_->src->close();  // open failed; never touched vf_
    }
  }

  int sample_rate() const noexcept { return sample_rate_; }

private:
  explicit VorbisDecoder(std::unique_ptr<DecodeSource> src)
    : client_(std::make_unique<VorbisClient>(VorbisClient{std::move(src), {}})) {}

  std::unique_ptr<VorbisClient> client_;
  // mutable: vorbisfile's query API takes non-const OggVorbis_File* even
  // though it only reads; the audio thread is the sole user.
  mutable OggVorbis_File        vf_{};
  int                           cur_bitstream_ = 0;
  std::size_t                   pos_frames_    = 0;
  int                           sample_rate_   = 44100;
  bool                          seekable_      = false;
  bool                          vf_closed_     = false;
  bool                          opened_        = false;
};

// ---- native dispatch --------------------------------------------------------

// decode_native consumes `rc` (closes it on failure; the decoder owns it on
// success). Port of cliamp decodeWithExt's native switch.
std::expected<DecodeResult, std::string>
decode_native(std::unique_ptr<DecodeSource> rc, std::string_view ext,
              int /*sample_rate*/, int /*bit_depth*/) {
  std::expected<std::unique_ptr<StreamSeekCloser>, std::string> dec;
  if (ext == ".wav") dec = SndfileDecoder::open(std::move(rc));
  else if (ext == ".flac") dec = FlacDecoder::open(std::move(rc));
  else if (ext == ".ogg") dec = VorbisDecoder::open(std::move(rc));
  else if (ext == ".mp3") dec = SndfileDecoder::open(std::move(rc));
  else {
    rc->close();
    return std::unexpected("unsupported file extension: " + std::string(ext));
  }

  if (!dec) return std::unexpected(std::move(dec).error());
  int sr = 44100;
  if (auto* d = dynamic_cast<SndfileDecoder*>(dec->get())) sr = d->sample_rate();
  else if (auto* d = dynamic_cast<FlacDecoder*>(dec->get())) sr = d->sample_rate();
  else if (auto* d = dynamic_cast<VorbisDecoder*>(dec->get())) sr = d->sample_rate();
  return DecodeResult{std::move(*dec), AudioFormat{sr, 2, 2, 16}};
}

}  // namespace

// ---- public helpers (decode.go ports) ----------------------------------------

const std::set<std::string>& supported_exts() {
  static const std::set<std::string> kExts = {
      ".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac", ".aacp",
      ".m4b", ".alac", ".wma", ".opus", ".webm",
  };
  return kExts;
}

bool is_url(std::string_view path) {
  return path.starts_with("http://") || path.starts_with("https://");
}

bool needs_ffmpeg(std::string_view ext) {
  return ext == ".m4a" || ext == ".aac" || ext == ".aacp" || ext == ".m4b" ||
         ext == ".alac" || ext == ".wma" || ext == ".opus" || ext == ".webm";
}

bool is_hls(std::string_view ext) { return ext == ".m3u8"; }

std::string ext_from_content_type(std::string_view ct) {
  // Strip parameters (e.g. "audio/aacp; charset=utf-8" → "audio/aacp").
  if (auto i = ct.find(';'); i != std::string_view::npos) ct = ct.substr(0, i);
  std::string c = lower_ascii(trim_ascii(ct));
  if (c == "audio/aac" || c == "audio/aacp" || c == "audio/x-aac") return ".aac";
  if (c == "audio/mpeg" || c == "audio/mp3") return ".mp3";
  if (c == "audio/ogg" || c == "application/ogg") return ".ogg";
  if (c == "audio/flac") return ".flac";
  if (c == "audio/wav" || c == "audio/x-wav") return ".wav";
  if (c == "audio/mp4" || c == "audio/x-m4a") return ".m4a";
  if (c == "audio/opus") return ".opus";
  return "";
}

std::string format_ext(std::string_view path) {
  if (!is_url(path)) return file_ext(path);
  // URL: parse the path component (ignoring query params), check a "format"
  // query param as fallback, and default to ".mp3" (cliamp formatExt).
  auto scheme = path.find("://");
  if (scheme == std::string_view::npos) return ".mp3";
  std::size_t host_end = path.find_first_of("/?#", scheme + 3);
  std::size_t path_pos = std::string_view::npos;
  std::string_view query_part;
  if (host_end != std::string_view::npos && path[host_end] == '/') {
    path_pos = host_end;
  }
  std::string_view path_part;
  if (path_pos != std::string_view::npos) {
    auto q = path.find_first_of("?#", path_pos + 1);
    path_part = path.substr(path_pos + 1,
                            q == std::string_view::npos ? std::string_view::npos : q - path_pos - 1);
    if (q != std::string_view::npos) {
      auto hash = path.find('#', q + 1);
      query_part = path.substr(q + 1,
                               hash == std::string_view::npos ? std::string_view::npos : hash - q - 1);
    }
  } else {
    auto q = path.find('?', scheme + 3);
    if (q != std::string_view::npos) {
      auto hash = path.find('#', q + 1);
      query_part = path.substr(q + 1,
                               hash == std::string_view::npos ? std::string_view::npos : hash - q - 1);
    }
  }
  std::string ext = file_ext(path_part);
  if (ext.empty() || ext == ".view") {
    std::string f = query_param(query_part, "format");
    if (!f.empty()) return "." + lower_ascii(f);
    return ".mp3";
  }
  return ext;
}

// ---- FdSource ---------------------------------------------------------------

FdSource::FdSource(int fd) : fd_(fd) {
  struct stat st {};
  seekable_ = fd_ >= 0 && ::fstat(fd_, &st) == 0 && S_ISREG(st.st_mode);
}

std::pair<std::size_t, bool> FdSource::read(std::span<std::byte> dst) {
  if (fd_ < 0) return {0, false};
  for (;;) {
    ssize_t n = ::read(fd_, dst.data(), dst.size());
    if (n >= 0) return {static_cast<std::size_t>(n), n > 0};
    if (errno == EINTR) continue;
    return {0, false};
  }
}

bool FdSource::seek(std::int64_t offset, int whence) {
  if (fd_ < 0) return false;
  return ::lseek(fd_, static_cast<off_t>(offset), whence) >= 0;
}

std::int64_t FdSource::tell() const noexcept {
  if (fd_ < 0) return -1;
  off_t p = ::lseek(fd_, 0, SEEK_CUR);
  return p < 0 ? -1 : static_cast<std::int64_t>(p);
}

std::int64_t FdSource::length() const noexcept {
  if (fd_ < 0) return -1;
  struct stat st {};
  if (::fstat(fd_, &st) != 0) return -1;
  if (!S_ISREG(st.st_mode)) return -1;
  return static_cast<std::int64_t>(st.st_size);
}

void FdSource::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

// ---- open_local / probe_frames ----------------------------------------------

std::expected<int, std::string> open_local(std::string_view path) {
  int fd = ::open(std::string(path).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::unexpected(std::string("open ") + std::string(path) + ": " + std::strerror(errno));
  }
  return fd;
}

std::size_t probe_frames(std::string_view path, int sr) {
  // ffprobe reads only the container header, so this is near-instant even for
  // huge files. Deviation from cliamp: a 10s cap (cliamp's Output() has no
  // timeout and can hang on FIFOs/sockets — we bound it to keep the engine
  // thread safe).
  CapturedProc cap = run_capture(
      {"ffprobe", "-v", "error", "-show_entries", "format=duration",
       "-of", "default=noprint_wrappers=1:nokey=1", std::string(path)},
      kProbeTimeout);
  if (cap.exit_status < 0 || !WIFEXITED(cap.exit_status) || WEXITSTATUS(cap.exit_status) != 0) {
    return 0;
  }
  std::string_view s = trim_ascii(cap.out);
  if (s.empty()) return 0;
  char* end = nullptr;
  errno = 0;
  double secs = std::strtod(std::string(s).c_str(), &end);
  if (end == s.data() || *end != '\0' || errno == ERANGE || !std::isfinite(secs) || secs < 0) {
    return 0;
  }
  return static_cast<std::size_t>(secs * static_cast<double>(sr));
}

// ---- pipe streamers ----------------------------------------------------------

FfmpegPipeStreamer::FfmpegPipeStreamer(std::unique_ptr<FfmpegPipe> pipe,
                                       std::unique_ptr<IcyByteSource> src, bool live,
                                       int stdin_write_fd)
  : pipe_(std::move(pipe)), src_(std::move(src)), live_(live),
    stdin_write_fd_(stdin_write_fd) {
  if (src_) pump_ = std::jthread([this](std::stop_token stoken) { pump_loop(stoken); });
}

FfmpegPipeStreamer::~FfmpegPipeStreamer() { close(); }

std::pair<std::size_t, bool> FfmpegPipeStreamer::stream(std::span<Frame> dst) {
  if (pipe_ == nullptr) return {0, false};
  return pipe_->stream(dst);
}

std::string FfmpegPipeStreamer::err() const {
  return pipe_ ? pipe_->err() : std::string{};
}

void FfmpegPipeStreamer::known_duration_hint(std::chrono::duration<double> d) {
  (void)d;
  // A known duration identifies a finite HTTP source even when the server also
  // sends ICY headers: its clean EOF must remain an ordinary track end
  // (cliamp setKnownDuration → *ffmpegPipeStreamer.live = false).
  if (pipe_) pipe_->live = false;
}

void FfmpegPipeStreamer::close() {
  if (pipe_ == nullptr || closed_) return;
  closed_ = true;
  // interrupt releases the blocked stdout read / kills ffmpeg; the pump's
  // blocked write then errors (EPIPE). src_->close() unblocks a pump blocked
  // in a read. Join the pump before reaping so no thread touches the fd.
  pipe_->interrupt();
  if (src_) src_->close();
  if (pump_.joinable()) {
    pump_.request_stop();
    pump_.join();
  }
  std::string werr = pipe_->stop();
  (void)werr;
  pipe_.reset();
  src_.reset();
}

void FfmpegPipeStreamer::pump_loop(std::stop_token stoken) {
  // Sole owner of the stdin write end (the read end of the caller's pipe was
  // dup2'd onto ffmpeg's fd 0 by start_ffmpeg_pipe; FfmpegPipe::stdin_fd holds
  // it until interrupt()). We close our end on chain EOF so ffmpeg sees stdin
  // EOF, and on stop/EPIPE so close() can join us without an fd-reuse race.
  if (stdin_write_fd_ < 0) return;
  std::vector<std::byte> buf(64 * 1024);
  while (!stoken.stop_requested()) {
    auto [n, ok] = src_->read(buf);
    if (n == 0 || !ok) break;  // chain EOF → close stdin so ffmpeg sees EOF
    std::size_t off = 0;
    while (off < n) {
      struct pollfd pf{stdin_write_fd_, POLLOUT, 0};
      int pr = ::poll(&pf, 1, 50);
      if (pr == 0) {  // poll timeout: re-check stop so close()'s join is bounded
        if (stoken.stop_requested()) goto done;
        continue;
      }
      if (pr < 0) {
        if (errno == EINTR) continue;
        goto done;
      }
      ssize_t w = ::write(stdin_write_fd_, buf.data() + static_cast<std::ptrdiff_t>(off),
                          n - off);
      if (w > 0) {
        off += static_cast<std::size_t>(w);
        continue;
      }
      if (w < 0 && errno == EINTR) continue;
      goto done;  // EPIPE (ffmpeg exited) or I/O error
    }
  }
done:
  if (stdin_write_fd_ >= 0) {
    ::close(stdin_write_fd_);
    stdin_write_fd_ = -1;
  }
}

// ---- LocalFfmpegStreamer -----------------------------------------------------

LocalFfmpegStreamer::LocalFfmpegStreamer(std::string_view path, int sr, int bit_depth)
  : path_(path), sr_(sr), bit_depth_(bit_depth), f32_(bit_depth == 32) {}

LocalFfmpegStreamer::~LocalFfmpegStreamer() { close(); }

std::string LocalFfmpegStreamer::start() {
  total_ = probe_frames(path_, sr_);
  auto p = spawn(0);
  if (!p) return p.error();
  pipe_ = std::move(*p);
  return {};
}

std::expected<std::unique_ptr<FfmpegPipe>, std::string>
LocalFfmpegStreamer::spawn(std::size_t seek_frames) {
  double secs = 0.0;
  if (seek_frames > 0) secs = static_cast<double>(seek_frames) / static_cast<double>(sr_);
  auto p = start_ffmpeg_pipe(path_, -1, sr_, bit_depth_, secs);
  if (!p) return std::unexpected(p.error());
  // Snap the running position to the exact frame offset (the -ss demuxer seek
  // is only approximate; cliamp initializes state to seekPos).
  (*p)->state->pos.store(static_cast<std::int64_t>(seek_frames));
  std::string wait = (*p)->wait_for_audio_bytes(pcm_frame_size((*p)->f32), kFfmpegPipeTimeout);
  if (!wait.empty()) {
    (*p)->stop();
    return std::unexpected(wait);
  }
  return p;
}

std::pair<std::size_t, bool> LocalFfmpegStreamer::stream(std::span<Frame> dst) {
  if (pipe_ == nullptr) return {0, false};
  auto [n, ok] = pipe_->stream(dst);
  if (!ok && total_ == 0) total_ = pipe_->position();  // cliamp localFFmpegStreamer.Stream
  return {n, ok};
}

std::string LocalFfmpegStreamer::seek(std::size_t frame) {
  // clampSeekPosition (cliamp): pos is unsigned here, so only the upper bound.
  if (total_ > 0 && frame > total_) frame = total_;
  auto repl = spawn(frame);
  if (!repl) return repl.error();
  pipe_->interrupt();
  auto old = std::move(pipe_);
  pipe_ = std::move(*repl);
  if (old) old->stop();
  return {};
}

void LocalFfmpegStreamer::close() {
  if (pipe_) {
    pipe_->stop();
    pipe_.reset();
  }
}

void LocalFfmpegStreamer::known_duration_hint(std::chrono::duration<double> d) {
  if (d.count() <= 0) return;
  if (total_ == 0) total_ = static_cast<std::size_t>(d.count() * static_cast<double>(sr_));
}

std::expected<std::unique_ptr<LocalFfmpegStreamer>, std::string>
decode_ffmpeg_local(std::string_view path, int sr, int bit_depth) {
  if (!find_in_path("ffmpeg")) {
    return std::unexpected("ffmpeg is required to play " + file_ext(path) +
                           " files — install it with your package manager");
  }
  auto s = std::make_unique<LocalFfmpegStreamer>(path, sr, bit_depth);
  std::string err = s->start();
  if (!err.empty()) return std::unexpected(std::move(err));
  return s;
}

std::expected<std::unique_ptr<FfmpegPipeStreamer>, std::string>
decode_ffmpeg_url_stream(std::string_view path, int sr, int bit_depth) {
  if (!find_in_path("ffmpeg")) {
    return std::unexpected("ffmpeg is required to play " + file_ext(path) +
                           " files — install it with your package manager");
  }
  auto p = start_ffmpeg_pipe(path, -1, sr, bit_depth);
  if (!p) return std::unexpected(p.error());
  std::string wait = (*p)->wait_for_audio_bytes(pcm_frame_size((*p)->f32), kFfmpegPipeTimeout);
  if (!wait.empty()) {
    (*p)->stop();
    return std::unexpected(std::move(wait));
  }
  // live stays false: ffmpeg opens the URL itself and manages its own
  // reconnection (cliamp decodeFFmpegStream comment).
  return std::unique_ptr<FfmpegPipeStreamer>(new FfmpegPipeStreamer(std::move(*p)));
}

std::expected<std::unique_ptr<FfmpegPipeStreamer>, std::string>
decode_ffmpeg_pipe_stream(std::unique_ptr<IcyByteSource> src, int sr, int bit_depth, bool live) {
  if (!find_in_path("ffmpeg")) {
    src->close();
    return std::unexpected("ffmpeg is required to play this stream — install it with your package manager");
  }
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    src->close();
    return std::unexpected(std::string("ffmpeg stdin pipe: ") + std::strerror(errno));
  }
  auto p = start_ffmpeg_pipe("pipe:0", fds[0], sr, bit_depth);
  ::close(fds[0]);  // parent's read-end copy: ffmpeg owns its dup, the pump owns the write end
  if (!p) {
    ::close(fds[1]);
    src->close();
    return std::unexpected(p.error());
  }
  std::string wait = (*p)->wait_for_audio_bytes(pcm_frame_size((*p)->f32), kFfmpegPipeTimeout);
  if (!wait.empty()) {
    (*p)->stop();
    ::close(fds[1]);
    src->close();
    return std::unexpected(std::move(wait));
  }
  return std::unique_ptr<FfmpegPipeStreamer>(
      new FfmpegPipeStreamer(std::move(*p), std::move(src), live, fds[1]));
}

// ---- decode_with_ext --------------------------------------------------------

std::expected<DecodeResult, std::string>
decode_with_ext(int fd, std::string_view ext, std::string_view path,
                int sample_rate, int bit_depth) {
  if (needs_ffmpeg(ext)) {
    // cliamp decodeWithExt: spawns the local ffmpeg pipe and does NOT consume
    // the reader/fd (callers close it themselves — parity).
    auto d = decode_ffmpeg_local(path, sample_rate, bit_depth);
    if (!d) return std::unexpected(std::move(d).error());
    return DecodeResult{std::move(*d), ffmpeg_format(sample_rate, bit_depth)};
  }
  return decode_native(std::make_unique<FdSource>(fd), ext, sample_rate, bit_depth);
}

std::expected<DecodeResult, std::string>
decode_with_ext_source(std::unique_ptr<DecodeSource> rc, std::string_view ext,
                       std::string_view path, int sample_rate, int bit_depth) {
  if (needs_ffmpeg(ext)) {
    rc->close();
    return std::unexpected("needs-ffmpeg format " + std::string(ext) +
                           " must be routed by the caller");
  }
  return decode_native(std::move(rc), ext, sample_rate, bit_depth);
}

}  // namespace bootamp::audio
