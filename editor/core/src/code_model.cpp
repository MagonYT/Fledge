#include "fledge/editor/code_model.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <unordered_set>

namespace fledge::code {

namespace {
bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
int leadingSpaces(const std::string& s) {
    int n = 0;
    for (char c : s) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}
std::string indentOf(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(0, i);
}
char closerFor(char c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
    }
    return 0;
}
constexpr size_t kMaxUndo = 1000;
} // namespace

// ============================================================================== TextBuffer ===
void TextBuffer::setText(std::string_view text) {
    m_lines.clear();
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            m_lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    m_lines.push_back(std::move(cur));
    cursors = {Cursor{}};
    m_undo.clear();
    m_redo.clear();
    ++m_version;
    m_savedVersion = m_version;
}

std::string TextBuffer::text() const {
    std::string s;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (i) s += '\n';
        s += m_lines[i];
    }
    return s;
}

Pos TextBuffer::clamp(Pos p) const {
    p.line = std::clamp(p.line, 0, lineCount() - 1);
    p.col = std::clamp(p.col, 0, static_cast<int>(line(p.line).size()));
    return p;
}

std::string TextBuffer::textIn(Pos a, Pos b) const {
    a = clamp(a);
    b = clamp(b);
    if (b < a) std::swap(a, b);
    if (a.line == b.line) return line(a.line).substr(static_cast<size_t>(a.col), static_cast<size_t>(b.col - a.col));
    std::string s = line(a.line).substr(static_cast<size_t>(a.col));
    for (int l = a.line + 1; l < b.line; ++l) s += "\n" + line(l);
    s += "\n" + line(b.line).substr(0, static_cast<size_t>(b.col));
    return s;
}

void TextBuffer::setCursor(Pos p, bool select) {
    p = clamp(p);
    Cursor c = cursors.empty() ? Cursor{} : cursors[0];
    c.pos = p;
    if (!select) c.anchor = p;
    cursors = {c};
}

void TextBuffer::addCursor(Pos p) {
    p = clamp(p);
    cursors.push_back({p, p});
    normalizeCursors();
}

void TextBuffer::normalizeCursors() {
    for (auto& c : cursors) {
        c.pos = clamp(c.pos);
        c.anchor = clamp(c.anchor);
    }
    std::sort(cursors.begin(), cursors.end(), [](const Cursor& a, const Cursor& b) { return a.selStart() < b.selStart(); });
    std::vector<Cursor> out;
    for (auto& c : cursors) {
        if (!out.empty() && c.selStart() <= out.back().selEnd() && (c.hasSelection() || out.back().hasSelection() || c.pos == out.back().pos)) {
            Cursor& o = out.back();
            Pos s = std::min(o.selStart(), c.selStart()), e = std::max(o.selEnd(), c.selEnd());
            o.anchor = s;
            o.pos = e;
            continue;
        }
        out.push_back(c);
    }
    cursors = out.empty() ? std::vector<Cursor>{Cursor{}} : out;
}

std::string TextBuffer::selectedText() const { return cursors.empty() ? std::string() : textIn(cursors[0].selStart(), cursors[0].selEnd()); }

void TextBuffer::selectAll() {
    cursors = {Cursor{{lineCount() - 1, static_cast<int>(m_lines.back().size())}, {0, 0}}};
}

bool TextBuffer::addNextOccurrence() {
    Cursor& last = cursors.back();
    if (!last.hasSelection()) {
        Pos s, e;
        if (wordAt(*this, last.pos, &s, &e).empty()) return false;
        last.anchor = s;
        last.pos = e;
        return true;
    }
    std::string needle = textIn(last.selStart(), last.selEnd());
    if (needle.empty() || needle.find('\n') != std::string::npos) return false;
    Pos from = last.selEnd();
    for (int pass = 0; pass < 2; ++pass) {
        for (int l = pass == 0 ? from.line : 0; l < lineCount(); ++l) {
            size_t start = (pass == 0 && l == from.line) ? static_cast<size_t>(from.col) : 0;
            size_t f = line(l).find(needle, start);
            if (f == std::string::npos) continue;
            Pos s{l, static_cast<int>(f)}, e{l, static_cast<int>(f + needle.size())};
            for (auto& c : cursors)
                if (c.selStart() == s) return false;  // wrapped around to an existing one
            cursors.push_back({e, s});
            return true;
        }
    }
    return false;
}

void TextBuffer::pushUndo(bool mergeTyping) {
    if (mergeTyping && m_lastWasTyping && !m_undo.empty()) return;
    m_undo.push_back({m_lines, cursors, m_version});
    if (m_undo.size() > kMaxUndo) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_lastWasTyping = mergeTyping;
}

bool TextBuffer::undo() {
    if (m_undo.empty()) return false;
    m_redo.push_back({m_lines, cursors, m_version});
    Snapshot s = std::move(m_undo.back());
    m_undo.pop_back();
    m_lines = std::move(s.lines);
    cursors = std::move(s.cursors);
    m_version = s.version == m_savedVersion ? s.version : m_version + 1;
    if (s.version == m_savedVersion) m_version = m_savedVersion;
    m_lastWasTyping = false;
    return true;
}

bool TextBuffer::redo() {
    if (m_redo.empty()) return false;
    m_undo.push_back({m_lines, cursors, m_version});
    Snapshot s = std::move(m_redo.back());
    m_redo.pop_back();
    m_lines = std::move(s.lines);
    cursors = std::move(s.cursors);
    ++m_version;
    m_lastWasTyping = false;
    return true;
}

Pos TextBuffer::erase(Pos a, Pos b) {
    a = clamp(a);
    b = clamp(b);
    if (b < a) std::swap(a, b);
    if (a == b) return a;
    std::string tail = line(b.line).substr(static_cast<size_t>(b.col));
    m_lines[static_cast<size_t>(a.line)] = line(a.line).substr(0, static_cast<size_t>(a.col)) + tail;
    m_lines.erase(m_lines.begin() + a.line + 1, m_lines.begin() + b.line + 1);
    ++m_version;
    return a;
}

Pos TextBuffer::insertAt(Pos p, std::string_view s) {
    p = clamp(p);
    std::string head = line(p.line).substr(0, static_cast<size_t>(p.col)), tail = line(p.line).substr(static_cast<size_t>(p.col));
    std::vector<std::string> parts{""};
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') parts.emplace_back();
        else parts.back() += c;
    }
    Pos end;
    if (parts.size() == 1) {
        m_lines[static_cast<size_t>(p.line)] = head + parts[0] + tail;
        end = {p.line, p.col + static_cast<int>(parts[0].size())};
    } else {
        m_lines[static_cast<size_t>(p.line)] = head + parts[0];
        std::vector<std::string> mid(parts.begin() + 1, parts.end());
        end = {p.line + static_cast<int>(mid.size()), static_cast<int>(mid.back().size())};
        mid.back() += tail;
        m_lines.insert(m_lines.begin() + p.line + 1, mid.begin(), mid.end());
    }
    ++m_version;
    return end;
}

void TextBuffer::shiftCursorsAfterEdit(size_t index, Pos editStart, Pos oldEnd, Pos newEnd) {
    auto shift = [&](Pos& p) {
        if (p < editStart) return;
        if (p < oldEnd) {
            p = newEnd;
            return;
        }
        if (p.line == oldEnd.line) {
            p.col = newEnd.col + (p.col - oldEnd.col);
            p.line = newEnd.line;
        } else {
            p.line += newEnd.line - oldEnd.line;
        }
    };
    for (size_t i = index + 1; i < cursors.size(); ++i) {
        shift(cursors[i].pos);
        shift(cursors[i].anchor);
    }
}

void TextBuffer::insert(std::string_view s) {
    normalizeCursors();
    pushUndo(s.size() == 1 && s[0] != '\n' && cursors.size() == 1 && !cursors[0].hasSelection());
    for (size_t i = cursors.size(); i-- > 0;) {
        Cursor& c = cursors[i];
        Pos a = c.selStart(), oldEnd = c.selEnd();
        Pos p = erase(a, oldEnd);
        Pos e = insertAt(p, s);
        c.pos = c.anchor = e;
        shiftCursorsAfterEdit(i, a, oldEnd, e);
    }
}

void TextBuffer::replaceRange(Pos a, Pos b, std::string_view s) {
    pushUndo();
    Pos p = erase(a, b);
    Pos e = insertAt(p, s);
    setCursor(e);
}

void TextBuffer::typeChar(char c) {
    normalizeCursors();
    // skip over an auto-inserted closer
    if (autoClose && (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && cursors.size() == 1 && !cursors[0].hasSelection()) {
        Pos p = cursors[0].pos;
        if (p.col < static_cast<int>(line(p.line).size()) && line(p.line)[static_cast<size_t>(p.col)] == c) {
            cursors[0].pos.col++;
            cursors[0].anchor = cursors[0].pos;
            return;
        }
    }
    char closer = autoClose ? closerFor(c) : 0;
    if (closer && cursors.size() == 1) {
        Cursor cur = cursors[0];
        if (cur.hasSelection()) {  // wrap the selection
            std::string sel = selectedText();
            insert(std::string(1, c) + sel + closer);
            return;
        }
        Pos p = cur.pos;
        bool before = p.col >= static_cast<int>(line(p.line).size()) || !isWordChar(line(p.line)[static_cast<size_t>(p.col)]);
        bool quoteInWord = (c == '"' || c == '\'') && p.col > 0 && isWordChar(line(p.line)[static_cast<size_t>(p.col - 1)]);
        if (before && !quoteInWord) {
            insert(std::string(1, c) + closer);
            cursors[0].pos.col--;
            cursors[0].anchor = cursors[0].pos;
            return;
        }
    }
    insert(std::string(1, c));
    // outdent a lone closing brace
    if (c == '}' && cursors.size() == 1) {
        Pos p = cursors[0].pos;
        std::string l = line(p.line);
        std::string before = l.substr(0, static_cast<size_t>(p.col - 1));
        if (before.find_first_not_of(" \t") == std::string::npos && before.size() >= static_cast<size_t>(tabSize)) {
            m_lines[static_cast<size_t>(p.line)] = l.substr(static_cast<size_t>(tabSize));
            cursors[0].pos.col -= tabSize;
            cursors[0].anchor = cursors[0].pos;
        }
    }
}

void TextBuffer::newline() {
    normalizeCursors();
    pushUndo();
    for (size_t i = cursors.size(); i-- > 0;) {
        Cursor& c = cursors[i];
        Pos a = c.selStart(), oldEnd = c.selEnd();
        Pos p = erase(a, oldEnd);
        std::string ind = indentOf(line(p.line));
        std::string before = line(p.line).substr(0, static_cast<size_t>(p.col));
        std::string after = line(p.line).substr(static_cast<size_t>(p.col));
        size_t lastNs = before.find_last_not_of(" \t");
        bool opener = lastNs != std::string::npos && (before[lastNs] == '{' || before[lastNs] == '(' || before[lastNs] == '[' || before[lastNs] == ':');
        bool closerNext = !after.empty() && (after[0] == '}' || after[0] == ')' || after[0] == ']');
        std::string extra = opener ? std::string(static_cast<size_t>(tabSize), ' ') : "";
        Pos e;
        if (opener && closerNext) {
            e = insertAt(p, "\n" + ind + extra + "\n" + ind);
            e = {p.line + 1, static_cast<int>(ind.size() + extra.size())};
        } else {
            e = insertAt(p, "\n" + ind + extra);
        }
        c.pos = c.anchor = e;
        shiftCursorsAfterEdit(i, a, oldEnd, e);
    }
}

void TextBuffer::backspace() {
    normalizeCursors();
    pushUndo();
    for (size_t i = cursors.size(); i-- > 0;) {
        Cursor& c = cursors[i];
        Pos a = c.selStart(), b = c.selEnd();
        if (!c.hasSelection()) {
            if (a.col > 0) {
                const std::string& l = line(a.line);
                // remove an auto-closed pair "()" at once; spaces back to the previous tab stop
                if (a.col < static_cast<int>(l.size()) && closerFor(l[static_cast<size_t>(a.col - 1)]) == l[static_cast<size_t>(a.col)] &&
                    l[static_cast<size_t>(a.col - 1)] != l[static_cast<size_t>(a.col)])
                    b.col++;
                std::string before = l.substr(0, static_cast<size_t>(a.col));
                if (before.find_first_not_of(' ') == std::string::npos && a.col % tabSize == 0 && a.col >= tabSize) a.col -= tabSize;
                else a.col--;
            } else if (a.line > 0) {
                a = {a.line - 1, static_cast<int>(line(a.line - 1).size())};
            }
        }
        Pos e = erase(a, b);
        c.pos = c.anchor = e;
        shiftCursorsAfterEdit(i, a, b, e);
    }
}

void TextBuffer::del() {
    normalizeCursors();
    pushUndo();
    for (size_t i = cursors.size(); i-- > 0;) {
        Cursor& c = cursors[i];
        Pos a = c.selStart(), b = c.selEnd();
        if (!c.hasSelection()) {
            if (b.col < static_cast<int>(line(b.line).size())) b.col++;
            else if (b.line + 1 < lineCount()) b = {b.line + 1, 0};
        }
        Pos e = erase(a, b);
        c.pos = c.anchor = e;
        shiftCursorsAfterEdit(i, a, b, e);
    }
}

void TextBuffer::indent() {
    normalizeCursors();
    bool block = false;
    for (auto& c : cursors) block = block || (c.hasSelection() && c.selStart().line != c.selEnd().line);
    if (!block) {
        // insert spaces to the next tab stop
        std::string sp(static_cast<size_t>(tabSize - cursors[0].pos.col % tabSize), ' ');
        insert(sp);
        return;
    }
    pushUndo();
    std::string sp(static_cast<size_t>(tabSize), ' ');
    std::unordered_set<int> done;
    for (auto& c : cursors)
        for (int l = c.selStart().line; l <= c.selEnd().line; ++l) {
            if (l == c.selEnd().line && c.selEnd().col == 0 && l != c.selStart().line) continue;
            if (!done.insert(l).second || line(l).empty()) continue;
            m_lines[static_cast<size_t>(l)] = sp + line(l);
        }
    for (auto& c : cursors) {
        if (done.count(c.pos.line) && c.pos.col > 0) c.pos.col += tabSize;
        if (done.count(c.anchor.line) && c.anchor.col > 0) c.anchor.col += tabSize;
    }
    ++m_version;
}

void TextBuffer::outdent() {
    normalizeCursors();
    pushUndo();
    std::unordered_set<int> done;
    for (auto& c : cursors)
        for (int l = c.selStart().line; l <= c.selEnd().line; ++l) {
            if (!done.insert(l).second) continue;
            std::string& s = m_lines[static_cast<size_t>(l)];
            int n = 0;
            while (n < tabSize && n < static_cast<int>(s.size()) && s[static_cast<size_t>(n)] == ' ') ++n;
            s.erase(0, static_cast<size_t>(n));
            for (auto& cc : cursors) {
                if (cc.pos.line == l) cc.pos.col = std::max(0, cc.pos.col - n);
                if (cc.anchor.line == l) cc.anchor.col = std::max(0, cc.anchor.col - n);
            }
        }
    ++m_version;
}

void TextBuffer::toggleComment(std::string_view prefix) {
    normalizeCursors();
    pushUndo();
    std::vector<int> ls;
    for (auto& c : cursors)
        for (int l = c.selStart().line; l <= c.selEnd().line; ++l)
            if (std::find(ls.begin(), ls.end(), l) == ls.end() && !(l == c.selEnd().line && c.selEnd().col == 0 && l != c.selStart().line)) ls.push_back(l);
    bool allCommented = true;
    int minIndent = 1 << 30;
    for (int l : ls) {
        const std::string& s = line(l);
        size_t f = s.find_first_not_of(" \t");
        if (f == std::string::npos) continue;
        minIndent = std::min(minIndent, static_cast<int>(f));
        if (s.compare(f, prefix.size(), prefix) != 0) allCommented = false;
    }
    if (minIndent == (1 << 30)) return;
    std::string pre = std::string(prefix) + " ";
    for (int l : ls) {
        std::string& s = m_lines[static_cast<size_t>(l)];
        size_t f = s.find_first_not_of(" \t");
        if (f == std::string::npos) continue;
        if (allCommented) {
            size_t n = prefix.size() + (s.size() > f + prefix.size() && s[f + prefix.size()] == ' ' ? 1 : 0);
            s.erase(f, n);
        } else {
            s.insert(static_cast<size_t>(minIndent), pre);
        }
    }
    for (auto& c : cursors) {
        c.pos = clamp(c.pos);
        c.anchor = clamp(c.anchor);
    }
    ++m_version;
}

void TextBuffer::moveLines(int delta) {
    normalizeCursors();
    Cursor& c = cursors[0];
    int a = c.selStart().line, b = c.selEnd().line;
    if (c.hasSelection() && c.selEnd().col == 0 && b > a) --b;
    if ((delta < 0 && a == 0) || (delta > 0 && b + 1 >= lineCount())) return;
    pushUndo();
    if (delta < 0) std::rotate(m_lines.begin() + a - 1, m_lines.begin() + a, m_lines.begin() + b + 1);
    else std::rotate(m_lines.begin() + a, m_lines.begin() + b + 1, m_lines.begin() + b + 2);
    c.pos.line += delta;
    c.anchor.line += delta;
    cursors = {c};
    ++m_version;
}

void TextBuffer::moveCursors(int dLine, int dCol, bool select, bool word) {
    for (auto& c : cursors) {
        Pos p = c.pos;
        if (!select && c.hasSelection() && dLine == 0) {
            p = dCol < 0 ? c.selStart() : c.selEnd();
        } else if (dLine != 0) {
            p.line = std::clamp(p.line + dLine, 0, lineCount() - 1);
            p.col = std::min(p.col, static_cast<int>(line(p.line).size()));
        } else if (word) {
            const std::string& l = line(p.line);
            if (dCol < 0) {
                if (p.col == 0 && p.line > 0) p = {p.line - 1, static_cast<int>(line(p.line - 1).size())};
                else {
                    while (p.col > 0 && !isWordChar(l[static_cast<size_t>(p.col - 1)])) --p.col;
                    while (p.col > 0 && isWordChar(l[static_cast<size_t>(p.col - 1)])) --p.col;
                }
            } else {
                if (p.col >= static_cast<int>(l.size()) && p.line + 1 < lineCount()) p = {p.line + 1, 0};
                else {
                    while (p.col < static_cast<int>(l.size()) && !isWordChar(l[static_cast<size_t>(p.col)])) ++p.col;
                    while (p.col < static_cast<int>(l.size()) && isWordChar(l[static_cast<size_t>(p.col)])) ++p.col;
                }
            }
        } else {
            p.col += dCol;
            if (p.col < 0) {
                if (p.line > 0) p = {p.line - 1, static_cast<int>(line(p.line - 1).size())};
                else p.col = 0;
            } else if (p.col > static_cast<int>(line(p.line).size())) {
                if (p.line + 1 < lineCount()) p = {p.line + 1, 0};
                else p.col = static_cast<int>(line(p.line).size());
            }
        }
        c.pos = p;
        if (!select) c.anchor = p;
    }
    m_lastWasTyping = false;
    normalizeCursors();
}

void TextBuffer::home(bool select) {
    for (auto& c : cursors) {
        int first = static_cast<int>(indentOf(line(c.pos.line)).size());
        c.pos.col = c.pos.col == first ? 0 : first;
        if (!select) c.anchor = c.pos;
    }
}

void TextBuffer::end(bool select) {
    for (auto& c : cursors) {
        c.pos.col = static_cast<int>(line(c.pos.line).size());
        if (!select) c.anchor = c.pos;
    }
}

std::string wordAt(const TextBuffer& b, Pos p, Pos* start, Pos* end) {
    p = b.clamp(p);
    const std::string& l = b.line(p.line);
    int s = p.col, e = p.col;
    while (s > 0 && isWordChar(l[static_cast<size_t>(s - 1)])) --s;
    while (e < static_cast<int>(l.size()) && isWordChar(l[static_cast<size_t>(e)])) ++e;
    if (start) *start = {p.line, s};
    if (end) *end = {p.line, e};
    return l.substr(static_cast<size_t>(s), static_cast<size_t>(e - s));
}

// ============================================================================== tokenizer ===
Lang langForPath(std::string_view path) {
    std::string p = toLower(std::string(path));
    auto ends = [&](const char* e) { return p.size() >= std::strlen(e) && p.compare(p.size() - std::strlen(e), std::string::npos, e) == 0; };
    if (ends(".cpp") || ends(".h") || ends(".hpp") || ends(".cc") || ends(".cxx") || ends(".inl") || ends(".c")) return Lang::Cpp;
    if (ends(".json") || ends(".fledge") || ends(".fscene") || ends(".fmat") || ends(".fmodel") || ends(".meta")) return Lang::Json;
    if (ends(".md")) return Lang::Markdown;
    if (ends(".glsl") || ends(".hlsl") || ends(".vert") || ends(".frag") || ends(".fx") || ends(".hlsli")) return Lang::Shader;
    if (ends("cmakelists.txt") || ends(".cmake")) return Lang::CMake;
    return Lang::Plain;
}

const char* langName(Lang l) {
    switch (l) {
        case Lang::Cpp: return "C++";
        case Lang::Json: return "JSON";
        case Lang::Markdown: return "Markdown";
        case Lang::Shader: return "GLSL/HLSL";
        case Lang::CMake: return "CMake";
        default: return "Plain Text";
    }
}

namespace {
const std::unordered_set<std::string_view>& cppKeywords() {
    static const std::unordered_set<std::string_view> k = {
        "alignas", "alignof", "auto", "break", "case", "catch", "class", "co_await", "co_return", "co_yield", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "decltype", "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "final", "for", "friend", "goto", "if", "inline", "mutable", "namespace", "new", "noexcept", "nullptr",
        "operator", "override", "private", "protected", "public", "register", "reinterpret_cast", "requires", "return", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "using", "virtual", "volatile", "while"};
    return k;
}
const std::unordered_set<std::string_view>& cppTypes() {
    static const std::unordered_set<std::string_view> k = {"void", "bool", "char", "short", "int", "long", "float", "double", "unsigned", "signed",
                                                           "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
                                                           "uint64_t", "std", "string", "vector", "Vec2", "Vec3", "Vec4", "Quat", "Mat4"};
    return k;
}
const std::unordered_set<std::string_view>& shaderWords() {
    static const std::unordered_set<std::string_view> k = {
        "in", "out", "inout", "uniform", "layout", "cbuffer", "register", "struct", "return", "if", "else", "for", "while", "discard", "const",
        "static", "void", "float", "float2", "float3", "float4", "float3x3", "float4x4", "int", "int2", "uint", "bool", "vec2", "vec3", "vec4",
        "mat3", "mat4", "sampler2D", "samplerCube", "Texture2D", "TextureCube", "SamplerState", "Texture2DArray", "half", "precision"};
    return k;
}
const std::unordered_set<std::string_view>& cmakeWords() {
    static const std::unordered_set<std::string_view> k = {"if", "else", "elseif", "endif", "foreach", "endforeach", "function", "endfunction",
                                                           "macro", "endmacro", "set", "add_library", "add_executable", "target_link_libraries",
                                                           "target_include_directories", "project", "cmake_minimum_required", "option",
                                                           "include", "find_package", "message", "list", "string", "file", "install"};
    return k;
}

void cLike(Lang lang, std::string_view s, int& state, std::vector<Token>& out) {
    int i = 0, n = static_cast<int>(s.size());
    auto push = [&](int st, int len, Tok k) { out.push_back({st, len, k}); };
    if (state == 1) {  // inside /* */
        size_t e = s.find("*/");
        if (e == std::string_view::npos) {
            push(0, n, Tok::Comment);
            return;
        }
        push(0, static_cast<int>(e) + 2, Tok::Comment);
        i = static_cast<int>(e) + 2;
        state = 0;
    }
    int firstNs = static_cast<int>(s.find_first_not_of(" \t"));
    if (firstNs >= 0 && firstNs < n && s[static_cast<size_t>(firstNs)] == '#' && lang != Lang::CMake) {
        push(firstNs, n - firstNs, Tok::Preprocessor);
        // keep an include path / comment visible as the directive colour
        return;
    }
    while (i < n) {
        char c = s[static_cast<size_t>(i)];
        if (c == '/' && i + 1 < n && s[static_cast<size_t>(i + 1)] == '/' && lang != Lang::CMake) {
            push(i, n - i, Tok::Comment);
            return;
        }
        if (c == '#' && lang == Lang::CMake) {
            push(i, n - i, Tok::Comment);
            return;
        }
        if (c == '/' && i + 1 < n && s[static_cast<size_t>(i + 1)] == '*') {
            size_t e = s.find("*/", static_cast<size_t>(i) + 2);
            if (e == std::string_view::npos) {
                push(i, n - i, Tok::Comment);
                state = 1;
                return;
            }
            push(i, static_cast<int>(e) + 2 - i, Tok::Comment);
            i = static_cast<int>(e) + 2;
            continue;
        }
        if (c == '"' || (c == '\'' && lang != Lang::CMake)) {
            int j = i + 1;
            while (j < n && s[static_cast<size_t>(j)] != c) j += s[static_cast<size_t>(j)] == '\\' ? 2 : 1;
            j = std::min(j + 1, n);
            push(i, j - i, Tok::String);
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[static_cast<size_t>(i + 1)])))) {
            int j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(s[static_cast<size_t>(j)])) || s[static_cast<size_t>(j)] == '.' || s[static_cast<size_t>(j)] == '\'')) ++j;
            push(i, j - i, Tok::Number);
            i = j;
            continue;
        }
        if (isWordChar(c)) {
            int j = i;
            while (j < n && isWordChar(s[static_cast<size_t>(j)])) ++j;
            std::string_view w = s.substr(static_cast<size_t>(i), static_cast<size_t>(j - i));
            Tok k = Tok::Text;
            if (lang == Lang::Cpp) {
                if (cppKeywords().count(w)) k = Tok::Keyword;
                else if (cppTypes().count(w) || (std::isupper(static_cast<unsigned char>(w[0])) && w.size() > 1)) k = Tok::Type;
            } else if (lang == Lang::Shader) {
                if (shaderWords().count(w)) k = Tok::Keyword;
            } else if (lang == Lang::CMake) {
                std::string lw = toLower(std::string(w));
                if (cmakeWords().count(lw)) k = Tok::Keyword;
                else if (w.size() > 1 && std::all_of(w.begin(), w.end(), [](char ch) { return std::isupper(static_cast<unsigned char>(ch)) || ch == '_'; })) k = Tok::Type;
            }
            if (k == Tok::Text) {
                int m = j;
                while (m < n && s[static_cast<size_t>(m)] == ' ') ++m;
                if (m < n && s[static_cast<size_t>(m)] == '(') k = Tok::Function;
            }
            push(i, j - i, k);
            i = j;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(c))) push(i, 1, Tok::Punct);
        ++i;
    }
}

void json(std::string_view s, std::vector<Token>& out) {
    int i = 0, n = static_cast<int>(s.size());
    while (i < n) {
        char c = s[static_cast<size_t>(i)];
        if (c == '"') {
            int j = i + 1;
            while (j < n && s[static_cast<size_t>(j)] != '"') j += s[static_cast<size_t>(j)] == '\\' ? 2 : 1;
            j = std::min(j + 1, n);
            int m = j;
            while (m < n && s[static_cast<size_t>(m)] == ' ') ++m;
            out.push_back({i, j - i, m < n && s[static_cast<size_t>(m)] == ':' ? Tok::Key : Tok::String});
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            int j = i + 1;
            while (j < n && (std::isdigit(static_cast<unsigned char>(s[static_cast<size_t>(j)])) || std::strchr(".eE+-", s[static_cast<size_t>(j)]))) ++j;
            out.push_back({i, j - i, Tok::Number});
            i = j;
        } else if (std::isalpha(static_cast<unsigned char>(c))) {
            int j = i;
            while (j < n && std::isalpha(static_cast<unsigned char>(s[static_cast<size_t>(j)]))) ++j;
            out.push_back({i, j - i, Tok::Keyword});
            i = j;
        } else {
            if (!std::isspace(static_cast<unsigned char>(c))) out.push_back({i, 1, Tok::Punct});
            ++i;
        }
    }
}

void markdown(std::string_view s, int& state, std::vector<Token>& out) {
    int n = static_cast<int>(s.size());
    std::string_view t = s;
    size_t f = t.find_first_not_of(' ');
    if (f != std::string_view::npos && t.substr(f, 3) == "```") {
        out.push_back({0, n, Tok::Code});
        state = state == 2 ? 0 : 2;
        return;
    }
    if (state == 2) {
        out.push_back({0, n, Tok::Code});
        return;
    }
    if (f != std::string_view::npos && t[f] == '#') {
        out.push_back({0, n, Tok::Heading});
        return;
    }
    int i = 0;
    while (i < n) {
        char c = s[static_cast<size_t>(i)];
        if (c == '`') {
            size_t e = s.find('`', static_cast<size_t>(i) + 1);
            int j = e == std::string_view::npos ? n : static_cast<int>(e) + 1;
            out.push_back({i, j - i, Tok::Code});
            i = j;
        } else if (c == '*' || c == '_') {
            size_t e = s.find(c, static_cast<size_t>(i) + 1);
            if (e != std::string_view::npos && e > static_cast<size_t>(i) + 1) {
                out.push_back({i, static_cast<int>(e) + 1 - i, Tok::Emphasis});
                i = static_cast<int>(e) + 1;
            } else {
                ++i;
            }
        } else if (c == '[') {
            size_t e = s.find(']', static_cast<size_t>(i));
            int j = e == std::string_view::npos ? n : static_cast<int>(e) + 1;
            out.push_back({i, j - i, Tok::Keyword});
            i = j;
        } else {
            ++i;
        }
    }
}
} // namespace

std::vector<Token> tokenizeLine(Lang lang, std::string_view line, int& state) {
    std::vector<Token> out;
    switch (lang) {
        case Lang::Cpp:
        case Lang::Shader:
        case Lang::CMake: cLike(lang, line, state, out); break;
        case Lang::Json: json(line, out); break;
        case Lang::Markdown: markdown(line, state, out); break;
        default: break;
    }
    return out;
}

// ============================================================================== structure ===
std::vector<FoldRange> foldingRanges(const TextBuffer& b, Lang lang) {
    std::vector<FoldRange> out;
    if (lang == Lang::Markdown) {
        int start = -1;
        for (int l = 0; l < b.lineCount(); ++l)
            if (!b.line(l).empty() && b.line(l)[0] == '#') {
                if (start >= 0 && l - 1 > start) out.push_back({start, l - 1});
                start = l;
            }
        if (start >= 0 && b.lineCount() - 1 > start) out.push_back({start, b.lineCount() - 1});
        return out;
    }
    std::vector<int> stack;
    int state = 0;
    for (int l = 0; l < b.lineCount(); ++l) {
        auto toks = tokenizeLine(lang, b.line(l), state);
        for (auto& t : toks) {
            if (t.kind != Tok::Punct) continue;
            char c = b.line(l)[static_cast<size_t>(t.start)];
            if (c == '{' || c == '[') stack.push_back(l);
            else if ((c == '}' || c == ']') && !stack.empty()) {
                int s = stack.back();
                stack.pop_back();
                if (l > s + 1 || (l > s && b.line(l).find_first_not_of(" \t}]),;") != std::string::npos)) out.push_back({s, l - 1});
                else if (l > s) out.push_back({s, l - 1});
            }
        }
    }
    // indentation fallback for languages without braces
    if (lang == Lang::Plain || lang == Lang::CMake) {
        for (int l = 0; l + 1 < b.lineCount(); ++l) {
            int ind = leadingSpaces(b.line(l));
            if (b.line(l).empty() || leadingSpaces(b.line(l + 1)) <= ind || b.line(l + 1).empty()) continue;
            int e = l + 1;
            while (e + 1 < b.lineCount() && (b.line(e + 1).empty() || leadingSpaces(b.line(e + 1)) > ind)) ++e;
            out.push_back({l, e});
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& c) { return a.startLine < c.startLine; });
    out.erase(std::remove_if(out.begin(), out.end(), [](auto& r) { return r.endLine <= r.startLine; }), out.end());
    return out;
}

std::optional<Pos> matchBracket(const TextBuffer& b, Pos p) {
    p = b.clamp(p);
    auto bracketAt = [&](Pos q) -> char {
        const std::string& l = b.line(q.line);
        return q.col >= 0 && q.col < static_cast<int>(l.size()) && std::strchr("()[]{}", l[static_cast<size_t>(q.col)]) ? l[static_cast<size_t>(q.col)] : 0;
    };
    Pos at = p;
    char c = bracketAt(at);
    if (!c && p.col > 0) {
        at.col--;
        c = bracketAt(at);
    }
    if (!c) return std::nullopt;
    const char* open = "([{";
    const char* close = ")]}";
    bool forward = std::strchr(open, c) != nullptr;
    char want = forward ? close[std::strchr(open, c) - open] : open[std::strchr(close, c) - close];
    int depth = 0;
    Pos q = at;
    while (true) {
        char d = bracketAt(q);
        if (d == c) ++depth;
        else if (d == want && --depth == 0) return q;
        if (forward) {
            if (++q.col >= static_cast<int>(b.line(q.line).size())) {
                if (++q.line >= b.lineCount()) return std::nullopt;
                q.col = 0;
                if (b.line(q.line).empty()) q.col = -1;
            }
        } else {
            if (--q.col < 0) {
                if (--q.line < 0) return std::nullopt;
                q.col = static_cast<int>(b.line(q.line).size()) - 1;
            }
        }
        if (q.col < 0) continue;
    }
}

std::vector<Symbol> outline(const TextBuffer& b, Lang lang) {
    std::vector<Symbol> out;
    if (lang == Lang::Markdown) {
        for (int l = 0; l < b.lineCount(); ++l)
            if (!b.line(l).empty() && b.line(l)[0] == '#') {
                std::string s = b.line(l);
                out.push_back({s.substr(s.find_first_not_of("# ")), "heading", l});
            }
        return out;
    }
    if (lang != Lang::Cpp && lang != Lang::Shader) return out;
    static const std::regex typeRe(R"(^\s*(class|struct|enum(?:\s+class)?|namespace)\s+([A-Za-z_]\w*)(?:\s*(?:final|:|\{|$)))");
    static const std::regex fnRe(R"(^\s*(?:[\w:<>,\*&\s]+?\s+)?((?:[A-Za-z_]\w*::)*~?[A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const)?\s*(?:override|noexcept|final)?\s*\{?\s*$)");
    static const std::unordered_set<std::string> notFn = {"if", "for", "while", "switch", "return", "catch", "sizeof", "else"};
    for (int l = 0; l < b.lineCount(); ++l) {
        const std::string& s = b.line(l);
        std::smatch m;
        if (std::regex_search(s, m, typeRe)) {
            std::string kind = m[1].str();
            if (kind.rfind("enum", 0) == 0) kind = "enum";
            out.push_back({m[2].str(), kind, l});
        } else if (s.find(';') == std::string::npos && std::regex_search(s, m, fnRe)) {
            std::string name = m[1].str();
            std::string last = name.substr(name.rfind(':') == std::string::npos ? 0 : name.rfind(':') + 1);
            if (notFn.count(last) || s.find_first_not_of(" \t") == std::string::npos) continue;
            bool nextBrace = s.find('{') != std::string::npos || (l + 1 < b.lineCount() && b.line(l + 1).find_first_not_of(" \t") != std::string::npos &&
                                                                  b.line(l + 1)[b.line(l + 1).find_first_not_of(" \t")] == '{');
            if (nextBrace) out.push_back({name, s.find("::") != std::string::npos || std::isspace(static_cast<unsigned char>(s[0])) ? "method" : "function", l});
        }
    }
    return out;
}

// ============================================================================== search ===
namespace {
Result<std::regex> makeRegex(std::string_view query, const SearchOptions& o) {
    std::string pat;
    if (o.regex) {
        pat = std::string(query);
    } else {
        for (char c : query) {
            if (std::strchr(R"(\^$.|?*+()[]{})", c)) pat += '\\';
            pat += c;
        }
    }
    if (o.wholeWord) pat = "\\b(?:" + pat + ")\\b";
    try {
        return std::regex(pat, o.matchCase ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error& e) {
        return Result<std::regex>::failure(std::string("invalid regular expression: ") + e.what());
    }
}
} // namespace

Result<std::vector<Match>> findAll(const TextBuffer& b, std::string_view query, const SearchOptions& o) {
    std::vector<Match> out;
    if (query.empty()) return out;
    auto re = makeRegex(query, o);
    if (!re) return Result<std::vector<Match>>::failure(re.error());
    for (int l = 0; l < b.lineCount(); ++l) {
        const std::string& s = b.line(l);
        for (auto it = std::sregex_iterator(s.begin(), s.end(), *re); it != std::sregex_iterator(); ++it) {
            if (it->length(0) == 0) continue;
            int st = static_cast<int>(it->position(0));
            out.push_back({{l, st}, {l, st + static_cast<int>(it->length(0))}});
        }
    }
    return out;
}

Result<int> replaceAll(TextBuffer& b, std::string_view query, std::string_view replacement, const SearchOptions& o) {
    auto re = makeRegex(query, o);
    if (!re) return Result<int>::failure(re.error());
    auto matches = findAll(b, query, o);
    if (!matches || matches->empty()) return 0;
    int count = static_cast<int>(matches->size());
    std::string text = b.text(), out;
    std::string rep(replacement);
    std::vector<std::string> lines;
    for (int l = 0; l < b.lineCount(); ++l) {
        std::string s = b.line(l);
        lines.push_back(o.regex ? std::regex_replace(s, *re, rep) : std::regex_replace(s, *re, std::regex_replace(rep, std::regex(R"(\$)"), "$$$$")));
    }
    for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
    b.replaceRange({0, 0}, {b.lineCount() - 1, static_cast<int>(b.line(b.lineCount() - 1).size())}, out);
    return count;
}

// ============================================================================== completion ===
void SymbolIndex::addFile(const std::string& file, std::string_view text, Lang lang) {
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.file == file; }), m_entries.end());
    TextBuffer b(text);
    std::unordered_set<std::string> seen;
    for (auto& s : outline(b, lang)) {
        std::string n = s.name.substr(s.name.rfind(':') == std::string::npos ? 0 : s.name.rfind(':') + 1);
        m_entries.push_back({n, file, s.kind, s.line});
        seen.insert(n);
    }
    // every other identifier (first occurrence) for completion
    int state = 0;
    for (int l = 0; l < b.lineCount(); ++l)
        for (auto& t : tokenizeLine(lang, b.line(l), state)) {
            if (t.kind != Tok::Text && t.kind != Tok::Function && t.kind != Tok::Type) continue;
            std::string w = b.line(l).substr(static_cast<size_t>(t.start), static_cast<size_t>(t.len));
            if (w.size() < 3 || !isWordChar(w[0]) || std::isdigit(static_cast<unsigned char>(w[0])) || !seen.insert(w).second) continue;
            m_entries.push_back({w, file, t.kind == Tok::Function ? "function" : "identifier", l});
        }
}

std::vector<const SymbolIndex::Entry*> SymbolIndex::complete(std::string_view prefix, size_t max) const {
    std::vector<std::pair<int, const Entry*>> scored;
    std::string lp = toLower(std::string(prefix));
    std::unordered_set<std::string> names;
    for (auto& e : m_entries) {
        if (e.name == prefix) continue;
        std::string ln = toLower(e.name);
        int score = -1;
        if (e.name.rfind(prefix, 0) == 0) score = 0;
        else if (ln.rfind(lp, 0) == 0) score = 1;
        else {  // camel-hump: "gC" -> "getComponent"
            size_t k = 0;
            for (size_t i = 0; i < e.name.size() && k < prefix.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(e.name[i])) == std::tolower(static_cast<unsigned char>(prefix[k])) &&
                    (i == 0 || std::isupper(static_cast<unsigned char>(e.name[i])) || e.name[i - 1] == '_' || k > 0))
                    ++k;
            if (k == prefix.size() && !prefix.empty()) score = 2;
        }
        if (score < 0 || !names.insert(e.name).second) continue;
        if (e.kind != "identifier") score -= 1;  // definitions rank first
        scored.push_back({score, &e});
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first < b.first : a.second->name < b.second->name; });
    std::vector<const Entry*> out;
    for (auto& [s, e] : scored) {
        if (out.size() >= max) break;
        out.push_back(e);
    }
    return out;
}

const SymbolIndex::Entry* SymbolIndex::definition(std::string_view name) const {
    const Entry* best = nullptr;
    for (auto& e : m_entries)
        if (e.name == name && (!best || (best->kind == "identifier" && e.kind != "identifier"))) best = &e;
    return best;
}

} // namespace fledge::code
