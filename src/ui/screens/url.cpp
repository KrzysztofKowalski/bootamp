// ui/screens/url.cpp — URL input prompt screen implementation.
//
// Model is a port of cliamp ui/model keys.go handleURLInputKey (the "u" key
// opens at keys.go:776-779; esc closes; enter submits the trimmed query or
// re-arms the prompt when empty) plus inline_overlays.go renderURLBody.
#include "ui/screens/url.hpp"

#include <string>
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

UrlModel::UrlModel(Actions actions) : actions_(std::move(actions)) {}

void UrlModel::open() {
  // Go "u" (keys.go:776-779): urlInputting=true, urlInput="", urlErr="".
  active_ = true;
  query_.clear();
}

void UrlModel::cancel() {
  // Go esc (handleURLInputKey): urlInputting=false, no other side effect.
  active_ = false;
  if (actions_.on_cancel) {
    actions_.on_cancel();
  }
}

void UrlModel::submit() {
  // Go enter (handleURLInputKey): urlInputting=false, then trim the input —
  // empty re-arms the prompt with "Enter a stream, track, or playlist URL.",
  // non-empty resolves the URL (Go resolveRemoteCmd).
  std::string input = query_;
  const std::string::size_type first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return;  // empty → stay open (Go re-arms with the URL error)
  }
  input.erase(0, first);
  input.erase(input.find_last_not_of(" \t\r\n") + 1);
  active_ = false;
  if (actions_.on_submit) {
    actions_.on_submit(input);
  }
}

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): the exact ftxui API surface
// (7.0.3) needs a compile pass once FTXUI lands. The shell claims every key,
// so the Input is display-only (the host feeds characters into model.query()
// and routes enter/esc via the model); the CatchEvent below is defensive,
// matching the other screens' glue.
std::shared_ptr<ftxui::ComponentBase> make_url_component(UrlModel& model) {
  auto input = ftxui::Input(&model.query(), "https://…");
  input = ftxui::CatchEvent(input, [&model](const ftxui::Event& e) {
    const std::string& c = e.character();
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

  return ftxui::Renderer(input, [input, &model] {
    std::vector<ftxui::Element> lines = {
        ftxui::text("Open URL"),
        input->Render(),
        // Go renderURLBody hint line ("Paste a stream, track, or playlist
        // URL above.").
        ftxui::dim(ftxui::text(
            "  Paste a stream, track, or playlist URL. Enter submits, esc "
            "cancels.")),
    };
    return ftxui::vbox(lines);
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
