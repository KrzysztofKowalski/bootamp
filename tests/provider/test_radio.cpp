// tests/provider/test_radio.cpp — Catch2 port of cliamp/external/radio
// provider_test.go, favorites_test.go and catalog_test.go.
//
// No network in tests: catalog JSON parsing, URL template construction and
// error paths are exercised against canned fixture strings via the module's
// detail surface (provider/radio/radio_internal.hpp); the libcurl endpoints
// (search_stations/top_stations_offset/fetch_stats) are covered only by the
// URL-builder checks below. Favorites and station TOML files are exercised
// through the public API with BOOTAMP_CONFIG_DIR pointed at a temp dir so the
// real ~/.config/bootamp is never touched.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "provider/radio/radio.hpp"
#include "provider/radio/radio_internal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using bootamp::provider::radio::CatalogStation;
using bootamp::provider::radio::Favorites;
using bootamp::provider::radio::Provider;
using bootamp::provider::radio::Station;
using bootamp::provider::radio::format_catalog_name;
namespace detail = bootamp::provider::radio::detail;

namespace {

// TempConfigDir points BOOTAMP_CONFIG_DIR at a fresh temp dir for the test
// lifetime and removes it afterwards (Go tests use t.Setenv("HOME", t.TempDir())).
class TempConfigDir {
public:
  TempConfigDir() {
    dir = fs::temp_directory_path() /
          ("bootamp-radio-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(s_counter++));
    fs::create_directories(dir);
    ::setenv("BOOTAMP_CONFIG_DIR", dir.c_str(), 1);
  }
  ~TempConfigDir() {
    ::unsetenv("BOOTAMP_CONFIG_DIR");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path dir;

private:
  static int s_counter;
};
int TempConfigDir::s_counter = 0;

// write_file creates parent dirs and writes content (Go testhelpers writeFile).
void write_file(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out{path, std::ios::binary};
  out << content;
}

// read_all returns the full file contents.
std::string read_all(const fs::path& path) {
  std::ifstream in{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{in},
                     std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("provider has builtin station") {
  TempConfigDir tmp;
  Provider p;

  REQUIRE(p.name() == "Radio");

  auto infos = p.playlists();
  REQUIRE(infos);
  REQUIRE_FALSE(infos->empty());
  REQUIRE(infos->at(0).name == "cliamp radio");
  REQUIRE(infos->at(0).id == "l:0");
}

TEST_CASE("provider loads stations from radios.toml") {
  TempConfigDir tmp;
  write_file(tmp.dir / "radios.toml",
             "[[station]]\nname = \"Extra\"\nurl = \"https://extra.example/stream\"\n");

  Provider p;
  auto infos = p.playlists();
  REQUIRE(infos);
  REQUIRE(infos->size() == 2);
  REQUIRE(infos->at(1).name == "Extra");
  REQUIRE(infos->at(1).id == "l:1");
}

TEST_CASE("provider loads multiple stations") {
  TempConfigDir tmp;
  write_file(tmp.dir / "radios.toml",
             "# leading comment\n"
             "[[station]]\nname = \"A\"\nurl = \"http://a/\"\n"
             "\n"
             "[[station]]\nname = \"B\"\nurl = \"http://b/\"\n"
             "# trailing comment\n");

  Provider p;
  auto infos = p.playlists();
  REQUIRE(infos);
  REQUIRE(infos->size() == 3);
  REQUIRE(infos->at(1).name == "A");
  REQUIRE(infos->at(2).name == "B");
}

TEST_CASE("provider skips incomplete station entries") {
  TempConfigDir tmp;
  write_file(tmp.dir / "radios.toml",
             "[[station]]\nname = \"no-url\"\n"
             "\n"
             "[[station]]\nname = \"complete\"\nurl = \"http://c/\"\n");

  Provider p;
  auto infos = p.playlists();
  REQUIRE(infos);
  REQUIRE(infos->size() == 2);
  REQUIRE(infos->at(1).name == "complete");
}

TEST_CASE("provider missing radios.toml is ignored") {
  TempConfigDir tmp;  // no radios.toml
  Provider p;
  auto infos = p.playlists();
  REQUIRE(infos);
  REQUIRE(infos->size() == 1);  // builtin only
}

TEST_CASE("provider tracks local station") {
  TempConfigDir tmp;
  Provider p;
  auto tracks = p.tracks("l:0");
  REQUIRE(tracks);
  REQUIRE(tracks->size() == 1);
  REQUIRE(tracks->at(0).path == "https://radio.cliamp.stream/streams.m3u");
  REQUIRE(tracks->at(0).title == "cliamp radio");
  REQUIRE(tracks->at(0).stream);
  REQUIRE(tracks->at(0).realtime);
}

TEST_CASE("provider legacy numeric id maps to local station") {
  TempConfigDir tmp;
  Provider p;
  auto tracks = p.tracks("0");
  REQUIRE(tracks);
  REQUIRE(tracks->size() == 1);
  REQUIRE(tracks->at(0).path == "https://radio.cliamp.stream/streams.m3u");
  REQUIRE(tracks->at(0).stream);
  REQUIRE(tracks->at(0).realtime);
}

TEST_CASE("provider tracks invalid ids") {
  TempConfigDir tmp;
  Provider p;
  const std::vector<std::string> ids = {
      "l:99",      // local index out of range
      "c:0",       // catalog empty
      "s:0",       // search not active
      "f:5",       // no favorites
      "x:1",       // unknown prefix
      "notanid",   // non-numeric legacy id
      "l:notanumber",
  };
  for (const auto& id : ids) {
    REQUIRE_FALSE(p.tracks(id));
  }
}

TEST_CASE("provider id prefix") {
  TempConfigDir tmp;
  Provider p;
  REQUIRE(p.id_prefix("c:0") == "c");
  REQUIRE(p.id_prefix("l:5") == "l");
  REQUIRE(p.id_prefix("f:3") == "f");
  REQUIRE(p.id_prefix("s:0") == "s");
  REQUIRE(p.id_prefix("noprefix") == "");
  REQUIRE(p.id_prefix("") == "");
}

TEST_CASE("provider is favoritable id") {
  TempConfigDir tmp;
  Provider p;
  REQUIRE(p.is_favoritable_id("c:0"));
  REQUIRE(p.is_favoritable_id("f:3"));
  REQUIRE(p.is_favoritable_id("s:1"));
  REQUIRE_FALSE(p.is_favoritable_id("l:0"));
  REQUIRE_FALSE(p.is_favoritable_id("123"));
  REQUIRE_FALSE(p.is_favoritable_id(""));
}

TEST_CASE("provider catalog lifecycle") {
  TempConfigDir tmp;
  Provider p;

  CatalogStation a;
  a.name = "Radio A";
  a.url = "http://a/";
  a.bitrate = 192;
  a.country = "NO";
  CatalogStation b;
  b.name = "Radio B";
  b.url = "http://b/";
  p.append_catalog({a, b});

  auto infos = p.playlists();
  REQUIRE(infos);
  REQUIRE(infos->size() == 3);  // builtin (1) + catalog (2)
  REQUIRE(infos->at(1).id == "c:0");
  REQUIRE(infos->at(1).name == "Radio A [192k] · NO");
  REQUIRE(infos->at(2).id == "c:1");

  // Duplicate URL is skipped even with a different name.
  CatalogStation dup;
  dup.name = "Radio A duplicate";
  dup.url = "http://a/";
  p.append_catalog({dup});
  infos = p.playlists();
  REQUIRE(infos->size() == 3);

  auto tracks = p.tracks("c:0");
  REQUIRE(tracks);
  REQUIRE(tracks->size() == 1);
  REQUIRE(tracks->at(0).path == "http://a/");
  REQUIRE(tracks->at(0).title == "Radio A");
  REQUIRE(tracks->at(0).stream);
  REQUIRE(tracks->at(0).realtime);

  // ToggleFavorite on a catalog entry adds it (returns added=true, name).
  auto r = p.toggle_favorite("c:0");
  REQUIRE(r);
  REQUIRE(r->first);
  REQUIRE(r->second == "Radio A");

  // The favorite now shows up as its own f: entry and is ★ marked in the
  // catalog section: local (1) + favorite (1) + catalog (2) = 4.
  infos = p.playlists();
  REQUIRE(infos->size() == 4);
  REQUIRE(infos->at(1).id == "f:0");
  REQUIRE(infos->at(1).name == "★ Radio A [192k] · NO");
  REQUIRE(infos->at(2).id == "c:0");
  REQUIRE(infos->at(2).name == "★ Radio A [192k] · NO");

  // Toggle again removes it.
  r = p.toggle_favorite("c:0");
  REQUIRE(r);
  REQUIRE_FALSE(r->first);
  REQUIRE(r->second == "Radio A");
  infos = p.playlists();
  REQUIRE(infos->size() == 3);
}

TEST_CASE("provider toggle favorite local rejected") {
  TempConfigDir tmp;
  Provider p;
  REQUIRE_FALSE(p.toggle_favorite("l:0"));
}

TEST_CASE("provider toggle favorite invalid index") {
  TempConfigDir tmp;
  Provider p;
  REQUIRE_FALSE(p.toggle_favorite("c:99"));
}

TEST_CASE("provider search state") {
  TempConfigDir tmp;
  Provider p;
  REQUIRE_FALSE(p.is_searching());
  p.clear_search();
  REQUIRE_FALSE(p.is_searching());
}

TEST_CASE("format catalog name") {
  struct Case {
    CatalogStation in;
    std::string    want;
  };
  const std::vector<Case> cases = {
      {{.name = "Jazz"}, "Jazz"},
      {{.name = "Jazz", .bitrate = 128}, "Jazz [128k]"},
      {{.name = "Jazz", .country = "UK"}, "Jazz · UK"},
      {{.name = "Jazz", .country = "US", .bitrate = 320}, "Jazz [320k] · US"},
  };
  for (const auto& c : cases) {
    REQUIRE(format_catalog_name(c.in) == c.want);
  }
}

TEST_CASE("favorites add remove roundtrip") {
  TempConfigDir tmp;
  auto f = Favorites::load();
  REQUIRE(f);
  REQUIRE(f->stations().empty());

  CatalogStation s;
  s.name = "Test FM";
  s.url = "https://test.example.com/stream";
  s.country = "Norway";
  s.bitrate = 128;

  REQUIRE(f->add(s));
  REQUIRE(f->contains(s.url));
  REQUIRE(f->stations().size() == 1);

  // Duplicate add is a no-op.
  REQUIRE(f->add(s));
  REQUIRE(f->stations().size() == 1);

  // Persisted content matches Go's favorites.go save() byte for byte.
  const auto file = tmp.dir / "radio_favorites.toml";
  REQUIRE(fs::exists(file));
  const std::string expected =
      "[[station]]\n"
      "name = \"Test FM\"\n"
      "url = \"https://test.example.com/stream\"\n"
      "country = \"Norway\"\n"
      "bitrate = 128\n";
  REQUIRE(read_all(file) == expected);

  // Reload from disk.
  auto reloaded = Favorites::load();
  REQUIRE(reloaded);
  REQUIRE(reloaded->stations().size() == 1);
  REQUIRE(reloaded->stations()[0].name == "Test FM");
  REQUIRE(reloaded->stations()[0].url == "https://test.example.com/stream");
  REQUIRE(reloaded->stations()[0].country == "Norway");
  REQUIRE(reloaded->stations()[0].bitrate == 128);
  REQUIRE(reloaded->contains(s.url));

  // Remove persists too.
  REQUIRE(reloaded->remove(s.url));
  REQUIRE_FALSE(reloaded->contains(s.url));
  REQUIRE(reloaded->stations().empty());

  // Removing a non-favorite is a no-op.
  REQUIRE(reloaded->remove("https://nonexistent.example.com"));
}

TEST_CASE("favorites round trip fields") {
  TempConfigDir tmp;
  auto f = Favorites::load();

  CatalogStation jazz;
  jazz.name = "Jazz FM";
  jazz.url = "https://jazz.example.com/stream";
  jazz.country = "UK";
  jazz.bitrate = 320;
  jazz.codec = "mp3";
  CatalogStation rock;
  rock.name = "Rock Radio";
  rock.url = "https://rock.example.com/stream";
  rock.country = "US";
  rock.bitrate = 192;
  rock.tags = "rock,metal";
  REQUIRE(f->add(jazz));
  REQUIRE(f->add(rock));

  const std::string expected =
      "[[station]]\n"
      "name = \"Jazz FM\"\n"
      "url = \"https://jazz.example.com/stream\"\n"
      "country = \"UK\"\n"
      "bitrate = 320\n"
      "codec = \"mp3\"\n"
      "\n"
      "[[station]]\n"
      "name = \"Rock Radio\"\n"
      "url = \"https://rock.example.com/stream\"\n"
      "country = \"US\"\n"
      "bitrate = 192\n"
      "tags = \"rock,metal\"\n";
  REQUIRE(read_all(tmp.dir / "radio_favorites.toml") == expected);

  auto loaded = Favorites::load();
  REQUIRE(loaded);
  REQUIRE(loaded->stations().size() == 2);
  REQUIRE(loaded->stations()[0].country == "UK");
  REQUIRE(loaded->stations()[0].codec == "mp3");
  REQUIRE(loaded->stations()[0].bitrate == 320);
  REQUIRE(loaded->stations()[1].country == "US");
  REQUIRE(loaded->stations()[1].tags == "rock,metal");
  REQUIRE(loaded->stations()[1].bitrate == 192);
  // votes is not persisted (Go favorites.go save/load never touch it).
  REQUIRE(loaded->stations()[0].votes == 0);
}

TEST_CASE("favorites empty config dir loads empty") {
  TempConfigDir tmp;  // no radio_favorites.toml
  auto f = Favorites::load();
  REQUIRE(f);
  REQUIRE(f->stations().empty());
  REQUIRE_FALSE(f->contains("http://a/"));
}

TEST_CASE("catalog parse stations") {
  // Fixture from cliamp catalog_test.go TestSearchStationsSuccess.
  auto r = detail::parse_catalog_stations(
      R"([{"name":"Jazz FM","url_resolved":"https://jazz.example/stream","country":"UK","bitrate":128}])");
  REQUIRE(r);
  REQUIRE(r->size() == 1);
  REQUIRE(r->at(0).name == "Jazz FM");
  REQUIRE(r->at(0).url == "https://jazz.example/stream");
  REQUIRE(r->at(0).country == "UK");
  REQUIRE(r->at(0).bitrate == 128);
  // Absent fields default (Go zero values).
  REQUIRE(r->at(0).tags.empty());
  REQUIRE(r->at(0).codec.empty());
  REQUIRE(r->at(0).votes == 0);
  REQUIRE(r->at(0).homepage.empty());
}

TEST_CASE("catalog parse all fields") {
  // Fixture from cliamp catalog_test.go TestTopStationsOffset extended with
  // every CatalogStation field.
  auto r = detail::parse_catalog_stations(
      R"([{"name":"Top1","url_resolved":"http://t1/","country":"NO","tags":"a,b","codec":"mp3","bitrate":64,"votes":42,"homepage":"http://h/"}])");
  REQUIRE(r);
  REQUIRE(r->size() == 1);
  REQUIRE(r->at(0).name == "Top1");
  REQUIRE(r->at(0).url == "http://t1/");
  REQUIRE(r->at(0).country == "NO");
  REQUIRE(r->at(0).tags == "a,b");
  REQUIRE(r->at(0).codec == "mp3");
  REQUIRE(r->at(0).bitrate == 64);
  REQUIRE(r->at(0).votes == 42);
  REQUIRE(r->at(0).homepage == "http://h/");
}

TEST_CASE("catalog parse empty array") {
  auto r = detail::parse_catalog_stations(R"([])");
  REQUIRE(r);
  REQUIRE(r->empty());
}

TEST_CASE("catalog parse invalid json") {
  // Fixture from cliamp catalog_test.go TestFetchStationsInvalidJSON.
  REQUIRE_FALSE(detail::parse_catalog_stations("{not valid"));
}

TEST_CASE("catalog parse non-array") {
  REQUIRE_FALSE(detail::parse_catalog_stations(R"({"name":"x"})"));
}

TEST_CASE("catalog parse type mismatch") {
  // Go json.Unmarshal also errors on a string bitrate.
  REQUIRE_FALSE(detail::parse_catalog_stations(
      R"([{"name":"x","url_resolved":"http://x/","bitrate":"128"}])"));
}

TEST_CASE("radio stats parse") {
  // Fixture from cliamp catalog_test.go TestFetchStats.
  auto r = detail::parse_radio_stats(
      R"({"total_sessions":42,"total_listen_hours":12.5,"peak_listeners":7,"stations":{"lofi":{"total_sessions":40,"active_listeners":3}}})");
  REQUIRE(r);
  REQUIRE(r->total_sessions == 42);
  REQUIRE(r->total_listen_hours == Catch::Approx(12.5));
  REQUIRE(r->peak_listeners == 7);
  REQUIRE(r->stations.size() == 1);
  REQUIRE(r->stations.at("lofi").total_sessions == 40);
  REQUIRE(r->stations.at("lofi").active_listeners == 3);
  // Absent nested fields default.
  REQUIRE(r->stations.at("lofi").total_listen_hours == Catch::Approx(0.0));
  REQUIRE(r->stations.at("lofi").peak_listeners == 0);
}

TEST_CASE("radio stats parse minimal") {
  auto r = detail::parse_radio_stats(R"({})");
  REQUIRE(r);
  REQUIRE(r->total_sessions == 0);
  REQUIRE(r->total_listen_hours == Catch::Approx(0.0));
  REQUIRE(r->peak_listeners == 0);
  REQUIRE(r->stations.empty());
}

TEST_CASE("radio stats parse invalid json") {
  REQUIRE_FALSE(detail::parse_radio_stats("{bad"));
}

TEST_CASE("search stations url template") {
  // Exact Go template: %s/stations/byname/%s?limit=%d&order=votes&reverse=true&hidebroken=true
  REQUIRE(detail::search_stations_url("jazz", 10) ==
          "https://de1.api.radio-browser.info/json/stations/byname/jazz?limit=10&order=votes&reverse=true&hidebroken=true");
}

TEST_CASE("search stations default limit") {
  // cliamp catalog_test.go TestSearchStationsDefaultLimit: limit<=0 -> 50.
  REQUIRE(detail::search_stations_url("x", 0) ==
          "https://de1.api.radio-browser.info/json/stations/byname/x?limit=50&order=votes&reverse=true&hidebroken=true");
}

TEST_CASE("search stations escapes query") {
  // Go url.PathEscape: space -> %20, and '/' ';' ',' '?' are escaped.
  REQUIRE(detail::search_stations_url("jazz fm", 10) ==
          "https://de1.api.radio-browser.info/json/stations/byname/jazz%20fm?limit=10&order=votes&reverse=true&hidebroken=true");
  REQUIRE(detail::search_stations_url("a/b?c;d,e", 10) ==
          "https://de1.api.radio-browser.info/json/stations/byname/a%2Fb%3Fc%3Bd%2Ce?limit=10&order=votes&reverse=true&hidebroken=true");
  // Reserved characters kept by encodePathSegment stay literal.
  REQUIRE(detail::search_stations_url("x$&+:=@", 10) ==
          "https://de1.api.radio-browser.info/json/stations/byname/x$&+:=@?limit=10&order=votes&reverse=true&hidebroken=true");
}

TEST_CASE("top stations offset url template") {
  // cliamp catalog_test.go TestTopStationsOffset: topvote/<limit>?offset=<offset>
  REQUIRE(detail::top_stations_url(50, 25) ==
          "https://de1.api.radio-browser.info/json/stations/topvote/25?offset=50&hidebroken=true");
}

TEST_CASE("top stations offset clamps negatives") {
  // cliamp catalog_test.go TestTopStationsOffsetClampsNegatives.
  REQUIRE(detail::top_stations_url(-10, 0) ==
          "https://de1.api.radio-browser.info/json/stations/topvote/50?offset=0&hidebroken=true");
}
