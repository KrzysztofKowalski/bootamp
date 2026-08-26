// foundation/resume.cpp — persist last-played track/position for resume.
//
// Faithful port of cliamp/internal/resume/resume.go. State is JSON-serialized
// to <config_dir>/resume.json. save() no-ops on empty path or non-positive
// position; load() returns a zero State on missing/unparseable file.
#include "foundation/resume.hpp"

#include "foundation/appdir.hpp"
#include "foundation/fileutil.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

namespace bootamp::foundation {

namespace fs = std::filesystem;

std::expected<fs::path, std::string> resume_state_file() {
  auto dir = config_dir();
  if (!dir) return std::unexpected{dir.error()};
  return *dir / "resume.json";
}

std::expected<void, std::string> resume_save(const ResumeState& s) {
  // No-op on empty path or non-positive position — matches cliamp's guard
  // against overwriting a valid resume file with useless data.
  if (s.path.empty() || s.position_sec <= 0) {
    return {};
  }

  auto f = resume_state_file();
  if (!f) return std::unexpected{f.error()};

  // Marshal JSON with the same field names and omitempty semantics as Go:
  //   {"path":"...","position_sec":N,"playlist":"..."}
  // playlist is omitted when empty (Go tag `omitempty`).
  nlohmann::json j;
  j["path"]         = s.path;
  j["position_sec"] = s.position_sec;
  if (!s.playlist.empty()) {
    j["playlist"] = s.playlist;
  }
  std::string data = j.dump();

  // cliamp uses os.MkdirAll(0755) + os.WriteFile(0600) — NOT the atomic writer.
  // We mirror that exactly (a truncated resume.json is harmless on crash).
  std::error_code ec;
  fs::create_directories(f->parent_path(), ec);
  if (ec) {
    return std::unexpected{"resume_save: create directory: " + ec.message()};
  }

  const std::string& p = f->string();
  int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return std::unexpected{"resume_save: open: " + std::string(std::strerror(errno))};
  }
  const char* buf = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0) {
    ::ssize_t n = ::write(fd, buf, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      int saved = errno;
      ::close(fd);
      return std::unexpected{"resume_save: write: " + std::string(std::strerror(saved))};
    }
    buf += static_cast<std::size_t>(n);
    remaining -= static_cast<std::size_t>(n);
  }
  if (::close(fd) < 0) {
    return std::unexpected{"resume_save: close: " + std::string(std::strerror(errno))};
  }
  return {};
}

std::expected<ResumeState, std::string> resume_load() {
  auto f = resume_state_file();
  if (!f) return std::unexpected{f.error()};

  auto data = read_file(*f);
  if (!data) {
    // Missing file (or read error) -> zero state, matching cliamp.
    return ResumeState{};
  }

  try {
    auto j = nlohmann::json::parse(*data);
    ResumeState s;
    if (j.contains("path") && j["path"].is_string()) {
      s.path = j["path"].get<std::string>();
    }
    if (j.contains("position_sec") && j["position_sec"].is_number_integer()) {
      s.position_sec = j["position_sec"].get<int>();
    }
    if (j.contains("playlist") && j["playlist"].is_string()) {
      s.playlist = j["playlist"].get<std::string>();
    }
    return s;
  } catch (const std::exception&) {
    // Corrupt file -> zero state, matching cliamp's json.Unmarshal fallback.
    return ResumeState{};
  }
}

}  // namespace bootamp::foundation