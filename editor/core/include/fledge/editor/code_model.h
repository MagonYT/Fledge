#pragma once
// UI-free core of the built-in code editor (Code workspace): text buffer with multi-cursor editing
// and undo, a line tokenizer for C++ / JSON / Markdown / GLSL+HLSL / CMake, folding ranges, bracket
// matching, find/replace, outline symbols and the fallback completion index used when clangd is
// unavailable. Everything here is unit-tested (tests/test_code_editor.cpp).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/utility.h"

namespace fledge::code {

struct Pos {
    int line = 0, col = 0;  // col = byte offset in the line
    auto operator<=>(const Pos&) const = default;
};

struct Cursor {
    Pos pos;      // caret
    Pos anchor;   // selection start (== pos: no selection)
    bool hasSelection() const { return pos != anchor; }
    Pos selStart() const { return pos < anchor ? pos : anchor; }
    Pos selEnd() const { return pos < anchor ? anchor : pos; }
};

class TextBuffer {
public:
    TextBuffer() : m_lines{""} {}
    explicit TextBuffer(std::string_view text) { setText(text); }

    void setText(std::string_view text);  // resets undo + cursors
    std::string text() const;
    const std::vector<std::string>& lines() const { return m_lines; }
    int lineCount() const { return static_cast<int>(m_lines.size()); }
    const std::string& line(int i) const { return m_lines[static_cast<size_t>(i)]; }
    Pos clamp(Pos p) const;
    std::string textIn(Pos a, Pos b) const;

    // ---- cursors (index 0 = primary)
    std::vector<Cursor> cursors{Cursor{}};
    void setCursor(Pos p, bool select = false);
    void addCursor(Pos p);                // Alt+click
    bool addNextOccurrence();             // Ctrl+D: select word / next occurrence of the selection
    void selectAll();
    std::string selectedText() const;     // primary cursor
    void normalizeCursors();              // sort + merge overlapping

    // ---- editing (all cursors; each call is one undo step unless merged)
    void insert(std::string_view s);      // replaces selections; auto-indent after '\n'
    void typeChar(char c);                // insert + bracket/quote auto-closing + skip-over
    void backspace();
    void del();
    void newline();                       // keeps indentation, extra level after '{'
    void indent();                        // Tab: block indent with a selection, else insert spaces
    void outdent();                       // Shift+Tab
    void toggleComment(std::string_view prefix = "//");
    void moveLines(int delta);            // Alt+Up / Alt+Down
    void replaceRange(Pos a, Pos b, std::string_view s);  // programmatic edit (rename, replace)

    // ---- movement
    void moveCursors(int dLine, int dCol, bool select, bool word = false);
    void home(bool select);
    void end(bool select);

    // ---- undo
    bool undo();
    bool redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    uint64_t version() const { return m_version; }  // bumps on every change
    bool dirty() const { return m_version != m_savedVersion; }
    void markSaved() { m_savedVersion = m_version; }

    int tabSize = 4;
    bool autoClose = true;

private:
    struct Snapshot {
        std::vector<std::string> lines;
        std::vector<Cursor> cursors;
        uint64_t version;
    };
    void pushUndo(bool mergeTyping = false);
    Pos erase(Pos a, Pos b);
    Pos insertAt(Pos p, std::string_view s);
    void shiftCursorsAfterEdit(size_t index, Pos editStart, Pos oldEnd, Pos newEnd);
    std::vector<std::string> m_lines;
    std::vector<Snapshot> m_undo, m_redo;
    uint64_t m_version = 0, m_savedVersion = 0;
    bool m_lastWasTyping = false;
};

// ---------------------------------------------------------------------------- tokens ---
enum class Lang { Plain, Cpp, Json, Markdown, Shader, CMake };
Lang langForPath(std::string_view path);
const char* langName(Lang l);

enum class Tok : uint8_t { Text, Keyword, Type, Number, String, Comment, Preprocessor, Punct, Function, Heading, Emphasis, Code, Key };
struct Token {
    int start = 0, len = 0;
    Tok kind = Tok::Text;
};
// Tokenizes one line; `state` carries multi-line constructs (block comments, fenced code) to the next line.
std::vector<Token> tokenizeLine(Lang lang, std::string_view line, int& state);

// ---------------------------------------------------------------------------- structure ---
struct FoldRange {
    int startLine = 0, endLine = 0;  // both inclusive; fold hides startLine+1 .. endLine
};
std::vector<FoldRange> foldingRanges(const TextBuffer& b, Lang lang);
// The bracket matching the one at (or just before) p, if any.
std::optional<Pos> matchBracket(const TextBuffer& b, Pos p);

struct Symbol {
    std::string name, kind;  // kind: class, struct, function, method, enum, namespace, heading
    int line = 0;
};
std::vector<Symbol> outline(const TextBuffer& b, Lang lang);

// ---------------------------------------------------------------------------- search ---
struct SearchOptions {
    bool regex = false, matchCase = false, wholeWord = false;
};
struct Match {
    Pos start, end;
};
Result<std::vector<Match>> findAll(const TextBuffer& b, std::string_view query, const SearchOptions& o);
// Replaces every match (supports $1-style groups with regex); returns the count.
Result<int> replaceAll(TextBuffer& b, std::string_view query, std::string_view replacement, const SearchOptions& o);

// ---------------------------------------------------------------------------- completion ---
// Fallback ("basic") index: identifiers of open/project files and the Fledge Script SDK headers, with the file
// and line of a likely definition. Used when clangd is unavailable.
class SymbolIndex {
public:
    struct Entry {
        std::string name, file, kind;
        int line = 0;
    };
    void clear() { m_entries.clear(); }
    void addFile(const std::string& file, std::string_view text, Lang lang);
    std::vector<const Entry*> complete(std::string_view prefix, size_t max = 50) const;  // ranked: prefix, then camel-hump
    const Entry* definition(std::string_view name) const;
    size_t size() const { return m_entries.size(); }

private:
    std::vector<Entry> m_entries;
};

std::string wordAt(const TextBuffer& b, Pos p, Pos* start = nullptr, Pos* end = nullptr);

} // namespace fledge::code
