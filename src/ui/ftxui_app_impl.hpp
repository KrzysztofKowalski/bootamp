// ui/ftxui_app_impl.hpp — concrete FtxuiApp implementation + wiring hooks.
//
// The fixed contract header (ui/ftxui_app.hpp) declares only the abstract
// interface; make_ftxui_app() returns the concrete FtxuiAppImpl defined in
// ftxui_app.cpp (compiled only when BOOTAMP_HAS_FTXUI; the #else branch
// returns nullptr). The bootamp wiring agent (M5) may include this header to
// reach the hooks the fixed interface cannot express — the TickLoop context
// (analyze/stereo callbacks + playback flags that feed the visualizer) and
// the optional screens overlay component rendered in place of the vis frame
// while a screen (queue/browse/EQ/help) is open.
//
// Everything declared here is plain C++ (no FTXUI types in the unguarded
// surface); the FTXUI-typed hook is compiled only when BOOTAMP_HAS_FTXUI.
#pragma once

#include "ui/ftxui_app.hpp"
#include "ui/tick.hpp"  // TickLoop
#include "ui/vis_driver.hpp"  // VisTickContext

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if BOOTAMP_HAS_FTXUI
// The guarded hooks below use real FTXUI types in declarations, so the
// headers are included here (the impl is only compiled with FTXUI present).
#include <ftxui/component/app.hpp>        // ftxui::App
#include <ftxui/component/component_base.hpp>  // ftxui::ComponentBase
#include <ftxui/component/event.hpp>      // ftxui::Event
#include <ftxui/dom/canvas.hpp>           // ftxui::Canvas
#include <ftxui/dom/elements.hpp>         // ftxui::Element
#endif

namespace bootamp::ui {

// braille_bitmap decodes a braille rune (U+2800..U+28FF) into its 8 dots in
// cliamp brailleBit order: index 2*r+c (r = dot row 0..3, c = dot column 0..1,
// row 3 = bottom dots 7/8). runes outside the braille block decode to all
// false. The blit layer draws braille cells as text (DrawText with the rune —
// the terminal renders the glyph); this decode is the DrawPixel path (dot at
// pixel (col*2+c, row*4+r)) kept for parity with the task's braille spec and
// for tests.
std::array<bool, 8> braille_bitmap(char32_t rune);

// utf8_of encodes one codepoint as UTF-8 (ASCII passthrough).
std::string utf8_of(char32_t cp);

// canonical_key_name maps a raw FTXUI event input to the Bubbletea-style key
// name the bootamp screens' models and the global key table dispatch on
// ("up", "down", "shift+left", "ctrl+right", "enter", "space", ...). is_character
// distinguishes printable character events from special (escape-sequence)
// input. Returns "" for unclaimed input (function keys, mouse, unknown
// sequences, wide/combining characters). Pure: testable without FTXUI.
std::string canonical_key_name(bool is_character, std::string_view input);

// FtxuiAppImpl — the FTXUI-backed shell (see ui/ftxui_app.cpp). Only
// instantiated when BOOTAMP_HAS_FTXUI; the unguarded class declaration is
// what the stub build and the tests see.
class FtxuiAppImpl : public FtxuiApp {
public:
  FtxuiAppImpl(Visualizer& vis, FtxuiApp::KeyCallback on_key,
               FtxuiApp::StatusProvider status);
  ~FtxuiAppImpl() override;

  FtxuiAppImpl(const FtxuiAppImpl&)            = delete;
  FtxuiAppImpl& operator=(const FtxuiAppImpl&) = delete;

  // run blocks in the FTXUI event loop until quit() (or q / ctrl+c). The
  // TickLoop (owned here) drives visualizer tick→render→blit on a jthread;
  // the blit posts Event::Custom to wake the loop and force a repaint.
  void run() override;
  // quit requests the loop exit; thread-safe (idempotent). A quit() before
  // run() makes the subsequent run() return immediately.
  void quit() override;

  // --- bootamp wiring extensions (contract header has no channel for these) --
  // set_tick_context forwards to TickLoop::set_context: playback state and
  // the analyze/waveform/stereo callbacks that feed the visualizer. Without
  // the analyze callback the band modes stay silent. Thread-safe; may be
  // called at any time (also while run() is active).
  void set_tick_context(VisTickContext ctx);

#if BOOTAMP_HAS_FTXUI
  // set_overlay_component installs the screens composite (queue/help/browse/
  // eq overlays built by the wiring agent from ui/screens/*.hpp factories).
  // While a screen is visible (set_screen_visible(true)) document() swaps the
  // vis frame for the overlay's Render; while no screen is open the overlay
  // returns an empty element and the vis frame is shown. Keys are NOT
  // delivered to it (the shell translates all claimed keys to canonical names
  // and forwards them via the KeyCallback — the wiring agent dispatches them
  // to the screens' models). Must be called before run().
  void set_overlay_component(std::shared_ptr<ftxui::ComponentBase> overlay);
#endif

  // --- Screen hosting (wiring agent; plain C++ surface) --------------------
  // set_screen_visible tells document() whether a screen is open: while true,
  // the overlay's Render replaces the vis frame (the status + help lines stay
  // at the bottom of the frame). Thread-safe; the wiring agent sets it from
  // its mode switches.
  void set_screen_visible(bool visible);
  // set_resize_hook installs a callback invoked from document() on the loop
  // thread with the current terminal dims (cols, rows) on every repaint; the
  // wiring agent feeds the screens' set_visible_rows from it so their scroll
  // windows track the terminal (resizes included).
  using ResizeHook = std::function<void(int cols, int rows)>;
  void set_resize_hook(ResizeHook hook);

  // --- Playlist panel feed (wiring agent; plain C++ surface) ----------------
  // PlaylistSnapshot is one render's worth of the track list the Vis frame
  // shows in a panel under the spectrum (Go renderPlaylist: numbered rows,
  // current track highlighted, sliding window). revision mirrors
  // Playlist::revision() at snapshot time; current_index is the active
  // track's playlist row (Playlist::index()), -1 when the playlist is empty.
  struct PlaylistSnapshot {
    std::uint64_t revision = 0;  // Playlist::revision() when built
    std::vector<std::string> titles;  // display names, playlist order
    int current_index = -1;  // active track row, -1 when none
  };
  // PlaylistProvider supplies the snapshot on demand, revision-keyed: the
  // shell asks once per repaint with the revision it last rendered and the
  // provider returns nullopt when the playlist is unchanged (one atomic load
  // in main.cpp — no string copies on the per-frame path); a fresh snapshot
  // is returned only when the revision moved. Thread-safe: the shell queries
  // it from both the loop thread (document) and the tick thread (on_blit).
  using PlaylistProvider = std::function<std::optional<PlaylistSnapshot>(
      std::uint64_t seen_revision)>;
  // set_playlist_provider installs the provider (Go station list at startup).
  // Must be called before run(); the snapshot cache it feeds is guarded by
  // grid_mu_ and refreshed on both threads above.
  void set_playlist_provider(PlaylistProvider provider);

private:
#if BOOTAMP_HAS_FTXUI
  // on_event is the CatchEvent handler: key translation -> dispatch.
  bool on_event(const ftxui::Event& e);
  // handle_key_name claims one canonical key (app-owned keys first, then
  // forwards everything else via the KeyCallback).
  bool handle_key_name(const std::string& name);
  // document builds the frame Element: vis canvas + status/help lines (or
  // the too-small hint). Called on the loop thread per repaint.
  ftxui::Element document();
  // draw_grid blits the latest CellGrid into the frame's Canvas (fresh canvas
  // per frame; space cells skipped — a blank canvas is all spaces).
  void draw_grid(ftxui::Canvas& canvas);
  // on_blit runs on the tick thread: sizes the vis to the terminal (ioctl
  // winsize), stores the grid, and wakes the loop with Event::Custom.
  void on_blit(const CellGrid& grid);
  // playlist_panel_rows refreshes the revision-keyed playlist snapshot (see
  // set_playlist_provider) and returns the panel height in terminal rows:
  // header + up to kPlaylistRows tracks, 1 for an empty playlist, 0 when no
  // provider is installed. Called from on_blit (tick thread) and document
  // (loop thread) under grid_mu_.
  int playlist_panel_rows();
  // render_playlist builds the panel Element under the spectrum (Go
  // renderPlaylist): header + a TrackWindow of numbered rows, the current
  // track highlighted (▶ marker + bold green); empty playlist → a hint.
  // Loop thread, under grid_mu_.
  ftxui::Element render_playlist(int cols);

  // screen_ is set by run() on the loop thread and read by the tick thread
  // (on_blit); valid only while run() is inside App::Loop.
  std::atomic<ftxui::App*>               screen_{nullptr};
  std::shared_ptr<ftxui::ComponentBase> overlay_;
#endif

  Visualizer&              vis_;
  FtxuiApp::KeyCallback    on_key_;
  FtxuiApp::StatusProvider status_;
  std::unique_ptr<TickLoop> ticks_;

  std::mutex        grid_mu_;
  CellGrid          latest_;
  bool              has_grid_ = false;
  std::atomic<bool> fullscreen_{false};
  std::atomic<bool> quit_{false};
  std::atomic<bool> screen_visible_{false};
  ResizeHook        resize_hook_;

  // Playlist panel feed + revision-keyed snapshot cache (both guarded by
  // grid_mu_: playlist_panel_rows/refresh runs on the tick thread in on_blit
  // and on the loop thread in document(), render_playlist on the loop thread).
  PlaylistProvider playlist_provider_;
  std::optional<PlaylistSnapshot> playlist_snap_;
};

}  // namespace bootamp::ui
