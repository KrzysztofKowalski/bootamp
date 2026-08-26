// ui/cell.hpp — CellGrid: the 2D {rune,color} grid (port of Bubbletea canvas).
//
// Per the plan: drivers fill a CellGrid {rune,color}[rows×cols] (a near-1:1
// port of Bubbletea's canvas). It is the golden-test surface — deterministic vs
// Go, no FTXUI in the loop. The FTXUI backend blits a CellGrid to an
// ftxui::Canvas at render time. Cell holds one unicode codepoint + an ANSI
// color index (palette slot, theme-resolved at blit time).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bootamp::ui {

// Color is a palette slot (0 = default foreground). The visualizer maps the
// 3-tier spectrum (low/mid/high) and the theme palette to these slots; the
// FTXUI blit resolves them to ftxui::Color. Keeping an index here (not an
// ftxui::Color) keeps the grid backend-agnostic and golden-testable.
using Color = std::uint8_t;
inline constexpr Color kColorDefault = 0;

// Cell is one grid position. rune is a unicode codepoint (UTF-32); drivers
// that emit multi-cell braille/block glyphs place one Cell per terminal cell.
struct Cell {
  char32_t rune  = U' ';
  Color    color = kColorDefault;
};

// CellGrid is a rows×cols grid of Cells. Drivers write into it; the FTXUI
// blit reads it. resize() keeps the contents where possible. fill() clears.
class CellGrid {
public:
  CellGrid() = default;
  CellGrid(int rows, int cols) { resize(rows, cols); }

  void resize(int rows, int cols);
  void fill(Cell c = {});

  int  rows() const noexcept { return rows_; }
  int  cols() const noexcept { return cols_; }

  // at() — unchecked cell access (drivers stay fast; bounds are the driver's
  // responsibility and the visualizer clamps the column count).
  Cell&       at(int r, int c)       { return cells_[static_cast<std::size_t>(r) * cols_ + c]; }
  const Cell& at(int r, int c) const { return cells_[static_cast<std::size_t>(r) * cols_ + c]; }

  // set() — bounds-checked set; out-of-range writes are dropped.
  void set(int r, int c, Cell cell);

  // raw_cells exposes the flat storage for golden-file dump/compare.
  const std::vector<Cell>& cells() const noexcept { return cells_; }

private:
  int                rows_ = 0;
  int                cols_ = 0;
  std::vector<Cell>  cells_;
};

// dump_grid returns a deterministic text dump of the grid (one line per row,
// runes concatenated). Used by golden tests; color is omitted (the golden
// captures runes; color is asserted separately if needed).
std::string dump_grid(const CellGrid& g);

}  // namespace bootamp::ui