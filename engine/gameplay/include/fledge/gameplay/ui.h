#pragma once
// In-game UI toolkit (docs/GAMEPLAY.md "HUD & UI"). It builds coloured triangles in pixels (top-left origin)
// that the renderer draws as its 2D overlay: rectangles, frames, bars, text (a TrueType Font when set, else
// stb_easy_font), circles, lines,
// anchoring, and a rotating minimap. The HudElement component and Fledge Script files use it through GameWorld.
#include <string>
#include <vector>

#include "fledge/core/math.h"

namespace fledge::gameplay {

struct UiVertex {
    Vec2 pos;
    Vec4 color;
    Vec2 uv{0, 0};  // font atlas coordinates (0, 0 = solid: shapes)
};

class Font;

enum class Anchor { TopLeft = 0, Top = 1, TopRight = 2, Left = 3, Center = 4, Right = 5, BottomLeft = 6, Bottom = 7, BottomRight = 8 };
Anchor anchorFromString(const std::string& s);

class UiCanvas {
public:
    UiCanvas(float width, float height, float scale = 1.0f) : m_w(width), m_h(height), m_scale(scale) {}
    float width() const { return m_w; }
    float height() const { return m_h; }
    float scale() const { return m_scale; }
    // Top-left of a box of `size` placed at `anchor` with `offset` (pixels, pointing inwards from the anchor).
    Vec2 place(Anchor anchor, Vec2 offset, Vec2 size) const;

    void rect(float x, float y, float w, float h, Vec4 color);
    // Filled rounded rectangle (r = corner radius in pixels). r <= 0 is a sharp rect.
    void roundedRect(float x, float y, float w, float h, float radius, Vec4 color);
    void frame(float x, float y, float w, float h, float thickness, Vec4 color);
    void bar(float x, float y, float w, float h, float fraction, Vec4 fill, Vec4 background);
    void line(Vec2 a, Vec2 b, float thickness, Vec4 color);
    void triangle(Vec2 a, Vec2 b, Vec2 c, Vec4 color);
    void circle(Vec2 c, float r, Vec4 color, int segments = 24);
    // TrueType text (the renderer must have the same font's atlas as its overlay texture). nullptr = stb_easy_font.
    void setFont(const Font* font) { m_font = font; }
    const Font* font() const { return m_font; }
    // Text: size in pixels per font unit (stb_easy_font glyphs are ~7 units tall; a TrueType font is scaled so a
    // line is 7.5 units). UTF-8 (stb_easy_font shows non-ASCII as '?'). Returns the advance width.
    float text(float x, float y, const std::string& s, Vec4 color, float size = 2.0f, bool shadow = true);
    Vec2 measure(const std::string& s, float size = 2.0f) const;

    const std::vector<UiVertex>& vertices() const { return m_v; }
    std::vector<UiVertex>& vertices() { return m_v; }
    size_t triangles() const { return m_v.size() / 3; }

private:
    float m_w, m_h, m_scale;
    std::vector<UiVertex> m_v;
    const Font* m_font = nullptr;
};

// Minimap: world polylines (roads) and blips drawn in a square, centred on `center`, rotated so that `yaw`
// (the player's facing, 0 = -Z) points up. metersAcross = world size shown across the square.
struct MinimapBlip {
    Vec3 position;
    Vec4 color;
    float size = 4;
};
void drawMinimap(UiCanvas& ui, Vec2 topLeft, float sizePx, Vec3 center, float yaw, float metersAcross, const std::vector<std::vector<Vec3>>& roads,
                 const std::vector<MinimapBlip>& blips, const Vec3* waypoint = nullptr);

} // namespace fledge::gameplay
