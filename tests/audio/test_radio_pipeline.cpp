// tests/audio/test_radio_pipeline.cpp — Catch2 tests for the M6 radio
// pipeline (audio/radio_pipeline.{hpp,cpp}).
//
// Ported Go tests:
//   * player/decode_test.go TestOpenSourceClassifiesHTTPResponse — the
//     live/prefetch classification rules, pinned pure (the Go test exercises
//     them through a live httptest server; here has_icy_header +
//     stream_prefetch cover the same three cases without sockets);
//   * daemon_stream_title_test.go TestDaemonStreamTitleFields — the five
//     apply_stream_title contract cases, plus the strings.Cut split details.
//
// Pure logic only (no sockets, no subprocesses): header classification,
// icy-metaint parsing, content-type extension mapping/override, stream title
// split. The socket→ffmpeg chain itself is exercised by the engine tests.
#include <catch2/catch_test_macros.hpp>

#include "audio/decode.hpp"
#include "audio/radio_pipeline.hpp"
#include "playlist/playlist.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bootamp::audio;
using namespace bootamp::playlist;

namespace {

// headers helper: build a lowercased-key header vector (HttpResponse contract).
std::vector<std::pair<std::string, std::string>>
headers(std::initializer_list<std::pair<std::string, std::string>> hs) {
  return {hs.begin(), hs.end()};
}

}  // namespace

// ---------------------------------------------------------------------------
// Header classification — cliamp openSource (decode.go:236-249). The Go test
// TestOpenSourceClassifiesHTTPResponse covers three response shapes:
//   chunked radio    → live=true,  prefetch=true
//   finite chunked   → live=false, prefetch=true  (Content-Length -1)
//   content length   → live=false, prefetch=false (Content-Length known)
TEST_CASE("RadioPipeline: has_icy_header detects any icy-* response header",
          "[audio][radio_pipeline]") {
  SECTION("no icy headers") {
    REQUIRE_FALSE(detail::has_icy_header(
        headers({{"content-type", "audio/mpeg"}, {"content-length", "12345"}})));
    REQUIRE_FALSE(detail::has_icy_header(headers({})));
  }
  SECTION("any icy-* key counts, value irrelevant (Go header-name loop)") {
    REQUIRE(detail::has_icy_header(headers({{"icy-name", "Test Radio"}})));
    REQUIRE(detail::has_icy_header(headers({{"icy-metaint", "16000"}})));
    REQUIRE(detail::has_icy_header(headers(
        {{"content-type", "audio/aacp"}, {"icy-br", "128"}, {"server", "Icecast"}})));
  }
  SECTION("case: keys are lowercased by the client; mixed-case input still matches") {
    REQUIRE(detail::has_icy_header(headers({{"Icy-Name", "Test Radio"}})));
  }
}

TEST_CASE("RadioPipeline: prefetch = live || content_length < 0 (Go cases)",
          "[audio][radio_pipeline]") {
  // "chunked radio": icy header ⇒ live ⇒ prefetch regardless of length.
  REQUIRE(detail::stream_prefetch(true, -1));
  REQUIRE(detail::stream_prefetch(true, 12345));
  // "finite chunked file": no Content-Length ⇒ prefetch, not live.
  REQUIRE(detail::stream_prefetch(false, -1));
  // "content length": known length, no icy ⇒ neither.
  REQUIRE_FALSE(detail::stream_prefetch(false, 12345));
}

TEST_CASE("RadioPipeline: parse_icy_metaint is a strict positive-requiring int",
          "[audio][radio_pipeline]") {
  SECTION("valid intervals") {
    REQUIRE(detail::parse_icy_metaint("16000") == 16000);
    REQUIRE(detail::parse_icy_metaint("8192") == 8192);
  }
  SECTION("absent / invalid / trailing garbage (strconv.Atoi parity)") {
    REQUIRE(detail::parse_icy_metaint("") == 0);
    REQUIRE(detail::parse_icy_metaint("abc") == 0);
    REQUIRE(detail::parse_icy_metaint("16000x") == 0);
    REQUIRE(detail::parse_icy_metaint(" 16000") == 0);  // leading space not accepted
  }
  SECTION("zero and negative are parsed but rejected by callers (metaInt > 0)") {
    REQUIRE(detail::parse_icy_metaint("0") == 0);
    REQUIRE(detail::parse_icy_metaint("-5") == -5);
  }
}

// ---------------------------------------------------------------------------
// Content-Type extension mapping — cliamp extFromContentType (decode.go:254-277).
TEST_CASE("RadioPipeline: ext_from_content_type mapping table", "[audio][radio_pipeline]") {
  SECTION("audio/aac family → .aac") {
    REQUIRE(ext_from_content_type("audio/aac") == ".aac");
    REQUIRE(ext_from_content_type("audio/aacp") == ".aac");
    REQUIRE(ext_from_content_type("audio/x-aac") == ".aac");
  }
  SECTION("mp3 family → .mp3") {
    REQUIRE(ext_from_content_type("audio/mpeg") == ".mp3");
    REQUIRE(ext_from_content_type("audio/mp3") == ".mp3");
  }
  SECTION("ogg family → .ogg") {
    REQUIRE(ext_from_content_type("audio/ogg") == ".ogg");
    REQUIRE(ext_from_content_type("application/ogg") == ".ogg");
  }
  SECTION("other recognized types") {
    REQUIRE(ext_from_content_type("audio/flac") == ".flac");
    REQUIRE(ext_from_content_type("audio/wav") == ".wav");
    REQUIRE(ext_from_content_type("audio/x-wav") == ".wav");
    REQUIRE(ext_from_content_type("audio/mp4") == ".m4a");
    REQUIRE(ext_from_content_type("audio/x-m4a") == ".m4a");
    REQUIRE(ext_from_content_type("audio/opus") == ".opus");
  }
  SECTION("parameters stripped, case-insensitive") {
    REQUIRE(ext_from_content_type("audio/aacp; charset=utf-8") == ".aac");
    REQUIRE(ext_from_content_type("Audio/OGG; charset=utf-8") == ".ogg");
  }
  SECTION("unrecognized → empty") {
    REQUIRE(ext_from_content_type("") == "");
    REQUIRE(ext_from_content_type("application/octet-stream") == "");
    REQUIRE(ext_from_content_type("text/html") == "");
  }
}

TEST_CASE("RadioPipeline: content-type override only for .mp3 URL extension",
          "[audio][radio_pipeline]") {
  SECTION("cliamp pipeline.go:263-268") {
    REQUIRE(detail::ext_override_from_content_type(".mp3", "audio/aacp") == ".aac");
    REQUIRE(detail::ext_override_from_content_type(".mp3", "audio/ogg") == ".ogg");
    // Same-type override is a no-op value-wise.
    REQUIRE(detail::ext_override_from_content_type(".mp3", "audio/mpeg") == ".mp3");
  }
  SECTION("no override when the type is unrecognized or empty") {
    REQUIRE(detail::ext_override_from_content_type(".mp3", "") == "");
    REQUIRE(detail::ext_override_from_content_type(".mp3", "application/octet-stream") == "");
  }
  SECTION("only the .mp3 extension is overridden") {
    REQUIRE(detail::ext_override_from_content_type(".aac", "audio/ogg") == "");
    REQUIRE(detail::ext_override_from_content_type(".ogg", "audio/aacp") == "");
  }
}

// ---------------------------------------------------------------------------
// apply_stream_title — daemon.go:1099-1114. Port of the five cases in
// daemon_stream_title_test.go TestDaemonStreamTitleFields (info starts as
// {Title: track.Title, Artist: track.Artist}).
TEST_CASE("RadioPipeline: apply_stream_title folds ICY metadata (daemon contract)",
          "[audio][radio_pipeline]") {
  SECTION("artist and title split on separator") {
    const Track cur{/*path*/ "", /*title*/ "NCS Trap Stream", /*artist*/ "",
                              /*album*/ "", /*genre*/ "", 0, 0,
                              /*stream*/ true};
    const StreamTitleInfo info = apply_stream_title(cur, "Tycho - Awake");
    REQUIRE(info.title == "Awake");
    REQUIRE(info.artist == "Tycho");
    REQUIRE(info.station == "NCS Trap Stream");
    REQUIRE(info.stream_title == "Tycho - Awake");
  }
  SECTION("empty title after the separator keeps the station") {
    const Track cur{/*path*/ "", /*title*/ "Lofi Stream", /*artist*/ "",
                              /*album*/ "", /*genre*/ "", 0, 0,
                              /*stream*/ true};
    const StreamTitleInfo info = apply_stream_title(cur, "Tycho - ");
    REQUIRE(info.title == "Lofi Stream");
    REQUIRE(info.artist == "");
    REQUIRE(info.station == "");
    REQUIRE(info.stream_title == "Tycho - ");
  }
  SECTION("title-only metadata becomes the title") {
    const Track cur{/*path*/ "", /*title*/ "Lofi Stream", /*artist*/ "",
                              /*album*/ "", /*genre*/ "", 0, 0,
                              /*stream*/ true};
    const StreamTitleInfo info = apply_stream_title(cur, "Morning Session");
    REQUIRE(info.title == "Morning Session");
    REQUIRE(info.artist == "");
    REQUIRE(info.station == "Lofi Stream");
    REQUIRE(info.stream_title == "Morning Session");
  }
  SECTION("no metadata leaves the entry untouched") {
    const Track cur{/*path*/ "", /*title*/ "Lofi Stream", /*artist*/ "",
                              /*album*/ "", /*genre*/ "", 0, 0,
                              /*stream*/ true};
    const StreamTitleInfo info = apply_stream_title(cur, "");
    REQUIRE(info.title == "Lofi Stream");
    REQUIRE(info.artist == "");
    REQUIRE(info.station == "");
    REQUIRE(info.stream_title == "");
  }
  SECTION("non-stream track is never rewritten") {
    const Track cur{/*path*/ "", /*title*/ "Alien Boy", /*artist*/ "Oliver Tree",
                              /*album*/ "", /*genre*/ "", 0, 0,
                              /*stream*/ false};
    const StreamTitleInfo info = apply_stream_title(cur, "Tycho - Awake");
    REQUIRE(info.title == "Alien Boy");
    REQUIRE(info.artist == "Oliver Tree");
    REQUIRE(info.station == "");
    REQUIRE(info.stream_title == "");
  }
}

TEST_CASE("RadioPipeline: apply_stream_title split details (strings.Cut)",
          "[audio][radio_pipeline]") {
  const Track cur{/*path*/ "", /*title*/ "Station", /*artist*/ "",
                            /*album*/ "", /*genre*/ "", 0, 0,
                            /*stream*/ true};
  SECTION("first separator wins; the rest stays in the title") {
    const StreamTitleInfo info = apply_stream_title(cur, "A - B - C");
    REQUIRE(info.artist == "A");
    REQUIRE(info.title == "B - C");
  }
  SECTION("an empty split title leaves artist untouched and no station") {
    const StreamTitleInfo info = apply_stream_title(cur, "DJ - ");
    REQUIRE(info.artist == "");
    REQUIRE(info.title == "Station");
    REQUIRE(info.station == "");
  }
  SECTION("same title as the entry ⇒ no station") {
    const StreamTitleInfo info = apply_stream_title(cur, "Station");
    REQUIRE(info.title == "Station");
    REQUIRE(info.station == "");
  }
  SECTION("separator without surrounding spaces is not a split") {
    const StreamTitleInfo info = apply_stream_title(cur, "A-B");
    REQUIRE(info.artist == "");
    REQUIRE(info.title == "A-B");
    REQUIRE(info.station == "Station");
  }
}
