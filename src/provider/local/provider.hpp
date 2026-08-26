// provider/local/provider.hpp — local TOML-playlist Provider.
//
// Port of cliamp/external/local/provider.go. Reads/writes TOML-based playlists
// stored under ~/.config/bootamp/playlists/. Implements playlist::Provider plus
// the playlist-management capabilities (Writer/BatchWriter/Creator/Saver/
// Deleter/Renamer/BookmarkSetter/Searcher) that cliamp's local provider offers.
// Path traversal is rejected (safe_path ensures the result stays in dir).
#pragma once

#include "playlist/playlist.hpp"
#include "playlist/provider.hpp"
#include "provider/base.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bootamp::provider::local {

namespace fs = std::filesystem;

class Provider final : public playlist::Provider,
                       public Searcher {
public:
  // new creates a Provider rooted at <config_dir>/playlists/. Returns nullptr
  // if the config dir cannot be resolved (matches cliamp local.New returning nil).
  static std::unique_ptr<Provider> new_provider();

  std::string name() const override { return "Local"; }
  std::expected<std::vector<playlist::PlaylistInfo>, std::string> playlists() override;
  std::expected<std::vector<playlist::Track>, std::string>
  tracks(std::string_view playlist_id) override;

  // --- Searcher ---
  std::expected<std::vector<playlist::Track>, std::string>
  search_tracks(std::string_view query, int limit) override;

  // --- Playlist management (cliamp local provider methods) ---
  std::expected<void, std::string> add_track_to_playlist(std::string_view playlist_id,
                                                          const playlist::Track& t);
  std::expected<std::pair<int, int>, std::string>
  add_tracks_to_playlist(std::string_view playlist_id,
                         const std::vector<playlist::Track>& tracks);
  std::expected<void, std::string> save_playlist(std::string_view name,
                                                 const std::vector<playlist::Track>& tracks);
  std::expected<std::string, std::string> create_playlist(std::string_view name);
  std::expected<void, std::string> delete_playlist(std::string_view name);
  std::expected<void, std::string> remove_track(std::string_view name, int index);
  std::expected<void, std::string> rename_playlist(std::string_view old_name,
                                                    std::string_view new_name);
  std::expected<void, std::string> set_bookmark(std::string_view playlist_name, int idx);

private:
  explicit Provider(fs::path dir) : dir_(std::move(dir)) {}
  // safe_path validates a playlist name and returns the absolute TOML path,
  // rejecting traversal (cliamp local.safePath).
  std::expected<fs::path, std::string> safe_path(std::string_view name) const;

  fs::path dir_;
};

}  // namespace bootamp::provider::local