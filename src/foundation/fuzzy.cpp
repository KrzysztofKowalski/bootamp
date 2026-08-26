// foundation/fuzzy.cpp — case-insensitive fuzzy subsequence matching.
//
// Faithful port of cliamp/internal/fuzzy/fuzzy.go. A greedy subsequence scan
// with positional bonuses (first char / word boundary / consecutive run)
// ranks in-memory search results. Uses ASCII-only toLower (per project
// convention); UTF-8 bytes outside A-Z/a-z are compared as-is, so non-ASCII
// subsequence queries like "café" still match "Le Café" byte-for-byte.
#include "foundation/fuzzy.hpp"

#include <string_view>

namespace bootamp::foundation {

namespace {

// ascii_to_lower folds A-Z to a-z; all other bytes are returned unchanged.
inline char ascii_to_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
}

// is_separator reports whether r delimits a word for boundary scoring.
// Mirrors cliamp fuzzy.isSeparator exactly.
inline bool is_separator(char r) {
  switch (r) {
    case ' ': case '-': case '_': case '/': case '\\':
    case '.': case ':': case '(': case ')': case '[': case ']':
      return true;
    default:
      return false;
  }
}

}  // namespace

std::pair<int, bool> fuzzy_match(std::string_view query, std::string_view target) {
  if (query.empty()) {
    return {0, true};
  }

  std::size_t qi = 0;
  char qlow = ascii_to_lower(static_cast<unsigned char>(query[0]));  // current query rune, folded once per advance
  std::size_t ti = 0;          // byte index within target
  long last_match = -2;        // byte index of the previous matched rune
  char prev = '\0';            // previous target byte, for boundary detection
  int score = 0;

  for (char r : target) {
    if (ascii_to_lower(static_cast<unsigned char>(r)) == qlow) {
      score += kFuzzyScoreMatch;
      if (ti == 0) {
        score += kFuzzyBonusFirstChar;
      } else if (is_separator(prev)) {
        score += kFuzzyBonusBoundary;
      }
      if (static_cast<long>(ti) == last_match + 1) {
        score += kFuzzyBonusConsecutive;
      }
      last_match = static_cast<long>(ti);
      ++qi;
      if (qi == query.size()) {
        return {score, true};
      }
      qlow = ascii_to_lower(static_cast<unsigned char>(query[qi]));
    }
    prev = r;
    ++ti;
  }
  return {0, false};
}

}  // namespace bootamp::foundation