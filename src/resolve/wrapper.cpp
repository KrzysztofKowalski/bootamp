// resolve/wrapper.cpp — expand PLS/M3U wrapper URLs into the stream tracks.
//
// Port of cliamp ui/model/commands.go resolveWrapperURLs. The Go source calls
// resolve.Remote (which re-classifies the URL at fetch time); here the wrapper
// detection (IsURL && (IsPLS || IsM3U)) already narrows to m3u/pls — the
// extensions are mutually exclusive — so we call resolve_m3u / resolve_pls
// directly. Resolve errors and empty results fall back to the original track,
// matching Go's `if err == nil && len(resolved) > 0` swallow.
#include "resolve/wrapper.hpp"

#include "resolve/resolve.hpp"

#include "playlist/playlist.hpp"

#include <expected>
#include <vector>

namespace bootamp::resolve {

std::expected<WrapperExpansion, std::string> expand_wrapper_urls(
    const std::vector<playlist::Track>& tracks) {
  WrapperExpansion out;
  out.tracks.reserve(tracks.size());
  for (const playlist::Track& t : tracks) {
    if (playlist::is_url(t.path) &&
        (playlist::is_pls(t.path) || playlist::is_m3u(t.path))) {
      std::expected<std::vector<playlist::Track>, std::string> resolved =
          playlist::is_pls(t.path) ? resolve_pls(t.path) : resolve_m3u(t.path);
      if (resolved.has_value() && !resolved->empty()) {
        out.expanded = true;
        // Preserve the original title/artist on resolved tracks.
        for (playlist::Track& r : *resolved) {
          if (r.title.empty() || r.title == r.path) {
            r.title = t.title;
          }
          if (r.artist.empty()) {
            r.artist = t.artist;
          }
          if (t.realtime) {
            r.realtime = true;
          }
        }
        out.tracks.insert(out.tracks.end(), resolved->begin(), resolved->end());
        continue;
      }
    }
    out.tracks.push_back(t);
  }
  return out;
}

}  // namespace bootamp::resolve
