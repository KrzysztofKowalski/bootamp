// tests/audio/test_icy.cpp — Catch2 tests for the ICY metadata reader
// (cliamp player/icy.go -> audio/icy.cpp).
//
// Ports TestParseStreamTitle (all seven table cases, including the missing
// trailing semicolon and embedded-semicolon titles) and
// TestIcyReaderStripsMetadataAndReportsTitles: a golden canned SHOUTcast byte
// stream (metaint=8: audio block, title block, audio block, empty metadata
// block, audio block, title block, partial final audio block) read through
// 3-byte chunked reads to exercise the metaint-boundary clamping and the
// io.ReadFull-style metadata assembly. Plus an empty-StreamTitle block case
// (title parsed but on_meta not invoked).
#include <catch2/catch_test_macros.hpp>

#include "audio/icy.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace bootamp::audio;

namespace {

// MemSource: an in-memory IcyByteSource with a per-read byte cap (chunked
// reader counterpart of Go's chunkedReader).
class MemSource final : public IcyByteSource {
public:
  MemSource(std::string data, std::size_t chunk = 0)
      : data_(std::move(data)), chunk_(chunk) {}

  std::pair<std::size_t, bool> read(std::span<std::byte> dst) override {
    if (pos_ >= data_.size()) return {0, false};  // EOF
    std::size_t want = dst.size();
    if (chunk_ != 0) want = std::min(want, chunk_);
    want = std::min(want, data_.size() - pos_);
    std::memcpy(dst.data(), data_.data() + pos_, want);
    pos_ += want;
    return {want, true};
  }

  void close() override {}

private:
  std::string data_;
  std::size_t chunk_;
  std::size_t pos_ = 0;
};

// icy_block encodes one metadata block: a 1-byte length prefix (size/16)
// followed by the null-padded metadata (SHOUTcast/Icecast wire format).
std::string icy_block(const std::string& meta) {
  if (meta.empty()) return std::string(1, '\0');
  std::size_t n = (meta.size() + 15) / 16;
  std::string out(1 + n * 16, '\0');
  out[0] = static_cast<char>(n);
  std::memcpy(out.data() + 1, meta.data(), meta.size());
  return out;
}

// drain reads an IcyByteSource until EOF, appending the audio to `out`.
void drain(IcyByteSource& r, std::string& out) {
  std::array<std::byte, 64> buf{};
  for (;;) {
    auto [n, ok] = r.read(std::span(buf));
    if (!ok) return;
    out.append(reinterpret_cast<const char*>(buf.data()), n);
  }
}

}  // namespace

TEST_CASE("parse_stream_title extracts StreamTitle values", "[audio][icy]") {
  struct Case {
    const char* meta;
    const char* want;
  };
  const Case cases[] = {
      // "artist and title"
      {"StreamTitle='Daft Punk - Aerodynamic';StreamUrl='';", "Daft Punk - Aerodynamic"},
      // "title only"
      {"StreamTitle='Some Show';", "Some Show"},
      // "empty title"
      {"StreamTitle='';StreamUrl='';", ""},
      // "no stream title key"
      {"StreamUrl='https://example.com';", ""},
      // "empty block"
      {"", ""},
      // "missing trailing semicolon"
      {"StreamTitle='No Semicolon'", "No Semicolon"},
      // "title containing semicolon"
      {"StreamTitle='A; B - C';StreamUrl='';", "A; B - C"},
  };
  for (const auto& c : cases) {
    INFO("meta=" << c.meta);
    CHECK(parse_stream_title(c.meta) == c.want);
  }
}

TEST_CASE("icy_reader strips metadata and reports titles", "[audio][icy]") {
  const int meta_int = 8;
  std::string raw;
  raw += "AAAAAAAA";                                   // audio block 1
  raw += icy_block("StreamTitle='Song One';");         // title block 1
  raw += "BBBBBBBB";                                   // audio block 2
  raw += icy_block("");                                // empty metadata block (no change)
  raw += "CCCCCCCC";                                   // audio block 3
  raw += icy_block("StreamTitle='Song Two';");         // title block 2
  raw += "DDDDDDDD";                                   // audio block 4 (partial, no trailing meta)

  std::vector<std::string> titles;
  auto src = std::make_unique<MemSource>(raw, 3);  // chunked at 3 bytes
  IcyReader r(std::move(src), meta_int, [&titles](std::string title) {
    titles.push_back(std::move(title));
  });

  std::string got;
  drain(r, got);

  CHECK(got == "AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD");
  REQUIRE(titles.size() == 2);
  CHECK(titles[0] == "Song One");
  CHECK(titles[1] == "Song Two");
}

TEST_CASE("icy_reader does not report empty StreamTitle blocks", "[audio][icy]") {
  const int meta_int = 4;
  std::string raw;
  raw += "AAAA";
  raw += icy_block("StreamTitle='';StreamUrl='';");  // empty title: parse ok, no callback
  raw += "BBBB";

  std::vector<std::string> titles;
  auto src = std::make_unique<MemSource>(raw, 3);
  IcyReader r(std::move(src), meta_int, [&titles](std::string title) {
    titles.push_back(std::move(title));
  });

  std::string got;
  drain(r, got);

  CHECK(got == "AAAABBBB");
  CHECK(titles.empty());
}
