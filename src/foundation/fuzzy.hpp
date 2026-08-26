// foundation/fuzzy.hpp — case-insensitive fuzzy subsequence matching.
//
// Port of cliamp/internal/fuzzy/fuzzy.go. A greedy subsequence scan with
// positional bonuses (first char / word boundary / consecutive run) ranks
// in-memory search results (playlist search, file browser filter) without an
// external dependency. Allocation-light, called on every keystroke.
#pragma once

#include <string_view>
#include <utility>

namespace bootamp::foundation {

// Scoring weights (cliamp fuzzy.go): in order of preference, a candidate ranks
// higher when the match starts at the very beginning, follows a word boundary,
// or forms a run of consecutive matched characters.
inline constexpr int kFuzzyScoreMatch       = 1;
inline constexpr int kFuzzyBonusFirstChar   = 6;
inline constexpr int kFuzzyBonusBoundary     = 4;
inline constexpr int kFuzzyBonusConsecutive  = 4;

// match reports whether `query` is a case-insensitive subsequence of `target`
// and, if so, returns a relevance score (higher = better). An empty query
// matches every target with score 0. `ok` is false when query is not a
// subsequence. Exact 1:1 port of cliamp's fuzzy.Match.
std::pair<int, bool> fuzzy_match(std::string_view query, std::string_view target);

}  // namespace bootamp::foundation