// tests/audio/test_http_socket.cpp — Catch2 tests for the raw-socket HTTP
// client. Canned byte strings only (no network): the ICY "ICY " → "HTTP/1.0 "
// status-line rewrite, status/header parsing (lowercased keys, content-length,
// live = any icy-* header), bounded body reads incl. chunked decoding, and
// URL/location resolution. Ports the assertions of
// cliamp/internal/httpclient/client_test.go TestStreamingClientAcceptsICYStatus
// (body "hello" through the rewrite) plus the parse paths of decode.go /
// resolve.go.
#include <catch2/catch_test_macros.hpp>

#include "audio/http_socket.hpp"
#include "audio/http_socket_internal.hpp"

#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using bootamp::audio::HttpResponse;
using bootamp::audio::detail::ByteSource;
using bootamp::audio::detail::ParsedUrl;
using bootamp::audio::detail::parse_response;
using bootamp::audio::detail::parse_url;
using bootamp::audio::detail::read_body;
using bootamp::audio::detail::resolve_location;

// CannedSource serves a fixed byte string; eof() makes it fail with an
// error (transport error) instead of clean EOF.
class CannedSource final : public ByteSource {
public:
  explicit CannedSource(std::string data) : data_(std::move(data)) {}

  std::pair<std::size_t, bool> read(std::span<char> dst) override {
    if (pos_ >= data_.size()) return {0, false};  // clean EOF
    const std::size_t n = std::min(dst.size(), data_.size() - pos_);
    std::memcpy(dst.data(), data_.data() + pos_, n);
    pos_ += n;
    return {n, true};
  }
  std::string last_error() const override { return err_; }

  // Simulate a transport error (recv timeout) mid-stream.
  void set_err(std::string e) { err_ = std::move(e); }

private:
  std::string data_;
  std::size_t pos_ = 0;
  std::string err_;
};

// Convenience: parse a canned response, require success, return it.
static HttpResponse parse_ok(std::string_view bytes) {
  CannedSource src{std::string(bytes)};
  auto resp = parse_response(src);
  REQUIRE(resp.has_value());
  return std::move(*resp);
}

// ---------------------------------------------------------------------------
// is_icy_response — the 4-byte probe
// ---------------------------------------------------------------------------

TEST_CASE("is_icy_response detects SHOUTcast prefix", "[http_socket][icy]") {
  using bootamp::audio::is_icy_response;
  CHECK(is_icy_response("ICY "));
  CHECK_FALSE(is_icy_response("ICY"));   // 3 bytes
  CHECK_FALSE(is_icy_response("HTTP"));
  CHECK_FALSE(is_icy_response("IC"));
  CHECK_FALSE(is_icy_response(""));
  CHECK_FALSE(is_icy_response("ICYY"));
}

// ---------------------------------------------------------------------------
// status-line rewrite + header parsing
// ---------------------------------------------------------------------------

TEST_CASE("ICY status line is rewritten to HTTP/1.0 before parsing",
          "[http_socket][icy]") {
  // The exact canned response of cliamp's client_test.go
  // TestStreamingClientAcceptsICYStatus: the server writes a raw
  // "ICY 200 OK\r\n..." status line over the socket.
  auto resp = parse_ok("ICY 200 OK\r\nContent-Length: 5\r\nContent-Type: audio/mpeg\r\n\r\nhello");
  CHECK(resp.status == 200);
  CHECK(resp.status_text == "OK");
  CHECK(resp.content_length == 5);
  CHECK_FALSE(resp.live);

  // Headers are lowercased keys with trimmed values, in order.
  REQUIRE(resp.headers.size() == 2);
  CHECK(resp.headers[0].first == "content-length");
  CHECK(resp.headers[0].second == "5");
  CHECK(resp.headers[1].first == "content-type");
  CHECK(resp.headers[1].second == "audio/mpeg");

  // The body bytes after the header terminator are untouched by the parser
  // (a streaming caller hands the fd straight to the decoder).
  CannedSource src("ICY 200 OK\r\nContent-Length: 5\r\n\r\nhello");
  auto resp2 = parse_response(src);
  REQUIRE(resp2.has_value());
  auto body = read_body(src, false, 1024 * 1024);
  REQUIRE(body.has_value());
  CHECK(*body == "hello");
}

TEST_CASE("normal HTTP/1.1 response parses without rewrite", "[http_socket]") {
  auto resp = parse_ok("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\nxyz");
  CHECK(resp.status == 200);
  CHECK(resp.status_text == "OK");
  CHECK(resp.content_length == -1);  // no Content-Length header
  CHECK_FALSE(resp.live);
}

TEST_CASE("any icy-* header marks the stream live", "[http_socket][icy]") {
  {
    auto resp = parse_ok("ICY 200 OK\r\nicy-metaint: 8192\r\nicy-name: Example Radio\r\n"
                         "Content-Length: 3\r\n\r\nabc");
    CHECK(resp.status == 200);
    CHECK(resp.live);
    REQUIRE(resp.headers.size() == 3);
    CHECK(resp.headers[0].first == "icy-metaint");
    CHECK(resp.headers[0].second == "8192");
  }
  // Mixed-case keys are lowercased (decode.go lowercases before the icy- check).
  {
    auto resp = parse_ok("HTTP/1.1 200 OK\r\nIcy-MetaInt: 128\r\n\r\n");
    CHECK(resp.live);
  }
  // A bare "icy" key without the dash must NOT count (Go HasPrefix "icy-").
  {
    auto resp = parse_ok("HTTP/1.1 200 OK\r\nicy: nope\r\n\r\n");
    CHECK_FALSE(resp.live);
  }
}

TEST_CASE("status line variants", "[http_socket]") {
  // No reason phrase.
  {
    auto resp = parse_ok("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n");
    CHECK(resp.status == 200);
    CHECK(resp.status_text.empty());
  }
  // LF-only line endings (Go's textproto accepts bare \n).
  {
    auto resp = parse_ok("HTTP/1.1 200 OK\nContent-Type: audio/mpeg\n\nzzz");
    CHECK(resp.status == 200);
    CHECK(resp.status_text == "OK");
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].first == "content-type");
  }
  // 404 with phrase — the "http status 404 Not Found" caller error path.
  {
    auto resp = parse_ok("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    CHECK(resp.status == 404);
    CHECK(resp.status_text == "Not Found");
  }
}

TEST_CASE("header edge cases", "[http_socket]") {
  // Duplicate headers are preserved in order (Set-Cookie etc.).
  {
    auto resp = parse_ok("HTTP/1.1 200 OK\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\n");
    REQUIRE(resp.headers.size() == 2);
    CHECK(resp.headers[0].first == "set-cookie");
    CHECK(resp.headers[0].second == "a=1");
    CHECK(resp.headers[1].first == "set-cookie");
    CHECK(resp.headers[1].second == "b=2");
  }
  // obs-fold continuation appends to the previous value (textproto joins
  // with a space).
  {
    auto resp = parse_ok("HTTP/1.1 200 OK\r\nX-Foo: a\r\n b\r\n\r\n");
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].first == "x-foo");
    CHECK(resp.headers[0].second == "a b");
  }
  // Value whitespace is trimmed.
  {
    auto resp = parse_ok("HTTP/1.1 200 OK\r\nX-Pad:   spaced\t\r\n\r\n");
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].second == "spaced");
  }
}

TEST_CASE("redirect responses parse with location header", "[http_socket]") {
  auto resp = parse_ok("HTTP/1.1 302 Found\r\nLocation: /new/path\r\nContent-Length: 0\r\n\r\n");
  CHECK(resp.status == 302);
  REQUIRE(resp.headers.size() == 2);
  CHECK(resp.headers[0].first == "location");
  CHECK(resp.headers[0].second == "/new/path");
}

// ---------------------------------------------------------------------------
// parse failures
// ---------------------------------------------------------------------------

TEST_CASE("malformed responses error, never crash", "[http_socket]") {
  // Empty response.
  {
    CannedSource src("");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
  }
  // Peeked bytes then EOF: "IC" is served, then unexpected EOF (icyConn
  // drains the prefix before surfacing io.ReadFull's error).
  {
    CannedSource src("IC");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
  }
  // Status line then EOF before header terminator.
  {
    CannedSource src("ICY 200 OK");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
    if (!resp) CHECK(resp.error() == "unexpected EOF");
  }
  // Not HTTP at all.
  {
    CannedSource src("HELLO WORLD\r\n\r\n");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
    if (!resp) CHECK(resp.error().find("malformed HTTP response") != std::string::npos);
  }
  // Non-numeric status code.
  {
    CannedSource src("HTTP/1.1 xyz OK\r\n\r\n");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
    if (!resp) CHECK(resp.error().find("malformed HTTP status code") != std::string::npos);
  }
  // Header without a colon.
  {
    CannedSource src("HTTP/1.1 200 OK\r\nNoColonHere\r\n\r\n");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
  }
  // Transport error (recv timeout) surfaces as the source's last_error.
  {
    CannedSource src("HTTP/1.1 200 OK\r\nContent-");
    src.set_err("read: i/o timeout");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
    if (!resp) CHECK(resp.error() == "read: i/o timeout");
  }
}

TEST_CASE("content-length validation", "[http_socket]") {
  // Non-numeric.
  {
    CannedSource src("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
    if (!resp) CHECK(resp.error().find("invalid Content-Length") != std::string::npos);
  }
  // Duplicate Content-Length headers (Go rejects).
  {
    CannedSource src("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
    if (!resp) CHECK(resp.error().find("multiple Content-Length") != std::string::npos);
  }
  // Unsolicited transfer encoding (Go rejects anything but chunked).
  {
    CannedSource src("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n");
    auto resp = parse_response(src);
    CHECK_FALSE(resp.has_value());
    if (!resp) CHECK(resp.error().find("unsupported transfer encoding") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// body reads (resolve.go: io.ReadAll(io.LimitReader(resp.Body, 1<<20)))
// ---------------------------------------------------------------------------

TEST_CASE("read_body plain, delimited by content-length semantics",
          "[http_socket][body]") {
  CannedSource src("data beyond headers");
  auto body = read_body(src, false, 1024 * 1024);
  REQUIRE(body.has_value());
  CHECK(*body == "data beyond headers");
}

TEST_CASE("read_body truncates at max_bytes like io.LimitReader", "[http_socket][body]") {
  CannedSource src("0123456789");
  auto body = read_body(src, false, 4);
  REQUIRE(body.has_value());
  CHECK(*body == "0123");  // truncated, no error — LimitReader semantics
}

TEST_CASE("read_body decodes chunked framing", "[http_socket][body]") {
  {
    CannedSource src("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    auto body = read_body(src, true, 1024 * 1024);
    REQUIRE(body.has_value());
    CHECK(*body == "hello world");
  }
  // Chunk-size extensions ("5;ext=1") are ignored like Go.
  {
    CannedSource src("5;ext=1\r\nhello\r\n0;last\r\n\r\n");
    auto body = read_body(src, true, 1024 * 1024);
    REQUIRE(body.has_value());
    CHECK(*body == "hello");
  }
  // Trailer headers after the 0 chunk are consumed and dropped.
  {
    CannedSource src("5\r\nhello\r\n0\r\nX-Foo: bar\r\n\r\n");
    auto body = read_body(src, true, 1024 * 1024);
    REQUIRE(body.has_value());
    CHECK(*body == "hello");
  }
  // Cap applies to decoded bytes, not framing.
  {
    CannedSource src("5\r\nhello\r\n0\r\n\r\n");
    auto body = read_body(src, true, 3);
    REQUIRE(body.has_value());
    CHECK(*body == "hel");
  }
  // Malformed size line errors.
  {
    CannedSource src("zz\r\n\r\n");
    auto body = read_body(src, true, 1024 * 1024);
    CHECK_FALSE(body.has_value());
  }
  // EOF mid-chunk errors ("unexpected EOF"), like Go.
  {
    CannedSource src("5\r\nhel");
    auto body = read_body(src, true, 1024 * 1024);
    CHECK_FALSE(body.has_value());
  }
}

TEST_CASE("chunked response end-to-end parse", "[http_socket][body]") {
  CannedSource src("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "5\r\nhello\r\n0\r\n\r\n");
  auto resp = parse_response(src);
  REQUIRE(resp.has_value());
  CHECK(resp->status == 200);
  CHECK(resp->content_length == -1);  // Go: ContentLength -1 when chunked
  auto body = read_body(src, true, 1024 * 1024);
  REQUIRE(body.has_value());
  CHECK(*body == "hello");
}

// ---------------------------------------------------------------------------
// URL parsing + redirect resolution (Go net/url subset)
// ---------------------------------------------------------------------------

TEST_CASE("parse_url decomposes http(s) URLs", "[http_socket][url]") {
  {
    auto u = parse_url("http://host/path?q=1&r=2#frag");
    REQUIRE(u.has_value());
    CHECK(u->scheme == "http");
    CHECK(u->host == "host");
    CHECK(u->host_header == "host");
    CHECK(u->path == "/path");
    CHECK(u->query == "q=1&r=2");
    CHECK(u->port == 80);
  }
  // No path, no query.
  {
    auto u = parse_url("http://example.com");
    REQUIRE(u.has_value());
    CHECK(u->path == "/");
    CHECK(u->query.empty());
    CHECK(u->port == 80);
  }
  // Explicit default port stays in the Host header (Go keeps it as written).
  {
    auto u = parse_url("http://example.com:80/x");
    REQUIRE(u.has_value());
    CHECK(u->port == 80);
    CHECK(u->host_header == "example.com:80");
  }
  // Non-default port.
  {
    auto u = parse_url("https://example.com:8443/x");
    REQUIRE(u.has_value());
    CHECK(u->port == 8443);
    CHECK(u->host_header == "example.com:8443");
  }
  // IPv6 literal with port.
  {
    auto u = parse_url("http://[::1]:8080/x");
    REQUIRE(u.has_value());
    CHECK(u->host == "::1");
    CHECK(u->host_header == "[::1]:8080");
    CHECK(u->port == 8080);
  }
  // IPv6 literal without port.
  {
    auto u = parse_url("http://[2001:db8::1]/x");
    REQUIRE(u.has_value());
    CHECK(u->host == "2001:db8::1");
    CHECK(u->host_header == "[2001:db8::1]");
    CHECK(u->port == 80);
  }
}

TEST_CASE("parse_url rejects bad URLs", "[http_socket][url]") {
  CHECK_FALSE(parse_url("example.com/path"));                 // no scheme
  CHECK_FALSE(parse_url("ftp://example.com/x"));              // bad scheme
  CHECK_FALSE(parse_url("http://"));                          // missing host
  CHECK_FALSE(parse_url("http://host:abc/x"));                // bad port
  CHECK_FALSE(parse_url("http://host:/x"));                   // empty port
  {
    auto u = parse_url("ftp://example.com/x");
    REQUIRE_FALSE(u.has_value());
    if (!u) CHECK(u.error().find("unsupported protocol scheme") != std::string::npos);
  }
}

TEST_CASE("resolve_location resolves redirect targets like Go", "[http_socket][url]") {
  const std::string base = "http://example.com/dir/file.m3u";
  CHECK(resolve_location(base, "/new") == "http://example.com/new");
  CHECK(resolve_location(base, "other.m3u") == "http://example.com/dir/other.m3u");
  CHECK(resolve_location(base, "http://b.net/x") == "http://b.net/x");
  CHECK(resolve_location(base, "https://b.net/x") == "https://b.net/x");
  CHECK(resolve_location(base, "//c.net/x") == "http://c.net/x");
  CHECK(resolve_location(base, "?q=2") == "http://example.com/dir/file.m3u?q=2");
  CHECK(resolve_location(base, "") == "http://example.com/dir/file.m3u");
  // A bare path (no slash) resolves against the base directory.
  CHECK(resolve_location("http://example.com/dir/", "x.m3u") == "http://example.com/dir/x.m3u");
}

// ---------------------------------------------------------------------------
// HttpClient surface (no network): placeholder + idempotent close
// ---------------------------------------------------------------------------

TEST_CASE("HttpClient surface contracts hold without a network", "[http_socket]") {
  bootamp::audio::HttpClient client;
  // set_cookies_from_browser is a no-op placeholder (yt-dlp owns cookies).
  client.set_cookies_from_browser("chrome");
  client.set_cookies_from_browser("");

  HttpResponse resp;
  resp.body_fd = -1;
  client.close_body(resp);   // -1 is fine
  client.close_body(resp);   // idempotent
  CHECK(resp.body_fd == -1);
}
