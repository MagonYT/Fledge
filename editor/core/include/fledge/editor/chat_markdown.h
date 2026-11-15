#pragma once
// Markdown subset for the AI Assistant panel: models answer in Markdown, the panel draws it as rich text.
// Pure parsing (no ImGui): headings, paragraphs, bullet / numbered lists (nested), block quotes, fenced code,
// rules, pipe tables and inline **bold**, *italic*, `code`, ~~strike~~, [text](url).
// Anything it does not understand stays visible as plain text; input is never dropped.
#include <string>
#include <string_view>
#include <vector>

namespace fledge::editor {

struct MdSpan {
    std::string text;
    bool bold = false, italic = false, code = false, strike = false;
    std::string link;  // target of [text](url); empty = not a link

    bool operator==(const MdSpan&) const = default;
};

struct MdBlock {
    enum class Kind { Paragraph, Heading, Bullet, Numbered, Quote, Code, Rule, Table } kind = Kind::Paragraph;
    int level = 0;              // Heading: 1..6; Bullet / Numbered / Quote: nesting depth (0 = top)
    std::string marker;         // Numbered: "1." as written
    std::vector<MdSpan> spans;  // inline content (not Code / Rule / Table)
    std::string code, lang;     // Code: text without the fences, and the info string
    std::vector<std::vector<std::vector<MdSpan>>> rows;  // Table: rows -> cells -> spans; row 0 is the header
};

// Inline pass alone (one logical line).
std::vector<MdSpan> parseMarkdownInline(std::string_view text);

// Whole message. An unclosed code fence runs to the end (a reply that is still being revealed).
std::vector<MdBlock> parseChatMarkdown(std::string_view text);

// The message without markup (clipboard "copy as plain text", tests).
std::string markdownPlainText(const std::vector<MdBlock>& blocks);

} // namespace fledge::editor
