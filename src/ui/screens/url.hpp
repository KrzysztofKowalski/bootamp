// ui/screens/url.hpp — URL input prompt screen: model + FTXUI component.
//
// Port of cliamp's URL input overlay (ui/model/keys.go handleURLInputKey;
// "u" opens it at keys.go:776-779; inline_overlays.go renderURLBody). "u"
// shows a one-line prompt; enter submits the (trimmed) query to the host (Go
// resolveRemoteCmd); esc closes. An empty submit stays open (Go re-arms with
// "Enter a stream, track, or playlist URL.").
//
// The model is plain C++ (no FTXUI): it owns the query buffer and visibility
// state and fires host-wired action callbacks. The FTXUI Input below is
// display-only — the shell claims every key and the host feeds printable
// characters into query() (same division as the browse search prompt). The
// FTXUI Component glue is compiled only when BOOTAMP_HAS_FTXUI is defined.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#if BOOTAMP_HAS_FTXUI
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// Actions the host app must wire (Go: the update-loop side effects). Defined
// at namespace scope (not nested in UrlModel) so the constructor's default
// argument can value-initialize it — a nested struct is still incomplete at
// the point the default argument is parsed.
struct UrlActions {
  // enter with a non-empty query — resolve and play the URL (Go
  // resolveRemoteCmd). The query is whitespace-trimmed before the callback
  // fires (Go strings.TrimSpace).
  std::function<void(std::string_view query)> on_submit{};
  // esc — close without submitting; host-side cleanup such as clearing a
  // transient status message. Must not call cancel() back.
  std::function<void()> on_cancel{};
};

// UrlModel is the URL input prompt (Go "u": urlInputting=true, urlInput="",
// urlErr=""). open() shows the prompt with a fresh buffer; submit() with a
// non-empty query fires on_submit and closes, an empty submit stays open
// (Go re-arms with the URL error); cancel() closes and fires on_cancel.
class UrlModel {
public:
  using Actions = UrlActions;

  UrlModel(Actions actions = Actions{});
  ~UrlModel() = default;
  UrlModel(const UrlModel&)            = delete;
  UrlModel& operator=(const UrlModel&) = delete;

  void set_actions(Actions a) { actions_ = std::move(a); }

  // --- Visibility ---------------------------------------------------------
  // open shows the prompt with a fresh query buffer (Go "u").
  void open();
  // cancel closes the prompt and fires actions_.on_cancel if wired (Go esc).
  void cancel();
  bool active() const { return active_; }

  // --- Text input ---------------------------------------------------------
  // query exposes the live query buffer: the host app feeds printable
  // characters into it and the FTXUI Input binds to it display-only.
  std::string& query() { return query_; }

  // submit — enter. A non-empty (trimmed) query fires actions_.on_submit and
  // closes the prompt (Go: urlInputting=false + resolveRemoteCmd); an empty
  // query stays open (Go re-arms with "Enter a stream, track, or playlist
  // URL.").
  void submit();

private:
  Actions     actions_;
  std::string query_;
  bool        active_ = false;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see url.cpp).
// Renders the URL prompt: header + Input bound to the model's live query
// buffer (display-only; the host routes keys and feeds text). Defined in
// url.cpp.
std::shared_ptr<ftxui::ComponentBase> make_url_component(UrlModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
