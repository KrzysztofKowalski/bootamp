// audio/http_socket_internal.hpp — internal detail surface of the http_socket
// module. Shared with tests (canned-byte response parsing, no network). Not
// part of the public module interface; do not depend on this from outside the
// audio subsystem.
#pragma once

#include "audio/http_socket.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace bootamp::audio::detail {

// ByteSource is the minimal pull reader the response parser consumes.
// read() returns {n, ok}; ok=false means EOF or error. last_error()
// distinguishes: non-empty ⇒ transport error (e.g. read timeout), empty ⇒
// clean EOF. This mirrors the icyConn/`io.Reader` seam of cliamp so the
// ICY rewrite + status/header parsing can be tested from canned bytes.
class ByteSource {
public:
  virtual ~ByteSource() = default;
  virtual std::pair<std::size_t, bool> read(std::span<char> dst) = 0;
  virtual std::string last_error() const { return {}; }
};

// parse_response consumes the status line + headers from `src`, applying the
// SHOUTcast "ICY " → "HTTP/1.0 " rewrite to the first 4 bytes (cliamp
// internal/httpclient/icy.go icyConn.Read). Body bytes are never consumed
// beyond the header terminator, so a streaming caller can hand the
// underlying fd straight to the decoder. Headers are stored lowercased;
// content_length is parsed from Content-Length (or -1), live is set when any
// icy-* header is present (cliamp decode.go openSource). transfer-encoding
// chunked responses set content_length = -1 and are decoded by read_body.
std::expected<HttpResponse, std::string> parse_response(ByteSource& src);

// read_body reads up to max_bytes of decoded body. When chunked is true the
// HTTP chunked framing is decoded (cliamp resolve.go reads io.LimitReader(resp
// .Body, 1<<20) — cap truncates silently, exactly like LimitReader). Trailer
// headers after the terminal 0-chunk are consumed and discarded.
std::expected<std::string, std::string> read_body(ByteSource& src, bool chunked,
                                                  std::size_t max_bytes);

// ParsedUrl is the decomposed http(s) URL used by the request path.
struct ParsedUrl {
  std::string scheme;       // "http" (https is rejected — no TLS in this client)
  std::string host;         // hostname or IPv6 literal without brackets
  std::string host_header;  // Host header value: [host] with :port iff explicit
  std::string path;         // always starts with '/'
  std::string query;        // without the '?'
  int         port = 80;    // default port filled in
};

// parse_url splits an http(s) URL. Errors mirror Go's net/url messages where
// practical ("unsupported protocol scheme", "missing host", "invalid port").
std::expected<ParsedUrl, std::string> parse_url(std::string_view url);

// resolve_location resolves a Location header against the current URL (Go's
// resp.Request.URL.Parse(loc) semantics, minus userinfo/opaque subtleties).
// Returns an absolute URL; errors on non-http(s) targets.
std::expected<std::string, std::string> resolve_location(std::string_view base_url,
                                                         std::string_view location);

}  // namespace bootamp::audio::detail
