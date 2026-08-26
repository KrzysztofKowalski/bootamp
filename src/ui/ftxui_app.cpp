// ui/ftxui_app.cpp — the FTXUI shell (contract: ui/ftxui_app.hpp).
//
// Compiled in every build; the guarded section is the real FTXUI frontend and
// the #else branch is the nullptr stub (tests and non-FTXUI builds).
//
// Architecture: one foreground process. FTXUI owns raw mode, SIGWINCH, the
// keyboard and the alternate screen. A TickLoop jthread drives
// visualizer.tick()→render()→CellGrid at the driver's cadence; the blit
// callback (on_blit) runs on that thread and posts ftxui::Event::Custom to
// wake the event loop and force a repaint (FTXUI's loop redraws whenever any
// event is handled). Each repaint re-composes the frame Element: the vis
// canvas (a persistent ftxui::Canvas member — draw_grid updates only the
// cells that changed since the last draw), a playlist panel under the
// spectrum (Go renderPlaylist — the station list, fed revision-keyed by the
// host, see set_playlist_provider), the status line (StatusProvider, one
// plain-text line, no ANSI — text() renders escapes literally) and a dim
// help hint, or a centered too-small hint.
// While a screen (queue/browse/EQ/help) is open — set_screen_visible(true) —
// the overlay component's Render replaces the canvas (status + help stay).
//
// Perf: FTXUI 7.x re-emits the WHOLE screen every frame (no diffing), so a
// repaint costs a full re-serialization + terminal write. Repaints are
// therefore gated, not just throttled: on_blit fingerprints the grid (~5µs
// XOR-64 over rune+color bytes) and skips the store + post when the content
// is unchanged; posts are floored to the driver's own cadence
// (vis_.tick_interval on the same ctx the TickLoop sleeps on); and while the
// window is unfocused (DECSET 1004 focus events) no posts are made at all —
// an unfocused bootamp idles at ~0% CPU. The `o` key switches the vis off
// (VisMode::None: render() returns false, so no blits happen at all).
//
// Canvas model (FTXUI 7.x): a terminal cell is 2x4 canvas pixels;
// DrawText(x, y, glyph) maps glyph -> cell (x/2, y/4) and advances x by 2 per
// glyph, so the CellGrid blit is DrawText(col*2, row*4, utf8(rune), color).
// Braille cells are drawn as text too (the rune renders via the terminal
// font — byte-identical output to the DrawPoint accumulation path; cliamp
// likewise hands braille runes to its canvas). braille_bitmap() in
// ftxui_app_impl.hpp is the DrawPixel decode kept for parity + tests.
//
// The canvas is a member (canvas_), not rebuilt per frame: draw_grid
// compares the latest grid against prev_ (the last drawn grid) and DrawText's
// only the dirty cells — unchanged cells are already in the canvas, and the
// Element is re-created per repaint as a referencing ConstRef
// (canvas(&canvas_)) so no canvas copy happens either. A cell cleared to a
// space is overwritten with a space glyph; a size change recreates the canvas
// and forces a full redraw.
//
// Keys: CatchEvent wraps the renderer. The shell claims the full key set,
// translates to canonical names (see canonical_key_name), and forwards via
// the KeyCallback. v (cycle vis), V (fullscreen vis), o (vis off — saves and
// restores the current mode around VisMode::None) and q/ctrl+c (quit) are
// app-owned: the fixed interface gives the caller no channel to cycle the
// visualizer, toggle fullscreen, switch it off or wake the tick loop, so the
// shell handles them in-process (v/V/o are not forwarded — the status
// provider sees the new mode via vis.mode_name(); q/ctrl+c are forwarded
// first, then quit()).
//
// Layout: vis rows = terminal rows - 2 (status + help) - playlist panel
// (header + up to 8 track rows + album-header rows when set_album_headers
// enables them, or 1 hint row when the playlist is empty) or - 0 in
// fullscreen mode (V hides the panel too); canvas pixels =
// cols*2 x rows*4; too-small (<40x10, Go layout gate) shows the resize hint
// instead. Terminal size is read with ioctl TIOCGWINSZ on both threads
// (FTXUI re-reads it too; transiently different reads resolve within one
// frame).
//
// Threading: the TickLoop runs visualizer.tick()/render() on its jthread and
// set_size() is called from on_blit on the same thread — and once from run()
// before the loop starts, so render()/tick() do not deadlock on the first
// frames (a bootstrap sizing that matches on_blit's math). Mode changes
// (cycle_mode/set_mode) come from the key handler on the loop thread — a
// benign race on the mode enum/ints (no lock in the contract header; see the
// task report). The grid passes between threads under grid_mu_; the Canvas is
// never shared (fresh per frame).
#include "ui/ftxui_app_impl.hpp"

#include "ui/fit.hpp"
#include "ui/screens/too_small.hpp"
#include "ui/styles.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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
  // Per-rune memo cache: the blit encodes every drawn cell and the active
  // rune set is small (braille block + a few block glyphs), so caching avoids
  // the encoding work per cell. Callers are draw_grid (loop thread) and the
  // unit tests — never concurrent.
  static std::unordered_map<char32_t, std::string> cache;
  if (const auto it = cache.find(cp); it != cache.end()) {
    return it->second;
  }
  std::string out;
  if (cp <= 0x7F) {
    out.assign(1, static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.assign({static_cast<char>(0xC0 | (cp >> 6)),
                static_cast<char>(0x80 | (cp & 0x3F))});
  } else if (cp <= 0xFFFF) {
    out.assign({static_cast<char>(0xE0 | (cp >> 12)),
                static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                static_cast<char>(0x80 | (cp & 0x3F))});
  } else {
    out.assign({static_cast<char>(0xF0 | (cp >> 18)),
                static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
                static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                static_cast<char>(0x80 | (cp & 0x3F))});
  }
  cache.emplace(cp, out);
  return out;
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
  // FTXUI 7.0.3 normalizes CR→LF at the input parser (terminal_input_parser.cpp
  // g_uniformize {"\r","\n"}), and Event::Return == Event::CtrlJ — both are
  // byte 0x0A ("\n") — so Enter and Ctrl+J arrive byte-identically and FTXUI
  // cannot distinguish them. bootamp treats both as Enter. The "\r" branch
  // stays for robustness/documentation (raw-mode Enter without normalization).
  if (input == "\r") return "enter";
  if (input == "\n") return "enter";
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
    "+/- volume  ↑/↓ cursor  s stop  z shuffle  "
    "n next  < prev  v vis  V fullscreen  e EQ  l queue  R/b browse  ? help  q quit";

#if BOOTAMP_HAS_FTXUI

namespace {

// Terminal size via ioctl (same source FTXUI uses; fallback 80x24). Reading
// it on both threads is fine: FTXUI re-reads on every draw and SIGWINCH.
// Cached with a ~100ms TTL: the ioctl ran on every blit (tick thread) and
// every repaint (loop thread) — ~20+ syscalls/sec. Both threads share the
// cache, so their views of the size stay mutually consistent; a stale read
// only transiently mismatches FTXUI's own fresh read within one frame.
struct TermSize {
  int cols = 80;
  int rows = 24;
};

TermSize term_size() {
  static std::mutex mu;
  static TermSize cached;
  static std::chrono::steady_clock::time_point cached_at;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(mu);
  if (now - cached_at < std::chrono::milliseconds(100)) {
    return cached;
  }
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
  cached = out;
  cached_at = now;
  return out;
}

// Playlist panel window (Go maxVisibleTracks): the number of track rows shown
// under the spectrum; the window slides to keep the current track centered.
inline constexpr int kPlaylistRows = 8;

// PlaylistView — the visible track window for one render plus the album-header
// row budget. playlist_panel_rows() (vis sizing, tick + loop threads) and
// render_playlist() (loop thread) both derive their row counts from
// playlist_view() so the panel height the vis is sized to always equals the
// number of rows the panel actually renders — the frame then sums exactly to
// the terminal height (no overflow, no leftover gap).
struct PlaylistView {
  int first = 0;        // first visible track row (list index)
  int last  = 0;        // one past the last visible track row
  int header_rows = 0;  // album "── {album} ──" rows emitted in [first, last)
};

// album_run_start — true when track i (inside the rendered window [first,
// last)) opens an album run and gets a dim "── {album} ──" header row: the
// track's album is non-empty and differs from the previous rendered track's.
// The window's first row opens a run whenever its own album is non-empty —
// the track above the window is not rendered, so no comparison is possible,
// and re-opening the run keeps the visible text coherent. Tracks without
// album data (empty string, or an albums vector shorter than titles — hosts
// that do not feed album metadata) never start a run, so such playlists
// render exactly as they would without headers.
bool album_run_start(const FtxuiAppImpl::PlaylistSnapshot& snap, int i,
                     int first) {
  const std::size_t u = static_cast<std::size_t>(i);
  if (i >= static_cast<int>(snap.albums.size()) || snap.albums[u].empty()) {
    return false;
  }
  return i == first || snap.albums[u] != snap.albums[u - 1];
}

// playlist_view — window + header budget for one render (see PlaylistView).
// Window math is the Go TrackWindow, unchanged from before: up to
// kPlaylistRows track rows centered on the current track, clamped to the list
// (start < 0 → 0; an overrunning end pulls the window back so it never
// exceeds the list). The current track is therefore always a rendered track
// row inside [first, last); album header rows are inserted above track rows
// and never displace them, so the highlight is never pushed off-screen. The
// headers only extend the panel height, which both sizing threads reserve via
// playlist_panel_rows — the vis shrinks accordingly. Degenerate terminals
// shorter than the whole frame rely on FTXUI's bottom clipping, exactly like
// any oversized vbox (the <40x10 gate already covers the extreme).
PlaylistView playlist_view(const FtxuiAppImpl::PlaylistSnapshot& snap,
                           bool album_headers) {
  PlaylistView v;
  const int n = static_cast<int>(snap.titles.size());
  if (n == 0) {
    return v;  // empty list: the caller renders the hint line instead
  }
  const int current = std::clamp(snap.current_index, 0, n - 1);
  int first = 0;
  if (n > kPlaylistRows) {
    first = std::clamp(current - (kPlaylistRows - 1) / 2, 0, n - kPlaylistRows);
  }
  v.first = first;
  v.last  = std::min(n, first + kPlaylistRows);
  if (album_headers) {
    for (int i = v.first; i < v.last; ++i) {
      if (album_run_start(snap, i, first)) {
        ++v.header_rows;
      }
    }
  }
  return v;
}

// Resolve a CellGrid palette slot to an ftxui::Color. Slot 0 (default) is the
// transparent Color::Default — NOT Palette16::Black (ANSI 0 means the
// terminal default foreground; mapping it to black would tint everything).
// The 16 palette slots resolve through the same default_ansi switch every
// frame, so they're memoized in a fixed cache.
ftxui::Color color_of(bootamp::ui::Color slot) {
  if (slot == kColorDefault) {
    return ftxui::Color::Default;
  }
  static const std::array<ftxui::Color, 16> kCache = [] {
    std::array<ftxui::Color, 16> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i] = ftxui::Color(static_cast<ftxui::Color::Palette16>(
          ui::default_ansi(static_cast<bootamp::ui::Color>(i))));
    }
    return out;
  }();
  if (slot > 15) {
    // Out-of-range slot: keep the uncached path for exact behavior parity
    // (default_ansi never exceeds 15, so this returns Palette16(0) = black,
    // exactly as before the cache).
    const std::uint8_t ansi = ui::default_ansi(slot);
    if (ansi > 15) {
      return ftxui::Color::Default;
    }
    return ftxui::Color(static_cast<ftxui::Color::Palette16>(ansi));
  }
  return kCache[slot];
}

// grid_fingerprint — cheap content hash of a CellGrid: an XOR-64 mix over
// every cell's rune+color bytes (~5µs for a 1920-cell grid). Identical grids
// hash identically; a collision would only skip one repaint of a visually
// identical frame (the next differing frame reposts). Tick thread (on_blit).
std::uint64_t grid_fingerprint(const CellGrid& grid) {
  constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ull;  // golden ratio
  std::uint64_t h = kMix;
  const int rows = grid.rows();
  const int cols = grid.cols();
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const Cell cell = grid.at(r, c);
      const std::uint64_t v =
          (static_cast<std::uint64_t>(cell.rune) << 8) ^ cell.color;
      h ^= v + kMix + (h << 6) + (h >> 2);
    }
  }
  return h;
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
  {
    // Cache a copy for on_blit's post floor: the driver cadence
    // (vis_.tick_interval) derives from playing/paused/overlay_active, which
    // only change through this call — the same call that feeds the TickLoop,
    // so the copy stays exactly as fresh as the loop's own ctx.
    std::lock_guard<std::mutex> lk(ctx_mu_);
    tick_ctx_ = ctx;
  }
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

void FtxuiAppImpl::set_playlist_provider(PlaylistProvider provider) {
  std::lock_guard<std::mutex> lk(grid_mu_);
  playlist_provider_ = std::move(provider);
  playlist_snap_.reset();  // next refresh builds a fresh snapshot
}

void FtxuiAppImpl::set_album_headers(bool on) {
  album_headers_.store(on);
  // The panel height changes with the toggle; wake the tick loop so on_blit
  // re-sizes the vis and posts a repaint immediately (same as the V key).
  ticks_->wake();
}

bool FtxuiAppImpl::album_headers() const {
  return album_headers_.load();
}

// refresh + panel height, shared by both threads (see the header comment).
int FtxuiAppImpl::playlist_panel_rows() {
  std::lock_guard<std::mutex> lk(grid_mu_);
  if (!playlist_provider_) {
    return 0;  // no panel installed — the vis keeps the full height
  }
  // Revision-keyed refresh: an unchanged playlist costs one atomic load in
  // the provider (no string copies on the per-repaint path); a moved
  // revision returns a fresh snapshot which is cached here for render_playlist.
  const std::uint64_t seen =
      playlist_snap_ ? playlist_snap_->revision
                     : std::numeric_limits<std::uint64_t>::max();
  if (std::optional<PlaylistSnapshot> snap = playlist_provider_(seen)) {
    playlist_snap_ = std::move(snap);
  }
  if (!playlist_snap_ || playlist_snap_->titles.empty()) {
    return 1;  // hint line only
  }
  // Panel height = list header + track rows + album-header rows. The window
  // math is shared with render_playlist (playlist_view, same lock) so the
  // height reserved here always equals the number of rows the panel renders.
  const PlaylistView v = playlist_view(*playlist_snap_, album_headers_.load());
  return 1 + (v.last - v.first) + v.header_rows;
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

  // Size the vis before the tick loop starts: on_blit (the only other
  // set_size caller) cannot fire until the first render() succeeds, and
  // render()/tick() early-return while cols_/rows_ are unset — a bootstrap
  // deadlock. Same sizing math as on_blit (the loop re-sizes each blit),
  // including the playlist-panel height so the first frame already fits
  // both the spectrum and the panel.
  const TermSize ts = term_size();
  if (ts.cols > 0) {
    const int vis_rows =
        fullscreen_.load() ? ts.rows
                           : std::max(1, ts.rows - 2 - playlist_panel_rows());
    vis_.set_size(ts.cols, vis_rows);
  }
  ticks_->start();
  // Focus tracking (DECSET 1004): xterm-family terminals report focus in/out
  // as CSI sequences handled in on_event; terminals without support simply
  // ignore the enable. Focus-out freezes repaints (on_blit skips the store +
  // post), so an unfocused bootamp window idles at ~0% CPU instead of
  // re-emitting the whole screen every tick.
  constexpr char kFocusEnable[]  = "\x1B[?1004h";
  constexpr char kFocusDisable[] = "\x1B[?1004l";
  const auto focus_on =
      ::write(STDOUT_FILENO, kFocusEnable, sizeof(kFocusEnable) - 1);
  (void)focus_on;
  app.Loop(component);  // blocks until Exit()
  const auto focus_off =
      ::write(STDOUT_FILENO, kFocusDisable, sizeof(kFocusDisable) - 1);
  (void)focus_off;
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
  // Focus events (DECSET 1004, enabled in run()): ESC[I = focus in, ESC[O =
  // focus out. FTXUI 7.0.3 uniformizes CSI ESC[O to "\x1BOR" — the F3 bytes
  // (g_uniformize in terminal_input_parser.cpp) — because focus-out shares
  // ESC[O with scoansi F3, so the app can only ever receive "\x1BOR" for
  // focus-out. Real F3 (SS3 ESC OR) is byte-identical and bootamp never
  // claims F3, so treating it as focus-out is safe. ESC[I is not uniformized
  // and arrives raw. Checked before key translation: focus-in wakes the tick
  // loop so the frozen frame repaints immediately; focus-out just records the
  // state (on_blit stops posting).
  const std::string input = e.character();
  if (input == "\x1B[I") {
    focused_.store(true);
    ticks_->wake();
    return true;
  }
  if (input == "\x1BOR" || input == "\x1B[O") {
    focused_.store(false);
    return true;
  }
  const std::string name = canonical_key_name(e.is_character(), input);
  if (name.empty()) {
    return false;  // unclaimed: function keys, unknown sequences, mouse
  }
  // Key thaw: focus-out above cannot be told apart from a real F3, and a
  // terminal that never sends focus-in on return must still unfreeze — so any
  // claimed key resumes the screen. Mouse is returned at the top, so only
  // real keys land here.
  if (!focused_.load()) {
    focused_.store(true);
    ticks_->wake();
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
  if (name == "o") {
    // Vis-off toggle: None renders nothing — render() returns false, so the
    // tick loop stops blitting and an idle window uses ~0% vis CPU. The
    // current mode is saved and restored on the next `o`. The frame change
    // (blank or restored) is forced with a direct post: no tick posts will
    // follow while None is active, and the restore's fresh frame may hash
    // identically to the last posted one (static content), which would
    // otherwise leave the fingerprint gate silent.
    if (vis_.mode() != VisMode::None) {
      saved_vis_mode_ = vis_.mode();
      vis_.set_mode(VisMode::None);
      {
        std::lock_guard<std::mutex> lk(grid_mu_);
        has_grid_ = false;   // next repaint draws a blank canvas
        prev_ = CellGrid{};  // reset the dirty tracker: full redraw on restore
      }
      canvas_ = ftxui::Canvas(canvas_.width(), canvas_.height());  // wipe
    } else if (saved_vis_mode_ != VisMode::None) {
      vis_.set_mode(saved_vis_mode_);  // also sets the refresh flag
      ticks_->wake();
    }
    if (ftxui::App* s = screen_.load()) {
      s->PostEvent(ftxui::Event::Custom);
    }
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
  // tick()/render()); the loop picks up the new size next iteration. The
  // playlist panel shrinks the vis area in non-fullscreen; both threads
  // refresh the snapshot cache under grid_mu_ so the sizing matches what
  // document() renders.
  const TermSize ts = term_size();
  const int vis_rows =
      fullscreen_.load() ? ts.rows
                         : std::max(1, ts.rows - 2 - playlist_panel_rows());
  vis_.set_size(ts.cols, vis_rows);

  if (!focused_.load()) {
    // Focus out (DECSET 1004): freeze the displayed frame. No store and no
    // post — the event loop idles (~0% CPU) while the tick thread keeps its
    // cheap work; a focus-in event wakes a repaint. latest_/prev_ still match
    // what's on screen, so the freeze is seamless.
    return;
  }

  // Frame-unchanged skip + post floor: a content fingerprint (XOR-64, ~5µs
  // for 1920 cells) gates BOTH the grid copy and the repaint post — an
  // identical frame costs a hash instead of a full canvas re-serialization +
  // terminal write. Posts are additionally floored to the driver's own
  // cadence (vis_.tick_interval on the same ctx the TickLoop sleeps on), so
  // wake() storms cannot queue repaints faster than the driver animates. A
  // skipped store keeps latest_ == what's displayed (prev_ stays in sync).
  const std::uint64_t fp = grid_fingerprint(grid);
  const auto now = std::chrono::steady_clock::now();
  if (fp != last_posted_fp_ && now - last_post_at_ >= min_post_interval()) {
    {
      std::lock_guard<std::mutex> lk(grid_mu_);
      latest_ = grid;
      has_grid_ = true;
    }
    last_posted_fp_ = fp;
    last_post_at_ = now;
    if (ftxui::App* s = screen_.load()) {
      s->PostEvent(ftxui::Event::Custom);  // wake the loop + force a repaint
    }
  }
}

ftxui::Element FtxuiAppImpl::render_playlist(int cols) {
  std::lock_guard<std::mutex> lk(grid_mu_);
  std::vector<ftxui::Element> lines;
  if (!playlist_provider_) {
    return ftxui::emptyElement();
  }
  if (!playlist_snap_ || playlist_snap_->titles.empty()) {
    // Empty playlist: a one-line hint (Go renders no body rows for an empty
    // list; the header carries the count).
    lines.push_back(ftxui::text(ui::clip_text("Playlist  (empty)", std::max(cols, 0))));
    return ftxui::vbox(std::move(lines));
  }
  const int n       = static_cast<int>(playlist_snap_->titles.size());
  const int current = std::clamp(playlist_snap_->current_index, 0, n - 1);
  // Window math is shared with playlist_panel_rows (playlist_view, same
  // lock): up to kPlaylistRows track rows centered on the current track,
  // plus a dim "── {album} ──" row above the first track of each album run
  // inside the window when album headers are enabled. The header rows are
  // additive — they never displace a track row, and the current track is
  // always inside [first, last), so the highlight can never be pushed
  // off-screen by the headers. v.header_rows (counted from the same
  // album_run_start predicate used below) is exactly the number of header
  // lines emitted, so the height playlist_panel_rows() reserved matches this
  // vbox row-for-row.
  const bool album_headers = album_headers_.load();
  const PlaylistView v = playlist_view(*playlist_snap_, album_headers);
  const int first = v.first;
  const int last  = v.last;
  // Header: "Playlist  pos/total" (Go sepHeaderN, same as the queue header).
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Playlist  %d/%d", current + 1, n);
  lines.push_back(
      ftxui::text(ui::clip_text(buf, std::max(cols, 0))));
  const int room = std::max(1, cols - 8);  // Go PanelWidth-8 row truncation
  for (int i = first; i < last; ++i) {
    const std::size_t u = static_cast<std::size_t>(i);
    if (album_headers && album_run_start(*playlist_snap_, i, first)) {
      // Dim run header for the first track of each album run; clipped to the
      // panel width like the list header ("── " + album + " ──" is 6 columns
      // wider than the album itself). Album access is bounds-safe here:
      // album_run_start already verified i < albums.size() and non-empty.
      std::string head = "── " + playlist_snap_->albums[u] + " ──";
      lines.push_back(ftxui::dim(
          ftxui::text(ui::clip_text(head, std::max(cols, 0)))));
    }
    std::snprintf(buf, sizeof(buf), "%d. ", i + 1);  // absolute 1-based number
    std::string label =
        std::string(buf) + ui::clip_text(playlist_snap_->titles[i], room);
    if (i == current) {
      label.insert(0, "▶ ");  // current-track marker (Go: bold + ColorPlaying)
    }
    ftxui::Element line = ftxui::text(std::move(label));
    if (i == current) {
      line = line | ftxui::bold | ftxui::color(ftxui::Color::Green);
    }
    lines.push_back(std::move(line));
  }
  return ftxui::vbox(std::move(lines));
}

ftxui::Element FtxuiAppImpl::document() {
  const TermSize ts = term_size();
  const int dimx = ts.cols;
  const int dimy = ts.rows;
  // resize_hook_ fires only on an actual size change (the wiring agent's
  // screen row windows track the terminal; repaints with an unchanged size
  // must not re-run it).
  if (resize_hook_ && (dimx != last_hook_cols_ || dimy != last_hook_rows_)) {
    last_hook_cols_ = dimx;
    last_hook_rows_ = dimy;
    resize_hook_(dimx, dimy);
  }
  if (screens::TooSmallModel::is_too_small(dimx, dimy)) {
    // Go layout gate (<40x10): resize hint centered instead of the frame.
    return ftxui::center(
        ftxui::text(screens::TooSmallModel::message(dimx, dimy)));
  }
  const ftxui::Element help = help_line(dimx);
  if (overlay_ && screen_visible_.load()) {
    // A screen (queue/browse/EQ/help) is open: full-frame swap — the screens
    // composite renders the active screen above the status + help lines. The
    // vis canvas is not drawn (it keeps ticking in the background).
    return ftxui::vbox({
        overlay_->Render(),
        status_element(dimx),
        help,
    });
  }
  const int vis_cols = dimx;
  if (fullscreen_.load()) {
    // V: visualizer alone (Go renderFullVisualizer) — the playlist panel is
    // hidden along with the status/help lines. With the vis off (`o`) the
    // grid is blank, so this is an empty canvas — Go VisNone fullscreen
    // renders nothing too.
    return vis_canvas(vis_cols, dimy);
  }
  // Playlist panel under the spectrum (Go stacks renderPlaylist below it);
  // the vis keeps the majority of the screen height. The panel height is
  // computed after a revision-keyed snapshot refresh, so an unchanged
  // playlist costs one atomic load per repaint — no string copies in
  // document() (the same sizing on_blit uses for set_size).
  const int vis_rows = std::max(1, dimy - 2 - playlist_panel_rows());
  return ftxui::vbox({
      vis_canvas(vis_cols, vis_rows),
      render_playlist(dimx),
      status_element(dimx),
      help,
  });
}

ftxui::Element FtxuiAppImpl::vis_canvas(int vis_cols, int vis_rows) {
  // The canvas persists across repaints (pixels = cols*2 x rows*4): the dirty
  // blit (draw_grid) updates only changed cells, so an unchanged frame costs
  // zero DrawText inserts. A size change recreates the canvas (fresh storage;
  // prev_'s dims mismatch then forces a full redraw). The Element is created
  // per repaint as a referencing ConstRef — no canvas copy either.
  const int w = vis_cols * 2;
  const int h = vis_rows * 4;
  if (canvas_.width() != w || canvas_.height() != h) {
    canvas_ = ftxui::Canvas(w, h);
  }
  draw_grid(canvas_);
  return ftxui::canvas(&canvas_);
}

ftxui::Element FtxuiAppImpl::help_line(int cols) {
  // Built once per terminal width (the clip width is baked into the Element)
  // and reused across repaints — the help text is static.
  if (cols != help_clip_cols_) {
    help_clip_cols_ = cols;
    help_el_ = ftxui::dim(
        ftxui::text(ui::clip_text(kHelpLine, std::max(cols, 0))));
  }
  return help_el_;
}

ftxui::Element FtxuiAppImpl::status_element(int cols) {
  // Rebuilt only when the provider output (or the clip width) changed — the
  // provider is still polled per repaint so changes are detected.
  const std::string status = status_ ? status_() : std::string();
  if (status != last_status_ || cols != last_status_cols_) {
    last_status_ = status;
    last_status_cols_ = cols;
    status_el_ = ftxui::text(ui::clip_text(status, std::max(cols, 0)));
  }
  return status_el_;
}

std::chrono::milliseconds FtxuiAppImpl::min_post_interval() {
  // The driver's current cadence — the same vis_.tick_interval(ctx) the
  // TickLoop sleeps on, from the ctx copy cached in set_tick_context (same
  // freshness as the loop's own ctx). The post floor for on_blit.
  std::lock_guard<std::mutex> lk(ctx_mu_);
  return vis_.tick_interval(tick_ctx_);
}

std::unique_ptr<FtxuiApp> make_ftxui_app(Visualizer& vis,
                                         FtxuiApp::KeyCallback on_key,
                                         FtxuiApp::StatusProvider status) {
  return std::make_unique<FtxuiAppImpl>(vis, std::move(on_key),
                                        std::move(status));
}

void FtxuiAppImpl::draw_grid(ftxui::Canvas& canvas) {
  std::lock_guard<std::mutex> lk(grid_mu_);
  if (!has_grid_) {
    return;  // blank canvas: mode None (vis off) or never sized
  }
  const int rows = latest_.rows();
  const int cols = latest_.cols();
  // Dirty-cell blit: the canvas persists across frames (see vis_canvas), so
  // only cells that changed since the last draw need DrawText — unchanged
  // cells are still present in the canvas. A size change (canvas recreated)
  // makes every cell dirty; prev_ then tracks the new grid. No grid copy:
  // latest_ is drawn in place under the lock.
  const bool full = prev_.rows() != rows || prev_.cols() != cols;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const Cell cell = latest_.at(r, c);
      if (!full) {
        const Cell prev = prev_.at(r, c);
        if (cell.rune == prev.rune && cell.color == prev.color) {
          continue;  // unchanged: the canvas already holds this cell
        }
      }
      // Terminal cell (c, r) = pixels (c*2 .. c*2+1, r*4 .. r*4+3).
      const int px = c * 2;
      const int py = r * 4;
      if (px >= canvas.width() || py + 4 > canvas.height()) {
        continue;  // stale grid larger than the vis area: clip
      }
      if (cell.rune == U' ' || cell.rune == U'\0') {
        if (full) {
          continue;  // a fresh canvas is already blank here
        }
        // Cleared cell: overwrite the persistent canvas entry — the fresh
        // canvas per frame no longer blanks it automatically.
        canvas.DrawText(px, py, " ", ftxui::Color::Default);
        continue;
      }
      canvas.DrawText(px, py, utf8_of(cell.rune), color_of(cell.color));
    }
  }
  prev_ = latest_;
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
