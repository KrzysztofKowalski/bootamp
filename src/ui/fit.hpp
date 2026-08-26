// ui/fit.hpp — frame fitting helpers (port of cliamp/ui/fit.go).
//
// FitRect clips text to a terminal rectangle without splitting ANSI escapes or
// wide characters. Unlike cliamp's Go version (which used charmbracelet/x/ansi),
// bootamp operates on the CellGrid (already-rune-decomposed), so fit here is a
// thin column/row clamp used by the FTXUI blit path.
#pragma once

#include "ui/cell.hpp"

#include <string>
#include <string_view>

namespace bootamp::ui {

// fit_grid clips `src` into `dst`, keeping the top-left `dst.rows()×dst.cols()`
// rectangle. Out-of-bounds src cells are dropped; dst is NOT padded (cliamp
// FitRect intentionally does not pad rows so callers can compose compact layouts).
void fit_grid(const CellGrid& src, CellGrid& dst);

// clip_text clips a single-line string to `width` display columns, mirroring
// cliamp FitRect's ansi.Truncate semantics: wide runes count 2 columns and are
// never split (dropped entirely when they do not fit), combining marks count 0,
// and ANSI escape sequences (CSI/OSC) pass through verbatim without counting.
// Used for status-line truncation.
std::string clip_text(std::string_view text, int width);

// utf_width returns the display width of one codepoint: 2 for East Asian
// wide/fullwidth runes, 0 for combining/control/format runes, else 1.
int utf_width(char32_t cp) noexcept;

}  // namespace bootamp::ui