// provider/local/internal.hpp — internal detail surface of the local
// playlist provider. Shared with the provider TU and with tests; NOT part of
// the public module interface (see provider.hpp for the contract). Do not
// depend on this from outside the provider/local subsystem.
//
// Port of cliamp/external/local/dirs.go (DirSource, playlistDoc,
// parsePlaylistDoc, expand, writeDir, rebuildDoc, isSubsequence,
// dirSuppliesFile) and the package-internal helpers of provider.go that do
// not belong on the Provider class (safePath/validateNewName/loadDoc/...).
#pragma once

#include "playlist/playlist.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace bootamp::provider::local {

namespace fs = std::filesystem;

namespace detail {

// kHistoryPlaylistName is the virtual "Recently Played" name cliamp serves
// from its history store (history.PlaylistName). bootamp has no history
// module, so the name stays reserved (mutations are rejected like cliamp)
// and reads yield an empty list — equivalent to cliamp's nil history store.
inline constexpr std::string_view kHistoryPlaylistName = "Recently Played";

// Error returned for mutations of the reserved history name (cliamp
// errReservedHistoryName).
inline constexpr std::string_view kReservedHistoryError =
    "\"Recently Played\" is a virtual history playlist and cannot be modified";

bool is_history_name(std::string_view name);

// --- Path helpers -----------------------------------------------------------

// expand_path expands a leading ~ and environment variables in p (cliamp
// ExpandPath: os.ExpandEnv, then ~ → $HOME).
std::string expand_path(std::string_view p);

// trim_space trims ASCII whitespace from both ends (Go strings.TrimSpace for
// ASCII space/tab/cr/lf/vt/ff).
std::string trim_space(std::string_view s);

// trim_suffix removes one occurrence of `suffix` when s ends with it
// (Go strings.TrimSuffix).
std::string trim_suffix(std::string_view s, std::string_view suffix);

// ascii_lower lowercases ASCII letters only.
std::string ascii_lower(std::string_view s);

// ext_of is filepath.Ext: the suffix from the final dot in the final element.
std::string ext_of(std::string_view path);

// --- Playlist document model (cliamp dirs.go) -------------------------------

// Section kinds tracked in PlaylistDoc::order (cliamp itemTrack/itemDir).
enum class Item : std::uint8_t {
  track = 0,
  dir   = 1,
};

// DirSource is a [[dir]] section: a directory scanned for audio files on
// every load instead of listing files explicitly (cliamp DirSource).
struct DirSource {
  std::string path;       // supports ~ and environment variables
  bool        recursive = true;  // scan subdirectories too (default true)
};

// PlaylistDoc is a parsed playlist file: explicit [[track]] entries and
// [[dir]] sources, with section order preserved for ordered expansion
// (cliamp playlistDoc).
struct PlaylistDoc {
  std::vector<playlist::Track> tracks;
  std::vector<DirSource>       dirs;
  std::vector<Item>            order;
};

// parse_playlist_doc parses a playlist TOML document. Directory sources are
// parsed but not scanned; call expand to resolve them into tracks. A [[dir]]
// section without a path is dropped entirely (cliamp parsePlaylistDoc).
PlaylistDoc parse_playlist_doc(std::string_view data);

// expand returns the full track list: explicit [[track]] entries plus tracks
// scanned from [[dir]] sources, in document order. A file supplied by a
// directory scan is skipped when an explicit [[track]] with the same path
// exists anywhere in the document, so explicit entries (with their custom
// metadata and bookmarks) always win. Directory-scanned tracks are marked
// dir_sourced. Unreadable or missing directories contribute no tracks.
// With with_tags=false directory tracks are returned without reading their
// tags (titles fall back to filename parsing) for cheap operations such as
// counting (cliamp playlistDoc.expand).
std::vector<playlist::Track> expand(const PlaylistDoc& doc, bool with_tags);

// write_track renders one [[track]] TOML section (cliamp writeTrack).
std::string write_track(const playlist::Track& t);

// write_dir renders one [[dir]] TOML section (cliamp writeDir).
std::string write_dir(const DirSource& src);

// rebuild_doc merges the caller's explicit tracks back into an existing
// parsed document, preserving the interleaving of [[track]] and [[dir]]
// sections (cliamp rebuildDoc). Directory sections keep their slots; explicit
// tracks are matched back onto their original slots by path; additions and
// bookmark materializations are inserted directly before the directory
// section that would supply them; tracks no directory provides are appended.
std::tuple<std::vector<playlist::Track>, std::vector<DirSource>, std::vector<Item>>
rebuild_doc(const PlaylistDoc& existing, const std::vector<playlist::Track>& explicit_tracks);

// is_subsequence reports whether `sub` appears in `orig` in the same relative
// order (cliamp isSubsequence).
bool is_subsequence(const std::vector<std::string>& orig,
                    const std::vector<std::string>& sub);

// dir_supplies_file reports whether a scan of dir would include file: the
// path has a supported audio extension, lives under the expanded directory
// and, for non-recursive sources, not below an immediate subdirectory. The
// check is path-only so save-time rewrites do not repeat the filesystem walk
// done at load (cliamp dirSuppliesFile).
bool dir_supplies_file(const DirSource& dir, std::string_view file);

// --- File collection + track building --------------------------------------

// audio_files returns sorted audio file paths under dir. When recursive is
// false only the directory's immediate children are considered. A file path
// with a supported extension is returned directly. Hidden files are included
// (Go never filters them); unreadable subdirectories are skipped. Port of
// cliamp resolve.AudioFiles.
std::expected<std::vector<std::string>, std::string>
audio_files(std::string_view dir, bool recursive);

// tracks_from_paths converts file paths to Tracks concurrently (tag reading),
// preserving order. Port of cliamp resolve.scanTracks.
std::vector<playlist::Track> tracks_from_paths(const std::vector<std::string>& files);

// track_from_filename creates a Track by parsing "Artist - Title" from the
// filename, or using the bare filename as the title (cliamp
// playlist.TrackFromFilename). NOTE: cliamp also runs sanitizeTag (mojibake
// repair) on the derived strings; that helper is internal to playlist.cpp and
// the only caller of this variant is the count-only expand(false) path, whose
// titles are never surfaced — so it is omitted here.
playlist::Track track_from_filename(std::string_view path);

// --- Search ---------------------------------------------------------------

// track_match_score returns the best fuzzy score for query across the track's
// title, artist and album, and whether any of them matched (cliamp
// trackMatchScore).
std::pair<int, bool> track_match_score(const playlist::Track& t,
                                       std::string_view query);

// --- Document IO -----------------------------------------------------------

// read_file reads the whole file (cliamp os.ReadFile). On failure `err_out`
// receives errno (0 on success) so callers can distinguish ENOENT, matching
// cliamp's errors.Is(err, fs.ErrNotExist).
std::expected<std::string, std::string> read_file(const fs::path& path, int& err_out);

// write_file writes `data` to `path`, creating it with 0644 & umask
// (cliamp os.WriteFile).
std::expected<void, std::string> write_file(const fs::path& path, std::string_view data);

// safe_path validates a playlist name and returns the absolute TOML path,
// ensuring the result stays within dir. Rejects names containing "/" or "\"
// and blank names; the result must keep the cleaned-dir prefix (cliamp
// local.safePath).
std::expected<fs::path, std::string> safe_path(const fs::path& dir, std::string_view name);

// validate_new_name rejects non-portable new playlist names (cliamp
// validateNewName). Returns an error message or std::nullopt.
std::optional<std::string> validate_new_name(std::string_view name);

// load_doc_at reads and parses the playlist file at `file` (cliamp loadDoc).
// `err_out` (optional) receives errno when reading fails.
std::expected<PlaylistDoc, std::string> load_doc_at(const fs::path& file, int* err_out = nullptr);

// load_doc resolves `name` via safe_path then parses it (cliamp
// loadDocByName).
std::expected<PlaylistDoc, std::string> load_doc(const fs::path& dir,
                                                 std::string_view name,
                                                 int* err_out = nullptr);

// existing_doc parses `path` into a document, returning an empty document
// only when the file does not exist. Read failures are wrapped and propagated
// so a broken playlist is never silently rewritten without its [[dir]]
// sections (cliamp existingDoc).
std::expected<PlaylistDoc, std::string> existing_doc(const fs::path& dir,
                                                     const fs::path& path);

// save_doc writes a parsed document back to disk preserving section order.
// The full document is rendered in memory before the atomic tmp+rename so a
// partial write can never clobber the existing playlist (cliamp saveDoc).
std::expected<void, std::string> save_doc(const fs::path& dir, std::string_view name,
                                          const PlaylistDoc& doc);

// save_playlist overwrites the named playlist with the given tracks,
// preserving [[dir]] sections and their interleaving with explicit
// [[track]] sections. Tracks marked dir_sourced are not persisted; they are
// re-derived from the directory sources on the next load (cliamp
// savePlaylist).
std::expected<void, std::string> save_playlist(const fs::path& dir, std::string_view name,
                                               const std::vector<playlist::Track>& tracks);

}  // namespace detail

}  // namespace bootamp::provider::local
