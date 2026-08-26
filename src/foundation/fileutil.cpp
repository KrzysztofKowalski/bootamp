// foundation/fileutil.cpp — atomic file writes and path helpers.
//
// Faithful port of cliamp/internal/fileutil (atomic.go, copy.go, sync_unix.go).
// write_file_atomic preserves the Go semantics 1:1:
//   - mkdir -p parent, chmod parent 0700
//   - if the destination exists, AND the requested perm with its current mode
//     bits (so a stricter existing mode wins — see atomic_test.go)
//   - write a sibling temp file (.tmp-XXXXXX), chmod, fsync, close
//   - rename over the destination (atomic on POSIX)
//   - fsync the parent directory
#include "foundation/fileutil.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace bootamp::foundation {

namespace fs = std::filesystem;

namespace {

// wrap_errno returns a std::string for the current errno, prefixed by ctx.
std::string wrap_errno(const std::string& ctx) {
  return ctx + ": " + std::strerror(errno);
}

// mode_bits returns the low 9 permission bits of a stat result.
unsigned mode_bits(struct ::stat& st) {
  return static_cast<unsigned>(st.st_mode & 0777);
}

}  // namespace

std::expected<void, std::string>
write_file_atomic(const fs::path& path, std::span<const std::byte> contents,
                   unsigned mode) {
  const fs::path dir = path.parent_path();
  if (dir.empty()) {
    return std::unexpected{"create directory: empty parent path"};
  }

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return std::unexpected{"create directory: " + ec.message()};
  }
  fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
  if (ec) {
    return std::unexpected{"secure directory: " + ec.message()};
  }

  // If the destination already exists, AND the requested perm with its
  // current bits — mirrors Go's `perm &= info.Mode().Perm()`.
  struct ::stat existing{};
  if (::stat(path.c_str(), &existing) == 0) {
    mode &= mode_bits(existing);
  } else if (errno != ENOENT) {
    return std::unexpected{std::string{"inspect existing file: "} + std::strerror(errno)};
  }

  // Create a sibling temp file via mkstemp (atomically unique).
  std::string tmpl = (dir / ".tmp-XXXXXX").string();
  int fd = ::mkstemp(tmpl.data());
  if (fd < 0) {
    return std::unexpected{std::string{"create temporary file: "} + std::strerror(errno)};
  }
  fs::path tmp_path{tmpl};

  auto cleanup = [&] {
    ::close(fd);
    std::error_code rc;
    fs::remove(tmp_path, rc);
  };

  if (::fchmod(fd, static_cast<::mode_t>(mode)) < 0) {
    std::string e = wrap_errno("set temporary file permissions");
    cleanup();
    return std::unexpected{e};
  }

  // Write the whole payload, retrying on short writes / EINTR.
  {
    const auto* p = reinterpret_cast<const char*>(contents.data());
    std::size_t remaining = contents.size();
    while (remaining > 0) {
      ::ssize_t n = ::write(fd, p, remaining);
      if (n < 0) {
        if (errno == EINTR) continue;
        std::string e = wrap_errno("write temporary file");
        cleanup();
        return std::unexpected{e};
      }
      p += static_cast<std::size_t>(n);
      remaining -= static_cast<std::size_t>(n);
    }
  }

  if (::fsync(fd) < 0) {
    std::string e = wrap_errno("sync temporary file");
    cleanup();
    return std::unexpected{e};
  }

  if (::close(fd) < 0) {
    std::string e = wrap_errno("close temporary file");
    fd = -1;
    cleanup();
    return std::unexpected{e};
  }
  fd = -1;  // ownership transferred to the path; do not double-close.

  std::error_code ren_ec;
  fs::rename(tmp_path, path, ren_ec);
  if (ren_ec) {
    std::error_code rc;
    fs::remove(tmp_path, rc);
    return std::unexpected{"replace file: " + ren_ec.message()};
  }

  if (auto r = sync_dir(dir); !r) {
    return std::unexpected{"sync parent directory: " + r.error()};
  }
  return {};
}

std::expected<void, std::string>
write_file_atomic(const fs::path& path, std::string_view contents, unsigned mode) {
  return write_file_atomic(
      path,
      std::span<const std::byte>{reinterpret_cast<const std::byte*>(contents.data()),
                                    contents.size()},
      mode);
}

std::expected<std::string, std::string> read_file(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return std::unexpected{"read file: " + path.string() + ": no such file"};
  }
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return std::unexpected{"read file: " + path.string() + ": cannot open"};
  }
  std::string out;
  // Read in chunks to avoid seeking issues with pipes/special files.
  constexpr std::size_t kChunk = 64 * 1024;
  std::array<char, kChunk> buf{};
  while (in) {
    in.read(buf.data(), kChunk);
    out.append(buf.data(), static_cast<std::size_t>(in.gcount()));
  }
  return out;
}

std::expected<void, std::string> copy_file(const fs::path& src, const fs::path& dst) {
  int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    return std::unexpected{wrap_errno("copy_file: open source")};
  }

  int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    std::string e = wrap_errno("copy_file: open destination");
    ::close(in);
    return std::unexpected{e};
  }

  auto fail = [&](const std::string& ctx) {
    ::close(in);
    ::close(out);
    std::error_code rc;
    fs::remove(dst, rc);  // clean up partial file, matching cliamp
    return std::unexpected{ctx};
  };

  // Preserve the source mode on the destination (matches a std::filesystem
  // copy, but Go's CopyFile does not set mode; cliamp uses it for cache
  // copies where the destination is freshly created. We mirror the source
  // bits so executable/readonly caches keep their permissions).
  struct ::stat st{};
  if (::fstat(in, &st) == 0) {
    ::fchmod(out, st.st_mode & 0777);
  }

  std::array<char, 64 * 1024> buf{};
  for (;;) {
    ::ssize_t n = ::read(in, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return fail(wrap_errno("copy_file: read"));
    }
    if (n == 0) break;
    std::size_t off = 0;
    while (off < static_cast<std::size_t>(n)) {
      ::ssize_t w = ::write(out, buf.data() + off, static_cast<std::size_t>(n) - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        return fail(wrap_errno("copy_file: write"));
      }
      off += static_cast<std::size_t>(w);
    }
  }

  if (::close(out) < 0) {
    out = -1;
    return fail(wrap_errno("copy_file: close destination"));
  }
  ::close(in);
  return {};
}

std::expected<void, std::string> sync_dir(const fs::path& dir) {
  int d = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (d < 0) {
    return std::unexpected{wrap_errno("sync_dir: open")};
  }
  int r = ::fsync(d);
  int saved = errno;
  ::close(d);
  if (r < 0) {
    errno = saved;
    return std::unexpected{wrap_errno("sync_dir: fsync")};
  }
  return {};
}

}  // namespace bootamp::foundation