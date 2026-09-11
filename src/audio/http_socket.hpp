// audio/http_socket.hpp — raw-socket HTTP/1.1 client + ICY rewrite.
//
// Per the plan: libcurl chokes on SHOUTcast `ICY 200 OK`, so radio audio uses a
// minimal hand-rolled HTTP/1.1 client. It forces HTTP/1.1 (Icecast has no HTTP/2),
// sends `User-Agent: bootamp/1.0` + `Icy-MetaData: 1`, and rewrites the 4-byte
// `ICY ` prefix to `HTTP/1.0 ` before parsing the status line. https URLs are
// served over OpenSSL TLS (cert verification ON, default verify paths — parity
// with Go's crypto/tls). Also used to fetch .m3u/.pls text playlists. libcurl
// stays for the Radio Browser JSON catalog (deferred to a later agent). NO boost.
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::audio {

// SigpipeGuard blocks SIGPIPE for this thread for its lifetime and consumes
// any pending instance before restoring the mask, so a write to a dead peer
// (OpenSSL handshake bytes to a RST'd socket, or a pipe whose read end just
// closed) returns EPIPE instead of raising SIGPIPE — which would kill the
// whole process silently ("nothing in the app ignores SIGPIPE" is the file
// invariant: plain socket writes use MSG_NOSIGNAL, OpenSSL and pipe writers
// arm this guard). consume() uses sigtimedwait with a zero timeout so it
// never blocks: a pending SIGPIPE was just raised by our own write, but if
// none is pending we must not sit in sigwait at an unwind point.
class SigpipeGuard {
public:
  SigpipeGuard() {
    sigset_t block;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGPIPE);
    armed_ = ::pthread_sigmask(SIG_BLOCK, &block, &old_) == 0;
  }
  ~SigpipeGuard() {
    if (!armed_) return;
    sigset_t pending;
    ::sigemptyset(&pending);
    ::sigaddset(&pending, SIGPIPE);
    siginfo_t        info {};
    struct timespec zero {};
    while (::sigtimedwait(&pending, &info, &zero) >= 0) {}  // never blocks
    ::pthread_sigmask(SIG_SETMASK, &old_, nullptr);
  }

private:
  sigset_t old_ {};
  bool     armed_ = false;
};

// HttpResponse is the minimal parsed response. headers are lowercased keys.
struct HttpResponse {
  int                            status  = 0;
  std::string                    status_text;
  std::vector<std::pair<std::string, std::string>> headers;
  // body_fd is the open socket fd for streaming reads (audio). The caller
  // drains it; close_http_body() closes it. For https streams it is the read
  // end of a socketpair fed by an internal TLS pump thread (the raw fd only
  // carries ciphertext); closing it tears the pump and the TLS session down.
  // For text fetches (m3u/pls) the body is read fully into `body`.
  int                            body_fd = -1;
  std::string                    body;        // populated by fetch_text()
  std::int64_t                   content_length = -1;
  bool                           live = false;  // any icy-* header present
};

// HttpClient is the raw-socket HTTP/1.1 client. One instance per request (no
// connection pooling — radio streams are long-lived, m3u/pls are one-shot).
class HttpClient {
public:
  // open_stream GETs `url` for audio: forces HTTP/1.1, sends Icy-MetaData: 1,
  // applies the ICY prefix rewrite, and returns the response with body_fd open
  // for streaming. The caller wraps it in IcyReader / stall_reader / ffmpeg.
  // `timeout` bounds the connect + initial response. Returns an error string
  // on failure (socket, DNS, non-200).
  std::expected<HttpResponse, std::string>
  open_stream(std::string_view url, std::chrono::milliseconds timeout = std::chrono::seconds{30});

  // fetch_text GETs `url` and reads the full body (≤1MB) into response.body.
  // Used for .m3u/.pls resolution. Same ICY-rewrite / HTTP/1.1 behavior.
  std::expected<HttpResponse, std::string>
  fetch_text(std::string_view url, std::size_t max_bytes = 1024 * 1024,
             std::chrono::milliseconds timeout = std::chrono::seconds{30});

  // close_body closes the streaming body fd. Idempotent.
  void close_body(HttpResponse& resp);

  // set_cookies_from_browser records the browser name for a future cookie jar
  // (unused for raw-socket audio; yt-dlp owns cookies). Placeholder.
  void set_cookies_from_browser(std::string_view browser) { cookies_from_ = browser; }

private:
  std::string cookies_from_;
};

// is_icy_response peeks the first 4 bytes of a socket response and reports
// whether it starts with "ICY " (SHOUTcast non-HTTP status line).
bool is_icy_response(std::string_view first4);

}  // namespace bootamp::audio