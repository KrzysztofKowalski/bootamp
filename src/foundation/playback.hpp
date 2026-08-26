// foundation/playback.hpp — playback DTOs and message kinds.
//
// Port of cliamp/internal/playback/playback.go. The Go program uses Bubbletea
// `tea.Msg` values to drive its model update loop; bootamp is a single
// foreground app with no daemon/IPC, so Msg here is a plain value the UI tick
// loop consumes from a queue. State/Track/Status/Notifier map 1:1 to the Go
// types and keep their field names and semantics.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>

namespace bootamp::foundation {

using namespace std::chrono_literals;

// Status mirrors cliamp's `Status` string enum (Stopped/Playing/Paused).
enum class Status : std::uint8_t {
  Stopped,
  Playing,
  Paused,
};

// Duration alias — seconds as double (matches Go's time.Duration-as-float usage
// in the player and avoids integer-overflow at very large stream positions).
using Seconds = std::chrono::duration<double>;

// Track is the playback-layer track descriptor (cliamp playback.Track).
// Distinct from playlist::Track which carries provider metadata; the player
// only needs display fields and the source URL.
struct Track {
  std::string title;
  std::string artist;
  std::string album;
  std::string genre;
  int         track_number = 0;
  std::string url;
  std::string art_url;
  double      duration_secs = 0.0;  // 0 = unknown
};

// State is the complete observable playback state (cliamp playback.State).
struct State {
  Status   status      = Status::Stopped;
  Track    track;
  double   volume_db   = 0.0;
  Seconds  position     = Seconds::zero();
  bool     seekable     = false;
};

// Notifier is the abstract observer the engine pushes state changes to.
// The UI tick loop implements this; tests use a recording notifier.
class Notifier {
public:
  virtual ~Notifier() = default;
  virtual void update(const State&) = 0;
  virtual void seeked(Seconds offset) = 0;
};

// MsgKind enumerates the control messages the UI emits to the engine.
// Mirrors the cliamp playback.*Msg structs (PlayPauseMsg/SeekMsg/SetVolumeMsg...).
enum class MsgKind : std::uint8_t {
  PlayPause,
  Play,
  Pause,
  Next,
  Prev,
  Stop,
  Quit,
  Seek,         // payload: offset_secs
  SetPosition,  // payload: position_secs
  SetVolume,    // payload: volume_db
};

// Msg is the tagged control message. The payload union is intentionally small
// (three doubles) so Msg is trivially copyable and fits in a lock-free queue.
struct Msg {
  MsgKind kind = MsgKind::Stop;
  double  offset_secs   = 0.0;  // Seek: signed offset from current position
  double  position_secs = 0.0;  // SetPosition: absolute position
  double  volume_db     = 0.0;  // SetVolume: new volume in dB

  // Factory helpers mirroring the Go constructors.
  static Msg playPause()                 { return Msg{MsgKind::PlayPause}; }
  static Msg play()                       { return Msg{MsgKind::Play}; }
  static Msg pause()                      { return Msg{MsgKind::Pause}; }
  static Msg next()                       { return Msg{MsgKind::Next}; }
  static Msg prev()                       { return Msg{MsgKind::Prev}; }
  static Msg stop()                       { return Msg{MsgKind::Stop}; }
  static Msg quit()                       { return Msg{MsgKind::Quit}; }
  static Msg seek(double off)             { return Msg{MsgKind::Seek, off}; }
  static Msg setPosition(double p)        { return Msg{MsgKind::SetPosition, 0.0, p}; }
  static Msg setVolume(double db)         { return Msg{MsgKind::SetVolume, 0.0, 0.0, db}; }
};

}  // namespace bootamp::foundation