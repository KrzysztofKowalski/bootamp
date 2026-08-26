// ui/screens/help.hpp — keybindings help screen: model + FTXUI component.
//
// Port of cliamp's keymap overlay (ui/model/keymap.go buildKeymapEntries /
// updateKeymapFilter / renderKeymapList / handleKeymapKey, and the
// commandRegistry table from ui/model/command_registry.go — the single source
// of key metadata). The model is plain C++: a static keybinding table, a
// filter, and cursor/scroll state. The FTXUI Component glue is compiled only
// when BOOTAMP_HAS_FTXUI.
//
// Keys (help screen): j/k or up/down move (wrap), PgUp/PgDn or Ctrl+U/Ctrl+D
// page, Home/End or g/G jump, "/" filters, backspace/h clears the filter
// (or closes), enter/l/esc closes.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if BOOTAMP_HAS_FTXUI
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// HelpEntry is one row of the keybinding table (Go keymapEntry). Rows with
// divider = true are unselectable section headers.
struct HelpEntry {
  std::string key;      // human-readable key label (Go KeyLabel)
  std::string action;   // action label (Go Label)
  bool        divider = false;
};

// help_entries returns the full keybinding table in Go commandRegistry order:
// the Keymap-marked commands (deduped by key+action), global player/library
// keys first (Go buildKeymapEntries main-context list). The table is static;
// bootamp has no Lua plugin bindings, so there is no "— plugins —" section.
const std::vector<HelpEntry>& help_entries();

// HelpModel holds the help screen state: cursor/scroll over the (filtered)
// table, and the filter query.
class HelpModel {
public:
  HelpModel() = default;
  ~HelpModel() = default;
  HelpModel(const HelpModel&)            = delete;
  HelpModel& operator=(const HelpModel&) = delete;

  // --- Visibility ---------------------------------------------------------
  void open();   // resets cursor/scroll/filter (Go openKeymap)
  void close() { visible_ = false; }
  bool visible() const { return visible_; }

  // --- Filter -------------------------------------------------------------
  void set_filter(std::string_view query);  // Go updateKeymapFilter
  void clear_filter() { set_filter(""); }
  bool filtering() const { return !filter_.empty(); }
  const std::string& filter() const { return filter_; }

  // --- List state ---------------------------------------------------------
  int  count() const;      // filtered entry count (Go keymapCount)
  int  cursor() const { return cursor_; }
  int  scroll() const { return scroll_; }
  void set_visible_rows(int rows);
  // filtered_entry maps a filtered index to the underlying table entry.
  const HelpEntry& entry_at(int filtered_index) const;
  // row_label renders "key action" left-padded to 10 columns (Go
  // renderKeymapList: fmt.Sprintf("%-10s %s", key, action)); divider rows
  // render the action text alone with a leading em dash.
  std::string row_label(int filtered_index) const;

  // --- Navigation (Go handleKeymapKey) ------------------------------------
  void cursor_up();      // wrap
  void cursor_down();    // wrap
  void page_up();        // PgUp / Ctrl+U
  void page_down();      // PgDn / Ctrl+D
  void go_top();         // Home / g
  void go_bottom();      // End / G

  // handle_key dispatches a Bubbletea-style key name; returns true if the
  // help screen consumed it. Text input for the filter is fed via set_filter.
  bool handle_key(std::string_view key);

private:
  void normalize();

  bool        visible_      = false;
  bool        filtering_    = false;
  std::string filter_;
  std::vector<int> filtered_;  // indices into help_entries()
  int         cursor_       = 0;
  int         scroll_       = 0;
  int         visible_rows_ = 0;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see help.cpp).
// Renders the (filtered) keybinding table as a Menu.
std::shared_ptr<ftxui::ComponentBase> make_help_component(HelpModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
