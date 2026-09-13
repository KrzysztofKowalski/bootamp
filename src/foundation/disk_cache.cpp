// foundation/disk_cache.cpp — implementation (contract:
// foundation/disk_cache.hpp). Layout: <root>/<ns-encoded>/<hash16>, one file
// per key prefixed by a fixed header (magic + unix-second expiry/written +
// raw key) so TTL, eviction order and hash-collision checks never depend on
// the filesystem metadata. Writes go through write_file_atomic (tmp + rename
// + fsync); a crash mid-write leaves at most a `.tmp-*` leftover, which
// accounting and eviction both ignore.
#include "foundation/disk_cache.hpp"

#include "foundation/fileutil.hpp"

#include <cstdlib>
#include <ctime>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <utility>

namespace bootamp::foundation {

namespace fs = std::filesystem;

namespace {

// Cache file header (all integers big-endian):
//   magic    8 bytes  "BCACH001"
//   expiry   8 bytes  unix seconds; 0 = no expiry
//   written  8 bytes  unix seconds at write time (the eviction clock)
//   key_len  4 bytes  uint32
//   key      key_len bytes (raw, for collision checks)
//   payload  the rest
constexpr std::string_view kMagic      = "BCACH001";
constexpr std::uint64_t     kHeaderFixed = 8 + 8 + 8 + 4;
constexpr std::int64_t      kNoExpiry   = 0;

// env_or_empty returns the value of `name` if set and non-empty, else "".
// Mirrors the appdir.cpp helper (same env conventions in this layer).
std::string env_or_empty(const char* name) {
  if (const char* v = std::getenv(name)) {
    if (v[0] != '\0') return v;
  }
  return {};
}

// user_home_dir mirrors Go's os.UserHomeDir() on Linux: $HOME first, then the
// password database for the current uid. Returns empty on failure. Duplicated
// from appdir.cpp to keep the cache self-contained (appdir never needed a
// cache dir).
std::string user_home_dir() {
  if (std::string h = env_or_empty("HOME"); !h.empty()) return h;
  const uid_t uid = getuid();
  if (auto* pw = getpwuid(uid); pw != nullptr && pw->pw_dir != nullptr) {
    return pw->pw_dir;
  }
  return {};
}

void append_u32(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>(v & 0xFF));
}

void append_i64(std::string& out, std::int64_t v) {
  const std::uint64_t u = static_cast<std::uint64_t>(v);
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
  }
}

std::uint32_t read_u32(std::string_view s, std::size_t off) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3]));
}

std::int64_t read_i64(std::string_view s, std::size_t off) {
  std::uint64_t u = 0;
  for (int i = 0; i < 8; ++i) {
    u = (u << 8) | static_cast<unsigned char>(s[off + i]);
  }
  return static_cast<std::int64_t>(u);
}

// fnv1a64 is the (deterministic, offline) name hash — identical keys always
// map to the same file; the raw key in the header guards the 2^-64 collision.
std::uint64_t fnv1a64(std::string_view s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

// is_tmp_leftover matches write_file_atomic's ".tmp-XXXXXX" scratch names —
// leftovers of a crash between mkstemp and rename; never counted or evicted.
bool is_tmp_leftover(const fs::path& p) {
  const std::string name = p.filename().string();
  return name.rfind(".tmp-", 0) == 0;
}

}  // namespace

DiskCache::DiskCache()
    : now_(+[] { return static_cast<std::int64_t>(::time(nullptr)); }) {}

DiskCache::DiskCache(fs::path root, NowFn now, std::int64_t size_cap)
    : root_(std::move(root)), now_(std::move(now)), cap_(size_cap) {}

void DiskCache::set_max_size(const std::int64_t bytes) {
  if (bytes <= 0) return;  // a cap of 0 would evict the whole cache on put
  std::lock_guard<std::mutex> lk(mu_);
  cap_ = bytes;
  if (initialized_) evict_oldest_locked();  // shrink below a smaller cap now
}

std::string DiskCache::key_filename(const std::string_view key) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::uint64_t h = fnv1a64(key);
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[h & 0x0F];
    h >>= 4;
  }
  return out;
}

std::string DiskCache::encode_namespace(const std::string_view ns) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(ns.size());
  for (const unsigned char c : ns) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      out.push_back(static_cast<char>(c));
    } else {
      out += '%';
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::expected<void, std::string> DiskCache::ensure_root_locked() {
  if (initialized_) return {};
  if (root_.empty()) {
    // $XDG_CACHE_HOME/bootamp, else ~/.cache/bootamp (appdir precedence).
    if (std::string x = env_or_empty("XDG_CACHE_HOME"); !x.empty()) {
      root_ = fs::path{x} / "bootamp";
    } else if (std::string h = env_or_empty("HOME"); !h.empty()) {
      root_ = fs::path{h} / ".cache" / "bootamp";
    } else {
      // `x` and `h` from the chain are still in scope here — use a fresh name.
      std::string home = user_home_dir();
      if (home.empty()) {
        return std::unexpected{"cache: could not determine home directory"};
      }
      root_ = fs::path{home} / ".cache" / "bootamp";
    }
  }
  std::error_code ec;
  fs::create_directories(root_, ec);
  if (ec) {
    return std::unexpected{"cache: create " + root_.string() + ": " + ec.message()};
  }
  // Match the config/data dirs' 0700 privacy (the cache holds radio metadata).
  fs::permissions(root_, fs::perms::owner_all, fs::perm_options::replace, ec);
  if (ec) {
    return std::unexpected{"cache: secure " + root_.string() + ": " + ec.message()};
  }
  rescan_locked();
  initialized_ = true;
  return {};
}

std::int64_t DiskCache::read_written_locked(const fs::path& file) const {
  std::ifstream in{file, std::ios::binary};
  std::string head(kHeaderFixed, '\0');
  if (!in || !in.read(head.data(), static_cast<std::streamsize>(kHeaderFixed))) {
    return 0;  // unreadable/truncated header → oldest, evicted first
  }
  if (std::string_view{head}.substr(0, kMagic.size()) != kMagic) return 0;
  return read_i64(head, 16);
}

void DiskCache::rescan_locked() {
  index_.clear();
  total_size_ = 0;
  std::error_code ec;
  if (root_.empty() || !fs::is_directory(root_, ec)) return;
  for (fs::recursive_directory_iterator it{root_, ec}, end; it != end;
       it.increment(ec)) {
    if (ec) {  // skip what we cannot read, keep going
      ec.clear();
      continue;
    }
    std::error_code fec;
    if (it->is_directory(fec) || is_tmp_leftover(it->path())) continue;
    Entry e;
    e.rel     = it->path().lexically_relative(root_).string();
    e.written = read_written_locked(it->path());
    e.size    = static_cast<std::int64_t>(it->file_size(fec));
    if (fec || e.size < 0) e.size = 0;
    index_.push_back(std::move(e));
    total_size_ += e.size;
  }
}

void DiskCache::evict_oldest_locked() {
  // Oldest-first until the cap holds. If nothing more can be removed while
  // still over the cap, the index drifted (files deleted by another process,
  // sizes changed under us) — rescan for ground truth once and retry, then
  // give up rather than spin.
  for (int pass = 0; pass < 2 && total_size_ > cap_; ++pass) {
    std::vector<Entry> order = index_;
    std::sort(order.begin(), order.end(), [](const Entry& a, const Entry& b) {
      if (a.written != b.written) return a.written < b.written;
      return a.rel < b.rel;  // deterministic tie-break
    });
    for (const Entry& e : order) {
      if (total_size_ <= cap_) break;
      std::error_code ec;
      fs::remove(root_ / e.rel, ec);
      if (ec) continue;  // already gone — index drift tolerated
      total_size_ -= e.size;
      index_.erase(std::remove_if(index_.begin(), index_.end(),
                                  [&](const Entry& x) { return x.rel == e.rel; }),
                   index_.end());
    }
    if (total_size_ > cap_) rescan_locked();
  }
}

void DiskCache::upsert_index_locked(const fs::path& file, std::int64_t written,
                                    std::int64_t size) {
  const std::string rel = file.lexically_relative(root_).string();
  for (Entry& e : index_) {
    if (e.rel == rel) {
      total_size_ -= e.size;  // replaces an old entry's byte count
      e.written = written;
      e.size    = size;
      total_size_ += size;
      return;
    }
  }
  index_.push_back(Entry{rel, written, size});
  total_size_ += size;
}

void DiskCache::purge_path_locked(const fs::path& file) {
  const std::string rel = file.lexically_relative(root_).string();
  std::error_code ec;
  fs::remove(file, ec);
  for (auto it = index_.begin(); it != index_.end(); ++it) {
    if (it->rel == rel) {
      total_size_ -= it->size;
      index_.erase(it);
      return;
    }
  }
}

std::expected<void, std::string>
DiskCache::put(const std::string_view key, const std::span<const std::byte> contents,
               const std::int64_t ttl_seconds) {
  if (key.empty()) return std::unexpected{"cache: empty key"};
  std::lock_guard<std::mutex> lk(mu_);
  if (auto r = ensure_root_locked(); !r) return r;

  const std::int64_t now    = now_();
  const std::int64_t expiry = ttl_seconds > 0 ? now + ttl_seconds : kNoExpiry;
  // Namespace = the first path segment; a key without '/' uses itself.
  const std::string  ns  = std::string{key.substr(0, key.find('/'))};
  const fs::path     dir = root_ / encode_namespace(ns);
  const fs::path     file = dir / key_filename(key);

  std::string blob;
  blob.reserve(static_cast<std::size_t>(kHeaderFixed) + key.size() +
               contents.size());
  blob += kMagic;
  append_i64(blob, expiry);
  append_i64(blob, now);
  append_u32(blob, static_cast<std::uint32_t>(key.size()));
  blob.append(key);
  blob.append(reinterpret_cast<const char*>(contents.data()), contents.size());
  if (static_cast<std::int64_t>(blob.size()) > cap_) {
    // An entry that cannot fit is rejected outright — evicting the whole
    // cache for it would defeat the purpose.
    return std::unexpected{"cache: entry larger than the size cap"};
  }
  if (auto w = write_file_atomic(file, std::string_view{blob}); !w) {
    return std::unexpected{w.error()};
  }
  upsert_index_locked(file, now, static_cast<std::int64_t>(blob.size()));
  if (total_size_ > cap_) evict_oldest_locked();
  return {};
}

std::expected<void, std::string>
DiskCache::put(const std::string_view key, const std::string_view contents,
               const std::int64_t ttl_seconds) {
  return put(key, std::span<const std::byte>{
                     reinterpret_cast<const std::byte*>(contents.data()),
                     contents.size()},
             ttl_seconds);
}

std::optional<std::string> DiskCache::get(const std::string_view key) {
  if (key.empty()) return std::nullopt;
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_root_locked()) return std::nullopt;  // unusable root = a miss

  const std::string ns   = std::string{key.substr(0, key.find('/'))};
  const fs::path    file = root_ / encode_namespace(ns) / key_filename(key);
  auto raw = read_file(file);
  if (!raw) return std::nullopt;  // absent file = a miss

  // Corrupt content (bad magic / truncated / wrong key magic) reads as a
  // miss and gets purged — never a crash, never a stale hit.
  const std::string_view view{*raw};
  if (raw->size() < kHeaderFixed ||
      view.substr(0, kMagic.size()) != kMagic) {
    purge_path_locked(file);
    return std::nullopt;
  }
  const std::int64_t  expiry  = read_i64(view, 8);
  const std::uint32_t key_len = read_u32(view, 24);
  if (static_cast<std::uint64_t>(kHeaderFixed) + key_len > raw->size() ||
      raw->compare(static_cast<std::size_t>(kHeaderFixed),
                   static_cast<std::size_t>(key_len), std::string{key}) != 0) {
    purge_path_locked(file);  // key mismatch → foreign/collided file
    return std::nullopt;
  }
  if (expiry != kNoExpiry && expiry <= now_()) {
    purge_path_locked(file);  // expired = miss + purge
    return std::nullopt;
  }
  // A fresh hit: track it (files written by another process get indexed here)
  // so a later eviction can pick it.
  upsert_index_locked(file, now_(), static_cast<std::int64_t>(raw->size()));
  return std::optional<std::string>{raw->substr(
      static_cast<std::size_t>(kHeaderFixed) + key_len)};
}

std::expected<std::size_t, std::string>
DiskCache::evict_namespace(const std::string_view namespace_prefix) {
  if (namespace_prefix.empty()) {
    return std::unexpected{"cache: empty namespace"};
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (auto r = ensure_root_locked(); !r) {
    return std::unexpected{r.error()};
  }
  const fs::path dir = root_ / encode_namespace(namespace_prefix);
  std::size_t removed = 0;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return removed;
  for (fs::recursive_directory_iterator it{dir, ec}, end; it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_regular_file(ec) && !is_tmp_leftover(it->path())) {
      purge_path_locked(it->path());
      ++removed;
    }
  }
  return removed;
}

}  // namespace bootamp::foundation