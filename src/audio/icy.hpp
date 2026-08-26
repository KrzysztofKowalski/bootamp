// audio/icy.hpp — ICY (SHOUTcast/Icecast) metadata reader.
//
// Port of cliamp/player/icy.go. The server sends `metaint` bytes of audio,
// then a 1-byte length prefix (×16 = metadata size), then the metadata block,
// then repeats. icy_reader strips the metadata so decoders only see audio, and
// parses StreamTitle='...' from each block, invoking on_meta. The title is
// published to the engine's atomic<shared_ptr<const string>>.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace bootamp::audio {

// OnMeta is invoked with each parsed StreamTitle. The engine implementation
// publishes it to atomic<shared_ptr<const std::string>>.
using OnMeta = std::function<void(std::string title)>;

// parse_stream_title extracts the StreamTitle value from an ICY metadata
// block. Format: "StreamTitle='Artist - Title';StreamUrl='...';...".
// Tolerates a missing trailing semicolon (cliamp parseStreamTitle).
std::string parse_stream_title(std::string_view meta);

// IcyReader wraps a byte source (an existing ReadCloser-like) and strips
// interleaved ICY metadata. The audio thread reads bytes via read(); on each
// metadata block it parses StreamTitle and invokes on_meta.
//
// The underlying byte source is abstracted as a reader with read()/close()
// methods (raw-socket HTTP client or a stall_reader wrapper). We model it as
// a small abstract class to keep this header independent of http_socket.hpp.
class IcyByteSource {
public:
  virtual ~IcyByteSource() = default;
  // read up to dst.size() bytes; return count + ok (false on EOF/error).
  virtual std::pair<std::size_t, bool> read(std::span<std::byte> dst) = 0;
  virtual void close() = 0;
};

class IcyReader final : public IcyByteSource {
public:
  // meta_int is the audio-byte interval between metadata blocks.
  IcyReader(std::unique_ptr<IcyByteSource> src, int meta_int, OnMeta on_meta);
  ~IcyReader() override;

  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override;
  void close() override;

private:
  std::unique_ptr<IcyByteSource> src_;
  int          meta_int_;
  int          remaining_;
  OnMeta       on_meta_;

  std::pair<std::size_t, bool> consume_meta();
};

}  // namespace bootamp::audio