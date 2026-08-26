// ui/screens/info.hpp — track info overlay screen: model + FTXUI component.
//
// Port of cliamp's track-info overlay (ui/model/keys.go: the "i" key at
// keys.go:764-765 opens with infoScroll=0, keys.go:272-283 handle the scroll
// keys; inline_overlays.go renderInfoBody/infoLines/infoMaybeAdjustScroll).
// The overlay lists the current track's metadata fields; up/k and down/j
// scroll a line window through the list (Go: up guarded at 0, down re-clamped
// by infoMaybeAdjustScroll).
//
// The model is plain C++ (no FTXUI): it stores a copy of the track, owns the
// visibility/scroll state, and exposes the rendered lines for the component
// (and tests). No actions are wired — closing is the only side effect. The
// FTXUI Component glue is compiled only when BOOTAMP_HAS_FTXUI is defined.
#pragma once

#include "playlist/playlist.hpp"

#include <memory>
#include <string>
#include <vector>

#if BOOTAMP_HAS_FTXUI
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// InfoModel shows the current track's metadata (Go infoLines): Title, Artist,
// Album, Genre, Year (!=0), Track number (!=0), Duration (duration_secs > 0,
// bootamp addition), and Path; empty fields are skipped and a fully empty
// track renders "No track metadata available." (Go). open(t) snapshots the
// track (the host hands it the current track; the live playlist can advance
// underneath, so the copy freezes what "i" showed).
class InfoModel {
public:
  InfoModel() = default;
  ~InfoModel() = default;
  InfoModel(const InfoModel&)            = delete;
  InfoModel& operator=(const InfoModel&) = delete;

  // --- Visibility ---------------------------------------------------------
  // open snapshots the track and shows the overlay with the scroll reset
  // (Go "i": showInfo=true, infoScroll=0).
  void open(const playlist::Track& t);
  // close hides the overlay (Go esc/i/enter: showInfo=false).
  void close() { active_ = false; }
  bool active() const { return active_; }

  // --- Scrolling (Go info keys: up/k down/j + infoMaybeAdjustScroll) -------
  // scroll moves the line window by `delta` and clamps it into
  // [0, line_count-1] (Go: up guarded at 0, down re-clamped so the window
  // stays in range).
  void scroll(int delta);
  int  scroll() const { return scroll_; }

  // --- Track data ----------------------------------------------------------
  const playlist::Track& track() const { return track_; }
  // line_count is the number of rendered metadata lines (Go infoLines).
  int line_count() const { return static_cast<int>(lines_.size()); }
  // line renders "  Label: value" for entry i (Go dimStyle label +
  // trackStyle value; bootamp keeps the lines plain and lets the component
  // style them).
  std::string line(int i) const;

private:
  void rebuild_lines();  // Go infoLines

  playlist::Track track_;
  std::vector<std::string> lines_;
  bool active_ = false;
  int  scroll_ = 0;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see info.cpp).
// Renders the track info overlay: a header plus the line window starting at
// the model scroll (frame-clipped to the terminal). Defined in info.cpp.
std::shared_ptr<ftxui::ComponentBase> make_info_component(InfoModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
