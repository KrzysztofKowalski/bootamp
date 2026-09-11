// foundation/resume.hpp — persist last-played track/position for resume.
//
// Port of cliamp/internal/resume/resume.go. State is JSON-serialized to
// <config_dir>/resume.json. save() no-ops when there is nothing to resume
// (an empty source and no open panel) or a negative position — cliamp's
// guard against overwriting valid state with junk. bootamp extends the
// state with the open screen so the next launch reopens the same panel.
#pragma once

#include <filesystem>
#include <expected>
#include <string>

namespace bootamp::foundation {

namespace fs = std::filesystem;

// State holds enough information to resume a previous playback session.
struct ResumeState {
  std::string path;              // source URL/path of the last-played track
  int         position_sec = 0;  // last playback position in seconds
  std::string playlist;          // playlist name, if any
  // bootamp: the panel the session left open (cliamp has no screen state).
  // screen is "" for the main frame, "gieres" for the archive browser (p) or
  // "radio" for the radio browse screen (R). screen_tab holds the Gieres tab
  // — "live" | "orgonity" | "echelon" — and stays empty for every other
  // screen.
  std::string screen;
  std::string screen_tab;
};

// state_file returns <config_dir>/resume.json.
std::expected<fs::path, std::string> resume_state_file();

// save writes the resume state. No-ops (returns {}) when there is nothing
// to resume — an empty source AND no open panel (cliamp's empty-path guard;
// a zero position persists, it is what a live-stream session leaves) — or a
// negative position. Errors are swallowed (returned, not thrown) so a failed
// write never disrupts normal exit.
std::expected<void, std::string> resume_save(const ResumeState& s);

// load reads the resume state. Returns a zero ResumeState if the file is
// missing or unparseable.
std::expected<ResumeState, std::string> resume_load();

}  // namespace bootamp::foundation