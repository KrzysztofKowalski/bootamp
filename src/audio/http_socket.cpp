// audio/http_socket.cpp — raw-socket HTTP/1.1 client + ICY prefix rewrite.
//
// Port of:
//   - cliamp/internal/httpclient/icy.go icyConn.Read (10-44): peek the first
//     4 bytes and rewrite "ICY " → "HTTP/1.0 " so the status line parses.
//   - cliamp/internal/httpclient/client.go newStreamingTransport (24-59):
//     HTTP/1.1 only (no HTTP/2 — Icecast/SHOUTcast choke on ALPN), 30s
//     response-header timeout, no overall timeout so live streams survive.
//   - cliamp/player/decode.go openSource: GET with User-Agent + Icy-MetaData: 1,
//     "http status <code> <text>" errors on non-200, live = any icy-* header.
//   - cliamp/resolve/resolve.go: text fetch capped at 1 MiB (maxM3UBody),
//     caller checks status; redirects followed as Go's http.Client does
//     (301/302/303/307/308, up to 10, Location resolved against the URL).
//
// NO boost, no libcurl. Plain POSIX: getaddrinfo + nonblocking connect with
// the caller's timeout, byte-exact header reads so the body fd is never
// over-consumed. https URLs run over OpenSSL TLS (TLS_client_method, cert +
// hostname verification ON, system default verify paths — parity with Go's
// crypto/tls): every read/write goes through SSL_read/SSL_write, and https
// streaming bodies reach the caller through a socketpair fed by an internal
// pump thread (the raw fd only carries ciphertext).
#include "audio/http_socket.hpp"
#include "audio/http_socket_internal.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>  // struct timespec (sigtimedwait)
#include <expected>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <signal.h>  // SIGPIPE block/consume around OpenSSL's internal writes
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace bootamp::audio {

namespace {

using detail::ByteSource;

constexpr int         kMaxRedirects    = 10;
constexpr std::size_t kMaxHeaderBytes  = 1u << 20;  // http.DefaultMaxHeaderBytes
constexpr std::size_t kMaxChunkLine    = 4096;
constexpr std::size_t kReadChunk       = 65536;

std::string_view trim_ws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::string ascii_lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// header_value: first value for a (lowercased) key, "" if absent.
std::string_view header_value(const HttpResponse& resp, std::string_view key) {
  for (const auto& [k, v] : resp.headers)
    if (k == key) return v;
  return {};
}

bool is_redirect_status(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// ---------------------------------------------------------------------------
// URL handling (net/url subset)
// ---------------------------------------------------------------------------

std::string display_authority(const std::string& host, int port) {
  if (host.find(':') != std::string::npos) return "[" + host + "]:" + std::to_string(port);
  return host + ":" + std::to_string(port);
}

std::expected<detail::ParsedUrl, std::string> parse_url_impl(std::string_view url) {
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos)
    return std::unexpected("parse \"" + std::string(url) + "\": missing protocol scheme");
  const std::string scheme = ascii_lower(url.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https")
    return std::unexpected("Get \"" + std::string(url) + "\": unsupported protocol scheme \"" +
                           scheme + "\"");

  std::string_view rest = url.substr(scheme_end + 3);
  const std::size_t auth_end = rest.find_first_of("/?#");
  std::string_view  auth     = rest.substr(0, auth_end);
  if (auth.empty()) return std::unexpected("parse \"" + std::string(url) + "\": missing host");
  if (auth.find('@') != std::string_view::npos)
    return std::unexpected("parse \"" + std::string(url) + "\": invalid userinfo in URL");

  detail::ParsedUrl p;
  p.scheme = scheme;
  p.port   = scheme == "http" ? 80 : 443;
  bool port_explicit = false;
  int  port_parsed   = 0;

  if (auth.front() == '[') {  // IPv6 literal
    const std::size_t close = auth.find(']');
    if (close == std::string_view::npos)
      return std::unexpected("parse \"" + std::string(url) + "\": missing ']' in host");
    p.host = std::string(auth.substr(1, close - 1));
    if (close + 1 < auth.size()) {
      if (auth[close + 1] != ':')
        return std::unexpected("parse \"" + std::string(url) + "\": invalid port");
      const std::string_view ps = auth.substr(close + 2);
      if (ps.empty())
        return std::unexpected("parse \"" + std::string(url) + "\": invalid port");
      auto [ptr, ec] = std::from_chars(ps.data(), ps.data() + ps.size(), port_parsed);
      if (ec != std::errc{} || ptr != ps.data() + ps.size())
        return std::unexpected("parse \"" + std::string(url) + "\": invalid port \":" + std::string(ps) +
                               "\"");
      port_explicit = true;
    }
  } else {
    const std::size_t colon = auth.rfind(':');
    if (colon != std::string_view::npos) {
      const std::string_view ps = auth.substr(colon + 1);
      if (ps.empty())
        return std::unexpected("parse \"" + std::string(url) + "\": invalid port");
      auto [ptr, ec] = std::from_chars(ps.data(), ps.data() + ps.size(), port_parsed);
      if (ec != std::errc{} || ptr != ps.data() + ps.size())
        return std::unexpected("parse \"" + std::string(url) + "\": invalid port \":" + std::string(ps) +
                               "\"");
      p.host        = std::string(auth.substr(0, colon));
      port_explicit = true;
    } else {
      p.host = std::string(auth);
    }
  }
  if (p.host.empty()) return std::unexpected("parse \"" + std::string(url) + "\": missing host");
  if (port_explicit) p.port = port_parsed;
  if (p.port <= 0 || p.port > 65535)
    return std::unexpected("parse \"" + std::string(url) + "\": invalid port \"" +
                           std::to_string(p.port) + "\"");

  p.host_header = p.host.find(':') != std::string::npos ? "[" + p.host + "]" : p.host;
  if (port_explicit) p.host_header += ":" + std::to_string(p.port);

  std::string_view pathq = auth_end == std::string_view::npos ? std::string_view{} : rest.substr(auth_end);
  if (const std::size_t hash = pathq.find('#'); hash != std::string_view::npos)
    pathq = pathq.substr(0, hash);
  if (const std::size_t q = pathq.find('?'); q != std::string_view::npos) {
    p.query = std::string(pathq.substr(q + 1));
    pathq   = pathq.substr(0, q);
  }
  p.path = pathq.empty() ? "/" : std::string(pathq);
  if (p.path.front() != '/') p.path.insert(p.path.begin(), '/');  // should not happen
  return p;
}

std::expected<std::string, std::string> resolve_location_impl(std::string_view base_url,
                                                              std::string_view loc) {
  if (loc.starts_with("http://") || loc.starts_with("https://")) return std::string(loc);
  auto base = parse_url_impl(base_url);
  if (!base) return std::unexpected(base.error());
  if (loc.starts_with("//")) return base->scheme + ":" + std::string(loc);

  const std::string base_origin = base->scheme + "://" + base->host_header;
  if (loc.empty()) return base_origin + base->path + (base->query.empty() ? "" : "?" + base->query);
  if (loc.front() == '/') return base_origin + std::string(loc);
  if (loc.front() == '?')
    return base_origin + base->path + std::string(loc);
  // relative path: directory of the base path + loc
  std::string dir = base->path;
  const std::size_t slash = dir.rfind('/');
  dir = slash == std::string::npos ? "/" : dir.substr(0, slash + 1);
  return base_origin + dir + std::string(loc);
}

// ---------------------------------------------------------------------------
// Socket plumbing
// ---------------------------------------------------------------------------

// dial connects to u.host:u.port with `timeout` covering the connect (Go's
// net.Dialer has no timeout here, but the contract bounds connect+response).
// Tries every getaddrinfo result; returns the last error like Go.
std::expected<int, std::string> dial(const detail::ParsedUrl& u,
                                     std::chrono::milliseconds timeout) {
  const std::string addr   = display_authority(u.host, u.port);
  const std::string port_s = std::to_string(u.port);

  struct addrinfo hints {};
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  struct addrinfo*  res = nullptr;
  const int gai = ::getaddrinfo(u.host.c_str(), port_s.c_str(), &hints, &res);
  if (gai != 0) return std::unexpected("dial " + addr + ": getaddrinfo: " + ::gai_strerror(gai));
  struct AddrinfoGuard {
    struct addrinfo* p;
    ~AddrinfoGuard() { ::freeaddrinfo(p); }
  } guard{res};

  std::string   last_err = "no addresses";
  const int     poll_ms  = static_cast<int>(
      std::clamp<long long>(timeout.count(), 1, 600000));
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) {
      last_err = std::string("socket: ") + std::strerror(errno);
      continue;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
      last_err = std::string("connect: ") + std::strerror(errno);
      ::close(fd);
      continue;
    }
    if (rc != 0) {
      struct pollfd pfd {fd, static_cast<short>(POLLOUT), 0};
      const int prc = ::poll(&pfd, 1, poll_ms);
      if (prc <= 0) {
        last_err = "connect: i/o timeout";
        ::close(fd);
        continue;
      }
      int        err = 0;
      socklen_t  len = sizeof(err);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        last_err = std::string("connect: ") + std::strerror(err != 0 ? err : errno);
        ::close(fd);
        continue;
      }
    }
    ::fcntl(fd, F_SETFL, flags);  // back to blocking

    // The 30s response-header timeout of the Go transport, applied as socket
    // receive/send timeouts. Deliberately left in place for the body too:
    // cliamp has no overall timeout (live streams must never be killed), but
    // our body readers are either capped (fetch_text) or wrapped in
    // stall_reader (streams), and a 30s wall-clock backstop beats a hang.
    struct timeval tv {};
    tv.tv_sec  = timeout.count() / 1000;
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
  }
  return std::unexpected("dial " + addr + ": " + last_err);
}

// --- TLS (https) plumbing: OpenSSL over the connected fd ---------------------
// Go parity (crypto/tls): TLS_client_method, cert chain + hostname (or IP
// SAN) verification ON against the system default verify paths, SNI sent for
// hostnames (RFC 6066 forbids IP literals). The fd stays blocking with
// dial()'s SO_RCVTIMEO/SO_SNDTIMEO; SSL_ERROR_WANT_READ/WANT_WRITE is retried
// via a bounded poll — never a busy spin — and a socket timeout surfaces as
// "read: i/o timeout" exactly like the plain-socket path. SIGPIPE is blocked
// for the thread around every OpenSSL call that can write (SSL_connect,
// SSL_write, SSL_shutdown): OpenSSL's socket BIO writes via write(2) without
// MSG_NOSIGNAL, and nothing in the app ignores SIGPIPE (Go parity: crypto/tls
// surfaces EPIPE/ECONNRESET as errors instead of dying).

// SigpipeGuard blocks SIGPIPE for this thread for its lifetime and consumes
// any pending instance before restoring the mask, so an OpenSSL call writing
// to a RST'd peer returns EPIPE instead of raising SIGPIPE (which would kill
// the whole process — the plain-socket path uses MSG_NOSIGNAL for the same
// reason). consume() uses sigtimedwait with a zero timeout so it never
// blocks: a pending SIGPIPE was just raised by our own OpenSSL call, but if
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

std::string ssl_error_string() {
  char buf[256];
  const unsigned long e = ERR_get_error();
  if (e == 0) return "unknown error";
  ERR_error_string_n(e, buf, sizeof buf);
  return buf;
}

// tls_connect runs the TLS handshake on the connected `fd`. Returns the
// SSL_CTX/SSL pair on success; on failure frees them and returns an error in
// the file's style ("tls handshake host:port: ...").
std::expected<std::pair<SSL_CTX*, SSL*>, std::string>
tls_connect(int fd, const std::string& host, const std::string& addr,
            std::chrono::milliseconds timeout) {
  ERR_clear_error();
  SigpipeGuard sigpipe_guard;  // SSL_connect writes handshake bytes (no MSG_NOSIGNAL)
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == nullptr)
    return std::unexpected("tls handshake " + addr + ": " + ssl_error_string());
  auto fail_ctx = [&](std::string msg) {
    SSL_CTX_free(ctx);
    return std::unexpected(std::move(msg));
  };

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);  // verify the chain
  if (SSL_CTX_set_default_verify_paths(ctx) != 1)
    return fail_ctx("tls handshake " + addr + ": failed to load default verify paths");
  SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);  // blocking fd: retry renegotiation internally

  SSL* ssl = SSL_new(ctx);
  if (ssl == nullptr) return fail_ctx("tls handshake " + addr + ": " + ssl_error_string());
  auto fail = [&](std::string msg) {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return std::unexpected(std::move(msg));
  };
  if (SSL_set_fd(ssl, fd) != 1)
    return fail("tls handshake " + addr + ": " + ssl_error_string());

  // SNI + identity check: hostname against dNSName SANs, IP literal against
  // iPAddress SANs (X509_VERIFY_PARAM_set1_*). SSL_set_tlsext_host_name is
  // skipped for IP literals (RFC 6066 forbids them in SNI).
  bool is_ip = host.find(':') != std::string::npos;
  if (!is_ip) {
    struct in_addr v4 {};
    is_ip = ::inet_pton(AF_INET, host.c_str(), &v4) == 1;
  }
  X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
  if (is_ip) {
    if (X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) != 1)
      return fail("tls handshake " + addr + ": failed to set IP verification");
  } else {
    if (SSL_set_tlsext_host_name(ssl, host.c_str()) != 1)
      return fail("tls handshake " + addr + ": " + ssl_error_string());
    if (X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0) != 1)
      return fail("tls handshake " + addr + ": failed to set hostname verification");
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const int rc = SSL_connect(ssl);
    if (rc == 1) break;
    const int e = SSL_get_error(ssl, rc);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
      struct pollfd pfd {fd, static_cast<short>(e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return fail("tls handshake " + addr + ": i/o timeout");
      const long ms = std::clamp<long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 1, 600000);
      ::poll(&pfd, 1, static_cast<int>(ms));
      continue;
    }
    const long vr = SSL_get_verify_result(ssl);
    if (vr != X509_V_OK)
      return fail("tls handshake " + addr + ": certificate verify failed: " +
                  std::string(X509_verify_cert_error_string(vr)));
    if (e == SSL_ERROR_SYSCALL && errno != 0)
      return fail("tls handshake " + addr + ": " + std::strerror(errno));
    return fail("tls handshake " + addr + ": " + ssl_error_string());
  }
  const long vr = SSL_get_verify_result(ssl);
  if (vr != X509_V_OK)
    return fail("tls handshake " + addr + ": certificate verify failed: " +
                std::string(X509_verify_cert_error_string(vr)));
  return std::pair<SSL_CTX*, SSL*>{ctx, ssl};
}

// tls_read: SSL_read with WANT_READ/WANT_WRITE retried via a bounded poll.
// SO_RCVTIMEO expiry surfaces as "read: i/o timeout" (the plain-socket
// semantics); clean EOF (close_notify) returns {0, false} with err untouched.
std::pair<std::size_t, bool> tls_read(SSL* ssl, int fd, std::span<char> dst,
                                      std::chrono::milliseconds timeout,
                                      std::string& err) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    ERR_clear_error();
    const int n = SSL_read(ssl, dst.data(), static_cast<int>(dst.size()));
    if (n > 0) return {static_cast<std::size_t>(n), true};
    if (n == 0) return {0, false};  // clean EOF (close_notify)
    const int e = SSL_get_error(ssl, n);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
      struct pollfd pfd {fd, static_cast<short>(e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        err = "read: i/o timeout";
        return {0, false};
      }
      const long ms = std::clamp<long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 1, 250);
      ::poll(&pfd, 1, static_cast<int>(ms));
      continue;
    }
    if (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      err = "read: i/o timeout";
      return {0, false};
    }
    if (e == SSL_ERROR_SYSCALL && errno != 0) {
      err = std::string("read: ") + std::strerror(errno);
      return {0, false};
    }
    err = "read: " + ssl_error_string();
    return {0, false};
  }
}

// tls_write_all: SSL_write of the whole request; WANT_* retried with a
// bounded poll; socket-level errors in the file's "write: ..." style.
std::string tls_write_all(SSL* ssl, int fd, std::string_view data,
                          std::chrono::milliseconds timeout) {
  SigpipeGuard sigpipe_guard;  // SSL_write writes via the BIO (no MSG_NOSIGNAL)
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t off = 0;
  while (off < data.size()) {
    ERR_clear_error();
    const int n = SSL_write(ssl, data.data() + off, static_cast<int>(data.size() - off));
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    const int e = SSL_get_error(ssl, n);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
      struct pollfd pfd {fd, static_cast<short>(e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return "write: i/o timeout";
      const long ms = std::clamp<long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 1, 250);
      ::poll(&pfd, 1, static_cast<int>(ms));
      continue;
    }
    if (e == SSL_ERROR_SYSCALL && errno != 0)
      return "write: " + std::string(std::strerror(errno));
    return "write: " + ssl_error_string();
  }
  return {};
}

// pump_stream is the detached streaming pump for https bodies. The raw fd
// only carries ciphertext, so a helper thread drains the TLS stream and
// delivers decrypted bytes into a socketpair whose read end is returned to
// the caller as HttpResponse::body_fd. The caller's stall-cancel closes that
// end; the pump observes POLLERR/POLLHUP (or EPIPE on the next write) and
// tears the connection down, closing the real fd so nothing leaks. A `stall`
// of silence ends it too — the SO_RCVTIMEO backstop of dial(). Owns the SSL
// session + fd; frees everything on every exit path.
void pump_stream(SSL* ssl, SSL_CTX* ctx, int tcp_fd, int pair_w,
                 std::chrono::milliseconds stall) {
  // Covers SSL_read (TLS 1.2 renegotiation writes), deliver() and the
  // best-effort SSL_shutdown: an RST'd peer must surface as an error here,
  // never as a process-killing SIGPIPE.
  SigpipeGuard sigpipe_guard;
  char buf[kReadChunk];
  auto last_data = std::chrono::steady_clock::now();

  auto deliver = [&](const char* p, std::size_t len) -> bool {
    std::size_t off = 0;
    while (off < len) {
      // MSG_NOSIGNAL: the caller closes body_fd mid-stream (stall-cancel,
      // station stop, decoder EOF) while this pump is delivering — a plain
      // write() would raise SIGPIPE on the closed read end and kill the whole
      // process (nothing in the app ignores SIGPIPE).
      const ssize_t w = ::send(pair_w, p + off, len - off, MSG_NOSIGNAL);
      if (w > 0) {
        off += static_cast<std::size_t>(w);
        continue;
      }
      if (errno == EINTR) continue;
      return false;  // EPIPE/EBADF: the caller closed the read end
    }
    return true;
  };

  for (;;) {
    // Has the caller closed the read end? (POLLERR/POLLHUP are always
    // reported, even when not requested.)
    struct pollfd chk {pair_w, POLLOUT, 0};
    if (::poll(&chk, 1, 0) > 0 && (chk.revents & (POLLERR | POLLHUP)) != 0) break;

    ERR_clear_error();
    const int n = SSL_read(ssl, buf, static_cast<int>(sizeof buf));
    if (n > 0) {
      last_data = std::chrono::steady_clock::now();
      if (!deliver(buf, static_cast<std::size_t>(n))) break;
      continue;
    }
    if (n == 0) break;  // server EOF (close_notify)
    const int e = SSL_get_error(ssl, n);
    const bool retryable = e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE ||
                           (e == SSL_ERROR_SYSCALL &&
                            (errno == EAGAIN || errno == EWOULDBLOCK));
    if (!retryable) break;
    if (std::chrono::steady_clock::now() - last_data >= stall) break;  // backstop
    struct pollfd pfd {tcp_fd, static_cast<short>(e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
    ::poll(&pfd, 1, 250);
  }

  (void)SSL_shutdown(ssl);  // best-effort close_notify
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  ::close(tcp_fd);
  ::close(pair_w);
}

// Conn owns the connected fd and, for https, the TLS session (SSL_CTX/SSL).
// read/write_all route through SSL_read/SSL_write when TLS is active, so the
// response parser and body readers only ever see decrypted bytes. Streaming
// hand-off: plain http hands the raw fd (release_fd); https hands the read
// end of a socketpair fed by a detached pump thread (release_pump).
class Conn {
public:
  Conn(int fd, SSL_CTX* ctx, SSL* ssl, std::chrono::milliseconds timeout)
      : fd_(fd), ctx_(ctx), ssl_(ssl), timeout_(timeout) {}
  ~Conn() { close(); }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
  // Defaulted moves would copy the scalar members and leave a moved-from Conn
  // holding live pointers — null them so its dtor is a no-op.
  Conn(Conn&& o) noexcept
      : fd_(o.fd_), ctx_(o.ctx_), ssl_(o.ssl_),
        err_(std::move(o.err_)), timeout_(o.timeout_) {
    o.fd_ = -1;
    o.ctx_ = nullptr;
    o.ssl_ = nullptr;
  }
  Conn& operator=(Conn&&) = delete;

  static std::expected<Conn, std::string> connect(const detail::ParsedUrl& u,
                                                  std::chrono::milliseconds timeout) {
    auto fd = dial(u, timeout);
    if (!fd) return std::unexpected(fd.error());
    if (u.scheme != "https") return Conn{*fd, nullptr, nullptr, timeout};
    const std::string addr = display_authority(u.host, u.port);
    auto tls = tls_connect(*fd, u.host, addr, timeout);
    if (!tls) {
      ::close(*fd);
      return std::unexpected(tls.error());
    }
    return Conn{*fd, tls->first, tls->second, timeout};
  }

  std::pair<std::size_t, bool> read(std::span<char> dst) {
    if (fd_ < 0) return {0, false};
    if (ssl_ != nullptr) return tls_read(ssl_, fd_, dst, timeout_, err_);
    for (;;) {
      const ssize_t n = ::recv(fd_, dst.data(), dst.size(), 0);
      if (n > 0) return {static_cast<std::size_t>(n), true};
      if (n == 0) return {0, false};  // clean EOF (server closed)
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        err_ = "read: i/o timeout";
        return {0, false};
      }
      err_ = std::string("read: ") + std::strerror(errno);
      return {0, false};
    }
  }

  // write_all sends the whole request (small; loops partial sends). Returns
  // "" on success, else a "write: ..." error.
  std::string write_all(std::string_view data) {
    if (fd_ < 0) return "write: connection closed";
    if (ssl_ != nullptr) return tls_write_all(ssl_, fd_, data, timeout_);
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        return "write: " + std::string(std::strerror(errno));
      }
      off += static_cast<std::size_t>(n);
    }
    return {};
  }

  std::string last_error() const { return err_; }
  bool        tls() const { return ssl_ != nullptr; }

  // close shuts the connection down; idempotent (release paths null it).
  void close() {
    if (ssl_ != nullptr) {
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (ctx_ != nullptr) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  // release_fd hands the raw fd to the caller (plain http streaming body).
  int release_fd() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  // release_pump (https streaming): starts the detached pump thread and
  // returns the socketpair read end to stream from. The pump owns the SSL
  // session, ctx and fd from here on and frees them when it exits.
  std::expected<int, std::string> release_pump() {
    if (ssl_ == nullptr) return std::unexpected("tls pump: no TLS session");
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
      return std::unexpected(std::string("tls pump: socketpair: ") + std::strerror(errno));
    SSL*     ssl = ssl_;
    SSL_CTX* ctx = ctx_;
    const int tcp = fd_;
    ssl_ = nullptr;
    ctx_ = nullptr;
    fd_ = -1;  // the pump owns these now
    try {
      std::thread([ssl, ctx, tcp, w = sv[1], stall = timeout_] {
        pump_stream(ssl, ctx, tcp, w, stall);
      }).detach();
    } catch (const std::exception&) {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      ::close(tcp);
      ::close(sv[0]);
      ::close(sv[1]);
      return std::unexpected("tls pump: failed to start pump thread");
    }
    return sv[0];
  }

private:
  int                       fd_ = -1;
  SSL_CTX*                  ctx_ = nullptr;
  SSL*                      ssl_ = nullptr;
  std::string               err_;
  std::chrono::milliseconds timeout_{0};
};

// SocketSource is the ByteSource over a Conn (decrypted when TLS is active).
// A clean remote close is EOF; a stall is "read: i/o timeout" (EAGAIN /
// SO_RCVTIMEO). Streaming hand-off: release_fd for plain http, release_pump
// (socketpair + pump thread) for https — the raw fd only carries ciphertext.
class SocketSource final : public ByteSource {
public:
  explicit SocketSource(Conn& conn) : conn_(conn) {}
  ~SocketSource() override { close(); }
  SocketSource(const SocketSource&) = delete;
  SocketSource& operator=(const SocketSource&) = delete;

  std::pair<std::size_t, bool> read(std::span<char> dst) override {
    return conn_.read(dst);
  }
  std::string last_error() const override { return conn_.last_error(); }

  void close() { conn_.close(); }
  int  release_fd() { return conn_.release_fd(); }
  std::expected<int, std::string> release_pump() { return conn_.release_pump(); }

private:
  Conn& conn_;
};

// request performs the GET with redirects, per the two public entry points.
// `audio` adds the Icy-MetaData: 1 header; `keep_fd` streams the body through
// resp.body_fd (open_stream) instead of reading it (fetch_text).
std::expected<HttpResponse, std::string> request(std::string_view url,
                                                 std::chrono::milliseconds timeout,
                                                 bool audio, bool keep_fd,
                                                 std::size_t max_bytes,
                                                 const std::string& /*cookies_from*/) {
  std::string current(url);
  for (int redirects = 0;; ++redirects) {
    auto parsed = parse_url_impl(current);
    if (!parsed) return std::unexpected(parsed.error());
    // Both http and https are accepted on every hop. Conn dials and, for
    // https, TLS-handshakes here; a hop that changes scheme (http → https
    // upgrades and https → http downgrades) tears the previous connection
    // and its TLS session down when the loop-local Conn goes out of scope,
    // and the next hop re-setups from scratch.

    auto conn = Conn::connect(*parsed, timeout);
    if (!conn) return std::unexpected(conn.error());

    std::string req = "GET " + parsed->path;
    if (!parsed->query.empty()) req += "?" + parsed->query;
    req += " HTTP/1.1\r\n";
    req += "Host: " + parsed->host_header + "\r\n";
    req += "User-Agent: bootamp/1.0\r\n";
    if (audio) req += "Icy-MetaData: 1\r\n";
    req += "Connection: close\r\n\r\n";

    // All writes route through SSL_write for https (Conn::write_all).
    if (const std::string werr = conn->write_all(req); !werr.empty())
      return std::unexpected("http get: " + werr);  // Conn dtor closes + frees

    // The response parser reads through Conn, so for https every byte —
    // including the ICY 4-byte peek/rewrite — arrives decrypted via
    // SSL_read, and the parser itself is unchanged.
    SocketSource source(*conn);
    auto resp = detail::parse_response(source);
    if (!resp) {
      source.close();
      return std::unexpected(resp.error());
    }

    // Redirects (Go http.Client default CheckRedirect): 301/302/303/307/308
    // with a Location header, up to 10 hops, Location resolved against the
    // current URL. No connection reuse across hops.
    if (is_redirect_status(resp->status)) {
      const std::string_view loc = header_value(*resp, "location");
      if (!loc.empty()) {
        if (redirects >= kMaxRedirects) {
          source.close();
          return std::unexpected("Get \"" + std::string(url) + "\": stopped after " +
                                 std::to_string(kMaxRedirects) + " redirects");
        }
        auto next = detail::resolve_location(current, loc);
        source.close();  // no keep-alive
        if (!next) return std::unexpected(next.error());
        current = *next;
        continue;
      }
      // 3xx without Location: fall through — caller sees the non-200 status.
    }

    if (resp->status != 200) {
      // cliamp: fmt.Errorf("http status %s", resp.Status) — "404 Not Found".
      std::string msg = "http status " + std::to_string(resp->status);
      if (!resp->status_text.empty()) msg += " " + resp->status_text;
      source.close();
      return std::unexpected(std::move(msg));
    }

    if (!keep_fd) {
      // parse_response validated every transfer-encoding token as "chunked",
      // so the header's mere presence means chunked framing.
      bool chunked = false;
      for (const auto& [k, v] : resp->headers) {
        (void)v;
        if (k == "transfer-encoding") chunked = true;
      }
      auto body = detail::read_body(source, chunked, max_bytes);
      source.close();
      if (!body) return std::unexpected(body.error());
      resp->body = std::move(*body);
      return resp;
    }

    // Streaming: hand the connection to the caller. Plain http hands the raw
    // fd; https hands the read end of a socketpair that an internal pump
    // thread feeds from the (decrypted) TLS stream — the raw fd only carries
    // ciphertext. Closing body_fd (close_http_body / the pipeline's
    // stall-cancel) ends the pump and tears the TLS session down.
    if (conn->tls()) {
      auto pump = source.release_pump();
      if (!pump) return std::unexpected(pump.error());
      resp->body_fd = *pump;
    } else {
      resp->body_fd = source.release_fd();
    }
    return resp;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// detail implementations
// ---------------------------------------------------------------------------

namespace detail {

namespace {

// ByteCursor is the response-parse read state. It serves the peeked/
// rewritten prefix first (icyConn drains c.prefix before touching the conn
// again), then reports the peek error deferred by the rewrite, and only then
// reads the source. This mirrors icyConn.Read's exact error-surfacing order:
// a short peek serves what arrived and errors only once drained.
class ByteCursor {
public:
  explicit ByteCursor(ByteSource& src) : src_(src) {}

  // Peek the first 4 bytes and rewrite the SHOUTcast "ICY " prefix to
  // "HTTP/1.0 " before the status line is parsed (icyConn.Read). A read
  // failure mid-peek is deferred, not lost.
  void peek4() {
    first4_.reserve(4);
    while (first4_.size() < 4) {
      char c = 0;
      auto [n, ok] = src_.read(std::span<char>(&c, 1));
      if (!ok) {
        deferred_ = src_.last_error().empty() ? "unexpected EOF" : src_.last_error();
        break;
      }
      first4_.push_back(c);
    }
    if (first4_ == "ICY ") first4_ = "HTTP/1.0 ";
  }

  // Read one byte. On EOF/error sets err_ (empty ⇒ unreachable: err_ is
  // always filled) and returns false.
  bool read(char& out) {
    if (!first4_.empty()) {
      out = first4_.front();
      first4_.erase(first4_.begin());
      return true;
    }
    if (!deferred_.empty()) {
      err_ = deferred_;
      deferred_.clear();
      return false;
    }
    auto [n, ok] = src_.read(std::span<char>(&out, 1));
    if (ok) return true;
    err_ = src_.last_error().empty() ? "unexpected EOF" : src_.last_error();
    return false;
  }

  std::string err() const { return err_; }
  void fail(std::string e) { err_ = std::move(e); }

private:
  ByteSource& src_;
  std::string first4_;   // peeked (and possibly rewritten) prefix bytes
  std::string deferred_; // peek failure, reported after first4_ drains
  std::string err_;
};

// read_line reads one CRLF/LF-terminated line (terminal stripped), capped at
// `cap` bytes. Returns false on EOF/error or an overlong line; on failure
// cur.err() is always set.
bool read_line(ByteCursor& cur, std::string& out, std::size_t cap) {
  out.clear();
  for (;;) {
    char c;
    if (!cur.read(c)) return false;
    if (c == '\n') {
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (out.size() >= cap) {
      cur.fail("header line too long");
      return false;
    }
    out.push_back(c);
  }
}

std::string trim_right(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

}  // namespace

std::expected<HttpResponse, std::string> parse_response(ByteSource& src) {
  HttpResponse resp;
  ByteCursor   cur(src);
  cur.peek4();  // ICY "ICY " → "HTTP/1.0 " rewrite before any parsing

  // --- status line ---
  std::string line;
  for (;;) {
    char c = 0;
    if (!cur.read(c)) return std::unexpected(cur.err());
    if (c == '\n') break;
    line.push_back(c);
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  // "HTTP/1.0 200 OK" after the ICY rewrite; anything else is not HTTP.
  if (line.size() < 10 || line.substr(0, 5) != "HTTP/")
    return std::unexpected("malformed HTTP response \"" + line + "\"");
  const std::size_t sp = line.find(' ', 5);
  if (sp == std::string::npos)
    return std::unexpected("malformed HTTP response \"" + line + "\"");
  std::string_view code_view = trim_ws(std::string_view(line).substr(sp + 1));
  const std::size_t code_end = code_view.find_first_not_of("0123456789");
  const std::string_view code_s = code_view.substr(0, code_end);
  if (code_s.empty())
    return std::unexpected("malformed HTTP status code \"" + std::string(code_view) + "\"");
  int code = 0;
  auto [ptr, ec] = std::from_chars(code_s.data(), code_s.data() + code_s.size(), code);
  if (ec != std::errc{} || ptr != code_s.data() + code_s.size())
    return std::unexpected("malformed HTTP status code \"" + std::string(code_view) + "\"");
  resp.status = code;
  if (code_end != std::string_view::npos)
    resp.status_text = std::string(trim_ws(code_view.substr(code_end)));

  // --- headers ---
  bool chunked = false;
  bool cl_seen = false;
  std::size_t hdr_bytes = line.size();
  std::string last_key;
  for (;;) {
    std::string l;
    if (!read_line(cur, l, kMaxHeaderBytes)) return std::unexpected(cur.err());
    hdr_bytes += l.size() + 2;
    if (hdr_bytes > kMaxHeaderBytes)
      return std::unexpected("http: server response headers exceed 1 MiB");
    if (l.empty()) break;  // end of headers

    if (l.front() == ' ' || l.front() == '\t') {
      // obs-fold continuation: append to the previous header value (Go
      // textproto joins with a space).
      if (last_key.empty() || resp.headers.empty())
        return std::unexpected("malformed MIME header line \"" + l + "\"");
      resp.headers.back().second += " " + trim_right(l);
      continue;
    }
    const std::size_t colon = l.find(':');
    if (colon == std::string::npos)
      return std::unexpected("malformed MIME header line \"" + l + "\"");
    const std::string key = ascii_lower(trim_ws(std::string_view(l).substr(0, colon)));
    if (key.empty()) return std::unexpected("malformed MIME header line \"" + l + "\"");
    std::string value = std::string(trim_ws(std::string_view(l).substr(colon + 1)));
    resp.headers.emplace_back(key, value);
    last_key = key;

    if (key == "content-length") {
      if (cl_seen)
        return std::unexpected("http: message contains multiple Content-Length headers");
      cl_seen = true;
      const std::string_view v = trim_ws(value);
      if (v.empty() || (v.front() != '-' && v.front() < '0') || v.front() > '9')
        return std::unexpected("invalid Content-Length \"" + value + "\"");
      std::int64_t cl = 0;
      auto [vp, ec2] = std::from_chars(v.data(), v.data() + v.size(), cl);
      if (ec2 != std::errc{} || vp != v.data() + v.size() || cl < 0)
        return std::unexpected("invalid Content-Length \"" + value + "\"");
      resp.content_length = cl;
    } else if (key == "transfer-encoding") {
      // Only "chunked" is supported; it must be the final encoding (Go).
      std::string_view tok = value;
      std::string_view last_tok;
      while (!tok.empty()) {
        const std::size_t comma = tok.find(',');
        last_tok = comma == std::string_view::npos ? tok : tok.substr(0, comma);
        if (ascii_lower(trim_ws(last_tok)) != "chunked")
          return std::unexpected("unsupported transfer encoding \"" +
                                 std::string(trim_ws(last_tok)) + "\"");
        if (comma == std::string_view::npos) break;
        tok.remove_prefix(comma + 1);
      }
      chunked = true;
      resp.content_length = -1;
    }
  }

  if (chunked) resp.content_length = -1;

  // live: any icy-* header (cliamp openSource: HasPrefix(strings.ToLower(key),
  // "icy-")). Keys are already lowercased.
  for (const auto& [k, v] : resp.headers) {
    (void)v;
    if (k.starts_with("icy-")) {
      resp.live = true;
      break;
    }
  }
  return resp;
}

std::expected<std::string, std::string> read_body(ByteSource& src, bool chunked,
                                                  std::size_t max_bytes) {
  std::string body;
  body.reserve(std::min(max_bytes, kReadChunk));

  std::string pending;
  std::string err;

  // A clean EOF ends a delimited-by-close body; the source's last_error
  // distinguishes transport errors (timeouts) from EOF.
  auto source_read = [&](std::span<char> dst) -> std::pair<std::size_t, bool> {
    if (!pending.empty()) {
      const std::size_t n = std::min(pending.size(), dst.size());
      std::memcpy(dst.data(), pending.data(), n);
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
      return {n, true};
    }
    return src.read(dst);
  };

  auto read_line_cap = [&](std::string& out) -> bool {
    out.clear();
    while (out.size() <= kMaxChunkLine) {
      char c = 0;
      auto [n, ok] = source_read(std::span<char>(&c, 1));
      if (!ok) {
        err = src.last_error().empty() ? "unexpected EOF" : src.last_error();
        return false;
      }
      if (c == '\n') {
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      out.push_back(c);
    }
    err = "chunk line too long";
    return false;
  };

  auto read_exact = [&](std::span<char> dst) -> bool {
    std::size_t got = 0;
    while (got < dst.size()) {
      auto [n, ok] = source_read(dst.subspan(got));
      if (!ok) {
        err = src.last_error().empty() ? "unexpected EOF" : src.last_error();
        return false;
      }
      got += n;
    }
    return true;
  };

  if (!chunked) {
    char buf[kReadChunk];
    while (body.size() < max_bytes) {
      const std::size_t want = std::min(kReadChunk, max_bytes - body.size());
      auto [n, ok] = source_read(std::span<char>(buf, want));
      if (!ok) {
        if (!src.last_error().empty()) return std::unexpected(src.last_error());
        break;  // clean EOF — Go's ReadAll stops here
      }
      body.append(buf, n);
    }
    return body;
  }

  // Chunked framing: size line (hex, optional ";ext"), chunk, CRLF, repeat;
  // a 0 chunk is followed by trailer headers up to a blank line.
  for (;;) {
    std::string size_line;
    if (!read_line_cap(size_line)) return std::unexpected(err);
    std::string_view hex = size_line;
    if (const std::size_t semi = hex.find(';'); semi != std::string_view::npos)
      hex = hex.substr(0, semi);
    hex = trim_ws(hex);
    if (hex.starts_with("0x") || hex.starts_with("0X")) hex.remove_prefix(2);
    unsigned long long sz = 0;
    auto [hp, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), sz, 16);
    if (ec != std::errc{} || hp != hex.data() + hex.size())
      return std::unexpected("malformed chunked encoding: bad chunk size");

    if (sz == 0) {
      // trailers: skip to blank line
      std::string trailer;
      for (;;) {
        if (!read_line_cap(trailer)) return std::unexpected(err);
        if (trailer.empty()) break;
      }
      break;
    }

    std::size_t remaining = static_cast<std::size_t>(sz);
    char tmp[16384];
    while (remaining > 0) {
      const std::size_t want = std::min<std::size_t>(remaining, sizeof(tmp));
      if (!read_exact(std::span<char>(tmp, want))) return std::unexpected(err);
      if (body.size() < max_bytes) {
        const std::size_t take = std::min(want, max_bytes - body.size());
        body.append(tmp, take);
      }
      remaining -= want;
    }
    char crlf[2];
    if (!read_exact(std::span<char>(crlf, 2))) return std::unexpected(err);
  }
  return body;
}

std::expected<ParsedUrl, std::string> parse_url(std::string_view url) {
  return parse_url_impl(url);
}

std::expected<std::string, std::string> resolve_location(std::string_view base_url,
                                                         std::string_view location) {
  return resolve_location_impl(base_url, location);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// public contract
// ---------------------------------------------------------------------------

bool is_icy_response(std::string_view first4) { return first4 == "ICY "; }

std::expected<HttpResponse, std::string>
HttpClient::open_stream(std::string_view url, std::chrono::milliseconds timeout) {
  return request(url, timeout, /*audio=*/true, /*keep_fd=*/true, /*max_bytes=*/0, cookies_from_);
}

std::expected<HttpResponse, std::string>
HttpClient::fetch_text(std::string_view url, std::size_t max_bytes,
                       std::chrono::milliseconds timeout) {
  return request(url, timeout, /*audio=*/false, /*keep_fd=*/false, max_bytes, cookies_from_);
}

void HttpClient::close_body(HttpResponse& resp) {
  if (resp.body_fd >= 0) {
    ::close(resp.body_fd);
    resp.body_fd = -1;
  }
}

}  // namespace bootamp::audio
