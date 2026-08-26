// ui/vis_drivers/braillegrid.hpp — BrailleGrid: 4×2 dot-per-cell rasteriser
// (port of cliamp/ui/vis_braillegrid.go).
//
// Shared by visualizers that draw to a fine subgrid (Go: mirror, sand,
// speaker, lightning, crack, quake, geyser, strings). Each cell stores a tier
// (1..3 = low/mid/high colour, 0 = empty); render() composes one Braille glyph
// per terminal cell and maps the tier to the spectrum palette slot, exactly
// one Cell per terminal cell (the Go canvas composes one rune per character
// cell the same way). Header-only helper + factory stub: the registry declares
// make_braillegrid_driver() but cliamp has no BrailleGrid *mode* (vis_braillegrid.go
// only defines the rasteriser), so the factory returns nullptr — the mode
// table never calls it.
#pragma once

#include "ui/cell.hpp"
#include "ui/styles.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bootamp::ui::vis_drivers {

// brailleBit maps (row, col) in the 4×2 Braille dot grid to its bit value
// (Go brailleBit). Or-ed into U+2800 to form the final codepoint.
inline constexpr std::array<std::array<std::uint32_t, 2>, 4> kBrailleBit = {{
    {{0x01, 0x08}},  // row 0
    {{0x02, 0x10}},  // row 1
    {{0x04, 0x20}},  // row 2
    {{0x40, 0x80}},  // row 3
}};

// BrailleGrid is the fine dot rasteriser (Go brailleGrid). dotRows = rows*4,
// dotCols = cols*2; cell(x, y) holds the color tier (0 empty, 1..3).
class BrailleGrid {
public:
  // ensure (Go): reuse the current backing store if the dot dims match,
  // clearing it; otherwise (re)allocate.
  void ensure(int rows, int cols) {
    if (rows == dot_rows_ && cols == dot_cols_ &&
        cells_.size() == static_cast<std::size_t>(rows) * cols) {
      clear();
      return;
    }
    cells_.assign(static_cast<std::size_t>(rows) * cols, 0);
    dot_rows_ = rows;
    dot_cols_ = cols;
  }

  // clear (Go): zero every dot cell.
  void clear() {
    for (std::int8_t& c : cells_) {
      c = 0;
    }
  }

  // set (Go): bounds-checked max-update of the tier at dot (x, y).
  void set(int x, int y, int tier) {
    if (x < 0 || x >= dot_cols_ || y < 0 || y >= dot_rows_) {
      return;
    }
    std::int8_t& c = cells_[static_cast<std::size_t>(y) * dot_cols_ + x];
    if (tier > c) {
      c = static_cast<std::int8_t>(tier);
    }
  }

  // render packs the dot grid into `rows` terminal lines of `cols` cells,
  // emitting one Braille glyph + tier color per cell (Go render). If the dot
  // grid is smaller than rows*4 x cols*2 the output is left untouched (Go
  // returns blank lines; the framework pre-fills the grid with spaces).
  void render(int rows, int cols, CellGrid& out) const {
    if (dot_rows_ < rows * 4 || dot_cols_ < cols * 2) {
      return;  // Go: strings.Repeat("\n", max(0, rows-1))
    }
    for (int row = 0; row < rows; ++row) {
      for (int col = 0; col < cols; ++col) {
        std::uint32_t braille  = 0x2800;
        int           cell_tag = -1;
        for (int dr = 0; dr < 4; ++dr) {
          for (int dc = 0; dc < 2; ++dc) {
            const int y = row * 4 + dr;
            const int x = col * 2 + dc;
            const std::int8_t t =
                cells_[static_cast<std::size_t>(y) * dot_cols_ + x];
            if (t == 0) {
              continue;
            }
            braille |= kBrailleBit[static_cast<std::size_t>(dr)]
                                   [static_cast<std::size_t>(dc)];
            if (static_cast<int>(t) - 1 > cell_tag) {
              cell_tag = static_cast<int>(t) - 1;
            }
          }
        }
        if (cell_tag < 0) {
          cell_tag = 0;  // empty cells still take the low tier color (Go)
        }
        const Color color = (cell_tag == 2) ? kColorSpecHigh
                            : (cell_tag == 1) ? kColorSpecMid
                                              : kColorSpecLow;
        out.set(row, col, Cell{static_cast<char32_t>(braille), color});
      }
    }
  }

  // Accessors for the owning drivers and tests.
  bool   empty() const { return cells_.empty(); }
  int    dot_rows() const { return dot_rows_; }
  int    dot_cols() const { return dot_cols_; }
  std::int8_t tier_at(int x, int y) const {
    if (x < 0 || x >= dot_cols_ || y < 0 || y >= dot_rows_) {
      return 0;
    }
    return cells_[static_cast<std::size_t>(y) * dot_cols_ + x];
  }

private:
  std::vector<std::int8_t> cells_;
  int                      dot_rows_ = 0;
  int                      dot_cols_ = 0;
};

// rng64 advances a 64-bit LCG and returns a [0,1) double (Go rng64).
inline double rng64(std::uint64_t& state) {
  state = state * 6364136223846793005ULL + 1442695040888963407ULL;
  return static_cast<double>((state >> 33) % 1000) / 1000.0;
}

}  // namespace bootamp::ui::vis_drivers
