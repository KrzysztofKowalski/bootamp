// ui/screens/jump.hpp — jump-to-time prompt screen: model + FTXUI component.
//
// Port of cliamp's jump-to-time overlay (ui/model/jump.go parseJumpTarget +
// jump_test.go, keys.go openJumpMode/handleJumpKey). "ctrl+j" opens a one-line
// prompt accepting "ss", "mm:ss", or "hh:mm:ss"; enter parses the target and
// the host seeks (Go player.Seek); esc closes. A parse failure keeps the
// prompt open (bootamp clears the query for a retry; Go preserves the text —
// see the note in submit()).
//
// The model is plain C++ (no FTXUI): it owns the query buffer and visibility
// state and fires host-wired action callbacks; the host feeds printable
// characters into query() (same division as the URL prompt). The FTXUI
// Component glue is compiled only when BOOTAMP_HAS_FTXUI is defined.
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
// at namespace scope (not nested in JumpModel) so the constructor's default
// argument can value-initialize it — a nested struct is still incomplete at
// the point the default argument is parsed.
struct JumpActions {
  // enter with a parseable target — seek to `seconds` (Go handleJumpKey:
  // parseJumpTarget + player.Seek + closeJumpMode). The host validates
  // seekability/duration and surfaces seek errors (Go status.Warning).
  std::function<void(double seconds)> on_jump{};
  // esc — close without jumping; host-side cleanup such as clearing a
  // transient status message. Must not call cancel() back.
  std::function<void()> on_cancel{};
};

// JumpModel is the jump-to-time prompt (Go openJumpMode: jumping=true,
// jumpInput=""). submit() parses the query as "ss" / "mm:ss" / "hh:mm:ss"
// (Go parseJumpTarget, ui/model/jump.go): a valid target fires on_jump with
// the total seconds and closes; an invalid target stays open (bootamp clears
// the query for a retry). cancel() closes and fires on_cancel.
class JumpModel {
public:
  using Actions = JumpActions;

  JumpModel(Actions actions = Actions{});
  ~JumpModel() = default;
  JumpModel(const JumpModel&)            = delete;
  JumpModel& operator=(const JumpModel&) = delete;

  void set_actions(Actions a) { actions_ = std::move(a); }

  // --- Visibility ---------------------------------------------------------
  // open shows the prompt with a fresh query buffer (Go openJumpMode).
  void open();
  // cancel closes the prompt and fires actions_.on_cancel if wired (Go esc →
  // closeJumpMode).
  void cancel();
  bool active() const { return active_; }

  // --- Text input ---------------------------------------------------------
  // query exposes the live query buffer: the host app feeds printable
  // characters into it and the FTXUI Input binds to it display-only.
  std::string& query() { return query_; }

  // submit — enter. Parses the query (Go parseJumpTarget): valid → fire
  // actions_.on_jump(seconds) and close (Go seeks + closeJumpMode); invalid →
  // stay open and clear the query for a retry (bootamp; Go preserves the
  // input and surfaces the parse error in the status line).
  void submit();

private:
  Actions     actions_;
  std::string query_;
  bool        active_ = false;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see jump.cpp).
// Renders the jump prompt: header + Input bound to the model's live query
// buffer (display-only; the host routes keys and feeds text). Defined in
// jump.cpp.
std::shared_ptr<ftxui::ComponentBase> make_jump_component(JumpModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
