#pragma once
// TrueType fonts for the game UI (docs/GAMEPLAY.md "UI"): stb_truetype bakes Latin (ASCII, Latin-1, Latin
// Extended-A), Greek and Cyrillic into one alpha atlas at a pixel height; text is laid out with kerning into
// textured quads that UiCanvas draws through the renderer's overlay (atlas rows 0..3 are solid, so untextured
// shapes sample coverage 1 at uv (0, 0)). Missing glyphs draw as '?'.
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/utility.h"

namespace fledge::gameplay {

std::u32string decodeUtf8(std::string_view s);  // invalid bytes become U+FFFD

class Font {
public:
    ~Font();
    // ttf / otf bytes (kept: kerning reads them); pixelHeight = ascent - descent in pixels at scale 1.
    static std::shared_ptr<Font> load(Bytes ttf, float pixelHeight = 32, std::string* error = nullptr);

    float pixelHeight() const { return m_px; }
    float ascent() const { return m_ascent; }        // pixels above the baseline
    float lineHeight() const { return m_lineHeight; }  // baseline to baseline
    int atlasWidth() const { return m_w; }
    int atlasHeight() const { return m_h; }
    const std::vector<uint8_t>& atlas() const { return m_atlas; }  // coverage, 1 byte per pixel
    std::vector<uint8_t> atlasRgba() const;                        // white RGB, coverage in alpha (renderer upload)
    bool hasGlyph(char32_t c) const;
    int glyphCount() const;

    struct Quad {
        Vec2 p0, p1;    // pixels (top-left, bottom-right)
        Vec2 uv0, uv1;  // atlas
    };
    // Lays out UTF-8 text with its top-left at (x, y); '\n' starts a new line. Returns the widest line's advance.
    float layout(std::string_view utf8, float x, float y, float scale, std::vector<Quad>* out) const;
    Vec2 measure(std::string_view utf8, float scale = 1) const;  // width, lines * lineHeight

private:
    Font() = default;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    float m_px = 32, m_ascent = 0, m_lineHeight = 0;
    int m_w = 0, m_h = 0;
    std::vector<uint8_t> m_atlas;
};

} // namespace fledge::gameplay
