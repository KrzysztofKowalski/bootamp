// ui/screens/too_small.hpp — terminal-too-small hint screen: model + component.
//
// Port of cliamp's layout gate (ui/model/layout.go recomputeLayout:
// `width < 40 || height < 10` ⇒ layoutTooSmall) and the message rendered in
// ui/model/view.go View(): "Terminal too small. Resize to at least 40x10
// (current: %dx%d).". The model is plain C++ (no FTXUI); the FTXUI Component
// glue is compiled only when BOOTAMP_HAS_FTXUI.
#pragma once

#include <memory>
#include <string>

#if BOOTAMP_HAS_FTXUI
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// TooSmallModel — the resize hint screen.
class TooSmallModel {
public:
  // is_too_small mirrors Go's tier switch: true when width < 40 or height < 10.
  static bool is_too_small(int width, int height) {
    return width < kMinWidth || height < kMinHeight;
  }

  // message renders the Go view text verbatim (View() fmt.Sprintf):
  // "Terminal too small. Resize to at least 40x10 (current: <w>x<h>)."
  static std::string message(int width, int height);

  inline static constexpr int kMinWidth  = 40;
  inline static constexpr int kMinHeight = 10;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see too_small.cpp).
// Renders the hint for the given terminal size centered in the frame.
std::shared_ptr<ftxui::ComponentBase> make_too_small_component(int width, int height);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
