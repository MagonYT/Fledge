#include "fledge/editor/fuzzy.h"

#include <cctype>

namespace fledge {

namespace {
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool wordStart(std::string_view t, size_t i) {
    if (i == 0) return true;
    char p = t[i - 1], c = t[i];
    if (p == ' ' || p == '_' || p == '-' || p == '.' || p == ':' || p == '/' || p == '>' || p == '(') return true;
    return std::islower(static_cast<unsigned char>(p)) && std::isupper(static_cast<unsigned char>(c));
}
} // namespace

int fuzzyMatch(std::string_view pattern, std::string_view text, std::vector<int>* positions) {
    if (positions) positions->clear();
    // skip spaces in the pattern ("tog grid" matches "Toggle Grid")
    size_t pi = 0, ti = 0;
    int score = 0, last = -2;
    while (pi < pattern.size() && pattern[pi] == ' ') ++pi;
    if (pi == pattern.size()) return 0;
    while (pi < pattern.size()) {
        char pc = lower(pattern[pi]);
        if (pc == ' ') {
            ++pi;
            continue;
        }
        // Prefer an upcoming word-start occurrence of pc over the first plain one,
        // unless the plain one continues a run.
        size_t found = std::string_view::npos, firstPlain = std::string_view::npos;
        for (size_t k = ti; k < text.size(); ++k) {
            if (lower(text[k]) != pc) continue;
            if (firstPlain == std::string_view::npos) firstPlain = k;
            if (static_cast<int>(k) == last + 1) { found = k; break; }
            if (wordStart(text, k)) { found = k; break; }
        }
        if (found == std::string_view::npos) found = firstPlain;
        if (found == std::string_view::npos) return -1;
        score += 1;
        if (static_cast<int>(found) == last + 1) score += 6;
        if (wordStart(text, found)) score += 8;
        int gap = last < 0 ? static_cast<int>(found) : static_cast<int>(found) - last - 1;
        score -= gap > 10 ? 10 : gap;
        if (positions) positions->push_back(static_cast<int>(found));
        last = static_cast<int>(found);
        ti = found + 1;
        ++pi;
    }
    // shorter texts win ties
    score -= static_cast<int>(text.size() / 16);
    return score < 0 ? 0 : score;
}

int fuzzyScore(std::string_view pattern, std::string_view text) { return fuzzyMatch(pattern, text, nullptr); }

} // namespace fledge
