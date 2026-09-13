// foundation/disk_cache.hpp — small persistent byte cache on disk.
//
// New module (no cliamp counterpart): a TTL'd key/value cache rooted at
// <XDG_CACHE_HOME>/bootamp (fallback ~/.cache/bootamp), first consumer is the
// Gieres archive client (ui/gieres_client.cpp) which reuses HTTP GET bodies
// instead of hitting the LAN server on every repeat fetch. Design mirrors the
// foundation's existing idioms: std::expected error strings, appdir's env
// resolution, write_file_atomic for crash-safe persistence (tmp + rename), a
// plain std::mutex for thread-safety (the applog pattern). std only — no
// network, no logging.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::foundation {

namespace fs = std::filesystem;

// DiskCache is a TTL byte cache safe for concurrent use. Keys carry a
// namespace as their first path segment ("gieres/http://…"), which maps to a
// subdirectory under the cache root — evict_namespace() drops one whole
// subdirectory at a time. Every entry is an opaque file: a fixed header
// (magic, unix-second expiry + written timestamps, the raw key for hash
// collision checks) followed by the payload bytes, written atomically.
//
// Reads never throw and never propagate cache trouble to the caller: an
// expired or corrupt entry is an empty miss AND is purged from disk (the
// caller treats it as "go fetch"). A global byte cap (default 32 GiB — the
// user's call, with plenty of disk to spare; the module also caches audio
// streams, tens of MB per entry) is enforced on put, oldest-first by the
// entry's written timestamp. The cap is a named constant and a constructor
// parameter, so a build or config can override it. Entries larger than the
// cap are rejected outright rather than evicting the whole cache for them.
// get() returns the payload by value (one linear copy — tolerable even for
// stream-sized entries); the index only ever keeps (path, written, size),
// never the payload.
class DiskCache {
public:
  // Clock: unix seconds. The default reads the wall clock; tests inject a
  // fake one to exercise TTL expiry without sleeping.
  using NowFn = std::function<std::int64_t()>;

  // 32 GiB per the user's explicit decision (2026-09-13) — remote radio
  // audio, not just metadata, may ride through this cache one day.
  static constexpr std::int64_t kDefaultSizeCap = 32LL * 1024 * 1024 * 1024;

  // Default root: <XDG_CACHE_HOME>/bootamp, else ~/.cache/bootamp (appdir
  // style). Wall clock, kDefaultSizeCap. No I/O happens until the first
  // operation.
  DiskCache();
  // Injected root + clock + cap (tests). `root` is used verbatim — no env
  // resolution, no HOME fallback.
  DiskCache(fs::path root, NowFn now, std::int64_t size_cap);

  // Overrides the byte cap at runtime — the future home of the `[cache]
  // max_size` ini key (src/config is off-limits to this module right now, so
  // the config plumbing reads the key and drops it here). A smaller cap
  // evicts down to it immediately; the constructor parameter does the same.
  void set_max_size(std::int64_t bytes);

  // Stores `contents` under `key` ("<ns>/<rest>"). ttl_seconds > 0 sets the
  // lifetime; 0 or negative means no expiry. Fails (without writing) when the
  // cache root is unusable or a single entry exceeds the size cap — the
  // caller decides whether an error is fatal, it never corrupts an existing
  // entry (atomic replace).
  std::expected<void, std::string>
  put(std::string_view key, std::span<const std::byte> contents,
      std::int64_t ttl_seconds);
  // String payload convenience (bodies arrive as strings from read_file).
  std::expected<void, std::string>
  put(std::string_view key, std::string_view contents, std::int64_t ttl_seconds);

  // Returns the stored bytes on a fresh hit; std::nullopt on a miss —
  // missing key, expired entry, corrupt content (all purged from disk) or an
  // unavailable cache root. A miss is the caller's "refetch" signal, never a
  // hard error.
  std::optional<std::string> get(std::string_view key);

  // Removes every key under `namespace_prefix` (a single path segment — the
  // whole <root>/<ns> directory) and returns how many entries were dropped.
  // 0 when the namespace holds nothing.
  std::expected<std::size_t, std::string>
  evict_namespace(std::string_view namespace_prefix);

  // The resolved cache root ("" until the first operation resolves it).
  const fs::path& root() const { return root_; }

  // Deterministic, filename-safe name for `key` (FNV-1a 64, lowercase hex).
  // Exposed so tests can plant corrupt files at the exact cache path.
  static std::string key_filename(std::string_view key);
  // Path-safe encoding of a namespace segment (percent-escape; unreserved
  // ASCII kept). Exposed for the same corrupt-file tests.
  static std::string encode_namespace(std::string_view ns);

private:
  struct Entry {
    std::string  rel;      // path relative to the root ("<ns>/<hash>")
    std::int64_t written;  // unix seconds at write time (header; 0 unknown)
    std::int64_t size;     // bytes on disk (header + payload)
  };
  // ensure_root_locked resolves root_ (env for the default ctor), creates it
  // with 0700 and builds the initial entry index. Idempotent.
  std::expected<void, std::string> ensure_root_locked();
  // rescan_locked rebuilds index_/total_size_ from the directory — ground
  // truth for eviction when the index has drifted (foreign files, leftover
  // .tmp- artifacts are never counted).
  void rescan_locked();
  // Re-reads the written timestamp from one entry file's header (0 when the
  // header is unreadable — such entries are evicted first).
  std::int64_t read_written_locked(const fs::path& file) const;
  // evict_oldest_locked removes index_ entries oldest-first (written time,
  // path tie-break) until total_size_ <= cap_. Never removes .tmp- leftovers.
  void evict_oldest_locked();
  // upsert_index_locked records/refreshes one entry in the index.
  void upsert_index_locked(const fs::path& file, std::int64_t written,
                           std::int64_t size);
  // purge_path_locked deletes one file and its index entry (expired /
  // corrupt / evicted).
  void purge_path_locked(const fs::path& file);

  std::mutex          mu_;
  fs::path            root_;        // resolved lazily; "" = not yet
  NowFn               now_;         // unix seconds
  std::int64_t        cap_ = kDefaultSizeCap;
  bool                initialized_ = false;
  std::vector<Entry>  index_;
  std::int64_t        total_size_ = 0;
};

}  // namespace bootamp::foundation