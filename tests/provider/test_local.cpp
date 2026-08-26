// tests/provider/test_local.cpp — Catch2 port of cliamp
// external/local/provider_test.go + dirs_test.go.
//
// The Go tests construct Provider{dir: t.TempDir()} directly; bootamp's
// Provider has a private constructor, so tests point BOOTAMP_CONFIG_DIR at a
// temp root and go through new_provider() (dir = <root>/playlists).
//
// Not ported (features absent from the bootamp contract header): the
// "Recently Played" history virtual playlist (no history module), Exists,
// DirSources/AddDirSource(s), CreateDirPlaylist, SetBookmarkByPath,
// ClearHistory and validateDirSource. The history-name reservation itself IS
// ported and tested.
#include "provider/local/internal.hpp"
#include "provider/local/provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

using bootamp::playlist::PlaylistInfo;
using bootamp::playlist::Track;
using bootamp::provider::local::Provider;
namespace local = bootamp::provider::local;
namespace detail = bootamp::provider::local::detail;

std::size_t g_seq = 0;

// tmp_dir returns a fresh scratch directory under the system temp dir.
fs::path tmp_dir() {
  const fs::path d = fs::temp_directory_path() /
                     ("bootamp_local_" + std::to_string(::getpid()) + "_" +
                      std::to_string(g_seq++));
  fs::create_directories(d);
  return d;
}

// TempEnv points BOOTAMP_CONFIG_DIR at a temp root and builds a Provider on
// top of it, mirroring Go's newTestProvider.
struct TempEnv {
  fs::path                   root;  // BOOTAMP_CONFIG_DIR target
  std::unique_ptr<Provider>  p;

  static TempEnv make() {
    fs::path root = tmp_dir();
    ::setenv("BOOTAMP_CONFIG_DIR", root.c_str(), 1);
    auto provider = Provider::new_provider();
    return TempEnv{root, std::move(provider)};
  }
};

// write_audio_file creates an empty file with a supported extension. Tag
// reading falls back to filename parsing for empty files, so fixtures stay
// simple (Go writeAudioFile).
void write_audio_file(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write("", 0);
}

// make_audio_tree creates dir with two audio files and a subdir with one more
// (Go makeAudioTree).
void make_audio_tree(const fs::path& dir) {
  write_audio_file(dir / "b.mp3");
  write_audio_file(dir / "a.flac");
  write_audio_file(dir / "sub" / "c.ogg");
}

// write_playlist writes a raw playlist file (Go fixtures).
void write_playlist(const fs::path& dir, std::string_view name, std::string_view content) {
  fs::create_directories(dir);
  std::ofstream out(dir / (std::string(name) + ".toml"), std::ios::binary);
  out << content;
}

// quote returns a double-quoted TOML string literal (Go quote).
std::string quote(const std::string& s) { return "\"" + s + "\""; }

// write_interleaved_doc writes a hand-authored mix.toml with the order
// [[track]] x, [[dir]] dirA, [[track]] y, [[dir]] dirB so rewrites must not
// flatten the interleaving (Go writeInterleavedDoc).
void write_interleaved_doc(const fs::path& dir, const std::string& x, const std::string& dir_a,
                           const std::string& y, const std::string& dir_b) {
  const std::string doc =
      "[[track]]\npath = " + quote(x) + "\n\n" +
      "[[dir]]\npath = " + quote(dir_a) + "\n\n" +
      "[[track]]\npath = " + quote(y) + "\n\n" +
      "[[dir]]\npath = " + quote(dir_b) + "\n";
  write_playlist(dir, "mix", doc);
}

void check_order(const std::vector<Track>& tracks, const std::vector<std::string>& want) {
  REQUIRE(tracks.size() == want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    CHECK(fs::path(tracks[i].path).filename().string() == want[i]);
  }
}

}  // namespace

// --- safePath / name validation ---------------------------------------------

TEST_CASE("safePath rejects traversal and blank names", "[local][safePath]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);

  SECTION("valid name resolves to dir/name.toml") {
    const auto ok = env.p->add_track_to_playlist("rock", Track{"/a.mp3", "A"});
    REQUIRE(ok.has_value());
    CHECK(fs::exists(env.root / "playlists" / "rock.toml"));
  }
  SECTION("invalid names are rejected") {
    for (const char* name : {"", "   ", "foo/bar", "foo\\bar", "../escape"}) {
      CAPTURE(name);
      const auto res = env.p->add_track_to_playlist(name, Track{"/a.mp3", "A"});
      CHECK_FALSE(res.has_value());
    }
  }
}

TEST_CASE("validateNewName rejects non-portable names", "[local][safePath]") {
  for (const char* name : {"..", ".", "", "   ", "foo/bar", "foo\\bar", "bad:name", "bad?name"}) {
    CAPTURE(name);
    CHECK(detail::validate_new_name(name).has_value());
  }
  CHECK_FALSE(detail::validate_new_name("good name").has_value());
}

TEST_CASE("provider name is Local", "[local]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  CHECK(env.p->name() == "Local");
}

// --- writeTrack / writeDir ---------------------------------------------------

TEST_CASE("writeTrack minimal omits empty optional fields", "[local][writeTrack]") {
  const std::string got = detail::write_track(Track{"/music/song.mp3", "Song"});
  REQUIRE(got.find("[[track]]") != std::string::npos);
  REQUIRE(got.find("path = \"/music/song.mp3\"") != std::string::npos);
  REQUIRE(got.find("title = \"Song\"") != std::string::npos);
  CHECK(got.find("artist") == std::string::npos);
  CHECK(got.find("bookmark") == std::string::npos);
}

TEST_CASE("writeTrack emits all populated fields", "[local][writeTrack]") {
  Track t{"/music/song.flac", "Title"};
  t.artist = "Artist";
  t.album = "Album";
  t.genre = "Rock";
  t.year = 2024;
  t.track_number = 3;
  t.duration_secs = 240;
  t.bookmark = true;
  t.feed = true;
  t.realtime = true;
  t.embedded_lyrics = "[00:01.00]Line";
  t.album_art_url = "file:///tmp/cover.jpg";
  const std::string got = detail::write_track(t);
  for (const char* want : {
           "path = \"/music/song.flac\"", "title = \"Title\"", "artist = \"Artist\"",
           "album = \"Album\"", "genre = \"Rock\"", "year = 2024", "track_number = 3",
           "duration_secs = 240", "embedded_lyrics = \"[00:01.00]Line\"",
           "album_art_url = \"file:///tmp/cover.jpg\"", "bookmark = true", "feed = true",
           "realtime = true"}) {
    CHECK(got.find(want) != std::string::npos);
  }
}

TEST_CASE("writeDir round trips through parse", "[local][writeDir]") {
  const std::string text =
      detail::write_dir(detail::DirSource{"/music", true}) +
      detail::write_dir(detail::DirSource{"/other", false});
  const auto doc = detail::parse_playlist_doc(text);
  REQUIRE(doc.dirs.size() == 2);
  CHECK(doc.dirs[0].path == "/music");
  CHECK(doc.dirs[0].recursive);
  CHECK(doc.dirs[1].path == "/other");
  CHECK_FALSE(doc.dirs[1].recursive);
}

// --- TOML document parsing ---------------------------------------------------

TEST_CASE("parsePlaylistDoc mixed order", "[local][parse]") {
  const auto doc = detail::parse_playlist_doc(
      "[[track]]\npath = \"/a.mp3\"\ntitle = \"A\"\n\n"
      "[[dir]]\npath = \"/music\"\n\n"
      "[[track]]\npath = \"/b.mp3\"\n\n"
      "[[dir]]\npath = \"/other\"\nrecursive = false\n");
  REQUIRE(doc.tracks.size() == 2);
  REQUIRE(doc.dirs.size() == 2);
  CHECK(doc.dirs[0].path == "/music");
  CHECK(doc.dirs[0].recursive);
  CHECK(doc.dirs[1].path == "/other");
  CHECK_FALSE(doc.dirs[1].recursive);
  REQUIRE(doc.order.size() == 4);
  CHECK(doc.order[0] == detail::Item::track);
  CHECK(doc.order[1] == detail::Item::dir);
  CHECK(doc.order[2] == detail::Item::track);
  CHECK(doc.order[3] == detail::Item::dir);
}

TEST_CASE("parsePlaylistDoc skips empty dir path", "[local][parse]") {
  const auto doc = detail::parse_playlist_doc("[[dir]]\n[[track]]\npath = \"/a.mp3\"\n");
  CHECK(doc.dirs.empty());
  REQUIRE(doc.tracks.size() == 1);
}

TEST_CASE("parsePlaylistDoc honors legacy favorite alias and booleans", "[local][parse]") {
  const auto doc = detail::parse_playlist_doc(
      "[[track]]\npath = \"/a.mp3\"\ntitle = \"A\"\nfavorite = true\nfeed = true\n"
      "realtime = true\nyear = 2020\nduration_secs = 180\n");
  REQUIRE(doc.tracks.size() == 1);
  CHECK(doc.tracks[0].bookmark);
  CHECK(doc.tracks[0].feed);
  CHECK(doc.tracks[0].realtime);
  CHECK(doc.tracks[0].year == 2020);
  CHECK(doc.tracks[0].duration_secs == 180);
}

TEST_CASE("expandPath env and tilde", "[local][expandPath]") {
  const fs::path dir = tmp_dir();
  ::setenv("BOOTAMP_TEST_DIR", dir.c_str(), 1);
  CHECK(detail::expand_path("$BOOTAMP_TEST_DIR/sub") == (dir / "sub").string());
  CHECK(detail::expand_path("${BOOTAMP_TEST_DIR}/sub") == (dir / "sub").string());
  CHECK(detail::expand_path("plain") == "plain");
  // Undefined variables expand to nothing (os.ExpandEnv).
  CHECK(detail::expand_path("$DEFINITELY_NOT_SET_VAR/x") == "/x");
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    CHECK(detail::expand_path("~/music") == (fs::path(home) / "music").string());
    CHECK(detail::expand_path("~") == std::string(home));
  }
}

// --- expand() over [[dir]] sources -------------------------------------------

TEST_CASE("expand scans dirs sorted with dir_sourced set", "[local][dirs]") {
  const fs::path audio = tmp_dir();
  make_audio_tree(audio);
  const auto doc = detail::parse_playlist_doc("[[dir]]\npath = " + quote(audio.string()) + "\n");
  const auto tracks = detail::expand(doc, true);
  REQUIRE(tracks.size() == 3);
  for (const auto& t : tracks) CHECK(t.dir_sourced);
  // Sorted by full path: a.flac, b.mp3, sub/c.ogg.
  check_order(tracks, {"a.flac", "b.mp3", "c.ogg"});
}

TEST_CASE("expand non-recursive only immediate children", "[local][dirs]") {
  const fs::path audio = tmp_dir();
  make_audio_tree(audio);
  const auto doc = detail::parse_playlist_doc(
      "[[dir]]\npath = " + quote(audio.string()) + "\nrecursive = false\n");
  const auto tracks = detail::expand(doc, false);
  REQUIRE(tracks.size() == 2);
}

TEST_CASE("expand explicit track shadows dir-sourced duplicate", "[local][dirs]") {
  const fs::path audio = tmp_dir();
  make_audio_tree(audio);
  const std::string explicit_path = (audio / "b.mp3").string();
  const auto doc = detail::parse_playlist_doc(
      "[[dir]]\npath = " + quote(audio.string()) + "\n\n"
      "[[track]]\npath = " + quote(explicit_path) + "\ntitle = \"Custom\"\nbookmark = true\n");
  const auto tracks = detail::expand(doc, true);
  REQUIRE(tracks.size() == 3);
  std::size_t found = 0;
  for (const auto& t : tracks) {
    if (t.path == explicit_path) {
      ++found;
      CHECK(t.title == "Custom");
      CHECK(t.bookmark);
      CHECK_FALSE(t.dir_sourced);
    }
  }
  CHECK(found == 1);
}

TEST_CASE("expand missing dir contributes nothing", "[local][dirs]") {
  const auto doc = detail::parse_playlist_doc("[[dir]]\npath = \"/nonexistent/xyz\"\n");
  CHECK(detail::expand(doc, true).empty());
}

TEST_CASE("dirSuppliesFile path checks", "[local][dirs]") {
  const fs::path dir = tmp_dir();
  const detail::DirSource rec{dir.string(), true};
  const detail::DirSource non_rec{dir.string(), false};
  CHECK(detail::dir_supplies_file(rec, (dir / "song.mp3").string()));
  CHECK(detail::dir_supplies_file(non_rec, (dir / "song.mp3").string()));
  CHECK_FALSE(detail::dir_supplies_file(rec, (dir / "cover.jpg").string()));
  CHECK_FALSE(detail::dir_supplies_file(rec, (dir / "cover.JPG").string()));
  CHECK_FALSE(detail::dir_supplies_file(rec, (dir / "noextension").string()));
  CHECK_FALSE(detail::dir_supplies_file(non_rec, (dir / "sub" / "song.mp3").string()));
  CHECK_FALSE(detail::dir_supplies_file(rec, (dir / ".." / "outside.mp3").string()));
}

// --- Provider: Playlists / Tracks --------------------------------------------

TEST_CASE("Playlists empty when no playlists dir exists", "[local][playlists]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const auto lists = env.p->playlists();
  REQUIRE(lists.has_value());
  CHECK(lists->empty());
}

TEST_CASE("Playlists lists saved playlists with track counts", "[local][playlists]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->save_playlist("rock", std::vector<Track>{{"/a.mp3", "A"}}).has_value());
  REQUIRE(env.p->save_playlist("jazz", std::vector<Track>{{"/b.mp3", "B"}, {"/c.mp3", "C"}})
              .has_value());

  const auto lists = env.p->playlists();
  REQUIRE(lists.has_value());
  REQUIRE(lists->size() == 2);
  std::set<std::string> names;
  for (const auto& l : *lists) {
    names.insert(l.name);
    CHECK(l.id == l.name);
    CHECK(l.section.empty());  // local has no section grouping
  }
  REQUIRE(names.count("rock") == 1);
  REQUIRE(names.count("jazz") == 1);
  for (const auto& l : *lists) {
    if (l.name == "rock") CHECK(l.track_count == 1);
    if (l.name == "jazz") CHECK(l.track_count == 2);
  }
}

TEST_CASE("Playlists skips non-toml and directory entries", "[local][playlists]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  write_playlist(env.root / "playlists", "real", "[[track]]\npath = \"/a.mp3\"\n");
  write_playlist(env.root / "playlists", "ignored.txt", "not a playlist");
  fs::create_directories(env.root / "playlists" / "dir.toml");

  const auto lists = env.p->playlists();
  REQUIRE(lists.has_value());
  REQUIRE(lists->size() == 1);
  CHECK((*lists)[0].name == "real");
}

TEST_CASE("Playlists count includes dir-sourced tracks", "[local][playlists]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  make_audio_tree(audio);
  write_playlist(env.root / "playlists", "music",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");

  const auto lists = env.p->playlists();
  REQUIRE(lists.has_value());
  bool found = false;
  for (const auto& l : *lists) {
    if (l.name == "music") {
      found = true;
      CHECK(l.track_count == 3);
      CHECK(l.duration_secs == 0);  // no tag reads in the count path
    }
  }
  CHECK(found);
}

TEST_CASE("Tracks missing playlist errors", "[local][tracks]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const auto tracks = env.p->tracks("nonexistent");
  CHECK_FALSE(tracks.has_value());
}

TEST_CASE("Tracks parses comments and sets stream from URL path", "[local][tracks]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  write_playlist(env.root / "playlists", "commented",
                 "# This is a comment\n"
                 "[[track]]\n"
                 "path = \"/a.mp3\"\n"
                 "title = \"A\"\n"
                 "# inline comment\n");
  write_playlist(env.root / "playlists", "radio",
                 "[[track]]\npath = \"https://stream.example.com/live\"\ntitle = \"Live Radio\"\n");

  const auto commented = env.p->tracks("commented");
  REQUIRE(commented.has_value());
  REQUIRE(commented->size() == 1);
  CHECK((*commented)[0].title == "A");

  const auto radio = env.p->tracks("radio");
  REQUIRE(radio.has_value());
  REQUIRE(radio->size() == 1);
  CHECK((*radio)[0].stream);
}

TEST_CASE("Tracks round-trips saved metadata", "[local][tracks]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  Track a{"/a.mp3", "A"};
  a.artist = "Art1";
  a.album = "Alb";
  a.year = 2020;
  a.track_number = 1;
  a.duration_secs = 180;
  a.bookmark = true;
  a.embedded_lyrics = "Line 1\nLine 2";
  a.album_art_url = "file:///tmp/a.jpg";
  Track b{"/b.flac", "B"};
  b.genre = "Jazz";
  b.feed = true;
  REQUIRE(env.p->save_playlist("test", std::vector<Track>{a, b}).has_value());

  const auto loaded = env.p->tracks("test");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->size() == 2);
  CHECK((*loaded)[0].path == "/a.mp3");
  CHECK((*loaded)[0].title == "A");
  CHECK((*loaded)[0].artist == "Art1");
  CHECK((*loaded)[0].album == "Alb");
  CHECK((*loaded)[0].year == 2020);
  CHECK((*loaded)[0].track_number == 1);
  CHECK((*loaded)[0].duration_secs == 180);
  CHECK((*loaded)[0].bookmark);
  CHECK((*loaded)[0].embedded_lyrics == "Line 1\nLine 2");
  CHECK((*loaded)[0].album_art_url == "file:///tmp/a.jpg");
  CHECK((*loaded)[1].path == "/b.flac");
  CHECK((*loaded)[1].genre == "Jazz");
  CHECK((*loaded)[1].feed);
}

TEST_CASE("savePlaylist preserves realtime stream metadata", "[local][tracks]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  Track want{"https://stream.example.com/live", "Live Radio"};
  want.stream = true;
  want.realtime = true;
  REQUIRE(env.p->save_playlist("radio", std::vector<Track>{want}).has_value());

  const auto tracks = env.p->tracks("radio");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);
  CHECK((*tracks)[0].stream);
  CHECK((*tracks)[0].realtime);
}

// --- Add track(s) -------------------------------------------------------------

TEST_CASE("addTrack creates and appends", "[local][add]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->add_track_to_playlist("new", Track{"/x.mp3", "X"}).has_value());
  auto tracks = env.p->tracks("new");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);
  CHECK((*tracks)[0].title == "X");

  REQUIRE(env.p->add_track_to_playlist("new", Track{"/y.mp3", "Y"}).has_value());
  tracks = env.p->tracks("new");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 2);
}

TEST_CASE("addTracks skips duplicate paths and reports counts", "[local][add]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->add_track_to_playlist("dupes", Track{"/a.mp3", "A"}).has_value());

  const auto res = env.p->add_tracks_to_playlist(
      "dupes", std::vector<Track>{{"/a.mp3", "A again"}, {"/b.mp3", "B"}, {"/b.mp3", "B again"}});
  REQUIRE(res.has_value());
  CHECK(res->first == 1);  // added
  CHECK(res->second == 2);  // skipped

  const auto tracks = env.p->tracks("dupes");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 2);
  CHECK((*tracks)[0].path == "/a.mp3");
  CHECK((*tracks)[1].path == "/b.mp3");
}

TEST_CASE("addTracks on a legacy colon name stays writable", "[local][add]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  write_playlist(env.root / "playlists", "bad:name",
                 "[[track]]\npath = \"/a.mp3\"\ntitle = \"A\"\n");

  const auto tracks = env.p->tracks("bad:name");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);
  CHECK((*tracks)[0].path == "/a.mp3");

  // Existing legacy playlists remain writable even though the name would be
  // rejected for new playlists.
  REQUIRE(env.p->add_track_to_playlist("bad:name", Track{"/b.mp3", "B"}).has_value());
  const auto after = env.p->tracks("bad:name");
  REQUIRE(after.has_value());
  REQUIRE(after->size() == 2);
  CHECK((*after)[1].path == "/b.mp3");

  // ...but creating a NEW playlist with such a name is rejected.
  const auto created = env.p->create_playlist("new:name");
  CHECK_FALSE(created.has_value());
}

// --- create / delete / rename / remove / bookmark -----------------------------

TEST_CASE("createPlaylist creates empty file and rejects duplicates", "[local][create]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const auto id = env.p->create_playlist("empty");
  REQUIRE(id.has_value());
  CHECK(*id == "empty");

  const auto tracks = env.p->tracks("empty");
  REQUIRE(tracks.has_value());
  CHECK(tracks->empty());

  const auto dup = env.p->create_playlist("empty");
  CHECK_FALSE(dup.has_value());
}

TEST_CASE("deletePlaylist removes the file", "[local][delete]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->add_track_to_playlist("del", Track{"/a.mp3", "A"}).has_value());
  REQUIRE(env.p->delete_playlist("del").has_value());
  CHECK_FALSE(fs::exists(env.root / "playlists" / "del.toml"));
  // Deleting a missing playlist errors (Go returns the os.Remove error).
  CHECK_FALSE(env.p->delete_playlist("del").has_value());
}

TEST_CASE("renamePlaylist renames the file and rejects collisions", "[local][rename]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->add_track_to_playlist("old", Track{"/a.mp3", "A"}).has_value());
  REQUIRE(env.p->rename_playlist("old", "new").has_value());
  CHECK_FALSE(fs::exists(env.root / "playlists" / "old.toml"));
  CHECK(fs::exists(env.root / "playlists" / "new.toml"));
  const auto tracks = env.p->tracks("new");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);

  CHECK_FALSE(env.p->rename_playlist("new", "new").has_value());  // dest exists
  CHECK_FALSE(env.p->rename_playlist("missing", "x").has_value());  // src missing
  CHECK_FALSE(env.p->rename_playlist("new", "bad:name").has_value());  // invalid dest
}

TEST_CASE("savePlaylist overwrites existing playlist", "[local][save]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->add_tracks_to_playlist(
              "over", std::vector<Track>{{"/a.mp3", "A"}, {"/b.mp3", "B"}})
              .has_value());
  REQUIRE(env.p->save_playlist("over", std::vector<Track>{{"/c.mp3", "C"}}).has_value());
  const auto tracks = env.p->tracks("over");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);
  CHECK((*tracks)[0].title == "C");
}

TEST_CASE("removeTrack removes by index and keeps empty playlists", "[local][remove]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->add_tracks_to_playlist(
              "rem", std::vector<Track>{{"/a.mp3", "A"}, {"/b.mp3", "B"}, {"/c.mp3", "C"}})
              .has_value());
  REQUIRE(env.p->remove_track("rem", 1).has_value());
  const auto tracks = env.p->tracks("rem");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 2);
  CHECK((*tracks)[0].title == "A");
  CHECK((*tracks)[1].title == "C");

  CHECK_FALSE(env.p->remove_track("rem", 5).has_value());  // out of range

  REQUIRE(env.p->remove_track("rem", 1).has_value());
  const auto last = env.p->tracks("rem");
  REQUIRE(last.has_value());
  CHECK(last->empty());
  CHECK(fs::exists(env.root / "playlists" / "rem.toml"));  // kept on disk
}

TEST_CASE("setBookmark toggles and persists", "[local][bookmark]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->add_track_to_playlist("marks", Track{"/a.mp3", "A"}).has_value());
  REQUIRE(env.p->set_bookmark("marks", 0).has_value());
  auto tracks = env.p->tracks("marks");
  REQUIRE(tracks.has_value());
  CHECK((*tracks)[0].bookmark);

  REQUIRE(env.p->set_bookmark("marks", 0).has_value());  // toggle off
  tracks = env.p->tracks("marks");
  REQUIRE(tracks.has_value());
  CHECK_FALSE((*tracks)[0].bookmark);

  CHECK_FALSE(env.p->set_bookmark("marks", 5).has_value());   // out of range
  CHECK_FALSE(env.p->set_bookmark("marks", -1).has_value());  // negative
}

// --- Reserved "Recently Played" name ------------------------------------------

TEST_CASE("reserved history name rejects mutations", "[local][history]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const Track t{"/a.mp3", "A"};
  CHECK_FALSE(env.p->add_track_to_playlist("Recently Played", t).has_value());
  CHECK_FALSE(env.p->add_tracks_to_playlist("Recently Played", std::vector<Track>{t}).has_value());
  CHECK_FALSE(env.p->save_playlist("Recently Played", std::vector<Track>{t}).has_value());
  CHECK_FALSE(env.p->create_playlist("Recently Played").has_value());
  CHECK_FALSE(env.p->delete_playlist("Recently Played").has_value());
  CHECK_FALSE(env.p->remove_track("Recently Played", 0).has_value());
  CHECK_FALSE(env.p->set_bookmark("Recently Played", 0).has_value());
  CHECK_FALSE(env.p->rename_playlist("x", "Recently Played").has_value());
  CHECK_FALSE(env.p->rename_playlist("Recently Played", "x").has_value());
}

TEST_CASE("reserved history name reads as empty", "[local][history]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  // bootamp has no history store, so the virtual playlist is always empty
  // (cliamp returns nil when its store is nil).
  const auto tracks = env.p->tracks("Recently Played");
  REQUIRE(tracks.has_value());
  CHECK(tracks->empty());
  const auto lists = env.p->playlists();
  REQUIRE(lists.has_value());
  for (const auto& l : *lists) CHECK(l.name != "Recently Played");
}

// --- [[dir]] interplay with save/remove/bookmark ------------------------------

TEST_CASE("savePlaylist preserves dirs and skips dir-sourced tracks", "[local][dirs]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  const std::string explicit_path = (audio / "b.mp3").string();
  write_audio_file(explicit_path);
  write_playlist(env.root / "playlists", "music",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");

  Track dir_sourced{(audio / "a.mp3").string(), "A"};
  dir_sourced.dir_sourced = true;  // must not be persisted
  REQUIRE(env.p->save_playlist("music", std::vector<Track>{{explicit_path, "B"}, dir_sourced})
              .has_value());

  std::ifstream in(env.root / "playlists" / "music.toml", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto doc = detail::parse_playlist_doc(data);
  REQUIRE(doc.dirs.size() == 1);
  CHECK(doc.dirs[0].path == audio.string());
  REQUIRE(doc.tracks.size() == 1);
  CHECK(doc.tracks[0].path == explicit_path);
}

TEST_CASE("setBookmark materializes a dir-sourced track", "[local][bookmark]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  write_playlist(env.root / "playlists", "music",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");

  REQUIRE(env.p->set_bookmark("music", 0).has_value());
  const auto tracks = env.p->tracks("music");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);
  CHECK((*tracks)[0].bookmark);
  CHECK_FALSE((*tracks)[0].dir_sourced);  // materialized as explicit [[track]]

  std::ifstream in(env.root / "playlists" / "music.toml", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto doc = detail::parse_playlist_doc(data);
  REQUIRE(doc.tracks.size() == 1);
  CHECK(doc.tracks[0].bookmark);
}

TEST_CASE("removeTrack on dir-sourced track errors and leaves file untouched", "[local][remove]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  write_playlist(env.root / "playlists", "music",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");

  CHECK_FALSE(env.p->remove_track("music", 0).has_value());
  const auto tracks = env.p->tracks("music");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);
}

TEST_CASE("removeTrack explicit track after dir keeps the dir source", "[local][remove]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  const fs::path other = tmp_dir();
  const std::string explicit_path = (other / "b.mp3").string();
  write_audio_file(explicit_path);
  write_playlist(env.root / "playlists", "music",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");
  REQUIRE(env.p->add_track_to_playlist("music", Track{explicit_path, "B"}).has_value());

  // Expanded order: a.mp3 (dir), b.mp3 (explicit). Removing index 1 must drop
  // the explicit track but keep the dir source.
  REQUIRE(env.p->remove_track("music", 1).has_value());
  const auto tracks = env.p->tracks("music");
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 1);
  CHECK((*tracks)[0].path != explicit_path);
}

TEST_CASE("addTracks skips paths supplied by a dir source", "[local][add]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  write_playlist(env.root / "playlists", "music",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");

  const auto res = env.p->add_tracks_to_playlist(
      "music", std::vector<Track>{{(audio / "a.mp3").string(), ""}});
  REQUIRE(res.has_value());
  CHECK(res->first == 0);
  CHECK(res->second == 1);
}

TEST_CASE("addTracks persists cross-playlist dir-sourced tracks as explicit", "[local][add]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  write_playlist(env.root / "playlists", "src",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");
  REQUIRE(env.p->create_playlist("dst").has_value());

  const auto src_tracks = env.p->tracks("src");
  REQUIRE(src_tracks.has_value());
  REQUIRE(src_tracks->size() == 1);
  CHECK((*src_tracks)[0].dir_sourced);

  const auto res = env.p->add_tracks_to_playlist("dst", *src_tracks);
  REQUIRE(res.has_value());
  CHECK(res->first == 1);
  CHECK(res->second == 0);

  const auto dst_tracks = env.p->tracks("dst");
  REQUIRE(dst_tracks.has_value());
  REQUIRE(dst_tracks->size() == 1);
  CHECK((*dst_tracks)[0].path == (*src_tracks)[0].path);
  CHECK_FALSE((*dst_tracks)[0].dir_sourced);  // persisted as explicit [[track]]

  std::ifstream in(env.root / "playlists" / "dst.toml", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto doc = detail::parse_playlist_doc(data);
  REQUIRE(doc.tracks.size() == 1);
}

TEST_CASE("savePlaylist non-audio track appended, not placed before dir", "[local][save]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  const std::string cover = (audio / "cover.jpg").string();
  write_audio_file(cover);
  write_playlist(env.root / "playlists", "mix",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");

  const auto res = env.p->add_tracks_to_playlist("mix", std::vector<Track>{{cover, "Cover"}});
  REQUIRE(res.has_value());
  CHECK(res->first == 1);
  CHECK(res->second == 0);

  const auto tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  // a.mp3 (dir), then cover.jpg (explicit, appended at end).
  check_order(*tracks, {"a.mp3", "cover.jpg"});
}

TEST_CASE("save aborts when playlist cannot be read", "[local][save]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path audio = tmp_dir();
  write_audio_file(audio / "a.mp3");
  write_playlist(env.root / "playlists", "music",
                 "[[dir]]\npath = " + quote(audio.string()) + "\n");
  // Replace the playlist file with a directory so reading fails (EISDIR).
  const fs::path path = env.root / "playlists" / "music.toml";
  fs::remove(path);
  fs::create_directories(path);

  // A failed read must abort the rewrite instead of replacing the playlist
  // with a copy missing its [[dir]] sections.
  CHECK_FALSE(env.p->set_bookmark("music", 0).has_value());
  CHECK(fs::is_directory(path));
}

// --- Section-order preservation on rewrites -----------------------------------

TEST_CASE("savePlaylist preserves interleaved section order", "[local][order]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path dir_a = tmp_dir();
  write_audio_file(dir_a / "a.mp3");
  const fs::path dir_b = tmp_dir();
  write_audio_file(dir_b / "b.mp3");
  write_audio_file(dir_b / "c.ogg");
  const fs::path x = tmp_dir() / "x.mp3";
  const fs::path y = tmp_dir() / "y.mp3";
  write_audio_file(x);
  write_audio_file(y);
  write_interleaved_doc(env.root / "playlists", x.string(), dir_a.string(), y.string(),
                        dir_b.string());

  // Expanded order follows the document interleaving.
  auto tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  check_order(*tracks, {"x.mp3", "a.mp3", "y.mp3", "b.mp3", "c.ogg"});

  // Removing x must drop only its slot: y stays anchored before dirB.
  REQUIRE(env.p->remove_track("mix", 0).has_value());
  tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  check_order(*tracks, {"a.mp3", "y.mp3", "b.mp3", "c.ogg"});

  // A metadata update (enrich-style) keeps y in place.
  Track* yt = nullptr;
  for (auto& t : *tracks) {
    if (t.path == y.string()) yt = &t;
  }
  REQUIRE(yt != nullptr);
  yt->album = "Studio";
  REQUIRE(env.p->save_playlist("mix", *tracks).has_value());
  tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  check_order(*tracks, {"a.mp3", "y.mp3", "b.mp3", "c.ogg"});
  for (const auto& t : *tracks) {
    if (t.path == y.string()) CHECK(t.album == "Studio");
  }
}

TEST_CASE("setBookmark materialized track keeps its dir position", "[local][order]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path dir_a = tmp_dir();
  write_audio_file(dir_a / "a.mp3");
  const fs::path dir_b = tmp_dir();
  write_audio_file(dir_b / "b.mp3");
  write_audio_file(dir_b / "c.ogg");
  const fs::path x = tmp_dir() / "x.mp3";
  const fs::path y = tmp_dir() / "y.mp3";
  write_audio_file(x);
  write_audio_file(y);
  write_interleaved_doc(env.root / "playlists", x.string(), dir_a.string(), y.string(),
                        dir_b.string());

  // Expanded: x, a, y, b, c. Bookmark b (index 3).
  REQUIRE(env.p->set_bookmark("mix", 3).has_value());
  const auto tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  // The materialized track stays at b's position instead of jumping around.
  check_order(*tracks, {"x.mp3", "a.mp3", "y.mp3", "b.mp3", "c.ogg"});
  for (const auto& t : *tracks) {
    if (t.path == (dir_b / "b.mp3").string()) {
      CHECK(t.bookmark);
      CHECK_FALSE(t.dir_sourced);
    }
  }
}

TEST_CASE("savePlaylist reorder keeps dir slots anchored", "[local][order]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path dir_a = tmp_dir();
  write_audio_file(dir_a / "a.mp3");
  const fs::path dir_b = tmp_dir();
  write_audio_file(dir_b / "b.mp3");
  write_audio_file(dir_b / "c.ogg");
  const fs::path x = tmp_dir() / "x.mp3";
  const fs::path y = tmp_dir() / "y.mp3";
  write_audio_file(x);
  write_audio_file(y);
  write_interleaved_doc(env.root / "playlists", x.string(), dir_a.string(), y.string(),
                        dir_b.string());

  auto tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  // Reorder so y leads: [y, x, a, b, c].
  const std::vector<Track> reordered{(*tracks)[2], (*tracks)[0], (*tracks)[1],
                                     (*tracks)[3], (*tracks)[4]};
  REQUIRE(env.p->save_playlist("mix", reordered).has_value());
  tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  check_order(*tracks, {"y.mp3", "a.mp3", "x.mp3", "b.mp3", "c.ogg"});
}

TEST_CASE("savePlaylist multi-materialized tracks keep dir positions", "[local][order]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  const fs::path dir_a = tmp_dir();
  write_audio_file(dir_a / "a.mp3");
  const fs::path dir_b = tmp_dir();
  write_audio_file(dir_b / "b.mp3");
  const fs::path x = tmp_dir() / "x.mp3";
  write_audio_file(x);
  // Document: [[dir A]], [[track x]], [[dir B]].
  write_playlist(env.root / "playlists", "mix",
                 "[[dir]]\npath = " + quote(dir_a.string()) + "\n\n"
                 "[[track]]\npath = " + quote(x.string()) + "\n\n"
                 "[[dir]]\npath = " + quote(dir_b.string()) + "\n");

  // Both dir tracks are materialized bookmarks handed back out of document
  // order, as a TUI reorder after bookmarking would.
  Track bx{(dir_b / "b.mp3").string(), "B"};
  bx.bookmark = true;
  Track ax{(dir_a / "a.mp3").string(), "A"};
  ax.bookmark = true;
  REQUIRE(env.p->save_playlist("mix", std::vector<Track>{{x.string(), "X"}, bx, ax})
              .has_value());

  const auto tracks = env.p->tracks("mix");
  REQUIRE(tracks.has_value());
  check_order(*tracks, {"a.mp3", "x.mp3", "b.mp3"});
}

// --- SearchTracks (fuzzy) -----------------------------------------------------

TEST_CASE("trackMatchScore across title, artist and album", "[local][search]") {
  CHECK(detail::track_match_score(Track{"", "Sakura"}, "skr").second);
  CHECK(detail::track_match_score(Track{"", "", "Radiohead", ""}, "rdhd").second);
  Track t{"", ""};
  t.album = "In Rainbows";
  CHECK(detail::track_match_score(t, "rainbow").second);
  CHECK_FALSE(detail::track_match_score(Track{"", "Sakura"}, "zzz").second);
  CHECK_FALSE(detail::track_match_score(Track{}, "x").second);
}

TEST_CASE("searchTracks fuzzy ranks and matches subsequence", "[local][search]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->save_playlist("lib",
                               std::vector<Track>{{"/1.mp3", "Cherry Blossom (Sakura Mix)"},
                                                  {"/2.mp3", "Sakura"},
                                                  {"/3.mp3", "Thunderstruck"}})
              .has_value());

  // "skr" is a non-contiguous subsequence a substring search would miss.
  const auto got = env.p->search_tracks("skr", 0);
  REQUIRE(got.has_value());
  REQUIRE(got->size() == 2);
  // "Sakura" (prefix match) outranks "Cherry Blossom (Sakura Mix)".
  CHECK((*got)[0].title == "Sakura");
}

TEST_CASE("searchTracks blank query returns empty", "[local][search]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->save_playlist("lib", std::vector<Track>{{"/1.mp3", "Sakura"}}).has_value());
  const auto got = env.p->search_tracks("   ", 0);
  REQUIRE(got.has_value());
  CHECK(got->empty());
}

TEST_CASE("searchTracks respects limit", "[local][search]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->save_playlist("lib",
                               std::vector<Track>{{"/1.mp3", "Sakura One"},
                                                  {"/2.mp3", "Sakura Two"},
                                                  {"/3.mp3", "Sakura Three"}})
              .has_value());
  const auto got = env.p->search_tracks("sakura", 2);
  REQUIRE(got.has_value());
  REQUIRE(got->size() == 2);
}

TEST_CASE("searchTracks dedupes across playlists", "[local][search]") {
  auto env = TempEnv::make();
  REQUIRE(env.p != nullptr);
  REQUIRE(env.p->save_playlist("one", std::vector<Track>{{"/a.mp3", "Sakura"}}).has_value());
  REQUIRE(env.p->save_playlist("two", std::vector<Track>{{"/a.mp3", "Sakura"}}).has_value());
  const auto got = env.p->search_tracks("sakura", 0);
  REQUIRE(got.has_value());
  REQUIRE(got->size() == 1);
}
