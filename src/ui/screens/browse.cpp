// ui/screens/browse.cpp — provider browse screen implementation.
//
// Model ports: renderProviderList section logic (view.go), providerMoveUp/Down/
// PageUp/PageDown (keys.go), maybeLoadCatalogBatch + catalogBatch handling
// (keys_radio.go/providers.go/update.go — the page fetch is async: a worker
// thread runs load_page_fn_ and pump_catalog_result() merges the result on the
// render path, so the UI never blocks on the network), handleProvSearchKey /
// handleCatalogSearchKey (keys.go), and the net-search input/results model
// (keys.go handleNetSearchInputKey/handleNetSearchResultsKey, inline_overlays.go
// renderNetSearchBody — the results list replaces the provider list while
// results are active).
#include "ui/screens/browse.hpp"

#include "resolve/ytdl.hpp"
#include "ui/fit.hpp"

#include <algorithm>
#include <cstdio>
#include <stop_token>
#include <utility>

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): needs a compile pass against
// ftxui 7.0.3 once installed. The model owns all state; this wrapper renders
// the sectioned list + search Input and forwards keys to the model.
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

namespace {

// Port of Go clampScroll (ui/model/scroll.go) — see queue.cpp.
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

// Go renderProviderList section transitions: a header is emitted when the
// entry's section differs from the previous entry's section.
std::string section_for_prefix(std::string_view prefix) {
  if (prefix == "l") {
    return "Local";
  }
  if (prefix == "f") {
    return "★ Favorites";
  }
  if (prefix == "c") {
    return "Catalog";
  }
  if (prefix == "s") {
    return "Search";
  }
  return "";
}

}  // namespace

BrowseModel::BrowseModel(RefreshFn refresh, LoadPageFn load_page,
                         SearchFn search_catalog, NetResolveFn net_resolve,
                         PrefixFn id_prefix, FavoritableFn is_favoritable,
                         Actions actions)
    : refresh_fn_(std::move(refresh)),
      load_page_fn_(std::move(load_page)),
      search_catalog_fn_(std::move(search_catalog)),
      net_resolve_fn_(std::move(net_resolve)),
      id_prefix_fn_(std::move(id_prefix)),
      is_favoritable_fn_(std::move(is_favoritable)),
      actions_(std::move(actions)) {}

BrowseModel::~BrowseModel() {
  // Stop the catalog fetch thread. A fetch in flight may be blocked in the
  // network call (libcurl has its own 10s timeout, catalog.cpp http_get);
  // request_stop() prevents its result from being published and join() waits
  // for the call to unwind.
  if (fetch_thread_.joinable()) {
    fetch_thread_.request_stop();
    fetch_thread_.join();
  }
}

BrowseModel BrowseModel::for_provider(playlist::Provider& prov) {
  auto* loader    = dynamic_cast<provider::CatalogLoader*>(&prov);
  auto* searcher  = dynamic_cast<provider::CatalogSearcher*>(&prov);
  auto* sectioned = dynamic_cast<provider::SectionedList*>(&prov);
  auto* favoriter = dynamic_cast<provider::FavoriteToggler*>(&prov);
  (void)favoriter;  // favorites are toggled host-side via the on_favorite action

  RefreshFn refresh = [&prov]() { return prov.playlists(); };

  LoadPageFn load_page;
  if (loader != nullptr) {
    load_page = [loader](int offset, int limit) {
      return loader->load_catalog_page(offset, limit);
    };
  }

  SearchFn search_catalog;
  if (searcher != nullptr) {
    search_catalog = [searcher](std::string_view query) {
      return searcher->search_catalog(query);
    };
  }

  NetResolveFn net_resolve = [](std::string_view query) {
    // Go fetchNetSearchCmd → resolve.Remote; bootamp resolve_ytdl handles the
    // ytsearch10:/scsearch10: URLs via yt-dlp.
    return resolve::resolve_ytdl(query);
  };

  PrefixFn id_prefix;
  if (sectioned != nullptr) {
    id_prefix = [sectioned](std::string_view id) {
      return sectioned->id_prefix(id);
    };
  }

  FavoritableFn is_favoritable;
  if (sectioned != nullptr) {
    is_favoritable = [sectioned](std::string_view id) {
      return sectioned->is_favoritable_id(id);
    };
  }

  return BrowseModel(std::move(refresh), std::move(load_page),
                     std::move(search_catalog), std::move(net_resolve),
                     std::move(id_prefix), std::move(is_favoritable));
}

std::string BrowseModel::refresh() {
  // Go: fetchProviderPlaylists; failure surfaces provError.
  error_ = "";
  auto result = refresh_fn_();
  if (!result.has_value()) {
    error_ = std::move(result.error());
    lists_.clear();
    return error_;
  }
  lists_ = std::move(*result);
  has_sections_ = static_cast<bool>(id_prefix_fn_);
  // Keep the cursor valid (Go providerMaybeAdjustScroll).
  cursor_ = std::clamp(cursor_, 0, std::max(0, count() - 1));
  normalize();
  return "";
}

void BrowseModel::set_visible_rows(int rows) {
  visible_rows_ = std::max(rows, 0);
  normalize();
}

void BrowseModel::cursor_up() {
  // Go providerMoveUp: decrement, wrapping to the last entry.
  if (cursor_ > 0) {
    --cursor_;
  } else if (count() > 0) {
    cursor_ = count() - 1;
  }
  normalize();
}

void BrowseModel::cursor_down() {
  // Go providerMoveDown: increment, wrapping to the first entry.
  if (cursor_ < count() - 1) {
    ++cursor_;
  } else if (count() > 0) {
    cursor_ = 0;
  }
  normalize();
  maybe_load_more();
}

void BrowseModel::select_cursor() {
  // Go enter: load tracks for the selected playlist id.
  if (count() > 0 && cursor_ >= 0 && cursor_ < count()) {
    if (actions_.on_select) {
      actions_.on_select(lists_[static_cast<std::size_t>(cursor_)].id);
    }
  }
}

void BrowseModel::toggle_favorite() {
  // Go f (toggleProviderFavorite): only for favoritable entries.
  if (count() > 0 && cursor_ >= 0 && cursor_ < count()) {
    const std::string& id = lists_[static_cast<std::size_t>(cursor_)].id;
    if (is_favoritable_fn_ && is_favoritable_fn_(id)) {
      if (actions_.on_favorite) {
        actions_.on_favorite(id);
      }
    }
  }
}

void BrowseModel::page_up() {
  // Go providerPageUp: page by visible rows, top-anchor (scroll=cursor).
  if (count() <= 0) {
    return;
  }
  const int step = std::max(1, visible_rows_);
  if (cursor_ > 0) {
    cursor_ -= std::min(cursor_, step);
  }
  scroll_ = cursor_;
  normalize();
}

void BrowseModel::page_down() {
  // Go providerPageDown: page by visible rows (bottom-anchor).
  if (count() <= 0) {
    return;
  }
  const int step = std::max(1, visible_rows_);
  if (cursor_ < count() - 1) {
    cursor_ = std::min(count() - 1, cursor_ + step);
  }
  if (visible_rows_ > 0) {
    scroll_ = std::max(0, cursor_ - visible_rows_ + 1);
  }
  normalize();
}

void BrowseModel::go_top() {
  cursor_ = 0;
  normalize();
}

void BrowseModel::go_bottom() {
  if (count() > 0) {
    cursor_ = count() - 1;
  }
  normalize();
}

void BrowseModel::maybe_load_more() {
  // Go maybeLoadCatalogBatch: no loader → nothing; already loading/done →
  // nothing; while a search result set is showing → nothing (Go
  // CatalogSearcher.IsSearching); cursor must be within 10 rows of the end.
  if (!load_page_fn_ || catalog_loading_.load() || catalog_done_) {
    return;
  }
  if (search_mode_ == SearchMode::Catalog || search_mode_ == SearchMode::YouTube ||
      search_mode_ == SearchMode::SoundCloud) {
    return;
  }
  if (cursor_ < count() - kCatalogNearBottom) {
    return;
  }
  // Async fetch (Go: maybeLoadCatalogBatch returns a tea.Cmd — the UI stays
  // live while the page is fetched; the page lands via pump_catalog_result on
  // the next render). The single-flight guard (catalog_loading_) keeps one
  // fetch in flight. The worker calls only load_page_fn_ — the provider locks
  // its own state (provider.cpp) — and publishes a small immutable result.
  catalog_loading_.store(true);
  error_.clear();  // a retry clears the previous error (Go re-reports on fail)
  const int offset = catalog_offset_;
  fetch_thread_ = std::jthread([this, offset](std::stop_token stoken) {
    // Blocking network I/O (libcurl, 10s timeout) — never on the UI thread.
    auto result = load_page_fn_(offset, kCatalogBatchSize);
    CatalogFetchResult pub;
    if (result.has_value()) {
      pub.added = *result;
    } else {
      pub.error = std::move(result).error();
    }
    if (stoken.stop_requested()) {
      return;  // the model is being destroyed: no one will consume this
    }
    catalog_result_.store(
        std::make_shared<const CatalogFetchResult>(std::move(pub)));
  });
}

void BrowseModel::pump_catalog_result() {
  // Called by the render path every frame (UI thread): merge a completed
  // catalog fetch — Go update.go catalogBatchMsg: refresh the list, advance
  // the offset, and stop when a short page means the catalog is exhausted.
  auto result = catalog_result_.exchange(nullptr);
  if (!result) {
    return;
  }
  catalog_loading_.store(false);
  if (!result->error.empty()) {
    // On failure surface the error and leave the catalog loadable — the next
    // near-bottom scroll retries (Go does not mark the catalog done on error).
    error_ = result->error;
    return;
  }
  const int added = result->added;
  if (added <= 0) {
    catalog_done_ = true;
    return;
  }
  (void)refresh();
  catalog_offset_ += added;
  if (added < kCatalogBatchSize) {
    catalog_done_ = true;
  }
}

std::string BrowseModel::section_label(int i) const {
  if (!has_sections_ || i < 0 || i >= count()) {
    return "";
  }
  const std::string& id = lists_[static_cast<std::size_t>(i)].id;
  const std::string  pfx = id_prefix_fn_(id);
  const std::string  label = section_for_prefix(pfx);
  if (label.empty()) {
    return "";
  }
  // Emit only on section transitions (Go renderProviderList prevPrefix).
  if (i > 0) {
    const std::string prev = id_prefix_fn_(lists_[static_cast<std::size_t>(i - 1)].id);
    if (prev == pfx) {
      return "";
    }
  }
  return label;
}

void BrowseModel::start_search(SearchMode mode) {
  // Go: "/" on the provider list opens provSearch (Catalog for radio),
  // Ctrl+F opens the net-search (YouTube) overlay. Both reset the query.
  search_mode_ = mode;
  search_query_.clear();
  search_loading_ = false;
  search_error_.clear();
  search_results_kept_ = false;
  net_results_.clear();
  net_cursor_ = 0;
  net_scroll_ = 0;
}

void BrowseModel::close_search() {
  // Go: esc on the catalog search restores the normal catalog view
  // (CatalogSearcher.ClearSearch) and exits the search screen.
  if (search_mode_ == SearchMode::Catalog && search_catalog_fn_) {
    // The host clears the search server-side; the model just leaves the mode.
    // (ClearSearch is provider state, mirrored by refresh() on next load.)
  }
  search_mode_ = SearchMode::None;
  search_loading_ = false;
  search_error_.clear();
  search_results_kept_ = false;
  net_results_.clear();
  net_cursor_ = 0;
  net_scroll_ = 0;
}

void BrowseModel::set_search_query(std::string_view q) {
  search_query_ = std::string(q);
  search_error_.clear();
}

std::string BrowseModel::submit_search() {
  // Go handleCatalogSearchKey enter: empty query restores catalog; otherwise
  // the API search fires. Go handleNetSearchInputKey enter: "ytsearch10:" /
  // "scsearch10:" + query via resolve.Remote.
  if (!search_active()) {
    return "";
  }
  search_loading_ = true;
  search_error_.clear();
  net_results_.clear();
  net_cursor_ = 0;
  net_scroll_ = 0;

  std::string failure;
  switch (search_mode_) {
    case SearchMode::Catalog: {
      if (search_query_.empty()) {
        close_search();
        search_loading_ = false;
        return "";
      }
      if (search_catalog_fn_) {
        auto result = search_catalog_fn_(search_query_);
        if (!result.has_value()) {
          failure = std::move(result.error());
          search_error_ = failure;
        }
      }
      if (actions_.on_search_submitted) {
        actions_.on_search_submitted(search_query_);
      }
      // The provider's Playlists() now returns only search results; refresh.
      (void)refresh();
      if (failure.empty()) {
        // Go handleCatalogSearchKey enter: provSearch.active = false — the
        // search prompt closes immediately and the result list stays in the
        // catalog view (the provider keeps returning search results until
        // clear_search). Only esc / empty-query enter restore the catalog.
        search_mode_ = SearchMode::None;
        search_results_kept_ = true;
      }
      break;
    }
    case SearchMode::YouTube:
    case SearchMode::SoundCloud: {
      if (search_query_.empty()) {
        failure = "Enter a search query.";
        search_error_ = failure;
        break;
      }
      const std::string_view prefix =
          search_mode_ == SearchMode::YouTube ? "ytsearch10:" : "scsearch10:";
      std::string query = std::string(prefix) + search_query_;
      if (actions_.on_search_submitted) {
        actions_.on_search_submitted(query);
      }
      if (!net_resolve_fn_) {
        failure = "network search unavailable";
        search_error_ = failure;
        break;
      }
      auto result = net_resolve_fn_(query);
      if (!result.has_value()) {
        failure = std::move(result.error());
        search_error_ = failure;
      } else {
        net_results_ = std::move(*result);
        net_normalize();
      }
      break;
    }
    case SearchMode::None:
      break;
  }
  search_loading_ = false;
  return failure;
}

void BrowseModel::net_cursor_up() {
  if (net_cursor_ > 0) {
    --net_cursor_;
  } else if (!net_results_.empty()) {
    net_cursor_ = static_cast<int>(net_results_.size()) - 1;
  }
  net_normalize();
}

void BrowseModel::net_cursor_down() {
  if (net_cursor_ < static_cast<int>(net_results_.size()) - 1) {
    ++net_cursor_;
  } else if (!net_results_.empty()) {
    net_cursor_ = 0;
  }
  net_normalize();
}

void BrowseModel::net_select_cursor() {
  // Go handleNetSearchResultsKey enter: queue+play the selected result, then
  // close the net-search overlay (the host's search-close detection restores
  // the catalog list).
  if (net_results_.empty() || net_cursor_ < 0 ||
      net_cursor_ >= static_cast<int>(net_results_.size())) {
    return;
  }
  // Copy: close_search() below clears net_results_.
  const playlist::Track t = net_results_[static_cast<std::size_t>(net_cursor_)];
  if (actions_.on_select) {
    actions_.on_select(t.path);
  }
  close_search();
}

bool BrowseModel::handle_key(std::string_view key) {
  // Search screens consume their own keys first (Go key dispatch order).
  if (search_active()) {
    if (key == "esc") {
      close_search();
      return true;
    }
    if (!net_results_.empty()) {
      // Net-search results navigation (Go handleNetSearchResultsKey): enter
      // selects the highlighted result; up/down move the cursor.
      if (key == "enter") {
        net_select_cursor();
        return true;
      }
      if (key == "up" || key == "k") {
        net_cursor_up();
        return true;
      }
      if (key == "down" || key == "j") {
        net_cursor_down();
        return true;
      }
    }
    if (key == "enter") {
      (void)submit_search();
      return true;
    }
    if (key == "up" || key == "down") {
      // Go handleCatalogSearchKey/handleNetSearchInputKey: while the prompt
      // is open, up/down only move the text cursor (no visible effect); the
      // result list is not navigable until enter closes the prompt. Consume
      // them so they don't fall through to the global volume keys.
      return true;
    }
    // Other keys (text input) are handled by the host text editor via
    // set_search_query; not consumed here.
    return false;
  }

  // Catalog list navigation (Go focusProvider keys).
  if (key == "up" || key == "k") {
    cursor_up();
    return true;
  }
  if (key == "down" || key == "j") {
    cursor_down();
    return true;
  }
  if (key == "pgup" || key == "ctrl+u") {
    page_up();
    return true;
  }
  if (key == "pgdown" || key == "ctrl+d") {
    page_down();
    return true;
  }
  if (key == "home" || key == "g") {
    go_top();
    return true;
  }
  if (key == "end" || key == "G") {
    go_bottom();
    return true;
  }
  if (key == "enter") {
    select_cursor();
    return true;
  }
  if (key == "f") {
    toggle_favorite();
    return true;
  }
  if (key == "/") {
    // Go: "/" opens provider search; radio providers use catalog search.
    start_search(search_catalog_fn_ ? SearchMode::Catalog : SearchMode::None);
    return true;
  }
  if (key == "ctrl+f") {
    start_search(SearchMode::YouTube);
    return true;
  }
  if (key == "r") {
    (void)refresh();
    return true;
  }
  return false;
}

std::string BrowseModel::row_label(int i, int panel_width) const {
  // Go cursorLine: "> label" when active, "  label" otherwise; label is the
  // playlist name truncated to the panel width (with room for the prefix).
  if (i < 0 || i >= count()) {
    return "";
  }
  const playlist::PlaylistInfo& p = lists_[static_cast<std::size_t>(i)];
  const int room = std::max(1, panel_width - 4);
  const std::string name = ui::clip_text(p.name, room);
  if (i == cursor_) {
    return "> " + name;
  }
  return "  " + name;
}

std::string BrowseModel::header_label() const {
  // Go sepHeaderN("Browse", cursor+1, count) — bootamp labels the browse
  // pane "Browse"; the radio provider name is "Radio".
  const int n = count();
  if (n <= 0) {
    return "Browse";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Browse  %d/%d", cursor_ + 1, n);
  return buf;
}

std::string BrowseModel::net_row_label(int i, int panel_width) const {
  // Go renderNetSearchBody: each row is the track's DisplayName ("Artist -
  // Title", Title alone when the artist is unknown) truncated to the panel
  // width; windowList marks the cursor row with ">".
  if (i < 0 || i >= static_cast<int>(net_results_.size())) {
    return "";
  }
  const playlist::Track& t = net_results_[static_cast<std::size_t>(i)];
  const int room = std::max(1, panel_width - 4);
  const std::string name =
      t.artist.empty() ? t.title : t.artist + " - " + t.title;
  if (i == net_cursor_) {
    return "> " + ui::clip_text(name, room);
  }
  return "  " + ui::clip_text(name, room);
}

std::string BrowseModel::net_header_label() const {
  // Go sepHeaderN("Online Results", cursor+1, len) — the results screen header.
  const int n = static_cast<int>(net_results_.size());
  if (n <= 0) {
    return "Online Results";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Online Results  %d/%d", net_cursor_ + 1, n);
  return buf;
}

std::string BrowseModel::search_prompt(int panel_width) const {
  // Go promptHeader: "  <label>: <query>" — bootamp mirrors the filter prompt
  // "/ query" for search input.
  (void)panel_width;
  if (search_mode_ == SearchMode::YouTube) {
    return "/ yt: " + search_query_;
  }
  if (search_mode_ == SearchMode::SoundCloud) {
    return "/ sc: " + search_query_;
  }
  return "/ " + search_query_;
}

void BrowseModel::normalize() {
  // Go providerMaybeAdjustScroll: clamp cursor to the list and keep it in the
  // visible window (section headers consume rows in Go; bootamp counts only
  // station rows in the budget — same as the un-sectioned path).
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

void BrowseModel::net_normalize() {
  // Go netSearchResultsMaybeAdjustScroll: clampScroll over the result list.
  const int n = static_cast<int>(net_results_.size());
  if (n == 0) {
    net_cursor_ = 0;
    net_scroll_ = 0;
    return;
  }
  net_cursor_ = std::clamp(net_cursor_, 0, n - 1);
  if (visible_rows_ <= 0) {
    net_scroll_ = std::clamp(net_scroll_, 0, n - 1);
    if (net_cursor_ < net_scroll_) {
      net_scroll_ = net_cursor_;
    }
    return;
  }
  clamp_scroll(net_cursor_, net_scroll_, n, visible_rows_);
}

#if BOOTAMP_HAS_FTXUI
std::shared_ptr<ftxui::ComponentBase> make_browse_component(BrowseModel& model) {
  // Search input (visible only while a search is active). The Input binds to
  // the model's live query buffer; Enter/Esc are claimed by the CatchEvent
  // below (model.handle_key), character keys fall through to the Input.
  auto search_input = ftxui::Input(model.search_query_ptr(), "");
  search_input = ftxui::CatchEvent(search_input, [&model](const ftxui::Event& e) {
    return model.handle_key(e.character());
  });

  // Sectioned menu: entries are rebuilt per render from lists(); section
  // headers are interleaved when the section changes.
  auto entries  = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);
  auto menu = ftxui::Menu(entries.get(), selected.get(),
                          ftxui::MenuOption::Vertical());

  auto renderer = ftxui::Renderer(menu, [&model, search_input, entries, selected, menu] {
    // Merge a completed background catalog fetch (no-op unless one landed).
    model.pump_catalog_result();
    entries->clear();
    const int n = model.count();
    if (model.net_results_active()) {
      // Net-search results screen (Go renderNetSearchBody): the results list
      // replaces the provider list body, windowed by net_scroll/net_cursor.
      const int width = 80;
      const int nr = static_cast<int>(model.net_results().size());
      for (int i = model.net_scroll(); i < nr; ++i) {
        entries->push_back(model.net_row_label(i, width));
      }
      *selected = std::max(0, model.net_cursor() - model.net_scroll());
    } else if (n > 0) {
      const int width = 80;
      std::string last_section;
      for (int i = model.scroll(); i < n; ++i) {
        const std::string sec = model.section_label(i);
        if (!sec.empty() && sec != last_section) {
          entries->push_back("── " + sec + " ──");
          last_section = sec;
        }
        entries->push_back(model.row_label(i, width));
      }
      *selected = std::max(0, model.cursor() - model.scroll());
    }
    std::vector<ftxui::Element> lines = {
        ftxui::text(model.net_results_active() ? model.net_header_label()
                                               : model.header_label()),
    };
    if (model.search_active()) {
      lines.push_back(search_input->Render());
      if (!model.search_error().empty()) {
        lines.push_back(ftxui::text("  " + model.search_error()));
      }
    }
    lines.push_back(menu->Render() | ftxui::frame);
    if (!model.error().empty()) {
      // Catalog/browse failure status row (Go renders provError in the status
      // line; bootamp surfaces it at the bottom of the browse pane).
      lines.push_back(ftxui::dim(ftxui::color(ftxui::Color::Red,
                                              ftxui::text("  " + model.error()))));
    }
    if (model.catalog_loading()) {
      lines.push_back(ftxui::text("  Loading more stations…"));
    }
    return ftxui::vbox(lines);
  });
  return renderer;
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
