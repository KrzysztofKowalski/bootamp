// foundation/fileutil.hpp — atomic file writes and path helpers.
//
// Port of cliamp/internal/fileutil (write_file_atomic: tmp + rename + fsync).
// Atomic writes are used for config, favorites, and resume state so a crash
// mid-write never leaves a truncated file.
#pragma once

#include <cstddef>
#include <filesystem>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace bootamp::foundation {

namespace fs = std::filesystem;

// write_file_atomic writes `contents` to `path` by:
//   1. writing to a sibling temp file (path + ".tmp.<pid>"),
//   2. fsync-ing it,
//   3. renaming over the destination (atomic on POSIX).
// `mode` is the file permission bits (e.g. 0600). Returns {} on success or an
// error message. Matches cliamp's fileutil.WriteFileAtomic semantics.
std::expected<void, std::string>
write_file_atomic(const fs::path& path, std::span<const std::byte> contents,
                   unsigned mode = 0600);

// Convenience overload for string payloads.
std::expected<void, std::string>
write_file_atomic(const fs::path& path, std::string_view contents,
                   unsigned mode = 0600);

// read_file reads the entire file into a string. Returns an error message on
// failure (missing file, read error). Used by config/favorites/resume.
std::expected<std::string, std::string> read_file(const fs::path& path);

// copy_file copies the file at `src` to `dst`, overwriting `dst` if it already
// exists. A partial destination is removed on error so callers never see a
// truncated file. Port of cliamp fileutil.CopyFile.
std::expected<void, std::string> copy_file(const fs::path& src, const fs::path& dst);

// sync_dir fsyncs the directory so a preceding rename is durable across
// crashes. Port of cliamp fileutil.syncDir (unix).
std::expected<void, std::string> sync_dir(const fs::path& dir);

}  // namespace bootamp::foundation