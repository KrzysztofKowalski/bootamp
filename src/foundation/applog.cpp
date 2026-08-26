// foundation/applog.cpp — dual-sink logging implementation.
//
// Port of cliamp/applog/applog.go. The file sink is spdlog (basic_file_sink,
// append mode); the ring is a plain std::vector guarded by a mutex. The
// logger is held in a std::atomic<std::shared_ptr<spdlog::logger>> so file
// reads (should_log/emit_file) are lock-free, mirroring Go's atomic.Pointer.
// The ring + current-file close token share a mutex (Go's `mu`+`currentFile`).
#include "foundation/applog.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>

namespace bootamp::foundation::applog {
namespace {

namespace fs = std::filesystem;

// The active file logger. Atomic shared_ptr gives lock-free reads on the
// hot path (Go: atomic.Pointer[slog.Logger]). Defaults to a null/discard
// sink so pre-init diagnostic calls are silently dropped.
std::atomic<std::shared_ptr<spdlog::logger>> g_logger{nullptr};

// Discard logger installed at first use and on close, matching Go's
// slog.NewTextHandler(io.Discard). Lazy-initialised.
auto discard_logger() -> std::shared_ptr<spdlog::logger> {
  static const auto k_discard =
      std::make_shared<spdlog::logger>("bootamp",
          std::make_shared<spdlog::sinks::null_sink_mt>());
  return k_discard;
}

// Ring buffer shares this mutex (Go: `mu`, `entries`). The active file's
// lifetime is owned by the spdlog sink inside the logger shared_ptr, so no
// separate file handle is needed here.
std::mutex g_mu;
std::vector<Entry> g_entries;

auto to_spdlog_level(Level l) -> spdlog::level::level_enum {
  switch (l) {
    case Level::debug: return spdlog::level::debug;
    case Level::info:  return spdlog::level::info;
    case Level::warn:  return spdlog::level::warn;
    case Level::error: return spdlog::level::err;
  }
  return spdlog::level::info;
}

auto current_logger() -> std::shared_ptr<spdlog::logger> {
  auto p = g_logger.load(std::memory_order_acquire);
  if (p) return p;
  // Install (and cache) the discard logger so repeated pre-init calls don't
  // race on every load. CAS ensures only one writer wins.
  auto disc = discard_logger();
  if (g_logger.compare_exchange_strong(p, disc, std::memory_order_acq_rel)) {
    return disc;
  }
  return p ? p : disc;
}

}  // namespace

auto parse_level(std::string_view s) -> std::expected<Level, std::string> {
  // Trim whitespace.
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  auto first = std::find_if_not(s.begin(), s.end(), is_space);
  auto last = std::find_if_not(s.rbegin(), s.rend(), is_space).base();
  if (first >= last) return Level::info;  // empty -> info (Go behaviour)
  std::string trimmed(first, last);

  // Case-insensitive compare (Go uses strings.EqualFold + UnmarshalText).
  auto lower = trimmed;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "warning") return Level::warn;  // alias
  if (lower == "debug") return Level::debug;
  if (lower == "info")  return Level::info;
  if (lower == "warn")   return Level::warn;
  if (lower == "error")  return Level::error;
  return std::unexpected(std::string(
      "invalid log level \"" + trimmed +
      "\" (want debug|info|warn|error)"));
}

auto level_name(Level l) -> std::string_view {
  switch (l) {
    case Level::debug: return "debug";
    case Level::info:  return "info";
    case Level::warn:  return "warn";
    case Level::error: return "error";
  }
  return "info";
}

auto init(const fs::path& path, Level level)
    -> std::expected<CloseFn, std::string> {
  // Create parent directory (Go: os.MkdirAll(filepath.Dir(path), 0o755)).
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      return std::unexpected(std::string("create log dir: ") + ec.message());
    }
  }

  // Open for append (Go: O_APPEND|O_CREATE|O_WRONLY, 0o644).
  // spdlog's basic_file_sink_mt opens in append mode by default (truncate=
  // false) and creates the file if missing — matching Go's OpenFile flags. We
  // let spdlog own the file handle; the returned CloseFn flushes + drops the
  // logger to the discard sink, whose destructor closes the file.
  auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      path.string(), /*truncate=*/false);
  auto lg = std::make_shared<spdlog::logger>("bootamp", sink);
  lg->set_level(to_spdlog_level(level));
  lg->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");  // text-handler-like
  lg->flush_on(spdlog::level::warn);

  // Swap the logger (lock-free reads pick up the new one).
  auto prev = g_logger.exchange(lg, std::memory_order_acq_rel);

  // Close the previous file (if any) before the new one stays open. spdlog
  // closes its file when the sink is destroyed; dropping the last reference
  // to `prev` (if it held a file sink) closes it. We drop it here.
  (void)prev;  // released when `prev` goes out of scope below.

  // The returned close function flushes + swaps back to the discard logger,
  // which closes the file (last sink ref drops). Idempotent via a shared
  // once-flag per init() call.
  auto closed = std::make_shared<std::once_flag>();
  return CloseFn([lg, closed]() mutable {
    std::call_once(*closed, [lg]() mutable {
      lg->flush();
      // Swap to discard if `lg` is still the active logger; compare_exchange
      // updates `expected` on failure (harmless here). `mutable` so the
      // by-value `lg` capture is non-const and can bind to the expected ref.
      auto expected = lg;
      g_logger.compare_exchange_strong(expected, discard_logger(),
                                       std::memory_order_acq_rel);
      // `lg` (and its file sink) destroyed when these captured copies go out
      // of scope at end of std::call_once -> file closed.
    });
  });
}

void set_level(Level level) {
  auto lg = current_logger();
  lg->set_level(to_spdlog_level(level));
}

auto should_log(Level level) -> bool {
  auto lg = current_logger();
  return lg->should_log(to_spdlog_level(level));
}

void emit_file(Level level, std::string_view msg) {
  auto lg = current_logger();
  // source_location defaulted -> spdlog records no source info (matches Go's
  // slog text handler which has no source).
  lg->log(to_spdlog_level(level), msg);
}

void push_ring(std::string msg) {
  // Go: strings.TrimRight(msg, "\n").
  while (!msg.empty() && msg.back() == '\n') msg.pop_back();
  if (msg.empty()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_entries.push_back(Entry{std::move(msg), std::chrono::system_clock::now()});
  if (g_entries.size() > max_entries) {
    // Keep the last max_entries (Go: entries[len-max:]).
    g_entries.erase(g_entries.begin(),
                    g_entries.begin() + (g_entries.size() - max_entries));
  }
}

auto ring_entries() -> std::vector<Entry> {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_entries;  // copy, oldest-first
}

auto drain() -> std::vector<Entry> {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_entries.empty()) return {};
  std::vector<Entry> out;
  out.swap(g_entries);
  return out;
}

}  // namespace bootamp::foundation::applog