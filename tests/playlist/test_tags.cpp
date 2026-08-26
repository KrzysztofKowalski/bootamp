// tests/playlist/test_tags.cpp — Catch2 tests for playlist/tags.cpp.
//
// Port of cliamp playlist/track_test.go tag-related cases (TestFileURL,
// TestCacheAlbumArtUsesContentHash, TestRefreshEmbeddedMetadata*) through the
// public read_tags() surface: missing/unreadable files must error, tagged
// files must yield trimmed, mojibake-sanitized fields, embedded lyrics and
// album art caching (sha256-named files under $HOME/.local/share/bootamp/
// album-art, file:// URLs with percent-encoded paths).
//
// Fixtures live in tests/playlist/testdata/: sample.* are the dhowden/tag
// samples (Test Title/Test Artist/Test Album, track 3, Jazz, 2000);
// tagged.mp3 adds USLT lyrics + a 1x1 PNG APIC; mojibake.mp3 has a TIT2
// written in Windows-1251 but marked Latin-1.
#include <catch2/catch_test_macros.hpp>

#include "playlist/tags.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

// sha256 of the 1x1 PNG embedded in tagged.mp3 (see fixture generator).
inline constexpr const char* kTaggedArtSha256 =
    "1db7d0d116a2861ae3ec18d9aa050f56a515c689b89ba5f8bdba68745296632f";

fs::path fixture_dir() {
  fs::path base = fs::path(__FILE__).parent_path();
  if (!base.is_absolute()) {
    base = fs::current_path() / base;
  }
  return base / "testdata";
}

fs::path fixture(const char* name) { return fixture_dir() / name; }

// scoped_home points HOME (and XDG_*) at an empty temp dir so the art cache
// and any other user-state writes stay inside it.
class scoped_home {
public:
  explicit scoped_home(std::string name) : dir_(fs::temp_directory_path() / name) {
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    prev_home_ = get_home();
    set_home(dir_.string());
  }
  ~scoped_home() {
    set_home(prev_home_);
    fs::remove_all(dir_);
  }
  scoped_home(const scoped_home&) = delete;
  scoped_home& operator=(const scoped_home&) = delete;

  const fs::path& dir() const { return dir_; }

private:
  static std::string get_home() {
    const char* h = std::getenv("HOME");
    return h == nullptr ? std::string() : std::string(h);
  }
  static void set_home(const std::string& h) {
    if (h.empty()) {
      ::unsetenv("HOME");
    } else {
      ::setenv("HOME", h.c_str(), 1);
    }
  }

  fs::path     dir_;
  std::string  prev_home_;
};

void write_file(const fs::path& p, const std::string& contents) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

}  // namespace

TEST_CASE("read_tags errors on a missing file", "[tags]") {
  const fs::path missing = fs::temp_directory_path() / "bootamp-test-missing.mp3";
  fs::remove(missing);
  auto result = bootamp::playlist::read_tags(missing.string());
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(result.error().empty());
}

TEST_CASE("read_tags errors on an empty file", "[tags]") {
  const fs::path empty = fs::temp_directory_path() / "bootamp-test-empty.mp3";
  write_file(empty, "");
  auto result = bootamp::playlist::read_tags(empty.string());
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(result.error().empty());
}

TEST_CASE("read_tags errors on a non-audio file", "[tags]") {
  const fs::path text = fs::temp_directory_path() / "bootamp-test-notaudio.mp3";
  write_file(text, "definitely not audio");
  auto result = bootamp::playlist::read_tags(text.string());
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(result.error().empty());
}

TEST_CASE("read_tags reads ID3v2.3 MP3 fields", "[tags]") {
  auto result = bootamp::playlist::read_tags(fixture("sample.id3v23.mp3").string());
  REQUIRE(result.has_value());
  const auto& info = *result;
  CHECK(info.title == "Test Title");
  CHECK(info.artist == "Test Artist");
  CHECK(info.album == "Test Album");
  CHECK(info.year == 2000);
  CHECK(info.track_number == 3);   // TRCK "03/06"
  CHECK(info.duration_secs > 0);   // real MPEG audio
  CHECK(info.lyrics.empty());
  CHECK(info.art_cache_path.empty());
}

TEST_CASE("read_tags reads ID3v2.4 MP3 fields", "[tags]") {
  auto result = bootamp::playlist::read_tags(fixture("sample.id3v24.mp3").string());
  REQUIRE(result.has_value());
  const auto& info = *result;
  CHECK(info.title == "Test Title");
  CHECK(info.artist == "Test Artist");
  CHECK(info.album == "Test Album");
  CHECK(info.genre == "Jazz");     // TCON "Jazz" (v2.3 sample stores "(8)")
  CHECK(info.year == 2000);        // TDRC
  CHECK(info.track_number == 3);
  CHECK(info.duration_secs > 0);
}

TEST_CASE("read_tags reads FLAC vorbis comments", "[tags]") {
  auto result = bootamp::playlist::read_tags(fixture("sample.flac").string());
  REQUIRE(result.has_value());
  const auto& info = *result;
  CHECK(info.title == "Test Title");
  CHECK(info.artist == "Test Artist");
  CHECK(info.album == "Test Album");
  CHECK(info.genre == "Jazz");
  CHECK(info.year == 2000);        // DATE
  CHECK(info.track_number == 3);   // TRACKNUMBER "03"
  CHECK(info.duration_secs > 0);
  CHECK(info.art_cache_path.empty());  // no picture block in this sample
}

TEST_CASE("read_tags reads Ogg vorbis comments", "[tags]") {
  auto result = bootamp::playlist::read_tags(fixture("sample.ogg").string());
  REQUIRE(result.has_value());
  const auto& info = *result;
  CHECK(info.title == "Test Title");
  CHECK(info.artist == "Test Artist");
  CHECK(info.album == "Test Album");
  CHECK(info.genre == "Jazz");
  CHECK(info.year == 2000);        // DATE
  CHECK(info.track_number == 3);
  CHECK(info.duration_secs > 0);
}

TEST_CASE("read_tags reads MP4 atoms", "[tags]") {
  auto result = bootamp::playlist::read_tags(fixture("sample.m4a").string());
  REQUIRE(result.has_value());
  const auto& info = *result;
  CHECK(info.title == "Test Title");
  CHECK(info.artist == "Test Artist");
  CHECK(info.album == "Test Album");
  CHECK(info.genre == "Jazz");
  CHECK(info.year == 2000);        // \xa9day
  CHECK(info.track_number == 3);   // trkn
  CHECK(info.duration_secs > 0);
}

TEST_CASE("read_tags extracts USLT lyrics and caches APIC art", "[tags]") {
  scoped_home home("bootamp-tags-art1");

  auto result = bootamp::playlist::read_tags(fixture("tagged.mp3").string());
  REQUIRE(result.has_value());
  const auto& info = *result;
  CHECK(info.title == "Test Song");
  CHECK(info.artist == "Test Artist");
  CHECK(info.album == "Test Album");
  CHECK(info.genre == "Jazz");
  CHECK(info.year == 2000);
  CHECK(info.track_number == 3);   // TRCK "3/10"
  CHECK(info.duration_secs > 0);
  CHECK(info.lyrics == "Line one\nLine two");

  // Album art: single sha256-named file under $HOME/.local/share/bootamp/
  // album-art, surfaced as a percent-encoded file:// URL (Go's
  // TestCacheAlbumArtUsesContentHash via the public API).
  REQUIRE_FALSE(info.art_cache_path.empty());
  CHECK(info.art_cache_path.rfind("file:///", 0) == 0);
  CHECK(info.art_cache_path.find(std::string(kTaggedArtSha256) + ".png") != std::string::npos);

  const fs::path art_dir = home.dir() / ".local" / "share" / "bootamp" / "album-art";
  REQUIRE(fs::exists(art_dir));
  int files = 0;
  for (const auto& entry : fs::directory_iterator(art_dir)) {
    ++files;
    CHECK(entry.path().filename().string() == std::string(kTaggedArtSha256) + ".png");
  }
  CHECK(files == 1);
}

TEST_CASE("read_tags art cache is content-addressed and idempotent", "[tags]") {
  scoped_home home("bootamp-tags-art2");

  const auto first = bootamp::playlist::read_tags(fixture("tagged.mp3").string());
  const auto second = bootamp::playlist::read_tags(fixture("tagged.mp3").string());
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  // Same bytes -> same cached file -> same URL (Go TestCacheAlbumArtUsesContentHash).
  CHECK(second->art_cache_path == first->art_cache_path);
  REQUIRE_FALSE(first->art_cache_path.empty());

  const fs::path art_dir = home.dir() / ".local" / "share" / "bootamp" / "album-art";
  const int files =
      static_cast<int>(std::distance(fs::directory_iterator(art_dir), fs::directory_iterator()));
  CHECK(files == 1);
}

TEST_CASE("read_tags art URL percent-encodes the cache path", "[tags]") {
  // HOME with a space in the path: the file:// URL must escape it (port of
  // Go's TestFileURL through the public API).
  scoped_home home("bootamp tags spaced");
  auto result = bootamp::playlist::read_tags(fixture("tagged.mp3").string());
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->art_cache_path.empty());
  CHECK(result->art_cache_path.rfind("file:///", 0) == 0);
  CHECK(result->art_cache_path.find("%20") != std::string::npos);
}

TEST_CASE("read_tags fixes mojibake from legacy codepages", "[tags]") {
  // mojibake.mp3 stores the title in Windows-1251 bytes but marks the frame
  // Latin-1; sanitize_tag must re-decode to UTF-8 (cliamp sanitizeTag).
  auto result = bootamp::playlist::read_tags(fixture("mojibake.mp3").string());
  REQUIRE(result.has_value());
  CHECK(result->title == "\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82 \xd0\xa2\xd1\x80\xd0\xb5\xd0\xba");  // "Тест Трек"
  CHECK(result->artist == "Artist");  // pure ASCII passes through untouched
  CHECK(result->album == "Album");
}
