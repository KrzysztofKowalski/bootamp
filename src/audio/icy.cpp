// audio/icy.cpp — ICY (SHOUTcast/Icecast) metadata reader.
//
// Port of cliamp/player/icy.go. The server sends `metaint` bytes of audio,
// then a 1-byte length prefix (x16 = metadata size), then the metadata block,
// then repeats. IcyReader strips the metadata so decoders only see audio, and
// parses StreamTitle='...' from each block, invoking on_meta. The audio thread
// uses this; it takes no locks (the source and on_meta are owned by the chain).
#include "audio/icy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::audio {
namespace {

// read_full reads until dst is full or the source reports EOF/error — the
// io.ReadFull loop from Go. A (0, true) return from the source is treated as
// EOF (Go's io.ReadFull would spin forever on it; the contract says ok=false
// on EOF/error).
std::pair<std::size_t, bool> read_full(IcyByteSource& src, std::span<std::byte> dst) {
  std::size_t got = 0;
  while (got < dst.size()) {
    auto [n, ok] = src.read(dst.subspan(got));
    got += n;
    if (!ok || n == 0) return {got, false};
  }
  return {got, true};
}

}  // namespace

std::string parse_stream_title(std::string_view meta) {
  constexpr std::string_view kPrefix = "StreamTitle='";
  auto pos = meta.find(kPrefix);  // strings.Cut
  if (pos == std::string_view::npos) return {};
  std::string_view after = meta.substr(pos + kPrefix.size());

  // strings.Index(after, "';") — first closing quote of the title value.
  auto j = after.find("';");
  if (j == std::string_view::npos) {
    // Tolerate missing trailing semicolon: fall back to the last quote.
    j = after.rfind('\'');  // strings.LastIndex(after, "'")
    if (j == std::string_view::npos) return {};
  }
  return std::string(after.substr(0, j));
}

IcyReader::IcyReader(std::unique_ptr<IcyByteSource> src, int meta_int, OnMeta on_meta)
    : src_(std::move(src)),
      meta_int_(meta_int),
      remaining_(meta_int),  // newIcyReader: remaining = metaInt
      on_meta_(std::move(on_meta)) {}

IcyReader::~IcyReader() = default;

std::pair<std::size_t, bool> IcyReader::read(std::span<std::byte> dst) {
  if (remaining_ == 0) {
    // Read and discard the metadata block.
    if (auto result = consume_meta(); !result.second) return {0, false};
    remaining_ = meta_int_;
  }

  // Clamp the read so we never cross into a metadata block (Go: p[:min(...)]).
  const auto want = std::min(dst.size(), static_cast<std::size_t>(remaining_));
  auto [n, ok] = src_->read(dst.first(want));
  remaining_ -= static_cast<int>(n);
  return {n, ok};
}

void IcyReader::close() { src_->close(); }

std::pair<std::size_t, bool> IcyReader::consume_meta() {
  // Length prefix: 1 byte, multiply by 16 for the actual metadata size.
  std::byte len_buf[1];
  if (!read_full(*src_, std::span(len_buf)).second) return {0, false};
  const int meta_len = static_cast<int>(std::to_integer<unsigned char>(len_buf[0])) * 16;
  if (meta_len == 0) return {0, true};  // empty block: no metadata to parse

  std::vector<std::byte> buf(static_cast<std::size_t>(meta_len));
  if (!read_full(*src_, buf).second) return {0, false};

  // Metadata is null-padded; trim before parsing (Go strings.TrimRight).
  std::string_view meta(reinterpret_cast<const char*>(buf.data()), buf.size());
  while (!meta.empty() && meta.back() == '\0') meta.remove_suffix(1);

  if (std::string title = parse_stream_title(meta); !title.empty() && on_meta_) {
    on_meta_(std::move(title));
  }
  return {0, true};
}

}  // namespace bootamp::audio
