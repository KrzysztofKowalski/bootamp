// ui/screens/too_small.cpp — terminal-too-small hint implementation.
//
// Port of cliamp ui/model/view.go View() lines 119-128: the message text and
// the 40x10 gate from ui/model/layout.go.
#include "ui/screens/too_small.hpp"

#include <cstdio>

#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

std::string TooSmallModel::message(int width, int height) {
  // Go: fmt.Sprintf("Terminal too small. Resize to at least 40x10 (current:
  // %dx%d).", m.width, m.height)
  char buf[128];
  std::snprintf(buf, sizeof(buf),
                "Terminal too small. Resize to at least 40x10 (current: %dx%d).",
                width, height);
  return buf;
}

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): needs a compile pass against
// ftxui 7.0.3 once installed. Renders the hint centered.
std::shared_ptr<ftxui::ComponentBase> make_too_small_component(int width, int height) {
  return ftxui::Renderer([width, height] {
    return ftxui::center(ftxui::text(TooSmallModel::message(width, height)));
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
