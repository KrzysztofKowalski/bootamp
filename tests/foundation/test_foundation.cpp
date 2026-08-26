// src/tests/test_foundation.cpp — Catch2 tests for the M0 foundation layer.
//
// Covers the cliamp `internal/{appdir,fileutil,resume,fuzzy,tomlutil}` ports:
//   - write_file_atomic roundtrip + stricter-mode preservation
//   - copy_file (roundtrip, missing source, overwrite, empty file)
//   - resume save/load (roundtrip, empty-path no-op, non-positive no-op,
//     missing-file zero, corrupt-file zero, parent creation, overwrite)
//   - fuzzy.Match (match cases + ranking order)
//   - tomlutil (unquote, parse_sections, parse_named_sections)
//
// Env handling mirrors the Go tests: a scoped guard sets BOOTAMP_CONFIG_DIR /
// XDG_CONFIG_HOME / HOME for the duration of one test and restores the
// previous values on exit, so resume.json resolves inside a temp directory.
#include "foundation/appdir.hpp"
#include "foundation/fileutil.hpp"
#include "foundation/fuzzy.hpp"
#include "foundation/resume.hpp"
#include "foundation/tomlutil.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

// ScopedEnv sets an environment variable for the lifetime of the guard and
// restores the prior value (or unsets it) on destruction. Replaces the
// t.Setenv pattern from the Go tests.
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

// make_temp_dir creates a fresh unique directory under the system temp path
// and returns it. Removed automatically via fs::remove_all in the test.
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

// TempRoot RAII-creates and removes a temp directory.
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

// file_mode returns the low-9 permission bits of `p`, or -1 on stat failure.
int file_mode(const fs::path& p) {
  struct ::stat st {};
  if (::stat(p.c_str(), &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 0777);
}

// map_value returns fields[key] or `def` when the key is absent. std::map has
// no .value(key, default) member (unlike std::optional), so this mirrors the
// Go map-lookup-with-zero-value pattern used in the cliamp tests.
std::string map_value(const bootamp::foundation::TomlFields& f,
                      const std::string& key, std::string def = "") {
  auto it = f.find(key);
  if (it == f.end()) return def;
  return it->second;
}

// write_raw writes `contents` to `p` with `mode`, without any atomic dance —
// used to set up pre-existing files for the atomic-write mode-preservation
// test (mirrors os.WriteFile in the Go test setup).
void write_raw(const fs::path& p, std::string_view contents, unsigned mode) {
  std::ofstream out{p, std::ios::binary | std::ios::trunc};
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.close();
  ::chmod(p.c_str(), static_cast<::mode_t>(mode));
}

// env_isolate_appdir clears the appdir env vars for the scope of the guard so
// config_dir() resolves predictably. Returns the three guards via a tuple of
// unique_ptrs so they live until the end of the scope.
auto env_isolate_appdir(const fs::path& home) {
  return std::tuple{
      std::make_unique<ScopedEnv>("BOOTAMP_CONFIG_DIR", nullptr),
      std::make_unique<ScopedEnv>("XDG_CONFIG_HOME", nullptr),
      std::make_unique<ScopedEnv>("HOME", home.string())};
}

}  // namespace

// ---------------------------------------------------------------------------
// appdir
// ---------------------------------------------------------------------------

TEST_CASE("appdir resolves under HOME when no overrides are set", "[foundation][appdir]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  auto d = bootamp::foundation::config_dir();
  REQUIRE(d.has_value());
  REQUIRE(*d == tmp.path() / ".config" / "bootamp");
}

TEST_CASE("appdir honors BOOTAMP_CONFIG_DIR override", "[foundation][appdir]") {
  TempRoot tmp;
  ScopedEnv ovr("BOOTAMP_CONFIG_DIR", (tmp.path() / "custom").string());
  ScopedEnv xdg("XDG_CONFIG_HOME", "");
  ScopedEnv home("HOME", tmp.path().string());

  auto d = bootamp::foundation::config_dir();
  REQUIRE(d.has_value());
  REQUIRE(*d == tmp.path() / "custom");
}

TEST_CASE("appdir honors XDG_CONFIG_HOME when BOOTAMP_CONFIG_DIR unset", "[foundation][appdir]") {
  TempRoot tmp;
  ScopedEnv ovr("BOOTAMP_CONFIG_DIR", nullptr);
  ScopedEnv xdg("XDG_CONFIG_HOME", tmp.path().string());
  ScopedEnv home("HOME", "");

  auto d = bootamp::foundation::config_dir();
  REQUIRE(d.has_value());
  REQUIRE(*d == tmp.path() / "bootamp");
}

TEST_CASE("appdir plugin_dir is a subdir of config_dir", "[foundation][appdir]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  auto base = bootamp::foundation::config_dir().value();
  auto plugin = bootamp::foundation::plugin_dir();
  REQUIRE(plugin.has_value());
  REQUIRE(*plugin == base / "plugins");
  // plugin_dir must be nested under config_dir.
  auto rel = fs::relative(*plugin, base);
  REQUIRE(!rel.empty());
  REQUIRE(rel.native().find("..") == std::string::npos);
}

TEST_CASE("appdir data_dir is under ~/.local/share/bootamp", "[foundation][appdir]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  auto d = bootamp::foundation::data_dir();
  REQUIRE(d.has_value());
  REQUIRE(*d == tmp.path() / ".local" / "share" / "bootamp");
}

TEST_CASE("appdir ensure_dir creates with 0700", "[foundation][appdir]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  auto d = bootamp::foundation::ensure_dir(tmp.path() / "newdir");
  REQUIRE(d.has_value());
  REQUIRE(file_mode(tmp.path() / "newdir") == 0700);
}

// ---------------------------------------------------------------------------
// fileutil — write_file_atomic
// ---------------------------------------------------------------------------

TEST_CASE("write_file_atomic roundtrips content", "[foundation][fileutil]") {
  TempRoot tmp;
  auto p = tmp.path() / "out.txt";
  std::string payload = "hello bootamp\n";
  auto r = bootamp::foundation::write_file_atomic(p, payload, 0600);
  REQUIRE(r.has_value());

  auto rd = bootamp::foundation::read_file(p);
  REQUIRE(rd.has_value());
  REQUIRE(*rd == payload);
  REQUIRE(file_mode(p) == 0600);
}

TEST_CASE("write_file_atomic preserves a stricter existing mode", "[foundation][fileutil]") {
  TempRoot tmp;
  auto p = tmp.path() / "secret";
  write_raw(p, "old", 0400);

  auto r = bootamp::foundation::write_file_atomic(p, std::string_view{"new"}, 0600);
  REQUIRE(r.has_value());

  REQUIRE(file_mode(p) == 0400);  // existing 0400 & requested 0600 == 0400

  auto rd = bootamp::foundation::read_file(p);
  REQUIRE(rd.has_value());
  REQUIRE(*rd == "new");
}

TEST_CASE("write_file_atomic creates missing parent directories", "[foundation][fileutil]") {
  TempRoot tmp;
  auto p = tmp.path() / "nested" / "deep" / "file.bin";
  auto r = bootamp::foundation::write_file_atomic(p, std::string_view{"x"}, 0600);
  REQUIRE(r.has_value());
  REQUIRE(fs::exists(p));
  // parent is secured to 0700 by the atomic writer.
  REQUIRE(file_mode(p.parent_path()) == 0700);
}

TEST_CASE("write_file_atomic overwrites previous content", "[foundation][fileutil]") {
  TempRoot tmp;
  auto p = tmp.path() / "cfg.toml";
  REQUIRE(bootamp::foundation::write_file_atomic(p, std::string_view{"v1"}, 0600).has_value());
  REQUIRE(bootamp::foundation::write_file_atomic(p, std::string_view{"version two"}, 0600).has_value());
  REQUIRE(bootamp::foundation::read_file(p).value() == "version two");
}

TEST_CASE("read_file errors on missing file", "[foundation][fileutil]") {
  TempRoot tmp;
  auto rd = bootamp::foundation::read_file(tmp.path() / "nope");
  REQUIRE_FALSE(rd.has_value());
}

// ---------------------------------------------------------------------------
// fileutil — copy_file
// ---------------------------------------------------------------------------

TEST_CASE("copy_file roundtrips content", "[foundation][fileutil]") {
  TempRoot tmp;
  auto src = tmp.path() / "src.txt";
  auto dst = tmp.path() / "dst.txt";
  std::string content = "hello world";
  write_raw(src, content, 0644);

  REQUIRE(bootamp::foundation::copy_file(src, dst).has_value());

  auto rd = bootamp::foundation::read_file(dst);
  REQUIRE(rd.has_value());
  REQUIRE(*rd == content);
}

TEST_CASE("copy_file errors on missing source", "[foundation][fileutil]") {
  TempRoot tmp;
  auto r = bootamp::foundation::copy_file(tmp.path() / "nonexistent", tmp.path() / "dst");
  REQUIRE_FALSE(r.has_value());
  REQUIRE_FALSE(fs::exists(tmp.path() / "dst"));
}

TEST_CASE("copy_file overwrites existing destination", "[foundation][fileutil]") {
  TempRoot tmp;
  auto src = tmp.path() / "src.txt";
  auto dst = tmp.path() / "dst.txt";
  write_raw(src, "new", 0644);
  write_raw(dst, "old", 0644);

  REQUIRE(bootamp::foundation::copy_file(src, dst).has_value());
  REQUIRE(bootamp::foundation::read_file(dst).value() == "new");
}

TEST_CASE("copy_file handles an empty source", "[foundation][fileutil]") {
  TempRoot tmp;
  auto src = tmp.path() / "empty.txt";
  auto dst = tmp.path() / "dst.txt";
  write_raw(src, "", 0644);

  REQUIRE(bootamp::foundation::copy_file(src, dst).has_value());
  auto rd = bootamp::foundation::read_file(dst);
  REQUIRE(rd.has_value());
  REQUIRE(rd->empty());
}

// ---------------------------------------------------------------------------
// resume
// ---------------------------------------------------------------------------

TEST_CASE("resume save/load roundtrip", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  bootamp::foundation::ResumeState s;
  s.path = "/music/song.mp3";
  s.position_sec = 42;
  s.playlist = "main";
  REQUIRE(bootamp::foundation::resume_save(s).has_value());

  auto loaded = bootamp::foundation::resume_load();
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->path == "/music/song.mp3");
  REQUIRE(loaded->position_sec == 42);
  REQUIRE(loaded->playlist == "main");
}

TEST_CASE("resume save ignores empty path", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  bootamp::foundation::ResumeState s;
  s.path = "";
  s.position_sec = 10;
  s.playlist = "p";
  REQUIRE(bootamp::foundation::resume_save(s).has_value());

  REQUIRE_FALSE(fs::exists(tmp.path() / ".config" / "bootamp" / "resume.json"));
}

TEST_CASE("resume save ignores non-positive position", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  bootamp::foundation::ResumeState s;
  s.path = "/music/song.mp3";
  s.position_sec = 0;
  s.playlist = "p";
  REQUIRE(bootamp::foundation::resume_save(s).has_value());
  s.position_sec = -5;
  REQUIRE(bootamp::foundation::resume_save(s).has_value());

  REQUIRE_FALSE(fs::exists(tmp.path() / ".config" / "bootamp" / "resume.json"));
}

TEST_CASE("resume load missing file returns zero", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  auto loaded = bootamp::foundation::resume_load();
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->path.empty());
  REQUIRE(loaded->position_sec == 0);
  REQUIRE(loaded->playlist.empty());
}

TEST_CASE("resume load corrupt file returns zero", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  auto dir = tmp.path() / ".config" / "bootamp";
  fs::create_directories(dir);
  write_raw(dir / "resume.json", "not json {{", 0600);

  auto loaded = bootamp::foundation::resume_load();
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->path.empty());
  REQUIRE(loaded->position_sec == 0);
  REQUIRE(loaded->playlist.empty());
}

TEST_CASE("resume save creates the parent directory", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  auto parent = tmp.path() / ".config" / "bootamp";
  REQUIRE_FALSE(fs::exists(parent));

  bootamp::foundation::ResumeState s;
  s.path = "/music/song.mp3";
  s.position_sec = 1;
  s.playlist = "";
  REQUIRE(bootamp::foundation::resume_save(s).has_value());

  REQUIRE(fs::is_directory(parent));
}

TEST_CASE("resume save overwrites previous state", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  bootamp::foundation::ResumeState a;
  a.path = "/a.mp3";
  a.position_sec = 10;
  a.playlist = "one";
  REQUIRE(bootamp::foundation::resume_save(a).has_value());

  bootamp::foundation::ResumeState b;
  b.path = "/b.mp3";
  b.position_sec = 20;
  b.playlist = "two";
  REQUIRE(bootamp::foundation::resume_save(b).has_value());

  auto loaded = bootamp::foundation::resume_load();
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->path == "/b.mp3");
  REQUIRE(loaded->position_sec == 20);
  REQUIRE(loaded->playlist == "two");
}

TEST_CASE("resume save omits playlist when empty", "[foundation][resume]") {
  TempRoot tmp;
  auto g = env_isolate_appdir(tmp.path());

  bootamp::foundation::ResumeState s;
  s.path = "/music/a.mp3";
  s.position_sec = 77;
  s.playlist = "";
  REQUIRE(bootamp::foundation::resume_save(s).has_value());

  auto raw = bootamp::foundation::read_file(tmp.path() / ".config" / "bootamp" / "resume.json");
  REQUIRE(raw.has_value());
  // omitempty: no "playlist" key in the serialized JSON.
  REQUIRE(raw->find("\"playlist\"") == std::string::npos);
  REQUIRE_FALSE(raw->empty());
}

// ---------------------------------------------------------------------------
// fuzzy
// ---------------------------------------------------------------------------

TEST_CASE("fuzzy match cases", "[foundation][fuzzy]") {
  struct Case {
    std::string name;
    std::string query;
    std::string target;
    bool want;
  };
  const Case cases[] = {
      {"empty query matches", "", "anything", true},
      {"exact substring", "love", "Love Story", true},
      {"case insensitive", "LOVE", "love story", true},
      {"non-contiguous subsequence", "lst", "Love Story", true},
      {"missing char", "lovex", "Love Story", false},
      {"out of order", "ba", "ab", false},
      {"empty target non-empty query", "x", "", false},
      {"unicode subsequence", "caf\xc3\xa9", "Le Caf\xc3\xa9", true},
  };
  for (const auto& c : cases) {
    auto [score, ok] = bootamp::foundation::fuzzy_match(c.query, c.target);
    INFO(c.name << ": match(" << c.query << ", " << c.target << ")");
    REQUIRE(ok == c.want);
    (void)score;
  }
}

TEST_CASE("fuzzy ranking", "[foundation][fuzzy]") {
  struct Case {
    std::string name;
    std::string query;
    std::string high;
    std::string low;
  };
  const Case cases[] = {
      {"prefix beats interior", "lov", "Love", "Beloved"},
      {"word boundary beats mid-word", "st", "Love Story", "august"},
      {"consecutive beats scattered", "abc", "abcdef", "axbxcx"},
      {"start beats later", "fo", "Foo Bar", "Bar Foo"},
  };
  for (const auto& c : cases) {
    auto [hi, okHi] = bootamp::foundation::fuzzy_match(c.query, c.high);
    auto [lo, okLo] = bootamp::foundation::fuzzy_match(c.query, c.low);
    INFO(c.name);
    REQUIRE(okHi);
    REQUIRE(okLo);
    REQUIRE(hi > lo);
  }
}

// ---------------------------------------------------------------------------
// tomlutil — unquote
// ---------------------------------------------------------------------------

TEST_CASE("tomlutil unquote", "[foundation][tomlutil]") {
  using bootamp::foundation::unquote;
  REQUIRE(unquote("\"hello\"") == "hello");
  REQUIRE(unquote("\"line\\nnewline\"") == "line\nnewline");
  REQUIRE(unquote("bare") == "bare");
  REQUIRE(unquote("") == "");
  REQUIRE(unquote("x") == "x");
  REQUIRE(unquote("\"\"") == "");
  REQUIRE(unquote("\"\\u0041\"") == "A");
  // Invalid escape -> naive strip (keeps the backslash), matching cliamp.
  REQUIRE(unquote("\"\\z\"") == "\\z");
}

// ---------------------------------------------------------------------------
// tomlutil — parse_sections / parse_named_sections
// ---------------------------------------------------------------------------

TEST_CASE("tomlutil parse_sections", "[foundation][tomlutil]") {
  const std::string data =
      "\n# a comment\n"
      "stray = \"ignored before any section\"\n\n"
      "[[station]]\n"
      "name = \"Radio A\"\n"
      "url = \"http://a\"\n"
      "bitrate = \"128\"\n\n"
      "[[station]]\n"
      "name = \"Radio B\"\n"
      "url = \"http://b\"\n";

  std::vector<bootamp::foundation::TomlFields> got;
  bootamp::foundation::parse_sections(data, "station",
      [&](const bootamp::foundation::TomlFields& f) { got.push_back(f); });

  REQUIRE(got.size() == 2);
  REQUIRE(got[0].size() == 3);
  REQUIRE(got[0].at("name") == "Radio A");
  REQUIRE(got[0].at("url") == "http://a");
  REQUIRE(got[0].at("bitrate") == "128");
  REQUIRE(got[1].size() == 2);
  REQUIRE(got[1].at("name") == "Radio B");
  REQUIRE(got[1].at("url") == "http://b");
}

TEST_CASE("tomlutil parse_sections last key wins", "[foundation][tomlutil]") {
  const std::string data = "[[t]]\nk = \"first\"\nk = \"second\"\n";
  std::string got;
  bootamp::foundation::parse_sections(data, "t",
      [&](const bootamp::foundation::TomlFields& f) { got = f.at("k"); });
  REQUIRE(got == "second");
}

TEST_CASE("tomlutil parse_sections empty section still emits", "[foundation][tomlutil]") {
  const std::string data = "[[t]]\n[[t]]\nk = \"v\"\n";
  int count = 0;
  bootamp::foundation::parse_sections(data, "t",
      [&](const bootamp::foundation::TomlFields&) { ++count; });
  REQUIRE(count == 2);
}

TEST_CASE("tomlutil parse_named_sections interleaved order", "[foundation][tomlutil]") {
  const std::string data =
      "[[dir]]\n"
      "path = \"/music\"\n"
      "recursive = \"false\"\n\n"
      "[[track]]\n"
      "path = \"/a.mp3\"\n"
      "title = \"A\"\n\n"
      "[[dir]]\n"
      "path = \"$EXTRA_DIR\"\n\n"
      "[[track]]\n"
      "path = \"/b.mp3\"\n"
      "title = \"B\"\n";

  std::vector<std::string> got;
  std::vector<std::string> sections{"track", "dir"};
  bootamp::foundation::parse_named_sections(data, sections,
      [&](std::string_view section, const bootamp::foundation::TomlFields& f) {
        if (section == "track") {
          got.push_back("track:" + f.at("path"));
        } else if (section == "dir") {
          got.push_back("dir:" + f.at("path") + " rec=" + map_value(f, "recursive"));
        }
      });

  const std::vector<std::string> want{
      "dir:/music rec=false",
      "track:/a.mp3",
      "dir:$EXTRA_DIR rec=",
      "track:/b.mp3",
  };
  REQUIRE(got == want);
}

TEST_CASE("tomlutil parse_named_sections unknown header ignored", "[foundation][tomlutil]") {
  const std::string data =
      "[[unknown]]\n"
      "path = \"/x\"\n\n"
      "[[track]]\n"
      "path = \"/a.mp3\"\n";
  int count = 0;
  std::vector<std::string> sections{"track"};
  bootamp::foundation::parse_named_sections(data, sections,
      [&](std::string_view, const bootamp::foundation::TomlFields&) { ++count; });
  REQUIRE(count == 1);
}

TEST_CASE("tomlutil parse_named_sections unknown header does not leak fields",
          "[foundation][tomlutil]") {
  const std::string data =
      "[[track]]\n"
      "path = \"/a.mp3\"\n\n"
      "[[unknown]]\n"
      "title = \"leaked\"\n\n"
      "[[track]]\n"
      "path = \"/b.mp3\"\n"
      "title = \"B\"\n";

  std::vector<std::string> got;
  std::vector<std::string> sections{"track"};
  bootamp::foundation::parse_named_sections(data, sections,
      [&](std::string_view section, const bootamp::foundation::TomlFields& f) {
        got.push_back(std::string{section} + ":" + f.at("path") + ":" + map_value(f, "title"));
      });

  const std::vector<std::string> want{
      "track:/a.mp3:",
      "track:/b.mp3:B",
  };
  REQUIRE(got == want);
}