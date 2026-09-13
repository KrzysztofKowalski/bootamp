// tests/foundation/test_disk_cache.cpp — DiskCache tests (foundation layer).
//
// Covers the new persistent byte cache (foundation/disk_cache.hpp/.cpp):
//   - put/get roundtrip (binary-safe) + overwrite
//   - TTL expiry via an injected fake clock (miss at the expiry instant,
//     purge from disk) and the no-expiry (ttl <= 0) path
//   - evict_namespace drops one namespace's whole directory
//   - the size cap evicts oldest-first by written time
//   - a garbage / truncated / key-mismatched file is a miss (purged), never
//     a crash; write_file_atomic's .tmp- leftovers are ignored by accounting
//     and eviction
//   - concurrent put/get from threads
//   - the default root honors $XDG_CACHE_HOME and $HOME/.cache
//   - set_max_size shrinks the cap and evicts down to it immediately
//
// Registered exactly like the other foundation tests: tests/foundation/*.cpp
// is globbed into the single test_foundation binary (CMakeLists.txt
// bootamp_test(test_foundation ...)); no extra CMake wiring was needed.
#include "foundation/disk_cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

// ScopedEnv sets an environment variable for the lifetime of the guard and
// restores the prior value (or unsets it) on destruction — the test_foundation
// pattern.
class ScopedEnv {
 public:
  explicit ScopedEnv(const char* name, std::string value) : name_(name) {
    if (const char* old = std::getenv(name)) {
      had_ = true;
      old_ = old;
    }
    ::setenv(name, value.c_str(), 1);
  }
  // nullptr variant: unset for the scope.
  explicit ScopedEnv(const char* name, std::nullptr_t) : name_(name) {
    if (const char* old = std::getenv(name)) {
      had_ = true;
      old_ = old;
    }
    ::unsetenv(name);
  }
  ~ScopedEnv() {
    if (had_) {
      ::setenv(name_.c_str(), old_.c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::string name_;
  bool had_ = false;
  std::string old_;
};

fs::path make_temp_dir() {
  static std::atomic<unsigned> seq{0};
  std::random_device rd;
  std::uniform_int_distribution<int> dist{0, 1'000'000};
  fs::path base = fs::temp_directory_path() / "bootamp_test";
  fs::create_directories(base);
  for (int tries = 0; tries < 16; ++tries) {
    fs::path candidate =
        base / ("t_" + std::to_string(getpid()) + "_" +
                std::to_string(seq.fetch_add(1)) + "_" + std::to_string(dist(rd)));
    std::error_code ec;
    if (fs::create_directory(candidate, ec)) return candidate;
  }
  return base / ("t_" + std::to_string(getpid()) + "_" +
                 std::to_string(seq.fetch_add(1)));
}

class TempRoot {
 public:
  TempRoot() : path_(make_temp_dir()) {}
  ~TempRoot() { std::error_code ec; fs::remove_all(path_, ec); }
  const fs::path& path() const { return path_; }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;

 private:
  fs::path path_;
};

// write_raw writes `contents` to `p` without any cache machinery — used to
// plant corrupt / foreign / leftover files.
void write_raw(const fs::path& p, std::string_view contents) {
  std::ofstream out{p, std::ios::binary | std::ios::trunc};
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

// dir_size sums every regular file under `p` (recursively).
std::int64_t dir_size(const fs::path& p) {
  std::int64_t total = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it{p, ec}, end; it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_regular_file(ec)) total += static_cast<std::int64_t>(it->file_size(ec));
  }
  return total;
}

using bootamp::foundation::DiskCache;

// fake_clock returns (now_shared, NowFn) so tests can advance time without
// sleeping. The shared int64 holds unix seconds.
auto fake_clock() {
  auto t = std::make_shared<std::int64_t>(1'000);
  return std::pair{t, DiskCache::NowFn{[t] { return *t; }}};
}

}  // namespace

// ---------------------------------------------------------------------------
// put/get roundtrip
// ---------------------------------------------------------------------------

TEST_CASE("disk cache put/get roundtrips binary payloads", "[foundation][disk_cache]") {
  TempRoot tmp;
  auto [now, clock] = fake_clock();
  DiskCache cache(tmp.path() / "c", clock, 1'000'000);

  // Binary-safe: NULs and control bytes survive byte-for-byte. (Split the
  // literals so the hex escapes don't swallow the following 'e'/'f' digits.)
  const std::string payload = "h\x00\x01\x02"
                              "ello\n\xff\xfe  x";
  REQUIRE(cache.put("ns1/k1", payload, 300).has_value());
  auto v = cache.get("ns1/k1");
  REQUIRE(v.has_value());
  REQUIRE(*v == payload);

  // Overwrite replaces the stored bytes.
  REQUIRE(cache.put("ns1/k1", "second", 300).has_value());
  REQUIRE(cache.get("ns1/k1").value() == "second");
}

TEST_CASE("disk cache get misses on an absent key", "[foundation][disk_cache]") {
  TempRoot tmp;
  DiskCache cache(tmp.path() / "c", fake_clock().second, 1'000'000);
  REQUIRE_FALSE(cache.get("ns1/nope").has_value());
}

// ---------------------------------------------------------------------------
// TTL / clock injection
// ---------------------------------------------------------------------------

TEST_CASE("disk cache TTL expires at the instant and purges the file",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  auto [now, clock] = fake_clock();
  DiskCache cache(tmp.path() / "c", clock, 1'000'000);

  REQUIRE(cache.put("ns1/k", "v", 100).has_value());  // written at t=1000
  REQUIRE(cache.get("ns1/k").has_value());

  *now = 1'099;  // still inside the 100 s window
  REQUIRE(cache.get("ns1/k").has_value());

  *now = 1'100;  // exactly at expiry → miss
  REQUIRE_FALSE(cache.get("ns1/k").has_value());
  // The expired entry was purged from disk, not left lying around.
  REQUIRE(cache.evict_namespace("ns1").value() == 0);
  REQUIRE(dir_size(cache.root()) == 0);
}

TEST_CASE("disk cache ttl <= 0 means no expiry", "[foundation][disk_cache]") {
  TempRoot tmp;
  auto [now, clock] = fake_clock();
  DiskCache cache(tmp.path() / "c", clock, 1'000'000);

  REQUIRE(cache.put("ns1/k", "v", 0).has_value());
  *now += 100'000;
  REQUIRE(cache.get("ns1/k").has_value());
}

// ---------------------------------------------------------------------------
// evict_namespace
// ---------------------------------------------------------------------------

TEST_CASE("disk cache evict_namespace drops one namespace only",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  DiskCache cache(tmp.path() / "c", fake_clock().second, 1'000'000);

  REQUIRE(cache.put("ns1/a", "1", 300).has_value());
  REQUIRE(cache.put("ns1/b", "2", 300).has_value());
  REQUIRE(cache.put("ns2/c", "3", 300).has_value());

  REQUIRE(cache.evict_namespace("ns1").value() == 2);
  REQUIRE_FALSE(cache.get("ns1/a").has_value());
  REQUIRE_FALSE(cache.get("ns1/b").has_value());
  REQUIRE(cache.get("ns2/c").has_value());  // the sibling namespace survives

  // Idempotent / empty namespace.
  REQUIRE(cache.evict_namespace("ns1").value() == 0);
  REQUIRE(cache.evict_namespace("ns3").value() == 0);
}

// ---------------------------------------------------------------------------
// Size cap, oldest-first eviction
// ---------------------------------------------------------------------------

TEST_CASE("disk cache size cap evicts oldest-first by written time",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  auto [now, clock] = fake_clock();
  // Each entry is ~535 bytes (28-byte header + 7-byte key + 500 payload):
  // two fit, the third evicts the first.
  DiskCache cache(tmp.path() / "c", clock, 1'500);
  const std::string big(500, 'x');

  *now = 1'000;
  REQUIRE(cache.put("ns1/k1", big, 300).has_value());
  *now = 1'001;
  REQUIRE(cache.put("ns1/k2", big, 300).has_value());
  *now = 1'002;
  REQUIRE(cache.put("ns1/k3", big, 300).has_value());

  REQUIRE_FALSE(cache.get("ns1/k1").has_value());  // oldest, evicted
  REQUIRE(cache.get("ns1/k2").has_value());
  REQUIRE(cache.get("ns1/k3").has_value());
  REQUIRE(dir_size(cache.root()) <= 1'500);
}

TEST_CASE("disk cache rejects an entry larger than the cap outright",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  DiskCache cache(tmp.path() / "c", fake_clock().second, 100);
  REQUIRE_FALSE(cache.put("ns1/big", std::string(200, 'x'), 300).has_value());
  REQUIRE_FALSE(cache.get("ns1/big").has_value());
}

// ---------------------------------------------------------------------------
// Corrupt files / atomicity
// ---------------------------------------------------------------------------

TEST_CASE("disk cache reads a garbage file at a key path as a miss and purges it",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  DiskCache cache(tmp.path() / "c", fake_clock().second, 1'000'000);

  // Plant a non-cache file exactly where key "ns1/garbage" would land.
  const fs::path dir = tmp.path() / "c" / DiskCache::encode_namespace("ns1");
  const fs::path path = dir / DiskCache::key_filename("ns1/garbage");
  fs::create_directories(dir);
  write_raw(path, "this is not a bootamp cache file, just garbage");

  auto v = cache.get("ns1/garbage");
  REQUIRE_FALSE(v.has_value());  // a miss, not a crash
  REQUIRE_FALSE(fs::exists(path));  // and the garbage was purged
}

TEST_CASE("disk cache reads a truncated cache file as a miss and purges it",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  DiskCache cache(tmp.path() / "c", fake_clock().second, 1'000'000);

  const fs::path dir = tmp.path() / "c" / DiskCache::encode_namespace("ns1");
  const fs::path path = dir / DiskCache::key_filename("ns1/trunc");
  fs::create_directories(dir);
  write_raw(path, "BCACH");  // magic prefix but nothing else

  REQUIRE_FALSE(cache.get("ns1/trunc").has_value());
  REQUIRE_FALSE(fs::exists(path));
}

TEST_CASE("disk cache treats a key-mismatched file (hash collision) as a miss",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  DiskCache cache(tmp.path() / "c", fake_clock().second, 1'000'000);

  REQUIRE(cache.put("ns1/keyb", "payload", 300).has_value());
  // Copy keyb's blob onto keya's path: the stored key ("ns1/keyb") no longer
  // matches the lookup key ("ns1/keya") → miss + purge, keyb untouched.
  const fs::path dir = tmp.path() / "c" / DiskCache::encode_namespace("ns1");
  fs::copy(dir / DiskCache::key_filename("ns1/keyb"),
           dir / DiskCache::key_filename("ns1/keya"));

  REQUIRE_FALSE(cache.get("ns1/keya").has_value());
  REQUIRE(cache.get("ns1/keyb").value() == "payload");
}

TEST_CASE("disk cache ignores write_file_atomic .tmp- leftovers",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  auto [now, clock] = fake_clock();
  // Two 535-byte entries fit under the 1300 cap ONLY if the 600-byte .tmp-*
  // leftover is not counted toward the total.
  DiskCache cache(tmp.path() / "c", clock, 1'300);
  const std::string big(500, 'x');

  const fs::path dir = tmp.path() / "c" / DiskCache::encode_namespace("ns1");
  fs::create_directories(dir);
  write_raw(dir / ".tmp-1234ab", std::string(600, 'z'));  // crash leftover

  *now = 1'000;
  REQUIRE(cache.put("ns1/k1", big, 300).has_value());
  *now = 1'001;
  REQUIRE(cache.put("ns1/k2", big, 300).has_value());

  REQUIRE(cache.get("ns1/k1").has_value());  // no spurious eviction
  REQUIRE(cache.get("ns1/k2").has_value());
  REQUIRE(fs::exists(dir / ".tmp-1234ab"));  // never counted, never evicted
}

// ---------------------------------------------------------------------------
// set_max_size
// ---------------------------------------------------------------------------

TEST_CASE("disk cache set_max_size evicts down to a smaller cap immediately",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  auto [now, clock] = fake_clock();
  DiskCache cache(tmp.path() / "c", clock, 2'000);

  const std::string big(500, 'x');
  *now = 1'000;
  REQUIRE(cache.put("ns1/k1", big, 300).has_value());
  *now = 1'001;
  REQUIRE(cache.put("ns1/k2", big, 300).has_value());

  cache.set_max_size(700);  // fits one 535-byte entry
  REQUIRE_FALSE(cache.get("ns1/k1").has_value());  // oldest goes first
  REQUIRE(cache.get("ns1/k2").has_value());
  REQUIRE(dir_size(cache.root()) <= 700);
}

// ---------------------------------------------------------------------------
// Env resolution of the default root
// ---------------------------------------------------------------------------

TEST_CASE("disk cache default root honors XDG_CACHE_HOME", "[foundation][disk_cache]") {
  TempRoot tmp;
  ScopedEnv xdg("XDG_CACHE_HOME", tmp.path().string());
  ScopedEnv home("HOME", "");  // the XDG var must win over HOME

  DiskCache cache;
  REQUIRE(cache.put("ns1/k", "v", 300).has_value());
  REQUIRE(cache.root() == tmp.path() / "bootamp");
  REQUIRE(cache.get("ns1/k").value() == "v");
}

TEST_CASE("disk cache default root falls back to ~/.cache/bootamp",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  ScopedEnv xdg("XDG_CACHE_HOME", nullptr);
  ScopedEnv home("HOME", tmp.path().string());

  DiskCache cache;
  REQUIRE(cache.put("ns1/k", "v", 300).has_value());
  REQUIRE(cache.root() == tmp.path() / ".cache" / "bootamp");
  REQUIRE(cache.get("ns1/k").value() == "v");
}

// ---------------------------------------------------------------------------
// Deterministic key/namespace encoding
// ---------------------------------------------------------------------------

TEST_CASE("disk cache key names are deterministic and filename-safe",
          "[foundation][disk_cache]") {
  REQUIRE(DiskCache::key_filename("ns1/k") == DiskCache::key_filename("ns1/k"));
  REQUIRE(DiskCache::key_filename("ns1/k") != DiskCache::key_filename("ns1/other"));
  REQUIRE(DiskCache::key_filename("ns1/a b/?c").size() == 16);
  REQUIRE(DiskCache::encode_namespace("gieres") == "gieres");
  REQUIRE(DiskCache::encode_namespace("ns 1/2") == "ns%201%2F2");
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST_CASE("disk cache survives concurrent put/get from threads",
          "[foundation][disk_cache]") {
  TempRoot tmp;
  DiskCache cache(tmp.path() / "c", fake_clock().second, 1LL << 30);
  std::atomic<int> failures{0};

  constexpr int kThreads = 8;
  constexpr int kKeys    = 50;
  std::vector<std::jthread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cache, &failures, t] {
      for (int k = 0; k < kKeys; ++k) {
        const std::string key = "t" + std::to_string(t) + "/c" + std::to_string(k);
        const std::string payload = "thread " + std::to_string(t) + " key " +
                                    std::to_string(k) + " payload";
        if (!cache.put(key, payload, 3'600).has_value()) {
          ++failures;
          continue;
        }
        auto v = cache.get(key);
        if (!v.has_value() || *v != payload) ++failures;
      }
    });
  }
  for (auto& th : threads) th.join();
  REQUIRE(failures.load() == 0);
}