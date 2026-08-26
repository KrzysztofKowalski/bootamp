// tests/resolve/resolve_test.cpp — port of cliamp/resolve/resolve_test.go.
//
// Only the network-free cases are ported: Args classification, the public
// is_hls_playlist URL predicate, and the ExpandYTPlaylist / ytdl cookies
// globals. Go's isHLSPlaylist (body-content check) is internal to resolve.cpp
// and is exercised through resolve_m3u, which needs HTTP — not testable here
// without a server (fixtures as strings, no network rule).
#include "resolve/resolve.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using bootamp::resolve::args;
using bootamp::resolve::expand_yt_playlist;
using bootamp::resolve::is_hls_playlist;
using bootamp::resolve::set_expand_yt_playlist;
using bootamp::resolve::set_ytdl_cookies_from;
using bootamp::resolve::ytdl_cookies_from;

}  // namespace

TEST_CASE("args treats Xiaoyuzhou episode URL as pending", "[resolve][args]") {
  const std::string url =
      "https://www.xiaoyuzhoufm.com/episode/69a13b07a22480add648dd03?s=eyJ1IjogIjYxODEzNmZiZTBmNWU3MjNiYjk2MmE5MiJ9";
  auto got = args({url});
  REQUIRE(got.has_value());
  CHECK(got->tracks.empty());
  REQUIRE(got->pending.size() == 1);
  CHECK(got->pending[0] == url);
}

TEST_CASE("args treats .m3u URL as pending without sniffing", "[resolve][args]") {
  auto got = args({"http://x.com/list.m3u"});
  REQUIRE(got.has_value());
  CHECK(got->tracks.empty());
  REQUIRE(got->pending.size() == 1);
  CHECK(got->pending[0] == "http://x.com/list.m3u");
}

TEST_CASE("args treats plain audio URL as an immediate track", "[resolve][args]") {
  // Known audio extension ⇒ sniffFeedURL skips the network round-trip and
  // returns false, so the URL becomes an immediate stream track.
  auto got = args({"http://x.com/song.mp3"});
  REQUIRE(got.has_value());
  CHECK(got->pending.empty());
  REQUIRE(got->tracks.size() == 1);
  CHECK(got->tracks[0].stream);
  CHECK(got->tracks[0].path == "http://x.com/song.mp3");
}

TEST_CASE("is_hls_playlist detects .m3u8 URLs", "[resolve][hls]") {
  CHECK(is_hls_playlist("http://x.com/playlist.m3u8"));
  CHECK_FALSE(is_hls_playlist("http://x.com/playlist.m3u"));
  CHECK(is_hls_playlist("http://x.com/PLAYLIST.M3U8"));  // case-insensitive
  CHECK(is_hls_playlist("http://x.com/playlist.m3u8?token=abc"));  // query stripped
}

TEST_CASE("expand_yt_playlist defaults to true", "[resolve][args]") {
  CHECK(expand_yt_playlist());
  set_expand_yt_playlist(false);
  CHECK_FALSE(expand_yt_playlist());
  set_expand_yt_playlist(true);
  CHECK(expand_yt_playlist());
}

TEST_CASE("ytdl_cookies_from round-trips through the setter", "[resolve][args]") {
  set_ytdl_cookies_from("firefox");
  CHECK(ytdl_cookies_from() == "firefox");
  set_ytdl_cookies_from("chrome");
  CHECK(ytdl_cookies_from() == "chrome");
  set_ytdl_cookies_from("");
  CHECK(ytdl_cookies_from() == "");
}
