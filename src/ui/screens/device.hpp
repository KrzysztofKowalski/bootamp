// ui/screens/device.hpp — audio device picker screen: model + FTXUI component.
//
// Port of cliamp's device picker overlay (ui/model/state.go devicePickerState,
// keys.go handleDeviceKey + the "d" key at keys.go:844-849, inline_overlays.go
// renderDeviceBody/deviceHeaderLine). Pressing "d" opens the picker; enter
// switches playback to the selected device; esc/d closes; up/k down/j move.
// bootamp enumerates devices natively through the AudioSink (miniaudio) — Go
// shells out to pactl (commands.go listDevicesCmd/switchDeviceCmd).
//
// The model is plain C++ (no FTXUI): it holds visible/cursor/scroll/loading
// state, dispatches keys, and fires host-wired action callbacks. Device I/O
// is injected as a LoadFn hook (the host wires engine.list_devices() + the
// current device; tests inject fakes) — the host triggers the load lazily
// after open(), mirroring Go's listDevicesCmd. The FTXUI Component glue is
// compiled only when BOOTAMP_HAS_FTXUI is defined.
//
// Keys (device picker): j/k or up/down move (wrap), enter = switch to the
// selected device + close, esc/d = close. ctrl+c/ctrl+x stay host-level
// (quit/expand) like Go.
#pragma once

#include <expected>
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
// at namespace scope (not nested in DevicePickerModel) so the constructor's
// default argument can value-initialize it — a nested struct is still
// incomplete at the point the default argument is parsed.
struct DeviceActions {
  // enter on a device — switch playback to it (Go switchDeviceCmd). The
  // engine reopens the sink on the new device and reconfigures the
  // resampler; the host surfaces switch errors in the status line.
  std::function<void(std::string_view name)> on_switch{};
  // close hook (esc/d and after a successful enter) — host-side cleanup such
  // as clearing a transient status message. Must not call close() back.
  std::function<void()> on_close{};
};

// DevicePickerModel shows the available output devices with the current one
// marked ("▶"). All blocking enumeration is funneled through the injected
// LoadFn so tests drive it synchronously and deterministically; hosts that
// load asynchronously can push results in via set_devices() instead.
class DevicePickerModel {
public:
  using Actions = DeviceActions;

  // Injected device I/O (host wires engine.list_devices(); tests inject
  // fakes). Mirrors BrowseModel's RefreshFn convention.
  using LoadFn = std::function<std::expected<std::vector<std::string>, std::string>()>;

  DevicePickerModel(Actions actions = Actions{}, LoadFn load = {});
  ~DevicePickerModel() = default;
  DevicePickerModel(const DevicePickerModel&)            = delete;
  DevicePickerModel& operator=(const DevicePickerModel&) = delete;

  void set_actions(Actions a) { actions_ = std::move(a); }
  void set_load(LoadFn load) { load_ = std::move(load); }

  // --- Visibility ---------------------------------------------------------
  // open shows the picker with cursor/scroll reset (Go "d": visible=true,
  // cursor=0, scroll=0). When the device list was never loaded, loading_ is
  // set and the HOST triggers the load via load() (Go listDevicesCmd).
  void open();
  // close hides the picker and fires actions_.on_close if wired.
  void close() {
    visible_ = false;
    if (actions_.on_close) {
      actions_.on_close();
    }
  }
  bool visible() const { return visible_; }

  // --- Device list --------------------------------------------------------
  // load runs the injected LoadFn (Go listDevicesCmd → devicesListedMsg).
  // Returns the error string on failure ("" on success); the error is also
  // cached for rendering.
  std::string load();
  // set_devices replaces the list and the "current device" marker (hosts
  // that enumerated on their own, or after a switch).
  void set_devices(std::vector<std::string> devices, std::string current_device);
  // set_current_device re-marks the active device (host after a switch).
  void set_current_device(std::string name) { current_device_ = std::move(name); }

  const std::vector<std::string>& devices() const { return devices_; }
  const std::string&              current_device() const { return current_device_; }
  const std::string&              error() const { return error_; }
  bool                            loading() const { return loading_; }
  int                             count() const { return static_cast<int>(devices_.size()); }
  int                             cursor() const { return cursor_; }
  int                             scroll() const { return scroll_; }
  // set_visible_rows sets the row budget and re-clamps cursor/scroll.
  void set_visible_rows(int rows);

  // --- Navigation (Go handleDeviceKey) -------------------------------------
  void cursor_up();    // wraps to last (Go)
  void cursor_down();  // wraps to first (Go)
  // select — enter: on_switch(devices_[cursor]) then close (Go sets
  // visible=false before issuing switchDeviceCmd).
  void select();

  // handle_key dispatches a Bubbletea-style key name; returns true if the
  // picker consumed it (host falls through to global keys otherwise).
  bool handle_key(std::string_view key);

  // --- Rendering data (Go renderDeviceBody) --------------------------------
  // row_label renders "▶ name" for the current device, "  name" otherwise
  // (Go appends the active marker to the label; bootamp prefixes it).
  std::string row_label(int i, int panel_width) const;
  // header_label renders "Audio Devices  pos/total" while loaded
  // (Go sepHeaderN on "Audio Devices"), plain "Audio Devices" otherwise.
  std::string header_label() const;

private:
  void normalize();  // port of Go clampScroll + deviceMaybeAdjustScroll

  Actions                  actions_;
  LoadFn                   load_;
  std::vector<std::string> devices_;
  std::string              current_device_;  // "" = system default
  std::string              error_;
  bool                     loading_      = false;
  bool                     visible_      = false;
  int                      cursor_       = 0;
  int                      scroll_       = 0;
  int                      visible_rows_ = 0;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see device.cpp).
// Renders the picker as a scrollable Menu plus header; loading/empty states
// render as plain lines; keys handled by the model are wired into the
// component. Defined in device.cpp.
std::shared_ptr<ftxui::ComponentBase> make_device_component(DevicePickerModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
