// resolve/wrapper.hpp — expand PLS/M3U wrapper URLs into the stream tracks.
//
// Port of cliamp ui/model/commands.go resolveWrapperURLs. Tracks whose path is
// a remote PLS/M3U playlist are replaced by the actual stream tracks the
// playlist contains, preserving the original track's title/artist. Resolve
// failures and empty results fall back to the original track unchanged.
#pragma once

#include "playlist/playlist.hpp"

#include <expected>
#include <string>
#include <vector>

namespace bootamp::resolve {

// WrapperExpansion is the result of expand_wrapper_urls (cliamp
// resolveWrapperURLs' two return values).
struct WrapperExpansion {
  std::vector<playlist::Track> tracks;
  bool expanded = false;  // true when at least one wrapper URL was expanded
};

// expand_wrapper_urls expands any PLS/M3U wrapper-URL tracks into the actual
// stream tracks they contain; non-wrapper tracks pass through unchanged. Port
// of cliamp ui/model/commands.go resolveWrapperURLs.
//
// The returned expected never holds an error: cliamp ignores resolve failures
// (`err == nil && len(resolved) > 0`) and keeps the original track, and this
// port mirrors that exactly. std::expected is kept for API symmetry.
std::expected<WrapperExpansion, std::string> expand_wrapper_urls(
    const std::vector<playlist::Track>& tracks);

}  // namespace bootamp::resolve
