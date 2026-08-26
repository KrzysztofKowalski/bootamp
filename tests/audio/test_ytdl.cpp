// tests/audio/test_ytdl.cpp — Catch2 v3 tests for the yt-dlp | ffmpeg pipe
// module (audio/ytdl.cpp): exact child argv, prefill/cause surfacing via
// wait_cause, probe duration parsing, availability, is_ytdl routing, and
// engine seek_ytdl generation-cancellation — all driven by fake yt-dlp/ffmpeg
// shell scripts shadowed onto PATH.
//
// Fixtures: FakeTools mkdtemp's a scratch dir, writes the fake scripts,
// chmod 0755, prepends the dir to PATH, and (in dir-only mode) replaces PATH.
// The fakes record their argv and pid into files named by env vars and branch
// on env vars (stderr/exit, probe output, infinite output, seek sleep), so
// every test asserts on deterministic files rather than wall-clock timing.
#include "audio/audio_sink.hpp"
#include "audio/engine.hpp"
#include "audio/ytdl.hpp"
#include "playlist/playlist.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using bootamp::audio::AudioEngine;
using bootamp::audio::EngineConfig;
using bootamp::audio::Frame;
using bootamp::audio::YtdlpPipeStreamer;
using bootamp::audio::make_null_sink;
using bootamp::audio::probe_ytdlp_duration;
using bootamp::audio::set_ytdl_cookies_from;
using bootamp::audio::ytdlp_available;

namespace fs = std::filesystem;

// --- tiny file/env helpers ---------------------------------------------------

void write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << content;
}


// read_lines strips trailing newlines; a missing file is an empty vector.
std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream f(path);
  std::string   line;
  while (std::getline(f, line)) {
    out.push_back(line);
  }
  return out;
}

// set_env with value == "" unsets the variable.
void set_env(const char* name, const char* value) {
  if (value == nullptr || *value == '\0') {
    ::unsetenv(name);
  } else {
    ::setenv(name, value, 1);
  }
}

// process_alive: kill(pid, 0) — false for both dead and reaped children.
bool process_alive(pid_t pid) {
  return ::kill(pid, 0) == 0;
}

// wait_until polls pred every 5ms until true or the timeout elapses.
bool wait_until(std::function<bool()> pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// --- FakeTools: fake yt-dlp / ffmpeg on PATH --------------------------------

// Env vars the fakes read; every one is set (to "" or a file path) by
// FakeTools so the scripts never hit an unset-variable redirect error.
constexpr const char* kFakeEnv[] = {
    "YTDLP_ARGV_FILE",    "YTDLP_PID_FILE",     "YTDLP_STDERR",
    "YTDLP_EXIT",         "YTDLP_INFINITE",     "YTDLP_PROBE_OUTPUT",
    "FFMPEG_ARGV_FILE",   "FFMPEG_STDERR",      "FFMPEG_EXIT",
    "FFMPEG_SEEK_SLEEP",
};

// Fake yt-dlp: records argv + pid, then branches:
//   1. YTDLP_STDERR set   → echo it, exit YTDLP_EXIT (default 1) — nothing
//      on stdout (a dead pipe → decode_ytdlp_pipe empty-EOF path).
//   2. argv contains --print → probe invocation: print YTDLP_PROBE_OUTPUT,
//      exit 0, WITHOUT recording a pid (the engine's concurrent duration
//      probe must not pollute the pid file).
//   3. else → record pid, then YTDLP_INFINITE ? endless zeros : 4096 bytes.
constexpr const char* kFakeYtdlp = R"(#!/bin/sh
echo "$*" >> "$YTDLP_ARGV_FILE"
if [ -n "$YTDLP_STDERR" ]; then
  echo "$YTDLP_STDERR" >&2
  exit "${YTDLP_EXIT:-1}"
fi
case " $* " in
  *" --print "*)
    printf '%s' "$YTDLP_PROBE_OUTPUT"
    exit 0
    ;;
esac
echo "$$" >> "$YTDLP_PID_FILE"
if [ -n "$YTDLP_INFINITE" ]; then
  while :; do
    head -c 65536 /dev/zero || break
  done
else
  head -c 4096 /dev/zero
fi
exit 0
)";

// Fake ffmpeg: records argv, then branches:
//   1. FFMPEG_STDERR set → echo it, exit FFMPEG_EXIT (default 1) — nothing
//      on stdout, so the decode prefill sees empty-EOF and surfaces the cause.
//   2. argv contains -ss → seek-build: sleep FFMPEG_SEEK_SLEEP before
//      reading, so the prefill blocks long enough for the test to cancel
//      mid-build (the seek restart's -ss lives in ffmpeg's argv, not
//      yt-dlp's — yt-dlp has no idea it is being seeked).
//   3. else → cat stdin to stdout (the PCM path), exit 0.
constexpr const char* kFakeFfmpeg = R"(#!/bin/sh
echo "$*" >> "$FFMPEG_ARGV_FILE"
if [ -n "$FFMPEG_STDERR" ]; then
  echo "$FFMPEG_STDERR" >&2
  exit "${FFMPEG_EXIT:-1}"
fi
case " $* " in
  *" -ss "*)
    if [ -n "$FFMPEG_SEEK_SLEEP" ]; then
      sleep "$FFMPEG_SEEK_SLEEP"
    fi
    ;;
esac
cat
exit 0
)";

// FakeTools owns one scratch dir. install_tools=false leaves it empty (for
// "tool missing" tests); dir_only=true replaces PATH instead of prepending
// (so real yt-dlp on the host can't satisfy look_path).
class FakeTools {
public:
  explicit FakeTools(bool install_tools = true, bool dir_only = false)
      : dir_(make_dir()) {
    old_path_ = std::getenv("PATH") ? std::getenv("PATH") : "";
    if (install_tools) {
      const std::string yt = dir_ + "/yt-dlp";
      const std::string ff = dir_ + "/ffmpeg";
      write_file(yt, kFakeYtdlp);
      write_file(ff, kFakeFfmpeg);
      ::chmod(yt.c_str(), 0755);
      ::chmod(ff.c_str(), 0755);
    }
    set_env("PATH", (dir_only ? dir_ : dir_ + ":" + old_path_).c_str());
    for (const char* v : kFakeEnv) {
      set_env(v, "");
    }
    set_env("YTDLP_ARGV_FILE", (dir_ + "/ytdlp.argv").c_str());
    set_env("YTDLP_PID_FILE", (dir_ + "/ytdlp.pids").c_str());
    set_env("FFMPEG_ARGV_FILE", (dir_ + "/ffmpeg.argv").c_str());
  }

  ~FakeTools() {
    for (const char* v : kFakeEnv) {
      ::unsetenv(v);
    }
    set_env("PATH", old_path_.c_str());
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  FakeTools(const FakeTools&)            = delete;
  FakeTools& operator=(const FakeTools&) = delete;

  const std::string& dir() const { return dir_; }
  std::string file(const std::string& name) const { return dir_ + "/" + name; }

  // pid_lines parses the yt-dlp pid file defensively (a concurrent append
  // can be caught mid-write).
  std::vector<pid_t> pids() const {
    std::vector<pid_t> out;
    for (const std::string& l : read_lines(file("ytdlp.pids"))) {
      try {
        out.push_back(static_cast<pid_t>(std::stoi(l)));
      } catch (...) {
        // partial line from a concurrent append — skip
      }
    }
    return out;
  }

private:
  static std::string make_dir() {
    char tmpl[] = "/tmp/bootamp_ytdl_XXXXXX";
    if (::mkdtemp(tmpl) == nullptr) {
      return "/tmp";
    }
    return tmpl;
  }

  std::string dir_;
  std::string old_path_;
};

// --- exact argv expectations ------------------------------------------------

const std::string kYtDlpBase =
    "yt-dlp -f bestaudio[protocol=https]/bestaudio[protocol=http]/"
    "bestaudio[protocol!=m3u8_native][protocol!=m3u8]/bestaudio/best "
    "--no-playlist --quiet --no-warnings --socket-timeout 15 -o -";

const std::string kFfmpegSuffix16 =
    "-i pipe:0 -f s16le -acodec pcm_s16le -ar 44100 -ac 2 -loglevel error pipe:1";

const std::string kFfmpegSuffix32 =
    "-i pipe:0 -f f32le -acodec pcm_f32le -ar 44100 -ac 2 -loglevel error pipe:1";

}  // namespace

// ---------------------------------------------------------------------------
// 1. Exact child argv
// ---------------------------------------------------------------------------

TEST_CASE("ytdl decode_ytdlp_pipe spawns yt-dlp | ffmpeg with the exact Go argv",
          "[ytdl]") {
  set_ytdl_cookies_from("");  // restore the global for a clean start
  FakeTools ft;
  const std::string url = "https://youtu.be/abc";

  // No cookies, bit depth 16, no seek.
  {
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 0);
    REQUIRE(r.has_value());
    CHECK(read_lines(ft.file("ytdlp.argv")).at(0) ==
          kYtDlpBase + " " + url);
    CHECK(read_lines(ft.file("ffmpeg.argv")).at(0) ==
          std::string("ffmpeg ") + kFfmpegSuffix16);
    (*r)->close();
  }

  // Cookies from browser chrome.
  {
    set_ytdl_cookies_from("chrome");
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 0);
    REQUIRE(r.has_value());
    CHECK(read_lines(ft.file("ytdlp.argv")).at(1) ==
          kYtDlpBase + " --cookies-from-browser chrome " + url);
    CHECK(read_lines(ft.file("ffmpeg.argv")).at(1) ==
          std::string("ffmpeg ") + kFfmpegSuffix16);
    (*r)->close();
    set_ytdl_cookies_from("");
  }

  // start_sec=7 → input-side ffmpeg -ss BEFORE -i pipe:0.
  {
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 7);
    REQUIRE(r.has_value());
    CHECK(read_lines(ft.file("ffmpeg.argv")).at(2) ==
          std::string("ffmpeg -ss 7 ") + kFfmpegSuffix16);
    (*r)->close();
  }

  // bit_depth=32 → f32le / pcm_f32le.
  {
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 32, 0);
    REQUIRE(r.has_value());
    CHECK(read_lines(ft.file("ffmpeg.argv")).at(3) ==
          std::string("ffmpeg ") + kFfmpegSuffix32);
    (*r)->close();
  }

  // Missing yt-dlp → the LookPath error, not a spawn error.
  {
    FakeTools empty(false, true);  // empty dir, PATH = dir only
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 0);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("yt-dlp is required") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// 2. wait_cause surfacing (prefill empty-EOF path)
// ---------------------------------------------------------------------------

TEST_CASE("ytdl decode prefill surfaces the cause, preferring yt-dlp", "[ytdl]") {
  set_ytdl_cookies_from("");
  const std::string url = "https://youtu.be/abc";

  // Both children fail: yt-dlp's reason wins (bot wall > ffmpeg decode err).
  {
    FakeTools ft;
    set_env("YTDLP_STDERR", "Sign in to confirm you're not a bot");
    set_env("YTDLP_EXIT", "1");
    set_env("FFMPEG_STDERR", "Invalid data found when processing input");
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 0);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() ==
          "yt-dlp: exit status 1: Sign in to confirm you're not a bot");
  }

  // yt-dlp exits clean but ffmpeg fails: ffmpeg's reason surfaces.
  {
    FakeTools ft;
    set_env("YTDLP_STDERR", "");
    set_env("FFMPEG_STDERR", "Invalid data found when processing input");
    set_env("FFMPEG_EXIT", "1");
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 0);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "ffmpeg: exit status 1: Invalid data found when processing input");
  }

  // Only yt-dlp reports (ffmpeg sees a clean EOF and exits 0).
  {
    FakeTools ft;
    set_env("YTDLP_STDERR", "HTTP Error 404: Not Found");
    set_env("YTDLP_EXIT", "1");
    set_env("FFMPEG_STDERR", "");
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 0);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "yt-dlp: exit status 1: HTTP Error 404: Not Found");
  }

  // Both clean: 4096 zero bytes → 1024 frames, then a clean EOF — no error
  // is ever published and the position advances exactly once per frame.
  {
    FakeTools ft;
    set_env("YTDLP_STDERR", "");
    set_env("FFMPEG_STDERR", "");
    auto r = YtdlpPipeStreamer::decode_ytdlp_pipe(url, 44100, 16, 0);
    REQUIRE(r.has_value());
    auto& s = *r;

    std::array<Frame, 512> buf{};
    const auto [n1, ok1] = s->stream(buf);
    CHECK(n1 == 512);
    CHECK(ok1);
    CHECK(s->err().empty());
    CHECK(s->position() == 512);

    const auto [n2, ok2] = s->stream(buf);
    CHECK(n2 == 512);
    CHECK(ok2);
    CHECK(s->err().empty());
    CHECK(s->position() == 1024);

    const auto [n3, ok3] = s->stream(buf);
    CHECK(n3 == 0);
    CHECK_FALSE(ok3);
    CHECK(s->err().empty());  // clean exit both sides ⇒ bare EOF, no error
    CHECK(s->position() == 1024);
    s->close();
  }
}

// ---------------------------------------------------------------------------
// 3. probe_ytdlp_duration
// ---------------------------------------------------------------------------

TEST_CASE("ytdl probe_ytdlp_duration parses --print duration output", "[ytdl]") {
  set_ytdl_cookies_from("");
  FakeTools ft;
  const std::string url = "https://youtu.be/abc";

  struct Case {
    const char* out;
    double      expect;
  };
  const Case cases[] = {
      {"123.5\n", 123.5}, {" 42 ", 42},  {"0", 0},        {"-4", 0},
      {"garbage", 0},     {"123abc", 0}, {"nan", 0},      {"inf", 0},
      {"", 0},            {"1e3", 1000},
  };
  for (const Case& c : cases) {
    set_env("YTDLP_PROBE_OUTPUT", c.out);
    const double got = probe_ytdlp_duration(url).count();
    CHECK(got == Catch::Approx(c.expect));
  }

  // Exact probe argv with cookies.
  set_ytdl_cookies_from("chrome");
  set_env("YTDLP_PROBE_OUTPUT", "99");
  CHECK(probe_ytdlp_duration(url).count() == Catch::Approx(99.0));
  set_ytdl_cookies_from("");
  CHECK(read_lines(ft.file("ytdlp.argv")).back() ==
        "yt-dlp --skip-download --no-playlist --socket-timeout 10 --print "
        "duration --cookies-from-browser chrome " + url);

  // Missing yt-dlp → 0 (spawn fails inside the probe).
  {
    FakeTools empty(false, true);
    CHECK(probe_ytdlp_duration(url).count() == Catch::Approx(0.0));
  }
}

// ---------------------------------------------------------------------------
// 4. ytdlp_available
// ---------------------------------------------------------------------------

TEST_CASE("ytdl ytdlp_available reflects PATH", "[ytdl]") {
  {
    FakeTools ft;  // fake yt-dlp prepended to PATH
    CHECK(ytdlp_available());
  }
  {
    FakeTools empty(false, true);  // PATH = empty dir only
    CHECK_FALSE(ytdlp_available());
  }
}

// ---------------------------------------------------------------------------
// 5. is_ytdl routing (cliamp playlist.go IsYTDL)
// ---------------------------------------------------------------------------

TEST_CASE("ytdl is_ytdl routes the YT/SC/Bandcamp/Bilibili family", "[ytdl]") {
  using bootamp::playlist::is_ytdl;

  CHECK(is_ytdl("https://youtube.com/watch?v=dQw4w9WgXcQ"));
  CHECK(is_ytdl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
  CHECK(is_ytdl("https://m.youtube.com/watch?v=dQw4w9WgXcQ"));
  CHECK(is_ytdl("https://youtu.be/dQw4w9WgXcQ"));
  CHECK(is_ytdl("https://music.youtube.com/watch?v=dQw4w9WgXcQ"));
  CHECK(is_ytdl("https://www.youtube.com/shorts/dQw4w9WgXcQ"));
  CHECK(is_ytdl("https://soundcloud.com/artist/track"));
  CHECK(is_ytdl("https://bandcamp.com/track/abc"));
  CHECK(is_ytdl("https://artist.bandcamp.com/track/abc"));
  CHECK(is_ytdl("https://www.bilibili.com/video/BV1xx411c7mD"));
  CHECK(is_ytdl("https://space.bilibili.com/123456"));
  CHECK(is_ytdl("https://b23.tv/AbC123"));
  CHECK(is_ytdl("https://music.163.com/#/song?id=123"));
  CHECK(is_ytdl("ytsearch:never gonna give you up"));
  CHECK(is_ytdl("scsearch:some tune"));

  CHECK_FALSE(is_ytdl("https://example.com/song.mp3"));
  CHECK_FALSE(is_ytdl("https://example.com/stream.m3u8"));
  CHECK_FALSE(is_ytdl("http://example.com"));
  CHECK_FALSE(is_ytdl("ftp://youtube.com/video.mp4"));
  CHECK_FALSE(is_ytdl("/home/user/Music/track.flac"));
  CHECK_FALSE(is_ytdl("track.mp3"));
}

// ---------------------------------------------------------------------------
// 6. Engine seek_ytdl generation cancellation (seek-by-restart)
// ---------------------------------------------------------------------------

TEST_CASE("ytdl engine seek_ytdl restarts the pipe with input-side -ss", "[ytdl][engine]") {
  set_ytdl_cookies_from("");
  FakeTools ft;
  set_env("YTDLP_INFINITE", "1");  // endless PCM — the loop never blocks
  set_env("YTDLP_PROBE_OUTPUT", "123.5\n");
  const std::string url = "https://youtu.be/abc";

  AudioEngine eng(make_null_sink(), EngineConfig{44100, 16});

  // Section A: play, then seek by 30s.
  CHECK(eng.play_ytdl(url, 0.0).empty());
  auto pids = ft.pids();
  REQUIRE(pids.size() == 1);  // probe invocation recorded no pid
  const pid_t old_pid = pids[0];

  // Freeze the drain before seeking so the position math is deterministic
  // (the audio loop would otherwise run away at full speed on infinite data).
  eng.toggle_pause();

  CHECK(eng.seek_ytdl(30.0).empty());

  // The committed pipeline sits at the seek point: stream_offset(30) +
  // a decoder position frozen at 0 (audio thread parked).
  const double pos = eng.position_secs();
  CHECK(pos > 29.0);
  CHECK(pos < 33.0);

  // The seek build spawned a second yt-dlp with ffmpeg -ss INPUT-side.
  auto pids2 = ft.pids();
  REQUIRE(pids2.size() == 2);
  CHECK(process_alive(pids2[1]));

  // The old pipeline was closed asynchronously — its yt-dlp dies.
  CHECK(wait_until([&] { return !process_alive(old_pid); },
                   std::chrono::seconds(2)));

  const auto ff_lines = read_lines(ft.file("ffmpeg.argv"));
  REQUIRE(ff_lines.size() == 2);
  CHECK(ff_lines[0] == std::string("ffmpeg ") + kFfmpegSuffix16);
  // Seek build: "ffmpeg -ss <N> -i pipe:0 ..." with N = 30 + the pre-pause
  // drain (bounded — the pause lands within microseconds of play), and the
  // rest of the template untouched: -ss goes BEFORE -i (input-side restart).
  const std::string line1 = ff_lines[1];
  CHECK(line1.rfind("ffmpeg -ss ", 0) == 0);
  const std::size_t i_pos = line1.find(" -i pipe:0 ");
  REQUIRE(i_pos != std::string::npos);
  const int start_sec = std::stoi(line1.substr(11, i_pos - 11));
  CHECK(start_sec >= 30);
  CHECK(start_sec <= 60);
  CHECK(line1.substr(i_pos + 1) == kFfmpegSuffix16);
}

TEST_CASE("ytdl engine cancel_seek_ytdl discards the in-flight build", "[ytdl][engine]") {
  set_ytdl_cookies_from("");
  FakeTools ft;
  set_env("YTDLP_INFINITE", "1");   // old stream keeps running forever
  set_env("YTDLP_PROBE_OUTPUT", "123.5\n");
  set_env("FFMPEG_SEEK_SLEEP", "2");  // seek build's ffmpeg stalls 2s
  const std::string url = "https://youtu.be/abc";

  AudioEngine eng(make_null_sink(), EngineConfig{44100, 16});
  CHECK(eng.play_ytdl(url, 0.0).empty());
  const auto pids = ft.pids();
  REQUIRE(pids.size() == 1);
  const pid_t old_pid = pids[0];

  // Seek in a background thread (it blocks in the 2s prefill); cancel after
  // the discarded build has spawned (its pid is recorded at spawn).
  std::string seek_err = "unset";
  std::thread t([&] { seek_err = eng.seek_ytdl(30.0); });
  REQUIRE(wait_until([&] { return ft.pids().size() >= 2; },
                     std::chrono::seconds(2)));
  const pid_t discarded_pid = ft.pids()[1];

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  eng.cancel_seek_ytdl();
  t.join();

  // The cancelled build is discarded silently (Go: closePipelines + no error).
  CHECK(seek_err.empty());
  // The old pipeline was never touched by the cancel.
  CHECK(process_alive(old_pid));
  // The discarded build's children are killed by the async closer.
  CHECK(wait_until([&] { return !process_alive(discarded_pid); },
                   std::chrono::seconds(2)));
}
