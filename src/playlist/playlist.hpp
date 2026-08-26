// playlist/playlist.hpp — ordered track list with shuffle/repeat/queue.
//
// Port of cliamp/playlist/playlist.go. Thread-safe (mutex), atomic<uint64_t>
// revision. Fisher-Yates shuffle, RepeatMode {Off,All,One}, play-next queue,
// snapshot/restore, M3U/PLS encode/decode helpers. The Track struct carries
// provider metadata (ProviderMeta map). URL classifiers (IsURL/IsYTDL/IsM3U...)
// are ported 1:1.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::playlist {

// RepeatMode controls playlist repeat behavior (cliamp RepeatMode).
enum class RepeatMode : std::uint8_t {
  Off  = 0,
  All  = 1,
  One  = 2,
};
std::string_view to_string(RepeatMode r);

// Track represents a single audio file or HTTP stream (cliamp playlist.Track).
struct Track {
  std::string path{};
  std::string title{};
  std::string artist{};
  std::string album{};
  std::string genre{};
  int         year          = 0;
  int         track_number  = 0;
  bool        stream        = false;  // true for HTTP/HTTPS URLs
  bool        realtime      = false;  // true for live/radio
  bool        feed          = false;  // RSS/podcast feed (resolved before play)
  int         duration_secs = 0;       // 0 = unknown
  bool        bookmark      = false;
  bool        unplayable    = false;
  bool        dir_sourced   = false;   // re-derived on load, never persisted
  std::string embedded_lyrics{};
  std::string album_art_url{};         // file:// URL for cached embedded art
  std::map<std::string, std::string> provider_meta{};

  // meta returns the value for a provider-specific key, or "" if unset.
  std::string meta(std::string_view key) const;
  bool        is_live() const { return realtime; }
  std::string  display_name() const;  // "Artist - Title" or "Title"
  bool        is_album() const;        // provider_meta[kind]=="album"
  std::string album_id() const;        // provider_meta[albumID] or ""
};

// ProviderMeta key constants shared across providers (cliamp playlist.go).
inline constexpr std::string_view kMetaKind      = "kind";
inline constexpr std::string_view kMetaKindAlbum  = "album";
inline constexpr std::string_view kMetaAlbumID   = "albumID";

// total_duration_secs sums DurationSecs, skipping unknown (0) entries.
int total_duration_secs(const std::vector<Track>& tracks);

// URL classifiers — ported 1:1 from cliamp playlist.go.
bool is_url(std::string_view path);
bool is_ytsearch(std::string_view path);
bool is_m3u(std::string_view path);
bool is_local_m3u(std::string_view path);
bool is_pls(std::string_view path);
bool is_local_pls(std::string_view path);
bool is_youtube_url(std::string_view path);
bool is_youtube_music_url(std::string_view path);
bool is_ytdl(std::string_view path);          // YT/SC/Bandcamp/Bilibili/ytsearch
bool is_xiaoyuzhou_episode(std::string_view p);
bool is_feed(std::string_view path);

// track_from_path creates a Track by parsing a filename/URL (cliamp
// TrackFromPath). For URLs extracts a clean title from the path; for local
// files tag-reading is done by tags.hpp (caller-side).
Track track_from_path(std::string_view path);

// RepeatMode cycle helper: Off→All→One.
RepeatMode cycle_repeat(RepeatMode r);

// QueueEntry identifies one play-next entry + the live track it references.
struct QueueEntry {
  int   track_index;
  Track track;
};

// SelectionActivation describes the playable track activated from the selected
// row (cliamp SelectionActivation).
struct SelectionActivation {
  Track track;
  int   index   = -1;
  bool  skipped = false;
};

// Snapshot preserves the complete mutable playback state for restoration.
// Its fields are public but Restore is the only sanctioned writer.
struct Snapshot {
  std::vector<Track> tracks;
  std::vector<int>   order;
  int                pos        = 0;
  bool               shuffle    = false;
  RepeatMode         repeat     = RepeatMode::Off;
  std::vector<int>   queue;
  int                queued_idx = -1;
};

// Playlist manages an ordered list of tracks (cliamp Playlist). All public
// methods are safe for concurrent use. revision() increments on any mutation
// that changes observable state.
class Playlist {
public:
  Playlist() = default;

  // --- Track management ---------------------------------------------------
  void replace(const std::vector<Track>& tracks);
  void add(const std::vector<Track>& tracks);
  int  len() const;
  std::uint64_t revision() const { return revision_.load(); }

  // --- Current / index ----------------------------------------------------
  std::pair<Track, int> current() const;
  int  index() const;
  bool current_is_queued() const;
  std::optional<SelectionActivation> activate_selected();
  std::pair<Track, bool> next();
  std::pair<Track, bool> peek_next() const;
  std::pair<Track, bool> prev();
  void set_index(int i);

  // --- Queue -------------------------------------------------------------
  void queue(int track_idx);
  bool dequeue(int track_idx);
  int  queue_position(int track_idx) const;
  int  queue_len() const;
  std::vector<Track>    queue_tracks() const;
  std::vector<QueueEntry> queue_entries() const;
  std::vector<Track>    queue_window(int start, int limit) const;
  void clear_queue();
  void remove_queue_at(int pos);
  bool move_queue(int from, int to);

  // --- Mutation ----------------------------------------------------------
  bool move(int from, int to);
  bool remove(int idx);
  void set_track(int i, const Track& t);
  std::vector<Track> tracks() const;
  std::optional<Track> track(int index) const;
  std::vector<Track> track_window(int start, int limit) const;
  void toggle_bookmark(int idx);
  int  bookmark_count() const;

  // --- Shuffle / repeat --------------------------------------------------
  void toggle_shuffle();
  bool shuffled() const;
  void cycle_repeat();
  void set_repeat(RepeatMode mode);
  RepeatMode repeat() const;

  // --- Snapshot / restore ------------------------------------------------
  Snapshot snapshot() const;
  void restore(const Snapshot& s);

private:
  mutable std::mutex       mu_;
  std::atomic<std::uint64_t> revision_{0};
  std::vector<Track>       tracks_;
  std::vector<int>         order_;
  int                      pos_         = 0;
  bool                     shuffle_     = false;
  RepeatMode               repeat_      = RepeatMode::Off;
  std::vector<int>         queue_;
  std::map<int,int>        queue_positions_;
  int                      queued_idx_  = -1;
  int                      bookmark_count_ = 0;

  void do_shuffle_locked();
  void rebuild_queue_positions_locked();
  void rebuild_bookmark_count_locked();
};

}  // namespace bootamp::playlist