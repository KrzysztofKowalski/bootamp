// ui/ftxui_app.hpp — FTXUI ScreenInteractive + CellGrid blit + key handling.
//
// Per the plan: FTXUI owns raw-mode, SIGWINCH/resize, keyboard, alt-screen (no
// hand-rolled termios). Each tick, blit the CellGrid to an ftxui::Canvas
// (DrawText per cell; braille-dot drivers use DrawPixel), compose with the
// status-line Element, and let FTXUI diff-redraw. Keyboard → engine atomics
// (space=pause, ←/→=seek, ↑/↓=vol, v=cycle vis, q=quit). Gated behind
// BOOTAMP_HAS_FTXUI in CMake; this header only declares the interface so the
// rest of the UI can compile without FTXUI present (tests never include it).
#pragma once

#include "ui/cell.hpp"
#include "ui/visualizer.hpp"

#include <functional>
#include <memory>
#include <string>

namespace bootamp::ui {

// FtxuiApp is the abstract FTXUI shell. The real impl (ftxui_app.cpp) is only
// compiled when FTXUI is found; tests and the non-UI build never link it. Key
// events are delivered via the KeyCallback (the app wires them to engine atomics).
class FtxuiApp {
public:
  using KeyCallback  = std::function<void(std::string_view key)>;
  // The status provider receives the current terminal width so the line can
  // keep its fixed tail (vol dB / speed / vis mode / ring) inside `cols` —
  // without it, a long ICY stream title clips the volume off the screen.
  using StatusProvider = std::function<std::string(int cols)>;

  virtual ~FtxuiApp() = default;

  // run enters the FTXUI event loop (blocks until quit). The visualizer is
  // driven by a TickLoop owned by the app; the CellGrid is blitted each tick.
  virtual void run() = 0;
  // quit requests the loop exit (callable from any key handler).
  virtual void quit() = 0;

  // screen_vis_rows: how many terminal rows the visualizer keeps under an
  // open screen (the gieres archive renders the spectrum below its list when
  // the terminal is tall enough; short terminals keep the old full-frame
  // screen swap). The screen's own chrome and a usable list window must fit
  // before the vis gets room. Shared by document(), the blit sizing and the
  // host's row-budget hook so all three agree on the split.
  static int screen_vis_rows(int term_rows);
};

// make_ftxui_app constructs the FTXUI shell. Returns nullptr if FTXUI is not
// available (the executable target is skipped in that case, so this is only
// called when BOOTAMP_HAS_FTXUI). Defined in ftxui_app.cpp.
std::unique_ptr<FtxuiApp>
make_ftxui_app(Visualizer& vis, typename FtxuiApp::KeyCallback on_key,
               typename FtxuiApp::StatusProvider status);

}  // namespace bootamp::ui