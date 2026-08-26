// foundation/applog.hpp — dual-sink logging for bootamp.
//
// Port of cliamp/applog/applog.go. Two sinks are layered behind one API:
//
//   - A file sink, written through spdlog, for diagnostic logs the user reads
//     after the fact (~/.config/bootamp/bootamp.log).
//   - An in-memory ring buffer (capacity 4) drained by the TUI status line for
//     short-lived, user-facing messages. The buffer exists because writing to
//     stderr would corrupt the TUI.
//
// Callers pick by intent, not sink:
//
//   - debug/info/warn/error  -> file only.
//   - status                -> ring only (transient UI feedback).
//   - user_warn/user_error  -> both (file + ring).
//
// init() must be called once at startup. Calls before init are silently
// dropped on the file side (a null/discard spdlog logger is installed by
// default); the ring always works. This mirrors Go's `init()` installing a
// discard handler.
//
// Formatting follows Go's fmt.Sprintf semantics: callers pass a runtime format
// string + args, formatted via std::vformat (C++23). The level guard avoids
// the formatting cost when the level is filtered out (matching Go's Enabled
// gate in logf).
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace bootamp::foundation::applog {

// Level mirrors slog.Level for the file sink threshold.
enum class Level : std::int8_t {
  debug = 0,
  info  = 1,
  warn  = 2,
  error = 3,
};

// Entry is a single footer message with a timestamp (port of Go applog.Entry).
struct Entry {
  std::string text;
  std::chrono::system_clock::time_point at;
};

// Maximum number of footer entries retained for the status line (Go: 4).
inline constexpr std::size_t max_entries = 4;

// CloseFn closes the underlying log file and resets the logger to a discard
// sink. Idempotent. Returned by init(); calling it is optional (the file is
// also closed if init() is called again). The previously returned CloseFn
// becomes a no-op once a later init() has swapped handlers.
using CloseFn = std::function<void()>;

// parse_level maps a string to a Level. Empty input maps to Level::info.
// "warning" is accepted as an alias for "warn" (case-insensitive, trimmed).
// Returns std::unexpected on unrecognised input.
auto parse_level(std::string_view s) -> std::expected<Level, std::string>;

// level_name returns the lowercase canonical name for a level
// ("debug"/"info"/"warn"/"error").
auto level_name(Level l) -> std::string_view;

// init opens `path` for append, installs a spdlog text logger at the given
// level, and returns a close function. Creates the parent directory with 0755
// (matching Go's os.MkdirAll(filepath.Dir(path), 0o755)). Calling init twice
// closes the previous file before swapping handlers. Pre-init diagnostic
// calls are dropped.
auto init(const std::filesystem::path& path, Level level)
    -> std::expected<CloseFn, std::string>;

// set_level changes the spdlog level threshold at runtime.
void set_level(Level level);

// ---- file-only diagnostic logs ----

// should_log returns whether the file logger would emit at `level`. Used by
// the templated debug/info/warn/error to gate the format cost (Go's Enabled).
auto should_log(Level level) -> bool;

// emit_file writes an already-formatted message to the file sink at `level`.
// No gating; caller must gate via should_log first.
void emit_file(Level level, std::string_view msg);

// debug/info/warn/error log only to the file. Runtime format string + args,
// std::format-style ({} placeholders). The level guard avoids the format
// cost when filtered out.
template <typename... Args>
void debug(std::string_view fmt, Args&&... args) {
  if (should_log(Level::debug)) {
    emit_file(Level::debug, std::vformat(fmt, std::make_format_args(args...)));
  }
}
template <typename... Args>
void info(std::string_view fmt, Args&&... args) {
  if (should_log(Level::info)) {
    emit_file(Level::info, std::vformat(fmt, std::make_format_args(args...)));
  }
}
template <typename... Args>
void warn(std::string_view fmt, Args&&... args) {
  if (should_log(Level::warn)) {
    emit_file(Level::warn, std::vformat(fmt, std::make_format_args(args...)));
  }
}
template <typename... Args>
void error(std::string_view fmt, Args&&... args) {
  if (should_log(Level::error)) {
    emit_file(Level::error, std::vformat(fmt, std::make_format_args(args...)));
  }
}

// ---- ring-only / dual-sink user logs ----

// push_ring appends a message to the footer ring, trimming trailing newlines
// and dropping empty results. Exposed for the user_* templates below.
void push_ring(std::string msg);

// status pushes a message into the ring only (no file write). Use for
// ephemeral, user-facing notifications that wouldn't help post-mortem
// debugging.
template <typename... Args>
void status(std::string_view fmt, Args&&... args) {
  push_ring(std::vformat(fmt, std::make_format_args(args...)));
}

// user_warn/user_error log to the file AND push the same formatted message
// into the ring. The format cost is paid unconditionally because the ring
// needs the formatted string (no level gate) — matching Go's UserWarn/UserError.
template <typename... Args>
void user_warn(std::string_view fmt, Args&&... args) {
  auto msg = std::vformat(fmt, std::make_format_args(args...));
  emit_file(Level::warn, msg);
  push_ring(std::move(msg));
}
template <typename... Args>
void user_error(std::string_view fmt, Args&&... args) {
  auto msg = std::vformat(fmt, std::make_format_args(args...));
  emit_file(Level::error, msg);
  push_ring(std::move(msg));
}

// ---- ring queries ----

// ring_entries returns a snapshot of the current footer ring (oldest-first,
// at most max_entries). Does NOT clear the buffer — the status line polls
// this each tick. Thread-safe.
auto ring_entries() -> std::vector<Entry>;

// drain returns and clears all buffered footer entries (oldest-first). The
// returned vector is empty if the buffer is empty. Port of Go applog.Drain.
auto drain() -> std::vector<Entry>;

}  // namespace bootamp::foundation::applog