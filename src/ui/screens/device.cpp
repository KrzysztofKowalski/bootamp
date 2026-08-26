// ui/screens/device.cpp — audio device picker screen implementation.
//
// Model is a 1:1 port of cliamp ui/model: handleDeviceKey (keys.go:2728-2765),
// deviceMaybeAdjustScroll (keys.go:2725), renderDeviceBody + deviceHeaderLine
// (inline_overlays.go), and clampScroll (scroll.go), translated onto the
// engine's list_devices()/switch_device() API (bootamp enumerates devices
// natively via miniaudio — no pactl subprocess).
#include "ui/screens/device.hpp"

#include "ui/fit.hpp"

#include <algorithm>
#include <cstdio>
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

namespace {

// Port of Go clampScroll (ui/model/scroll.go): keep cursor inside [0, count)
// and scroll such that the cursor is within the `visible`-row window.
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

}  // namespace

DevicePickerModel::DevicePickerModel(Actions actions, LoadFn load)
    : actions_(std::move(actions)), load_(std::move(load)) {}

void DevicePickerModel::open() {
  // Go "d" key: visible=true, cursor=0, scroll=0; the host fires the lazy
  // list load (Go listDevicesCmd) when the list was never loaded.
  visible_ = true;
  cursor_  = 0;
  scroll_  = 0;
  loading_ = devices_.empty();
  normalize();
}

std::string DevicePickerModel::load() {
  // Go listDevicesCmd → devicesListedMsg: run the enumeration, cache the
  // result (or the error), clear the loading flag.
  loading_ = true;
  error_.clear();
  if (!load_) {
    loading_ = false;
    error_   = "device listing unavailable";
    devices_.clear();
    normalize();
    return error_;
  }
  auto result = load_();
  loading_ = false;
  if (!result.has_value()) {
    error_ = std::move(result.error());
    devices_.clear();
    normalize();
    return error_;
  }
  devices_ = std::move(*result);
  normalize();
  return "";
}

void DevicePickerModel::set_devices(std::vector<std::string> devices,
                                    std::string current_device) {
  devices_         = std::move(devices);
  current_device_  = std::move(current_device);
  loading_         = false;
  error_.clear();
  normalize();
}

void DevicePickerModel::set_visible_rows(int rows) {
  visible_rows_ = std::max(rows, 0);
  normalize();
}

void DevicePickerModel::cursor_up() {
  // Go "up"/"k": decrement, wrapping to the last entry.
  if (cursor_ > 0) {
    --cursor_;
  } else if (count() > 0) {
    cursor_ = count() - 1;
  }
  normalize();
}

void DevicePickerModel::cursor_down() {
  // Go "down"/"j": increment, wrapping to the first entry.
  if (cursor_ < count() - 1) {
    ++cursor_;
  } else if (count() > 0) {
    cursor_ = 0;
  }
  normalize();
}

void DevicePickerModel::select() {
  // Go "enter": switch to the selected device, then close (Go sets
  // visible=false before issuing switchDeviceCmd).
  if (count() > 0 && cursor_ >= 0 && cursor_ < count()) {
    if (actions_.on_switch) {
      actions_.on_switch(devices_[static_cast<std::size_t>(cursor_)]);
    }
    close();
  }
}

bool DevicePickerModel::handle_key(std::string_view key) {
  // Go handleDeviceKey: ctrl+c → quit and ctrl+x → expand are host-level
  // keys; the picker consumes only its own navigation.
  if (key == "up" || key == "k") {
    cursor_up();
    return true;
  }
  if (key == "down" || key == "j") {
    cursor_down();
    return true;
  }
  if (key == "enter") {
    select();
    return true;
  }
  if (key == "esc" || key == "d") {
    close();
    return true;
  }
  return false;
}

std::string DevicePickerModel::row_label(int i, int panel_width) const {
  // Go renderDeviceBody: label = device description (bootamp: the enumerated
  // name), with the active device marked. Go appends the active marker;
  // bootamp prefixes "▶ " (the menu highlights the cursor row itself).
  if (i < 0 || i >= count()) {
    return "";
  }
  const std::string& name = devices_[static_cast<std::size_t>(i)];
  const int          room = std::max(1, panel_width - 3);
  if (name == current_device_) {
    return "▶ " + ui::clip_text(name, room);
  }
  return "  " + ui::clip_text(name, room);
}

std::string DevicePickerModel::header_label() const {
  // Go deviceHeaderLine: plain "Audio Devices" while loading, else
  // sepHeaderN("Audio Devices", cursor+1, len).
  const int n = count();
  if (loading_ || n <= 0) {
    return "Audio Devices";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Audio Devices  %d/%d", cursor_ + 1, n);
  return buf;
}

void DevicePickerModel::normalize() {
  // Port of Go deviceMaybeAdjustScroll: clampScroll keeps the cursor in the
  // visible window; an empty list resets cursor/scroll.
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
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): the exact ftxui API surface
// (7.0.3) needs a compile pass once FTXUI lands. The model handles all keys,
// so this wrapper only needs to present the list as a scrollable Menu.

std::shared_ptr<ftxui::ComponentBase> make_device_component(DevicePickerModel& model) {
  // Entries and selection are rebuilt at render time from the live model so
  // a host-side async load (set_devices) shows immediately. The menu window
  // starts at the model scroll; scrolling is delegated to the model via the
  // key event path below (same pattern as queue.cpp/browse.cpp).
  auto entries  = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);

  auto menu = ftxui::Menu(entries.get(), selected.get(),
                          ftxui::MenuOption::Vertical());

  auto component = ftxui::CatchEvent(menu, [&model](const ftxui::Event& e) {
    // Model consumes picker keys (j/k/enter/esc/d); everything else falls
    // through to the host (help/quit/expand).
    return model.handle_key(e.character());
  });

  return ftxui::Renderer(component, [&model, menu, entries, selected] {
    // Rebuild the visible window from the model on every render.
    entries->clear();
    const int n = model.count();
    if (!model.loading() && n > 0) {
      const int width = 80;  // host supplies panel width via set_visible_rows
      for (int i = model.scroll(); i < n; ++i) {
        entries->push_back(model.row_label(i, width));
      }
      *selected = std::max(0, model.cursor() - model.scroll());
    }
    std::vector<ftxui::Element> lines = {
        ftxui::text(model.header_label()),
    };
    if (model.loading()) {
      lines.push_back(ftxui::text("  Loading devices…"));
    } else if (n == 0) {
      lines.push_back(ftxui::text("  No audio output devices found."));
    } else {
      lines.push_back(menu->Render() | ftxui::frame);
    }
    return ftxui::vbox(lines);
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
