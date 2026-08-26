// ui/screens/info.cpp — track info overlay screen implementation.
//
// Model is a port of cliamp ui/model inline_overlays.go infoLines /
// renderInfoBody / infoMaybeAdjustScroll plus the "i"-overlay keys
// (keys.go:272-283, "i" opens at keys.go:764-765).
#include "ui/screens/info.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <vector>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

void InfoModel::open(const playlist::Track& t) {
  // Go "i" (keys.go:764-765): showInfo=true, infoScroll=0. The host hands the
  // current track; bootamp snapshots it so the overlay is stable while the
  // playlist advances.
  track_ = t;
  rebuild_lines();
  active_ = true;
  scroll_ = 0;
}

void InfoModel::scroll(int delta) {
  // Go info keys: up/k decrement only when > 0 (never negative); down/j
  // increment, then infoMaybeAdjustScroll clamps the scroll so the line
  // window stays in range. bootamp's model clamps into [0, line_count-1]
  // (the component frame-clips the rest).
  scroll_ = std::clamp(scroll_ + delta, 0, std::max(0, line_count() - 1));
}

std::string InfoModel::line(int i) const {
  if (i < 0 || i >= line_count()) {
    return "";
  }
  return lines_[static_cast<std::size_t>(i)];
}

void InfoModel::rebuild_lines() {
  // Go infoLines (inline_overlays.go): field(label, value) appends
  // "  label: value" for non-empty values; Year/Track only when non-zero.
  lines_.clear();
  const auto field = [this](std::string_view label, std::string_view value) {
    if (!value.empty()) {
      lines_.emplace_back("  " + std::string(label) + ": " + std::string(value));
    }
  };
  field("Title", track_.title);
  field("Artist", track_.artist);
  field("Album", track_.album);
  field("Genre", track_.genre);
  if (track_.year != 0) {
    field("Year", std::to_string(track_.year));
  }
  if (track_.track_number != 0) {
    field("Track", std::to_string(track_.track_number));
  }
  // bootamp addition over Go infoLines (the task spec lists duration; Go's
  // overlay stops at Path). Formatted mm:ss like Go formatJumpClock — minutes
  // may exceed 59 for long tracks.
  if (track_.duration_secs > 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", track_.duration_secs / 60,
                  track_.duration_secs % 60);
    field("Duration", buf);
  }
  field("Path", track_.path);
  if (lines_.empty()) {
    lines_.emplace_back("  No track metadata available.");
  }
}

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): the exact ftxui API surface
// (7.0.3) needs a compile pass once FTXUI lands. The shell claims every key
// and routes them to the model (host), so the CatchEvent below is defensive,
// matching the other screens' glue.
std::shared_ptr<ftxui::ComponentBase> make_info_component(InfoModel& model) {
  auto base = ftxui::Renderer([] { return ftxui::emptyElement(); });
  auto component = ftxui::CatchEvent(base, [&model](const ftxui::Event& e) {
    const std::string& c = e.character();
    if (c == "up" || c == "k") {
      model.scroll(-1);
      return true;
    }
    if (c == "down" || c == "j") {
      model.scroll(1);
      return true;
    }
    if (c == "esc" || c == "enter" || c == "i") {
      model.close();  // Go: esc/i close; bootamp also accepts enter
      return true;
    }
    return false;
  });

  return ftxui::Renderer(component, [&model] {
    // Go renderInfoBody: the line window starts at the model scroll; the
    // frame clips it to the terminal (Go's effectivePlaylistVisible budget).
    std::vector<ftxui::Element> lines = {ftxui::text("Track Info")};
    for (int i = model.scroll(); i < model.line_count(); ++i) {
      lines.push_back(ftxui::dim(ftxui::text(model.line(i))));
    }
    return ftxui::vbox(lines) | ftxui::frame;
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
