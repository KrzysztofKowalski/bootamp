// ui/screens/pl_picker.cpp — playlist picker overlay screen implementation.
//
// Model follows cliamp's theme-picker interaction (keys.go handleThemeKey
// up/down wrap, overlays.go themePickerSelect/themePickerCancel) applied to a
// generic host-supplied name list (playlists, per the task spec).
#include "ui/screens/pl_picker.hpp"

#include <algorithm>
#include <utility>

#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <vector>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

PlPickerModel::PlPickerModel(Actions actions) : actions_(std::move(actions)) {}

void PlPickerModel::open(std::vector<std::string> names) {
  names_  = std::move(names);
  cursor_ = 0;
  active_ = true;
}

void PlPickerModel::cancel() {
  // Go esc: themePickerCancel — close without picking.
  active_ = false;
  if (actions_.on_cancel) {
    actions_.on_cancel();
  }
}

void PlPickerModel::move(int d) {
  // Go handleThemeKey up/down: decrement wrapping to the last entry,
  // increment wrapping to the first; empty list stays at 0.
  const int n = count();
  if (n <= 0) {
    cursor_ = 0;
    return;
  }
  if (d > 0) {
    cursor_ = (cursor_ < n - 1) ? cursor_ + 1 : 0;
  } else if (d < 0) {
    cursor_ = (cursor_ > 0) ? cursor_ - 1 : n - 1;
  }
}

void PlPickerModel::submit() {
  // Go enter: themePickerSelect — apply the current entry and close; an
  // empty list applies nothing (bootamp: stay open, fire nothing).
  if (count() <= 0 || cursor_ < 0 || cursor_ >= count()) {
    return;
  }
  const std::string name = names_[static_cast<std::size_t>(cursor_)];
  active_ = false;
  if (actions_.on_pick) {
    actions_.on_pick(name);
  }
}

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): the exact ftxui API surface
// (7.0.3) needs a compile pass once FTXUI lands. The shell claims every key
// and routes them to the model (host), so the CatchEvent below is defensive,
// matching the other screens' glue (device.cpp/queue.cpp shape).
std::shared_ptr<ftxui::ComponentBase> make_pl_picker_component(PlPickerModel& model) {
  // Entries are rebuilt at render time from the live model so a host-side
  // reopen (open(names)) shows immediately.
  auto entries  = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);

  auto menu = ftxui::Menu(entries.get(), selected.get(),
                          ftxui::MenuOption::Vertical());

  auto component = ftxui::CatchEvent(menu, [&model](const ftxui::Event& e) {
    const std::string& c = e.character();
    if (c == "up" || c == "k") {
      model.move(-1);
      return true;
    }
    if (c == "down" || c == "j") {
      model.move(1);
      return true;
    }
    if (c == "enter") {
      model.submit();
      return true;
    }
    if (c == "esc") {
      model.cancel();
      return true;
    }
    return false;
  });

  return ftxui::Renderer(component, [&model, menu, entries, selected] {
    entries->clear();
    const int n = model.count();
    for (int i = 0; i < n; ++i) {
      entries->push_back("  " + model.names()[static_cast<std::size_t>(i)]);
    }
    *selected = model.cursor();
    std::vector<ftxui::Element> lines = {
        ftxui::text("Playlists"),
    };
    if (n == 0) {
      lines.push_back(ftxui::text("  No playlists available."));
    } else {
      lines.push_back(menu->Render() | ftxui::frame);
    }
    return ftxui::vbox(lines);
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
