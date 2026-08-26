// ui/ftxui_app.cpp — the FTXUI shell (contract: ui/ftxui_app.hpp).
//
// Compiled in every build; the guarded section is the real FTXUI frontend and
// the #else branch is the nullptr stub (tests and non-FTXUI builds).
//
// Architecture: one foreground process. FTXUI owns raw mode, SIGWINCH, the
// keyboard and the alternate screen. A TickLoop jthread drives
// visualizer.tick()→render()→CellGrid at the driver's cadence; the blit
// callback (on_blit) runs on that thread and copies the grid + posts
// ftxui::Event::Custom to wake the event loop and force a repaint (FTXUI's
// loop redraws whenever any event is handled). Each repaint re-composes the
// frame Element: a fresh ftxui::Canvas blitted from the latest grid, then the
// status line (StatusProvider, one plain-text line, no ANSI — text() renders
// escapes literally) and a dim help hint, or a centered too-small hint. While
// a screen (queue/browse/EQ/help) is open — set_screen_visible(true) — the
// overlay component's Render replaces the canvas (status + help stay).
//
// Canvas model (FTXUI 7.x): a terminal cell is 2x4 canvas pixels;
// DrawText(x, y, glyph) maps glyph -> cell (x/2, y/4) and advances x by 2 per
// glyph, so the CellGrid blit is DrawText(col*2, row*4, utf8(rune), color).
// Braille cells are drawn as text too (the rune renders via the terminal
// font — byte-identical output to the DrawPoint accumulation path; cliamp
// likewise hands braille runes to its canvas). braille_bitmap() in
// ftxui_app_impl.hpp is the DrawPixel decode kept for parity + tests.
//
// Keys: CatchEvent wraps the renderer. The shell claims the full key set,
// translates to canonical names (see canonical_key_name), and forwards via
// the KeyCallback. v (cycle vis), V (fullscreen vis) and q/ctrl+c (quit) are
// app-owned: the fixed interface gives the caller no channel to cycle the
// visualizer, toggle fullscreen or wake the tick loop, so the shell handles
// them in-process (v/V are not forwarded — the status provider sees the new
// mode via vis.mode_name(); q/ctrl+c are forwarded first, then quit()).
//
// Layout: vis rows = terminal rows - 2 (status + help) or - 0 in fullscreen
// mode; canvas pixels = cols*2 x rows*4; too-small (<40x10, Go layout gate)
// shows the resize hint instead. Terminal size is read with ioctl
// TIOCGWINSZ on both threads (FTXUI re-reads it too; transiently different
// reads resolve within one frame).
//
// Threading: the TickLoop runs visualizer.tick()/render() on its jthread and
// set_size() is called from on_blit on the same thread. Mode changes
// (cycle_mode/set_mode) come from the key handler on the loop thread — a
// benign race on the mode enum/ints (no lock in the contract header; see the
// task report). The grid passes between threads under grid_mu_; the Canvas is
// never shared (fresh per frame).
#include "ui/ftxui_app_impl.hpp"

#include "ui/fit.hpp"
#include "ui/screens/too_small.hpp"
#include "ui/styles.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if BOOTAMP_HAS_FTXUI
#include <sys/ioctl.h>  // TIOCGWINSZ / struct winsize
#include <unistd.h>     // STDOUT_FILENO

#include <ftxui/component/app.hpp>           // ftxui::App (ex ScreenInteractive)
#include <ftxui/component/component.hpp>     // CatchEvent, Renderer
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#endif

namespace bootamp::ui {

// ---------------------------------------------------------------------------
// Plain helpers (compiled in every build; unit-tested without FTXUI).
// ---------------------------------------------------------------------------

std::string utf8_of(char32_t cp) {
  if (cp <= 0x7F) {
    return std::string(1, static_cast<char>(cp));
  }
  if (cp <= 0x7FF) {
    return {static_cast<char>(0xC0 | (cp >> 6)),
            static_cast<char>(0x80 | (cp & 0x3F))};
  }
  if (cp <= 0xFFFF) {
    return {static_cast<char>(0xE0 | (cp >> 12)),
            static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
            static_cast<char>(0x80 | (cp & 0x3F))};
  }
  return {static_cast<char>(0xF0 | (cp >> 18)),
          static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
          static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
          static_cast<char>(0x80 | (cp & 0x3F))};
}

std::array<bool, 8> braille_bitmap(char32_t rune) {
  std::array<bool, 8> out{};
  if (rune < 0x2800 || rune > 0x28FF) {
    return out;
  }
  // cliamp brailleBit (visualizer.go): bit(r, c) = 1<<(3*c+r) for r < 3,
  // 1<<(6+c) for r == 3 — the standard Unicode 8-dot layout, identical to
  // FTXUI's g_map_braille. out[2*r+c]: dot row r (0..3), dot column c (0..1).
  const unsigned bits = static_cast<unsigned>(rune - 0x2800);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 2; ++c) {
      const unsigned bit = (r < 3) ? (1u << (3 * c + r)) : (1u << (6 + c));
      out[static_cast<std::size_t>(2 * r + c)] = (bits & bit) != 0;
    }
  }
  return out;
}

std::string canonical_key_name(bool is_character, std::string_view input) {
  if (is_character) {
    // Printable input: single ASCII chars pass through; space is the play/
    // pause key name; wide/combining/control input is unclaimed.
    if (input == " ") {
      return "space";
    }
    if (input.size() == 1) {
      const unsigned char c = static_cast<unsigned char>(input[0]);
      if (c >= 0x21 && c <= 0x7E) {
        return std::string(1, static_cast<char>(c));
      }
    }
    return {};
  }
  // Special (escape-sequence) input — inputs verified against FTXUI 7.0.3
  // event.cpp statics. Shift+arrows have no statics; xterm sends CSI 1;2<A-D>.
  if (input == "\x1B[A")   return "up";
  if (input == "\x1B[B")   return "down";
  if (input == "\x1B[C")   return "right";
  if (input == "\x1B[D")   return "left";
  if (input == "\x1B[1;5A") return "ctrl+up";
  if (input == "\x1B[1;5B") return "ctrl+down";
  if (input == "\x1B[1;5C") return "ctrl+right";
  if (input == "\x1B[1;5D") return "ctrl+left";
  if (input == "\x1B[1;2A") return "shift+up";
  if (input == "\x1B[1;2B") return "shift+down";
  if (input == "\x1B[1;2C") return "shift+right";
  if (input == "\x1B[1;2D") return "shift+left";
  if (input == "\x1B[Z")    return "shift+tab";
  if (input == "\x1B[H")    return "home";
  if (input == "\x1B[F")    return "end";
  if (input == "\x1B[5~")   return "pageup";
  if (input == "\x1B[6~")   return "pagedown";
  if (input == "\x1B[3~")   return "delete";
  if (input == "\x1B[2~")   return "insert";
  if (input == "\x1B")      return "esc";
  if (input == "\n" || input == "\r") return "enter";  // FTXUI normalizes \r
  if (input == "\t")        return "tab";
  if (input == "\x7F")      return "backspace";
  // Ctrl+letter arrives as a single control byte 0x01..0x1A.
  if (input.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(input[0]);
    if (c >= 1 && c <= 26) {
      return "ctrl+" + std::string(1, static_cast<char>('a' + c - 1));
    }
  }
  return {};
}

// Static help hint rendered dim under the status line (clipped to width).
inline constexpr std::string_view kHelpLine =
    "space play/pause  ←/→ seek  shift+←/→ ±30s  "
    "↑/↓ volume  n/p next/prev  v vis  V fullscreen vis  "
    "l queue  R/b browse  e EQ  s shuffle  r repeat  f favorite  ? help  q quit";

#if BOOTAMP_HAS_FTXUI

namespace {

// Terminal size via ioctl (same source FTXUI uses; fallback 80x24). Reading
// it on both threads is fine: FTXUI re-reads on every draw and SIGWINCH.
struct TermSize {
  int cols = 80;
  int rows = 24;
};

TermSize term_size() {
  TermSize out;
  winsize ws{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_col > 0) {
      out.cols = static_cast<int>(ws.ws_col);
    }
    if (ws.ws_row > 0) {
      out.rows = static_cast<int>(ws.ws_row);
    }
  }
  return out;
}

// Resolve a CellGrid palette slot to an ftxui::Color. Slot 0 (default) is the
// transparent Color::Default — NOT Palette16::Black (ANSI 0 means the
// terminal default foreground; mapping it to black would tint everything).
ftxui::Color color_of(bootamp::ui::Color slot) {
  if (slot == kColorDefault) {
    return ftxui::Color::Default;
  }
  const std::uint8_t ansi = ui::default_ansi(slot);
  if (ansi > 15) {
    return ftxui::Color::Default;
  }
  return ftxui::Color(static_cast<ftxui::Color::Palette16>(ansi));
}

}  // namespace

FtxuiAppImpl::FtxuiAppImpl(Visualizer& vis, FtxuiApp::KeyCallback on_key,
                           FtxuiApp::StatusProvider status)
    : vis_(vis),
      on_key_(std::move(on_key)),
      status_(std::move(status)),
      ticks_(std::make_unique<TickLoop>(
          vis, [this](const CellGrid& grid) { on_blit(grid); })) {}

FtxuiAppImpl::~FtxuiAppImpl() = default;

void FtxuiAppImpl::set_tick_context(VisTickContext ctx) {
  ticks_->set_context(std::move(ctx));
}

void FtxuiAppImpl::set_overlay_component(
    std::shared_ptr<ftxui::ComponentBase> overlay) {
  overlay_ = std::move(overlay);
}

void FtxuiAppImpl::set_screen_visible(bool visible) {
  screen_visible_.store(visible);
}

void FtxuiAppImpl::set_resize_hook(ResizeHook hook) {
  resize_hook_ = std::move(hook);
}

void FtxuiAppImpl::quit() {
  quit_.store(true);
  if (ftxui::App* s = screen_.load()) {
    s->Exit();  // thread-safe: posted as a loop task
  }
}

void FtxuiAppImpl::run() {
  if (quit_.exchange(false)) {
    return;  // quit() before run(): nothing to do
  }
  ftxui::App app = ftxui::App::Fullscreen();
  app.TrackMouse(false);  // mouse not used; keep the terminal clean
  screen_.store(&app);

  // The overlay composite is not an event child: the shell claims every key
  // and forwards canonical names via the KeyCallback, and document() composes
  // the overlay's Render into the frame while a screen is visible.
  ftxui::Component base = ftxui::Renderer([this] { return document(); });
  ftxui::Component component = ftxui::CatchEvent(
      base, [this](const ftxui::Event& e) { return on_event(e); });

  ticks_->start();
  app.Loop(component);  // blocks until Exit()
  ticks_->stop();
  screen_.store(nullptr);
}

bool FtxuiAppImpl::on_event(const ftxui::Event& e) {
  if (e.is_mouse()) {
    return false;
  }
  if (e == ftxui::Event::Custom) {
    return false;  // tick-loop redraw request, not a key
  }
  const std::string name =
      canonical_key_name(e.is_character(), e.character());
  if (name.empty()) {
    return false;  // unclaimed: function keys, unknown sequences, mouse
  }
  return handle_key_name(name);
}

bool FtxuiAppImpl::handle_key_name(const std::string& name) {
  // App-owned keys (the fixed interface gives the caller no channel to these
  // and the tick wake must happen in-process):
  if (name == "v") {
    vis_.cycle_mode();
    vis_.request_refresh();
    ticks_->wake();
    return true;
  }
  if (name == "V") {
    fullscreen_.store(!fullscreen_.load());
    ticks_->wake();
    return true;
  }
  // q / ctrl+c: forwarded first (pre-exit cleanup), then quit. FTXUI is told
  // the event is handled so it does not raise SIGINT on ctrl+c.
  if (name == "q" || name == "ctrl+c") {
    if (on_key_) {
      on_key_(name);
    }
    quit();
    return true;
  }
  // Everything else is claimed and forwarded; the host wires the screens'
  // models and the engine atomics from the canonical name.
  if (on_key_) {
    on_key_(name);
  }
  return true;
}

void FtxuiAppImpl::on_blit(const CellGrid& grid) {
  // Size the vis to the terminal on the tick thread (set_size must not race
  // tick()/render()); the loop picks up the new size next iteration.
  const TermSize ts = term_size();
  const int vis_rows =
      fullscreen_.load() ? ts.rows : std::max(1, ts.rows - 2);
  vis_.set_size(ts.cols, vis_rows);
  {
    std::lock_guard<std::mutex> lk(grid_mu_);
    latest_ = grid;
    has_grid_ = true;
  }
  if (ftxui::App* s = screen_.load()) {
    s->PostEvent(ftxui::Event::Custom);  // wake the loop + force a repaint
  }
}

ftxui::Element FtxuiAppImpl::document() {
  const TermSize ts = term_size();
  const int dimx = ts.cols;
  const int dimy = ts.rows;
  if (resize_hook_) {
    resize_hook_(dimx, dimy);
  }
  if (screens::TooSmallModel::is_too_small(dimx, dimy)) {
    // Go layout gate (<40x10): resize hint centered instead of the frame.
    return ftxui::center(
        ftxui::text(screens::TooSmallModel::message(dimx, dimy)));
  }
  if (overlay_ && screen_visible_.load()) {
    // A screen (queue/browse/EQ/help) is open: full-frame swap — the screens
    // composite renders the active screen above the status + help lines. The
    // vis canvas is not drawn (it keeps ticking in the background).
    const std::string status = status_ ? status_() : std::string();
    return ftxui::vbox({
        overlay_->Render(),
        ftxui::text(ui::clip_text(status, std::max(dimx, 0))),
        ftxui::dim(ftxui::text(ui::clip_text(kHelpLine, std::max(dimx, 0)))),
    });
  }
  const int vis_cols = dimx;
  const int vis_rows = fullscreen_.load() ? dimy : std::max(1, dimy - 2);
  ftxui::Element vis = ftxui::canvas(
      vis_cols * 2, vis_rows * 4, [this](ftxui::Canvas& c) { draw_grid(c); });
  if (fullscreen_.load()) {
    return vis;  // V: visualizer alone
  }
  const std::string status = status_ ? status_() : std::string();
  return ftxui::vbox({
      vis,
      ftxui::text(ui::clip_text(status, std::max(dimx, 0))),
      ftxui::dim(ftxui::text(ui::clip_text(kHelpLine, std::max(dimx, 0)))),
  });
}

std::unique_ptr<FtxuiApp> make_ftxui_app(Visualizer& vis,
                                         FtxuiApp::KeyCallback on_key,
                                         FtxuiApp::StatusProvider status) {
  return std::make_unique<FtxuiAppImpl>(vis, std::move(on_key),
                                        std::move(status));
}

void FtxuiAppImpl::draw_grid(ftxui::Canvas& canvas) {
  CellGrid grid;
  bool valid = false;
  {
    std::lock_guard<std::mutex> lk(grid_mu_);
    grid = latest_;
    valid = has_grid_;
  }
  if (!valid) {
    return;  // blank canvas: mode None or never sized
  }
  const int rows = grid.rows();
  const int cols = grid.cols();
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const Cell cell = grid.at(r, c);
      if (cell.rune == U' ' || cell.rune == U'\0') {
        continue;  // fresh canvas is all spaces
      }
      // Terminal cell (c, r) = pixels (c*2 .. c*2+1, r*4 .. r*4+3).
      const int px = c * 2;
      const int py = r * 4;
      if (px >= canvas.width() || py + 4 > canvas.height()) {
        continue;  // stale grid larger than the vis area: clip
      }
      canvas.DrawText(px, py, utf8_of(cell.rune), color_of(cell.color));
    }
  }
}

#else  // !BOOTAMP_HAS_FTXUI

// Stub: no FTXUI installed at build time. The executable target is skipped in
// that case (see CMakeLists.txt), so a caller should never see a nullptr app.
std::unique_ptr<FtxuiApp> make_ftxui_app(Visualizer& /*vis*/,
                                         FtxuiApp::KeyCallback /*on_key*/,
                                         FtxuiApp::StatusProvider /*status*/) {
  return nullptr;
}

#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui
