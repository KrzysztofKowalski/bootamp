// ui/screens/pl_picker.hpp — playlist picker overlay screen: model + FTXUI
// component.
//
// A generic name-list picker (playlists, per the task spec): the host opens
// it with a list of names ("t" opens the theme picker analog in Go), j/k or
// up/down move the cursor with wrap (Go handleThemeKey up/down wrap to the
// last/first entry), enter submits the picked name to the host (Go
// themePickerSelect: apply + close), esc cancels (Go themePickerCancel).
//
// The model is plain C++ (no FTXUI): it owns the name list, cursor, and
// visibility state and fires host-wired action callbacks. The FTXUI Component
// glue is compiled only when BOOTAMP_HAS_FTXUI is defined.
#pragma once

#include <functional>
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

// Actions the host app must wire (Go: the update-loop side effects). Defined
// at namespace scope (not nested in PlPickerModel) so the constructor's
// default argument can value-initialize it — a nested struct is still
// incomplete at the point the default argument is parsed.
struct PlPickerActions {
  // enter on an entry — load the picked playlist (Go themePickerSelect).
  // Fires only while the name list is non-empty.
  std::function<void(std::string_view name)> on_pick{};
  // esc — close without picking; host-side cleanup such as clearing a
  // transient status message. Must not call cancel() back.
  std::function<void()> on_cancel{};
};

// PlPickerModel shows a host-supplied list of names (e.g. playlists) with a
// cursor. open(names) replaces the list and resets the cursor; move(d) wraps
// around the ends (Go theme picker up/down: > 0 → --, 0 → last; < last → ++,
// last → 0); submit() fires on_pick(names[cursor]) and closes when the list
// is non-empty (Go themePickerSelect); cancel() closes and fires on_cancel.
class PlPickerModel {
public:
  using Actions = PlPickerActions;

  PlPickerModel(Actions actions = Actions{});
  ~PlPickerModel() = default;
  PlPickerModel(const PlPickerModel&)            = delete;
  PlPickerModel& operator=(const PlPickerModel&) = delete;

  void set_actions(Actions a) { actions_ = std::move(a); }

  // --- Visibility ---------------------------------------------------------
  // open shows the picker with the given names and the cursor reset to 0.
  void open(std::vector<std::string> names);
  // cancel hides the picker and fires actions_.on_cancel if wired (Go esc →
  // themePickerCancel).
  void cancel();
  bool active() const { return active_; }

  // --- List + cursor ------------------------------------------------------
  int count() const { return static_cast<int>(names_.size()); }
  int cursor() const { return cursor_; }
  const std::vector<std::string>& names() const { return names_; }

  // move moves the cursor by d with wrap (Go theme picker up/down). An empty
  // list is a no-op (cursor stays 0).
  void move(int d);

  // submit — enter. Fires actions_.on_pick(names_[cursor]) and closes while
  // the list is non-empty (Go themePickerSelect: apply + visible=false); an
  // empty list fires nothing and stays open.
  void submit();

private:
  Actions                 actions_;
  std::vector<std::string> names_;
  bool                    active_ = false;
  int                     cursor_ = 0;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see
// pl_picker.cpp). Renders the picker as a scrollable Menu (same shape as the
// device picker). Defined in pl_picker.cpp.
std::shared_ptr<ftxui::ComponentBase> make_pl_picker_component(PlPickerModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
