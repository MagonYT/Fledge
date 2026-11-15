#include "fledge/editor/chat_markdown.h"

#include <algorithm>
#include <cctype>

namespace fledge::editor {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && (isSpace(s.back()) || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

struct Style {
    bool bold = false, italic = false, strike = false;
};

void push(std::vector<MdSpan>& out, std::string text, const Style& st, bool code = false, std::string link = {}) {
    if (text.empty()) return;
    if (!out.empty() && !code && !out.back().code && link.empty() && out.back().link.empty() && out.back().bold == st.bold &&
        out.back().italic == st.italic && out.back().strike == st.strike) {
        out.back().text += text;
        return;
    }
    MdSpan s;
    s.text = std::move(text);
    s.bold = st.bold;
    s.italic = st.italic;
    s.strike = st.strike;
    s.code = code;
    s.link = std::move(link);
    out.push_back(std::move(s));
}

// Finds the closing delimiter `d` at or after `from`; npos when there is none (the opener stays literal).
size_t findClose(std::string_view s, size_t from, std::string_view d) {
    for (size_t i = from; i + d.size() <= s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] == '`') {  // skip code spans
            size_t e = s.find('`', i + 1);
            if (e == std::string_view::npos) return std::string_view::npos;
            i = e;
            continue;
        }
        if (s.compare(i, d.size(), d) != 0) continue;
        if (i > from && !isSpace(s[i - 1])) {
            // "*" must not be the middle of "**"; "_" must end a word
            if (d.size() == 1 && i + 1 < s.size() && s[i + 1] == d[0]) {
                ++i;
                continue;
            }
            if (d[0] == '_' && i + d.size() < s.size() && isWord(s[i + d.size()])) continue;
            return i;
        }
    }
    return std::string_view::npos;
}

void inlinePass(std::string_view s, Style st, std::vector<MdSpan>& out, int depth) {
    std::string plain;
    auto flush = [&] {
        push(out, std::move(plain), st);
        plain.clear();
    };
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            plain += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t n = 1;
            while (i + n < s.size() && s[i + n] == '`') ++n;
            size_t e = s.find(std::string(n, '`'), i + n);
            if (e != std::string_view::npos) {
                flush();
                push(out, std::string(trim(s.substr(i + n, e - i - n))), st, true);
                i = e + n;
                continue;
            }
        }
        if (depth < 8 && (c == '*' || c == '_' || c == '~')) {
            bool two = i + 1 < s.size() && s[i + 1] == c;
            size_t n = two ? 2 : 1;
            bool opens = i + n < s.size() && !isSpace(s[i + n]) && (c != '~' || two);
            if (c == '_' && i > 0 && isWord(s[i - 1])) opens = false;  // snake_case stays text
            if (opens) {
                size_t e = findClose(s, i + n, s.substr(i, n));
                if (e != std::string_view::npos) {
                    flush();
                    Style inner = st;
                    if (c == '~') inner.strike = true;
                    else if (two) inner.bold = true;
                    else inner.italic = true;
                    inlinePass(s.substr(i + n, e - i - n), inner, out, depth + 1);
                    i = e + n;
                    continue;
                }
            }
        }
        if (c == '[') {
            size_t close = s.find(']', i + 1);
            if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
                size_t end = s.find(')', close + 2);
                if (end != std::string_view::npos && close > i + 1) {
                    flush();
                    std::string url(trim(s.substr(close + 2, end - close - 2)));
                    if (size_t sp = url.find(' '); sp != std::string::npos) url.resize(sp);  // drop a "title"
                    std::vector<MdSpan> label;
                    inlinePass(s.substr(i + 1, close - i - 1), st, label, depth + 1);
                    for (auto& l : label) {
                        l.link = url;
                        out.push_back(std::move(l));
                    }
                    i = end + 1;
                    continue;
                }
            }
        }
        plain += c;
        ++i;
    }
    flush();
}

int indentOf(std::string_view line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

bool isRule(std::string_view t) {
    if (t.size() < 3 || (t[0] != '-' && t[0] != '*' && t[0] != '_')) return false;
    int n = 0;
    for (char c : t) {
        if (c == t[0]) ++n;
        else if (!isSpace(c)) return false;
    }
    return n >= 3;
}

// "- x", "* x", "+ x" -> content offset, else 0
size_t bulletPrefix(std::string_view t) {
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && isSpace(t[1])) return 2;
    return 0;
}

// "12. x" / "12) x" -> content offset, else 0
size_t numberPrefix(std::string_view t) {
    size_t i = 0;
    while (i < t.size() && i < 9 && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
    if (i == 0 || i + 1 >= t.size() || (t[i] != '.' && t[i] != ')') || !isSpace(t[i + 1])) return 0;
    return i + 2;
}

std::vector<std::string_view> splitRow(std::string_view t) {
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);
    std::vector<std::string_view> cells;
    size_t start = 0;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i < t.size() && t[i] == '\\') {
            ++i;
            continue;
        }
        if (i == t.size() || t[i] == '|') {
            cells.push_back(trim(t.substr(start, i - start)));
            start = i + 1;
        }
    }
    return cells;
}

bool isTableDivider(std::string_view t) {
    if (t.find('-') == std::string_view::npos || t.find('|') == std::string_view::npos) return false;
    for (char c : t)
        if (c != '-' && c != '|' && c != ':' && !isSpace(c)) return false;
    return true;
}

} // namespace

std::vector<MdSpan> parseMarkdownInline(std::string_view text) {
    std::vector<MdSpan> out;
    inlinePass(text, {}, out, 0);
    return out;
}

std::vector<MdBlock> parseChatMarkdown(std::string_view text) {
    std::vector<std::string_view> lines;
    for (size_t pos = 0; pos <= text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view l = text.substr(pos, nl - pos);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        lines.push_back(l);
        pos = nl + 1;
    }

    std::vector<MdBlock> out;
    std::string para;  // open paragraph / list item text (soft-wrapped lines join with a space)
    MdBlock open;
    bool hasOpen = false;
    auto close = [&] {
        if (!hasOpen) return;
        open.spans = parseMarkdownInline(para);
        if (!open.spans.empty()) out.push_back(std::move(open));
        open = {};
        para.clear();
        hasOpen = false;
    };
    auto start = [&](MdBlock::Kind k, int level, std::string_view content) {
        close();
        open.kind = k;
        open.level = level;
        para = std::string(content);
        hasOpen = true;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view raw = lines[i];
        std::string_view t = trim(raw);

        if (t.starts_with("```") || t.starts_with("~~~")) {
            close();
            std::string_view fence = t.substr(0, 3);
            MdBlock b;
            b.kind = MdBlock::Kind::Code;
            b.lang = std::string(trim(t.substr(3)));
            int base = indentOf(raw);
            size_t j = i + 1;
            for (; j < lines.size(); ++j) {
                if (trim(lines[j]).starts_with(fence)) break;
                std::string_view l = lines[j];
                int strip = std::min(base, indentOf(l));
                while (strip > 0 && !l.empty() && l.front() == ' ') {
                    l.remove_prefix(1);
                    --strip;
                }
                if (!b.code.empty()) b.code += '\n';
                b.code += l;
            }
            out.push_back(std::move(b));
            i = j;  // the closing fence (or the end)
            continue;
        }
        if (t.empty()) {
            close();
            continue;
        }
        if (isRule(t)) {
            close();
            MdBlock b;
            b.kind = MdBlock::Kind::Rule;
            out.push_back(std::move(b));
            continue;
        }
        if (t[0] == '#') {
            size_t n = 0;
            while (n < t.size() && t[n] == '#') ++n;
            if (n <= 6 && n < t.size() && isSpace(t[n])) {
                std::string_view h = trim(t.substr(n));
                while (!h.empty() && h.back() == '#') h.remove_suffix(1);
                start(MdBlock::Kind::Heading, static_cast<int>(n), trim(h));
                close();
                continue;
            }
        }
        if (t[0] == '>') {
            int depth = 0;
            std::string_view q = t;
            while (!q.empty() && q[0] == '>') {
                q = trim(q.substr(1));
                ++depth;
            }
            if (hasOpen && open.kind == MdBlock::Kind::Quote && open.level == depth - 1) para += " " + std::string(q);
            else start(MdBlock::Kind::Quote, depth - 1, q);
            continue;
        }
        if (t.find('|') != std::string_view::npos && i + 1 < lines.size() && isTableDivider(trim(lines[i + 1]))) {
            close();
            MdBlock b;
            b.kind = MdBlock::Kind::Table;
            auto addRow = [&](std::string_view row) {
                std::vector<std::vector<MdSpan>> cells;
                for (auto c : splitRow(row)) cells.push_back(parseMarkdownInline(c));
                b.rows.push_back(std::move(cells));
            };
            addRow(t);
            size_t j = i + 2;
            for (; j < lines.size(); ++j) {
                std::string_view r = trim(lines[j]);
                if (r.empty() || r.find('|') == std::string_view::npos) break;
                addRow(r);
            }
            out.push_back(std::move(b));
            i = j - 1;
            continue;
        }
        int level = std::min(indentOf(raw) / 2, 5);
        if (size_t p = bulletPrefix(t)) {
            start(MdBlock::Kind::Bullet, level, trim(t.substr(p)));
            continue;
        }
        if (size_t p = numberPrefix(t)) {
            start(MdBlock::Kind::Numbered, level, trim(t.substr(p)));
            open.marker = std::string(t.substr(0, p - 1));
            continue;
        }
        if (hasOpen) para += " " + std::string(t);  // continuation of the paragraph / list item
        else start(MdBlock::Kind::Paragraph, 0, t);
    }
    close();
    return out;
}

std::string markdownPlainText(const std::vector<MdBlock>& blocks) {
    std::string out;
    auto spans = [&](const std::vector<MdSpan>& ss) {
        for (auto& s : ss) out += s.text;
    };
    for (auto& b : blocks) {
        if (!out.empty()) out += '\n';
        switch (b.kind) {
            case MdBlock::Kind::Code: out += b.code; break;
            case MdBlock::Kind::Rule: out += "---"; break;
            case MdBlock::Kind::Table:
                for (size_t r = 0; r < b.rows.size(); ++r) {
                    if (r) out += '\n';
                    for (size_t c = 0; c < b.rows[r].size(); ++c) {
                        if (c) out += '\t';
                        spans(b.rows[r][c]);
                    }
                }
                break;
            case MdBlock::Kind::Bullet:
                out += std::string(static_cast<size_t>(b.level) * 2, ' ') + "- ";
                spans(b.spans);
                break;
            case MdBlock::Kind::Numbered:
                out += std::string(static_cast<size_t>(b.level) * 2, ' ') + b.marker + " ";
                spans(b.spans);
                break;
            default: spans(b.spans); break;
        }
    }
    return out;
}

} // namespace fledge::editor
