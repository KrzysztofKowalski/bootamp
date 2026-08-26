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
  using StatusProvider = std::function<std::string()>;

  virtual ~FtxuiApp() = default;

  // run enters the FTXUI event loop (blocks until quit). The visualizer is
  // driven by a TickLoop owned by the app; the CellGrid is blitted each tick.
  virtual void run() = 0;
  // quit requests the loop exit (callable from any key handler).
  virtual void quit() = 0;
};

// make_ftxui_app constructs the FTXUI shell. Returns nullptr if FTXUI is not
// available (the executable target is skipped in that case, so this is only
// called when BOOTAMP_HAS_FTXUI). Defined in ftxui_app.cpp.
std::unique_ptr<FtxuiApp>
make_ftxui_app(Visualizer& vis, typename FtxuiApp::KeyCallback on_key,
               typename FtxuiApp::StatusProvider status);

}  // namespace bootamp::ui