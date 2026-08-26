// ui/screens/browse.hpp — provider browse screen: model + FTXUI component.
//
// Port of cliamp's provider browse list (ui/model/view.go renderProviderList,
// keys.go focusProvider keys + handleProvSearchKey/handleCatalogSearchKey,
// keys_radio.go maybeLoadCatalogBatch, inline_overlays.go renderNetSearchBody,
// scroll.go clampScroll). For the radio provider the list is sectioned
// [Local] / [★ Favorites] / [Catalog] / [Search] (bootamp section labels; Go
// renders "Favorites"/"Catalog"/"Search Results" with no Local header), the
// catalog is lazy-loaded page by page (100/page, Go catalogBatchSize) on a
// background thread when the cursor nears the bottom — the UI stays live while
// the page is fetched and the entries land on the next render (Go: the catalog
// load is an async tea.Cmd, keys_radio.go/providers.go) — and search offers
// Radio Browser SearchStations
// (server-side) plus YouTube/SoundCloud net search via resolve::resolve_ytdl
// with ytsearch10:/scsearch10: queries.
//
// The model is plain C++ and testable: provider I/O is injected as std::function
// hooks (for_provider wires the real bootamp interfaces). The FTXUI Component
// glue is compiled only when BOOTAMP_HAS_FTXUI.
#pragma once

#include "playlist/playlist.hpp"
#include "playlist/provider.hpp"
#include "provider/base.hpp"

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if BOOTAMP_HAS_FTXUI
// FTXUI Component factory (compiled only when FTXUI is found; see browse.cpp).
// Composition: search Input (when search_active), a Menu that renders the
// sectioned provider list — or, while net_results_active(), the net-search
// results list (Go renderNetSearchBody) in its place — and a loading line
// while the catalog pages in.
namespace ftxui {
class ComponentBase;
}
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

// Actions the host app wires (side effects outside the model). Defined at
// namespace scope (not nested in BrowseModel) so the constructor's default
// argument can value-initialize it — a nested struct is still incomplete at
// the point the default argument is parsed.
struct BrowseActions {
  // enter on a station/playlist → host loads tracks(id) into the playlist.
  std::function<void(std::string_view id)> on_select{};
  // f on a favoritable entry → host toggles the favorite and refreshes.
  std::function<void(std::string_view id)> on_favorite{};
  // Search submitted (Catalog/YouTube/SoundCloud) — host may surface status.
  std::function<void(std::string_view query)> on_search_submitted{};
};

// BrowseModel shows a provider's playlist list with section headers, lazy
// catalog paging, and a search prompt (radio catalog search or YouTube/
// SoundCloud net search). All blocking provider work is funneled through the
// injected functions so tests drive it synchronously and deterministically.
class BrowseModel {
public:
  using Actions = BrowseActions;

  // Search mode (Go: provider catalog search vs the net-search overlay).
  enum class SearchMode : std::uint8_t {
    None,        // no search active
    Catalog,     // provider.CatalogSearcher (Radio Browser SearchStations)
    YouTube,     // ytsearch10: via resolve_ytdl
    SoundCloud,  // scsearch10: via resolve_ytdl
  };

  // Injected provider I/O (defaults wired by for_provider; tests inject fakes).
  using RefreshFn    = std::function<std::expected<std::vector<playlist::PlaylistInfo>, std::string>()>;
  using LoadPageFn   = std::function<std::expected<int, std::string>(int offset, int limit)>;
  using SearchFn     = std::function<std::expected<int, std::string>(std::string_view query)>;
  using NetResolveFn = std::function<std::expected<std::vector<playlist::Track>, std::string>(std::string_view query)>;
  using PrefixFn     = std::function<std::string(std::string_view id)>;
  using FavoritableFn = std::function<bool(std::string_view id)>;

  // Construct with explicit hooks (tests).
  BrowseModel(RefreshFn refresh, LoadPageFn load_page, SearchFn search_catalog,
              NetResolveFn net_resolve, PrefixFn id_prefix,
              FavoritableFn is_favoritable, Actions actions = {});
  ~BrowseModel();  // joins the catalog fetch thread (defined in browse.cpp)

  // for_provider wires the standard capability interfaces (radio provider via
  // dynamic_cast: CatalogLoader/CatalogSearcher/SectionedList/FavoriteToggler;
  // net search resolves through resolve::resolve_ytdl). Non-capable providers
  // degrade gracefully (no sections, no lazy paging, no catalog search).
  static BrowseModel for_provider(playlist::Provider& prov);

  void set_actions(Actions a) { actions_ = std::move(a); }

  // --- Catalog list -------------------------------------------------------
  // refresh reloads the provider's playlists (Go fetchProviderPlaylists).
  // Returns the error string on failure ("" on success); the error is also
  // cached for rendering.
  std::string refresh();
  const std::vector<playlist::PlaylistInfo>& lists() const { return lists_; }
  const std::string& error() const { return error_; }

  int  count() const { return static_cast<int>(lists_.size()); }
  int  cursor() const { return cursor_; }
  int  scroll() const { return scroll_; }
  void set_visible_rows(int rows);

  void cursor_up();    // Go providerMoveUp (wraps to last)
  void cursor_down();  // Go providerMoveDown (wraps to first) + maybe_load_more
  void select_cursor();  // Go enter: on_select(lists_[cursor].id)
  void toggle_favorite();  // Go f: on_favorite(id) when favoritable
  void page_up();       // Go providerPageUp (top-anchor)
  void page_down();     // Go providerPageDown
  void go_top();        // Go home/g
  void go_bottom();     // Go end/G

  // Lazy catalog paging (Go maybeLoadCatalogBatch): when the cursor reaches
  // within kCatalogNearBottom rows of the end and a CatalogLoader is present
  // (not loading, not done, not searching), fetch the next 100 entries. The
  // fetch runs on a background thread (the UI stays live); the result lands
  // via pump_catalog_result on the render path (Go catalogBatchMsg).
  void maybe_load_more();
  // pump_catalog_result merges a completed background catalog fetch into the
  // list state (refresh + offset advance + done-on-short-page, or the error
  // string on failure — the catalog stays loadable so the next near-bottom
  // scroll retries). Called by the render path every frame; a no-op while no
  // fetch has landed (one atomic load). UI thread only.
  void pump_catalog_result();
  bool catalog_loading() const { return catalog_loading_.load(); }
  bool catalog_done() const { return catalog_done_; }

  // --- Sections -----------------------------------------------------------
  // section_label returns the header label for the entry at index i ("" when
  // the previous entry shares the section — Go renders the header only on
  // section transitions). Prefix mapping (bootamp): l→"Local", f→"★ Favorites",
  // c→"Catalog", s→"Search", ""→none.
  std::string section_label(int i) const;
  bool        sectioned() const { return has_sections_; }

  // --- Search -------------------------------------------------------------
  void start_search(SearchMode mode);  // clears query, enters search screen
  void close_search();                 // esc: exit search (Go: restore catalog)
  bool search_active() const { return search_mode_ != SearchMode::None; }
  SearchMode search_mode() const { return search_mode_; }
  const std::string& search_query() const { return search_query_; }
  // search_query_ptr exposes the live query buffer for the FTXUI Input
  // component (which writes directly to the bound string on each keystroke).
  std::string* search_query_ptr() { return &search_query_; }
  void set_search_query(std::string_view q);  // typed text (no submit)
  bool search_loading() const { return search_loading_; }
  const std::string& search_error() const { return search_error_; }
  // search_results_kept — true after a successful catalog search submit
  // (enter): the prompt closed but the provider keeps returning the search
  // results, so the host must NOT clear the provider-side search (Go
  // handleCatalogSearchKey enter: provSearch.active = false without
  // restoreCatalog; esc and empty-query enter DO restore the catalog).
  bool search_results_kept() const { return search_results_kept_; }
  // submit_search runs the current mode's search (Go Enter). For Catalog the
  // SearchFn fires (Radio Browser SearchStations); for YouTube/SoundCloud the
  // query is prefixed ytsearch10:/scsearch10: and resolved via NetResolveFn.
  // Returns the error string on failure ("" on success).
  std::string submit_search();

  // Net-search results (YouTube/SoundCloud).
  const std::vector<playlist::Track>& net_results() const { return net_results_; }
  int  net_cursor() const { return net_cursor_; }
  int  net_scroll() const { return net_scroll_; }
  void net_cursor_up();    // wrap
  void net_cursor_down();  // wrap
  // net_select_cursor — Go handleNetSearchResultsKey enter: on_select on the
  // result's path (url), then close_search() (back to the catalog list).
  void net_select_cursor();
  bool net_results_active() const { return !net_results_.empty(); }

  // handle_key dispatches a Bubbletea-style key name; returns true if the
  // browse screen consumed it. Search input text is fed via set_search_query
  // (host text-editor keys handled there); Enter/Esc are handled here.
  bool handle_key(std::string_view key);

  // --- Rendering data (Go renderProviderList) -----------------------------
  // row_label renders "> label" / "  label" with a "▸"/">" cursor prefix
  // (bootamp uses "> " like Go cursorLine); plain text, no colors.
  std::string row_label(int i, int panel_width) const;
  // header_label renders "Browse  pos/total" (Go sepHeaderN on "Browse").
  std::string header_label() const;
  // net_row_label renders "> DisplayName" / "  DisplayName" for the net-search
  // results list (Go renderNetSearchBody; DisplayName = "Artist - Title").
  std::string net_row_label(int i, int panel_width) const;
  // net_header_label renders "Online Results  pos/total" (Go sepHeaderN).
  std::string net_header_label() const;
  // search_prompt renders the input line (Go promptHeader "/ query" style).
  std::string search_prompt(int panel_width) const;

  // kCatalogBatchSize — entries fetched per lazy page (Go catalogBatchSize).
  inline static constexpr int kCatalogBatchSize = 100;
  // kCatalogNearBottom — cursor distance from the list end that triggers a
  // page load (Go maybeLoadCatalogBatch: cursor >= len-10).
  inline static constexpr int kCatalogNearBottom = 10;

private:
  void normalize();
  void net_normalize();

  RefreshFn      refresh_fn_;
  LoadPageFn     load_page_fn_;
  SearchFn       search_catalog_fn_;
  NetResolveFn   net_resolve_fn_;
  PrefixFn       id_prefix_fn_;
  FavoritableFn  is_favoritable_fn_;
  Actions        actions_;

  std::vector<playlist::PlaylistInfo> lists_;
  std::string                         error_;
  bool                                has_sections_ = false;

  int cursor_       = 0;
  int scroll_       = 0;
  int visible_rows_ = 0;

  // Catalog fetch state: UI-thread-owned except catalog_result_ — the worker
  // thread publishes an immutable result there (std::atomic<std::shared_ptr>,
  // C++20 atomic snapshot) and pump_catalog_result() consumes it on the UI
  // thread. catalog_loading_ is atomic so the render path can read it without
  // a data race; it also provides the single-flight guard.
  std::atomic<bool> catalog_loading_{false};
  bool              catalog_done_   = false;
  int               catalog_offset_ = 0;
  struct CatalogFetchResult {
    int         added = 0;  // entries appended by the page (valid on success)
    std::string error;      // fetch failure message ("" on success)
  };
  std::atomic<std::shared_ptr<const CatalogFetchResult>> catalog_result_{nullptr};
  std::jthread fetch_thread_;  // in-flight fetch (request_stop + join in dtor)

  SearchMode      search_mode_    = SearchMode::None;
  std::string     search_query_;
  bool            search_loading_ = false;
  std::string     search_error_;
  bool            search_results_kept_ = false;

  std::vector<playlist::Track> net_results_;
  int                          net_cursor_ = 0;
  int                          net_scroll_ = 0;
};

#if BOOTAMP_HAS_FTXUI
std::shared_ptr<ftxui::ComponentBase> make_browse_component(BrowseModel& model);
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
