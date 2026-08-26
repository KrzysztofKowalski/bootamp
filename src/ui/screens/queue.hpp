// ui/screens/queue.hpp — queue manager screen: model + FTXUI component.
//
// Port of cliamp's queue manager overlay (ui/model/keys.go handleQueueKey,
// inline_overlays.go renderQueueBody, scroll.go clampScroll). The model is
// plain C++ (no FTXUI): it reads the live bootamp::playlist::Playlist queue,
// owns cursor/scroll/visibility state, dispatches keys, and fires host-wired
// action callbacks (play/remove/clear/shuffle/repeat/favorite). The FTXUI
// Component glue is compiled only when BOOTAMP_HAS_FTXUI is defined.
//
// Keys (queue screen): j/k or up/down move (wrap), Shift+Up/Down reorder,
// enter = play queued track (bootamp addition per screens spec; Go's daemon
// "queue.play" is the twin), d = remove, c = clear+close, s = toggle shuffle,
// r = cycle repeat, f = favorite (radio), esc/A = close, ctrl+k = help (host).
#pragma once

#include "playlist/playlist.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if BOOTAMP_HAS_FTXUI
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// Actions the host app must wire (Go: the update-loop side effects). Defined
// at namespace scope (not nested in QueueModel) so the constructor's default
// argument can value-initialize it — a nested struct is still incomplete at
// the point the default argument is parsed.
struct QueueActions {
  // enter — play the queued entry; track_index is the playlist index of the
  // queued track (Go daemon queue.play: SetIndex + play).
  std::function<void(int track_index)> on_play{};
  // d — remove the queue entry at `pos` (Go: RemoveQueueAt).
  std::function<void(int pos)>          on_remove_at{};
  // c — clear the whole queue (Go: ClearQueue).
  std::function<void()>                 on_clear{};
  // s — toggle shuffle (Go: playlist.ToggleShuffle).
  std::function<void()>                 on_toggle_shuffle{};
  // r — cycle repeat Off→All→One (Go: playlist.CycleRepeat).
  std::function<void()>                 on_cycle_repeat{};
  // f — toggle favorite for the selected entry's track (radio stations;
  // the host no-ops for non-favoritable entries).
  std::function<void(int pos)>          on_toggle_favorite{};
};

// QueueModel holds the queue-manager screen state. It references the live
// playlist, so every query reflects concurrent mutations (the playlist is
// thread-safe). All mutation side effects go through Actions — the host app
// wires them to the engine/playlist/provider; tests observe them directly.
class QueueModel {
public:
  using Actions = QueueActions;

  explicit QueueModel(playlist::Playlist& pl, Actions actions = Actions{});
  ~QueueModel() = default;
  QueueModel(const QueueModel&)            = delete;
  QueueModel& operator=(const QueueModel&) = delete;

  void set_actions(Actions a) { actions_ = std::move(a); }

  // --- Visibility ---------------------------------------------------------
  void open();                 // Go: "A" opens with cursor/scroll reset
  void close() { visible_ = false; }
  bool visible() const { return visible_; }

  // --- List state ---------------------------------------------------------
  int  count() const { return pl_.queue_len(); }     // Go QueueLen
  bool empty() const { return pl_.queue_len() == 0; }
  int  cursor() const { return cursor_; }
  int  scroll() const { return scroll_; }
  // set_visible_rows sets the row budget and re-clamps cursor/scroll
  // (Go normalizeQueueOverlay with visible = effectivePlaylistVisible).
  void set_visible_rows(int rows);

  // --- Navigation (Go handleQueueKey) -------------------------------------
  void cursor_up();                 // wraps to last (Go)
  void cursor_down();               // wraps to first (Go)
  void move_up();                   // Shift+Up: move_queue(cursor, cursor-1)
  void move_down();                 // Shift+Down: move_queue(cursor, cursor+1)
  void remove_at_cursor();          // d
  void clear();                     // c (also closes)
  void play_cursor();               // enter — plays the queued entry
  void toggle_shuffle();            // s
  void cycle_repeat();              // r
  void toggle_favorite();           // f

  // handle_key dispatches a Bubbletea-style key name; returns true if the
  // queue screen consumed it (host falls through to global keys otherwise).
  bool handle_key(std::string_view key);

  // --- Rendering data (Go renderQueueBody) --------------------------------
  // entry_at returns the i-th queue entry (absolute queue position i).
  playlist::QueueEntry entry_at(int i) const;
  // row_label renders "N. DisplayName" with the display name truncated to
  // panel_width-8 columns (Go: fmt.Sprintf("%d. %s", i+1, truncate(name,
  // PanelWidth-8))).
  std::string row_label(int i, int panel_width) const;
  // header_label renders "Queue  pos/total" (Go sepHeaderN; total<=0 → "Queue").
  std::string header_label() const;

private:
  void normalize();  // port of Go clampScroll + normalizeQueueOverlay

  playlist::Playlist& pl_;
  Actions             actions_;
  bool                visible_      = false;
  int                 cursor_       = 0;
  int                 scroll_       = 0;
  int                 visible_rows_ = 0;
};

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see queue.cpp).
// Renders the queue as a scrollable Menu plus the help line; keys handled by
// the model are also wired into the component. Defined in queue.cpp.
std::shared_ptr<ftxui::ComponentBase> make_queue_component(QueueModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
