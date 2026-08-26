// ui/screens/help.cpp — keybindings help screen implementation.
//
// Model ports cliamp ui/model/keymap.go: buildKeymapEntries (main-context
// list = every Keymap-marked command, deduped), updateKeymapFilter,
// handleKeymapKey, renderKeymapList; the table itself is the commandRegistry
// Keymap rows from ui/model/command_registry.go.
#include "ui/screens/help.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): needs a compile pass against
// ftxui 7.0.3 once installed. Renders the filtered table as a Menu.
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

namespace {

// Port of Go clampScroll (ui/model/scroll.go).
void clamp_scroll(int& cursor, int& scroll, int count, int visible) {
  if (visible <= 0) {
    return;
  }
  if (cursor < 0) {
    cursor = 0;
  }
  if (cursor >= count && count > 0) {
    cursor = count - 1;
  }
  if (cursor < scroll) {
    scroll = cursor;
  } else if (cursor >= scroll + visible) {
    scroll = cursor - visible + 1;
  }
  if (scroll + visible > count && count > 0) {
    scroll = std::max(0, count - visible);
  }
  if (scroll < 0) {
    scroll = 0;
  }
}

// The global keybinding table — port of the commandRegistry Keymap=true rows
// (Go command_registry.go), in registry order, deduped by key+action like Go
// buildKeymapEntries. bootamp's queue-screen keys (s shuffle, r repeat,
// f favorite, d remove, c clear, enter play) are screen-local and listed in
// queue.hpp; the global table below matches Go 1:1.
std::vector<HelpEntry> build_entries() {
  std::vector<HelpEntry> out;
  const auto add = [&out](std::string key, std::string action) {
    // Go add(): dedupe on KeyLabel + Label.
    for (const HelpEntry& e : out) {
      if (e.key == key && e.action == action) {
        return;
      }
    }
    out.push_back(HelpEntry{std::move(key), std::move(action)});
  };

  add("Space", "Play / Pause");
  add("s", "Stop");
  add("> .", "Next track");
  add("< ,", "Previous track");
  add("Left Right", "Seek +/-5s");
  add("Shift+Left Right", "Seek +/-large step");
  add("Nj", "Seek to N x 10% of track (e.g. 7j = 70%)");
  add("+ -", "Volume up/down");
  add("] [", "Speed up/down (+/-0.25x)");
  add("z", "Toggle shuffle");
  add("r", "Cycle repeat");
  add("m", "Toggle mono");
  add("e", "Cycle EQ preset");
  add("t", "Choose theme");
  add("v", "Cycle visualizer");
  add("Ctrl+V", "Choose visualizer");
  add("V", "Full-screen visualizer");
  add("Up Down", "Playlist scroll / EQ adjust (wraps around)");
  add("PgUp PgDn / Ctrl+U D", "Scroll playlist/browser by page");
  add("Home End / g G", "Go to top/end of playlist/browser");
  add("Shift+Up Down", "Move track up/down");
  add("h l", "EQ cursor left/right");
  add("Enter", "Play selected track");
  add("a", "Toggle queue (play next)");
  add("A", "Queue manager");
  add("x", "Remove selected track from playlist");
  add("w", "Write selected track/selection to playlist");
  add("o", "Open file browser");
  add("N", "Provider browser");
  add("L", "Browse local playlists");
  add("R", "Open radio provider");
  add("S", "Open Spotify provider");
  add("P", "Open Plex provider");
  add("Y", "Open YouTube provider");
  add("C", "Open SoundCloud provider");
  add("M", "Open NetEase provider");
  add("J", "Open Jellyfin provider");
  add("E", "Open Emby provider");
  add("B", "Open Audiobookshelf provider");
  add("Q", "Open Qobuz provider");
  add("T", "Open Tidal provider");
  add("Ctrl+J", "Jump to time");
  add("p", "Playlist manager");
  add("Ctrl+H", "Toggle album headers");
  add("i", "Track info / metadata");
  add("Ctrl+S", "Save/download track to ~/Music/cliamp");
  add("Ctrl+X", "Expand/collapse view");
  add("/", "Filter/search list");
  add("f", "Toggle bookmark/favorite");
  add("Ctrl+F", "Search active provider or YouTube");
  add("u", "Load URL (stream/playlist)");
  add("d", "Audio device picker");
  add("y", "Show lyrics");
  add("Tab", "Toggle focus");
  add("Esc", "Back to provider");
  add("Ctrl+K", "Help");
  add("?", "Help");
  add("q", "Quit");
  return out;
}

}  // namespace

const std::vector<HelpEntry>& help_entries() {
  static const std::vector<HelpEntry> kEntries = build_entries();
  return kEntries;
}

void HelpModel::open() {
  // Go openKeymap: reset filter/cursor/scroll.
  visible_ = true;
  filtering_ = false;
  filter_.clear();
  filtered_.clear();
  cursor_ = 0;
  scroll_ = 0;
  normalize();
}

void HelpModel::set_filter(std::string_view query) {
  // Go updateKeymapFilter: rebuild filtered indices over key+action
  // (case-insensitive contains), reset cursor/scroll. A cleared filter shows
  // the full table (Go treats "" as unfiltered).
  filter_ = std::string(query);
  filtering_ = !filter_.empty();
  filtered_.clear();
  cursor_ = 0;
  scroll_ = 0;
  if (filter_.empty()) {
    normalize();
    return;
  }
  std::string q;
  q.reserve(filter_.size());
  for (const char c : filter_) {
    q.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  const auto& table = help_entries();
  for (std::size_t i = 0; i < table.size(); ++i) {
    if (table[i].divider) {
      continue;  // Go: dividers never match
    }
    std::string key = table[i].key;
    std::string action = table[i].action;
    for (char& c : key) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (char& c : action) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (key.find(q) != std::string::npos ||
        action.find(q) != std::string::npos) {
      filtered_.push_back(static_cast<int>(i));
    }
  }
  normalize();
}

int HelpModel::count() const {
  return filtering_ ? static_cast<int>(filtered_.size())
                    : static_cast<int>(help_entries().size());
}

void HelpModel::set_visible_rows(int rows) {
  visible_rows_ = std::max(rows, 0);
  normalize();
}

const HelpEntry& HelpModel::entry_at(int filtered_index) const {
  if (filtering_) {
    if (filtered_index < 0 ||
        filtered_index >= static_cast<int>(filtered_.size())) {
      static const HelpEntry kEmpty;
      return kEmpty;
    }
    return help_entries()[static_cast<std::size_t>(
        filtered_[static_cast<std::size_t>(filtered_index)])];
  }
  if (filtered_index < 0 ||
      filtered_index >= static_cast<int>(help_entries().size())) {
    static const HelpEntry kEmpty;
    return kEmpty;
  }
  return help_entries()[static_cast<std::size_t>(filtered_index)];
}

std::string HelpModel::row_label(int filtered_index) const {
  // Go renderKeymapList: fmt.Sprintf("%-10s %s", key, action); divider rows
  // render "  — <action> —" (Go dims them; color is the view layer's job).
  const HelpEntry& e = entry_at(filtered_index);
  if (e.divider) {
    return "  " + e.action;
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%-10s", e.key.c_str());
  return std::string(buf) + " " + e.action;
}

void HelpModel::cursor_up() {
  // Go handleKeymapKey up/k: wrap.
  const int n = count();
  if (cursor_ > 0) {
    --cursor_;
  } else if (n > 0) {
    cursor_ = n - 1;
  }
  normalize();
}

void HelpModel::cursor_down() {
  // Go handleKeymapKey down/j: wrap.
  const int n = count();
  if (cursor_ < n - 1) {
    ++cursor_;
  } else if (n > 0) {
    cursor_ = 0;
  }
  normalize();
}

void HelpModel::page_up() {
  // Go pgup/ctrl+u: cursor -= min(cursor, visible).
  const int n = count();
  if (cursor_ > 0) {
    cursor_ -= std::min(cursor_, std::max(1, visible_rows_));
  }
  if (n > 0 && cursor_ > n - 1) {
    cursor_ = n - 1;
  }
  normalize();
}

void HelpModel::page_down() {
  // Go pgdown/ctrl+d: cursor = min(count-1, cursor+visible).
  const int n = count();
  if (cursor_ < n - 1) {
    cursor_ = std::min(n - 1, cursor_ + std::max(1, visible_rows_));
  }
  normalize();
}

void HelpModel::go_top() {
  cursor_ = 0;
  normalize();
}

void HelpModel::go_bottom() {
  const int n = count();
  if (n > 0) {
    cursor_ = n - 1;
  }
  normalize();
}

bool HelpModel::handle_key(std::string_view key) {
  // Go handleKeymapKey. While the filter field is active (searching), text
  // goes through set_filter — handled by the host text editor; here we only
  // take the structural keys.
  if (key == "up" || key == "k") {
    cursor_up();
    return true;
  }
  if (key == "down" || key == "j") {
    cursor_down();
    return true;
  }
  if (key == "pgup" || key == "ctrl+u") {
    page_up();
    return true;
  }
  if (key == "pgdown" || key == "ctrl+d") {
    page_down();
    return true;
  }
  if (key == "home" || key == "g") {
    go_top();
    return true;
  }
  if (key == "end" || key == "G") {
    go_bottom();
    return true;
  }
  if (key == "/") {
    set_filter("");
    return true;
  }
  if (key == "backspace") {
    if (filtering()) {
      clear_filter();
    } else {
      close();
    }
    return true;
  }
  if (key == "h") {
    if (filtering()) {
      clear_filter();
    } else {
      close();
    }
    return true;
  }
  if (key == "enter" || key == "l" || key == "esc") {
    close();
    return true;
  }
  return false;
}

void HelpModel::normalize() {
  const int n = count();
  if (n == 0) {
    cursor_ = 0;
    scroll_ = 0;
    return;
  }
  cursor_ = std::clamp(cursor_, 0, n - 1);
  if (visible_rows_ <= 0) {
    scroll_ = std::clamp(scroll_, 0, n - 1);
    if (cursor_ < scroll_) {
      scroll_ = cursor_;
    }
    return;
  }
  clamp_scroll(cursor_, scroll_, n, visible_rows_);
}

#if BOOTAMP_HAS_FTXUI
std::shared_ptr<ftxui::ComponentBase> make_help_component(HelpModel& model) {
  auto entries  = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);
  auto menu = ftxui::Menu(entries.get(), selected.get(),
                          ftxui::MenuOption::Vertical());

  auto component = ftxui::CatchEvent(menu, [&model](const ftxui::Event& e) {
    return model.handle_key(e.character());
  });

  return ftxui::Renderer(component, [entries, selected, menu, &model] {
    entries->clear();
    const int n = model.count();
    if (n > 0) {
      for (int i = model.scroll(); i < n; ++i) {
        entries->push_back(model.row_label(i));
      }
      *selected = std::max(0, model.cursor() - model.scroll());
    }
    std::vector<ftxui::Element> lines = {ftxui::text("Keymap")};
    if (model.filtering()) {
      lines.push_back(ftxui::text("  / " + model.filter()));
    }
    lines.push_back(menu->Render() | ftxui::frame);
    return ftxui::vbox(lines);
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
