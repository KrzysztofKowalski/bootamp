// ui/screens/queue.cpp — queue manager screen implementation.
//
// Model is a 1:1 port of cliamp ui/model: handleQueueKey (keys.go),
// normalizeQueueOverlay (keys.go), renderQueueBody (inline_overlays.go), and
// clampScroll (scroll.go), translated onto bootamp::playlist::Playlist.
#include "ui/screens/queue.hpp"

#include "ui/fit.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <vector>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

namespace {

// Port of Go clampScroll (ui/model/scroll.go): keep cursor inside [0, count)
// and scroll such that the cursor is within the `visible`-row window.
void clamp_scroll(int& cursor, int& scroll, int count, int visible) {
  if (visible <= 0) {
    return;
  }
  if (cursor < 0) {
    cursor = 0;
  }
  if (cursor >= count && count > 0) {
    cursor = count - 1;
  }
  if (cursor < scroll) {
    scroll = cursor;
  } else if (cursor >= scroll + visible) {
    scroll = cursor - visible + 1;
  }
  if (scroll + visible > count && count > 0) {
    scroll = std::max(0, count - visible);
  }
  if (scroll < 0) {
    scroll = 0;
  }
}

}  // namespace

QueueModel::QueueModel(playlist::Playlist& pl, Actions actions)
    : pl_(pl), actions_(std::move(actions)) {}

void QueueModel::open() {
  // Go "A" key: visible=true, cursor=0, scroll=0.
  visible_ = true;
  cursor_ = 0;
  scroll_ = 0;
  normalize();
}

void QueueModel::set_visible_rows(int rows) {
  visible_rows_ = std::max(rows, 0);
  normalize();
}

void QueueModel::cursor_up() {
  // Go "up"/"k": decrement, wrapping to the last entry.
  if (cursor_ > 0) {
    --cursor_;
  } else if (count() > 0) {
    cursor_ = count() - 1;
  }
  normalize();
}

void QueueModel::cursor_down() {
  // Go "down"/"j": increment, wrapping to the first entry.
  if (cursor_ < count() - 1) {
    ++cursor_;
  } else if (count() > 0) {
    cursor_ = 0;
  }
  normalize();
}

void QueueModel::move_up() {
  // Go Shift+Up: MoveQueue(cursor, cursor-1); cursor follows the entry.
  if (cursor_ > 0 && pl_.move_queue(cursor_, cursor_ - 1)) {
    --cursor_;
  }
  normalize();
}

void QueueModel::move_down() {
  // Go Shift+Down: MoveQueue(cursor, cursor+1); cursor follows the entry.
  if (cursor_ < count() - 1 && pl_.move_queue(cursor_, cursor_ + 1)) {
    ++cursor_;
  }
  normalize();
}

void QueueModel::remove_at_cursor() {
  // Go "d": RemoveQueueAt(cursor) then normalize. (The host records the undo
  // snapshot before this fires.)
  if (count() > 0) {
    if (actions_.on_remove_at) {
      actions_.on_remove_at(cursor_);
    } else {
      pl_.remove_queue_at(cursor_);
    }
    normalize();
  }
}

void QueueModel::clear() {
  // Go "c": ClearQueue, normalize, close.
  if (count() > 0) {
    if (actions_.on_clear) {
      actions_.on_clear();
    } else {
      pl_.clear_queue();
    }
  }
  normalize();
  close();
}

void QueueModel::play_cursor() {
  // bootamp queue screen "enter": play the queued entry. Go twin: daemon
  // "queue.play" — SetIndex(track index) + play current.
  if (count() > 0 && cursor_ >= 0 && cursor_ < count()) {
    const playlist::QueueEntry e = entry_at(cursor_);
    if (actions_.on_play) {
      actions_.on_play(e.track_index);
    }
  }
}

void QueueModel::toggle_shuffle() {
  if (actions_.on_toggle_shuffle) {
    actions_.on_toggle_shuffle();
  } else {
    pl_.toggle_shuffle();
  }
}

void QueueModel::cycle_repeat() {
  if (actions_.on_cycle_repeat) {
    actions_.on_cycle_repeat();
  } else {
    pl_.cycle_repeat();
  }
}

void QueueModel::toggle_favorite() {
  if (count() > 0 && cursor_ >= 0 && cursor_ < count()) {
    if (actions_.on_toggle_favorite) {
      actions_.on_toggle_favorite(cursor_);
    }
  }
}

bool QueueModel::handle_key(std::string_view key) {
  // Go handleQueueKey: ctrl+c → quit, ctrl+k/? → keymap, ctrl+x → expand are
  // host-level keys; the queue screen consumes only its own navigation.
  if (key == "up" || key == "k") {
    cursor_up();
    return true;
  }
  if (key == "down" || key == "j") {
    cursor_down();
    return true;
  }
  if (key == "shift+up") {
    move_up();
    return true;
  }
  if (key == "shift+down") {
    move_down();
    return true;
  }
  if (key == "d") {
    remove_at_cursor();
    return true;
  }
  if (key == "c") {
    clear();
    return true;
  }
  if (key == "enter") {
    play_cursor();
    return true;
  }
  if (key == "s") {
    toggle_shuffle();
    return true;
  }
  if (key == "r") {
    cycle_repeat();
    return true;
  }
  if (key == "f") {
    toggle_favorite();
    return true;
  }
  if (key == "esc" || key == "A") {
    close();
    return true;
  }
  return false;
}

playlist::QueueEntry QueueModel::entry_at(int i) const {
  const std::vector<playlist::QueueEntry> entries = pl_.queue_entries();
  if (i < 0 || i >= static_cast<int>(entries.size())) {
    return {};
  }
  return entries[static_cast<std::size_t>(i)];
}

std::string QueueModel::row_label(int i, int panel_width) const {
  // Go renderQueueBody: fmt.Sprintf("%d. %s", start+i+1, truncate(DisplayName,
  // PanelWidth-8)) with start = scroll. The number is the absolute 1-based
  // queue position (i is the absolute index; the caller passes window rows).
  char num[16];
  std::snprintf(num, sizeof(num), "%d.", i + 1);
  const playlist::QueueEntry e = entry_at(i);
  const int                  room = std::max(1, panel_width - 8);
  return std::string(num) + " " + ui::clip_text(e.track.display_name(), room);
}

std::string QueueModel::header_label() const {
  // Go sepHeaderN("Queue", cursor+1, count): label "Queue  pos/total".
  const int n = count();
  if (n <= 0) {
    return "Queue";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Queue  %d/%d", cursor_ + 1, n);
  return buf;
}

void QueueModel::normalize() {
  // Port of Go normalizeQueueOverlay: empty queue resets cursor/scroll;
  // otherwise clampScroll keeps the cursor in the visible window.
  const int n = count();
  if (n == 0) {
    cursor_ = 0;
    scroll_ = 0;
    return;
  }
  cursor_ = std::clamp(cursor_, 0, n - 1);
  if (visible_rows_ <= 0) {
    scroll_ = std::clamp(scroll_, 0, n - 1);
    if (cursor_ < scroll_) {
      scroll_ = cursor_;
    }
    return;
  }
  clamp_scroll(cursor_, scroll_, n, visible_rows_);
}

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): the exact ftxui API surface
// (7.0.3) needs a compile pass once FTXUI lands. The model handles all keys,
// so this wrapper only needs to present the queue as a scrollable list.

std::shared_ptr<ftxui::ComponentBase> make_queue_component(QueueModel& model) {
  // Entries and selection are rebuilt at render time from the live model so
  // concurrent playlist mutations (gapless advance, host enqueues) show
  // immediately. The menu window starts at the model scroll; scrolling within
  // the menu is delegated to the model via the key event path below.
  auto entries  = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);

  auto menu = ftxui::Menu(entries.get(), selected.get(),
                          ftxui::MenuOption::Vertical());

  auto component = ftxui::CatchEvent(menu, [&model](const ftxui::Event& e) {
    // Model consumes queue keys (j/k/d/c/enter/s/r/f/esc/...); everything
    // else falls through to the host (help/quit/expand).
    return model.handle_key(e.character());
  });

  return ftxui::Renderer(component, [menu, entries, selected, &model] {
    // Rebuild the visible window from the model on every render.
    entries->clear();
    const int n = model.count();
    if (n > 0) {
      const int width = 80;  // host supplies panel width via set_visible_rows
      for (int i = model.scroll(); i < n; ++i) {
        entries->push_back(model.row_label(i, width));
      }
      *selected = std::max(0, model.cursor() - model.scroll());
    }
    return ftxui::vbox({ftxui::text(model.header_label()),
                        menu->Render() | ftxui::frame});
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
