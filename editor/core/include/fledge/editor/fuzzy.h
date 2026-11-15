#pragma once
// Fuzzy matching for the command palette (subsequence match with bonuses for
// word starts, consecutive runs and prefix matches). Case-insensitive.
#include <string_view>
#include <vector>

namespace fledge {

// Returns -1 when `pattern` is not a subsequence of `text`; otherwise a score
// (higher is better). An empty pattern matches everything with score 0.
int fuzzyScore(std::string_view pattern, std::string_view text);

// Same, also returning the matched character positions (for highlighting).
int fuzzyMatch(std::string_view pattern, std::string_view text, std::vector<int>* positions);

} // namespace fledge
