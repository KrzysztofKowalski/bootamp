// ui/cell.cpp — CellGrid implementation (see ui/cell.hpp).
//
// The grid is the golden-test surface for the visualizer drivers: a
// deterministic {rune,color} canvas with no FTXUI dependency. resize() keeps
// the top-left overlap, fill() clears, set() is bounds-checked, and
// dump_grid() is the deterministic text rendering used by golden tests.
#include "ui/cell.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace bootamp::ui {

namespace {

// Append one Unicode codepoint as UTF-8. Unpaired surrogates and values above
// U+10FFFF are replaced with U+FFFD so dump_grid always emits valid UTF-8.
void append_utf8(std::string& out, char32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp >= 0xD800 && cp <= 0xDFFF) {
    append_utf8(out, 0xFFFD);
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    append_utf8(out, 0xFFFD);
  }
}

}  // namespace

// resize keeps the top-left overlap of the current contents; newly exposed
// cells are default-constructed (space, default color). Negative sizes clamp
// to 0 (defensive; drivers clamp the column count upstream).
void CellGrid::resize(int rows, int cols) {
  rows = std::max(rows, 0);
  cols = std::max(cols, 0);
  if (rows == rows_ && cols == cols_) {
    return;
  }

  std::vector<Cell> next(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
  const int copy_rows = std::min(rows, rows_);
  const int copy_cols = std::min(cols, cols_);
  for (int r = 0; r < copy_rows; ++r) {
    std::copy_n(cells_.begin() + static_cast<std::size_t>(r) * cols_,
                static_cast<std::size_t>(copy_cols),
                next.begin() + static_cast<std::size_t>(r) * cols);
  }
  cells_ = std::move(next);
  rows_ = rows;
  cols_ = cols;
}

void CellGrid::fill(Cell c) {
  std::fill(cells_.begin(), cells_.end(), c);
}

void CellGrid::set(int r, int c, Cell cell) {
  if (r < 0 || c < 0 || r >= rows_ || c >= cols_) {
    return;  // out of range — drop
  }
  at(r, c) = cell;
}

std::string dump_grid(const CellGrid& g) {
  std::string out;
  out.reserve(static_cast<std::size_t>(g.rows()) * g.cols());
  for (int r = 0; r < g.rows(); ++r) {
    if (r > 0) {
      out.push_back('\n');
    }
    for (int c = 0; c < g.cols(); ++c) {
      append_utf8(out, g.at(r, c).rune);
    }
  }
  return out;
}

}  // namespace bootamp::ui
