// provider/track.hpp — shared track helpers used across providers.
//
// cliamp re-exports playlist.Track through provider packages; bootamp keeps
// playlist::Track as the single type and adds here the few provider-side
// constructors that don't belong on the playlist struct (e.g. catalog station →
// Track). Keeps provider/ headers from depending on playlist internals.
#pragma once

#include "playlist/playlist.hpp"

#include <string>
#include <string_view>

namespace bootamp::provider {

// radio_track builds a Track for a single radio station URL (stream + realtime,
// matching cliamp radio.Tracks: []Track{{Path,Title,Stream:true,Realtime:true}}).
playlist::Track radio_track(std::string_view url, std::string_view title);

// ytdl_track builds a Track from a yt-dlp --flat-playlist JSON entry.
playlist::Track ytdl_track(std::string_view path, std::string_view title,
                           std::string_view artist, double duration_secs);

}  // namespace bootamp::provider