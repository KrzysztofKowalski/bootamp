// foundation/resume.hpp — persist last-played track/position for resume.
//
// Port of cliamp/internal/resume/resume.go. State is JSON-serialized to
// <config_dir>/resume.json. save() no-ops on empty path or non-positive
// position (matches Go's guard against overwriting valid state with junk).
#pragma once

#include <filesystem>
#include <expected>
#include <string>

namespace bootamp::foundation {

namespace fs = std::filesystem;

// State holds enough information to resume a previous playback session.
struct ResumeState {
  std::string path;           // source URL/path of the last-played track
  int         position_sec = 0;  // last playback position in seconds
  std::string playlist;       // playlist name, if any
};

// state_file returns <config_dir>/resume.json.
std::expected<fs::path, std::string> resume_state_file();

// save writes the resume state. No-ops (returns {}) for empty path or
// non-positive position, mirroring cliamp. Errors are swallowed (returned,
// not thrown) so a failed write never disrupts normal exit.
std::expected<void, std::string> resume_save(const ResumeState& s);

// load reads the resume state. Returns a zero ResumeState if the file is
// missing or unparseable.
std::expected<ResumeState, std::string> resume_load();

}  // namespace bootamp::foundation