#include "fledge/gameplay/ui.h"

#include "fledge/gameplay/font.h"

#include <stb_easy_font.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace fledge::gameplay {

Anchor anchorFromString(const std::string& s) {
    static const char* names[] = {"TopLeft", "Top", "TopRight", "Left", "Center", "Right", "BottomLeft", "Bottom", "BottomRight"};
    for (int i = 0; i < 9; ++i)
        if (s == names[i]) return static_cast<Anchor>(i);
    return Anchor::TopLeft;
}

Vec2 UiCanvas::place(Anchor a, Vec2 off, Vec2 size) const {
    int col = static_cast<int>(a) % 3, row = static_cast<int>(a) / 3;
    float x = col == 0 ? off.x : col == 1 ? (m_w - size.x) * 0.5f + off.x : m_w - size.x - off.x;
    float y = row == 0 ? off.y : row == 1 ? (m_h - size.y) * 0.5f + off.y : m_h - size.y - off.y;
    return {x, y};
}

void UiCanvas::rect(float x, float y, float w, float h, Vec4 c) {
    Vec2 a{x, y}, b{x + w, y}, d{x, y + h}, e{x + w, y + h};
    for (Vec2 p : {a, b, e, a, e, d}) m_v.push_back({p, c});
}

void UiCanvas::roundedRect(float x, float y, float w, float h, float r, Vec4 c) {
    r = std::min({std::max(0.0f, r), w * 0.5f, h * 0.5f});
    if (r < 0.5f) {
        rect(x, y, w, h, c);
        return;
    }
    rect(x + r, y, w - 2 * r, h, c);
    rect(x, y + r, r, h - 2 * r, c);
    rect(x + w - r, y + r, r, h - 2 * r, c);
    auto quarter = [&](Vec2 center, float a0) {
        constexpr int kSeg = 8;
        for (int i = 0; i < kSeg; ++i) {
            float t0 = a0 + 1.5707963f * static_cast<float>(i) / kSeg;
            float t1 = a0 + 1.5707963f * static_cast<float>(i + 1) / kSeg;
            triangle(center, {center.x + std::cos(t0) * r, center.y + std::sin(t0) * r},
                     {center.x + std::cos(t1) * r, center.y + std::sin(t1) * r}, c);
        }
    };
    quarter({x + r, y + r}, 3.14159265f);          // top-left
    quarter({x + w - r, y + r}, -1.5707963f);      // top-right
    quarter({x + w - r, y + h - r}, 0);            // bottom-right
    quarter({x + r, y + h - r}, 1.5707963f);       // bottom-left
}

void UiCanvas::frame(float x, float y, float w, float h, float t, Vec4 c) {
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y + t, t, h - 2 * t, c);
    rect(x + w - t, y + t, t, h - 2 * t, c);
}

void UiCanvas::bar(float x, float y, float w, float h, float f, Vec4 fill, Vec4 bg) {
    rect(x, y, w, h, bg);
    f = std::clamp(f, 0.0f, 1.0f);
    float pad = std::max(1.0f, h * 0.12f);
    if (f > 0) rect(x + pad, y + pad, (w - 2 * pad) * f, h - 2 * pad, fill);
}

void UiCanvas::triangle(Vec2 a, Vec2 b, Vec2 c, Vec4 color) {
    m_v.push_back({a, color});
    m_v.push_back({b, color});
    m_v.push_back({c, color});
}

void UiCanvas::line(Vec2 a, Vec2 b, float t, Vec4 c) {
    Vec2 d{b.x - a.x, b.y - a.y};
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-4f) return;
    Vec2 n{-d.y / len * t * 0.5f, d.x / len * t * 0.5f};
    Vec2 p0{a.x + n.x, a.y + n.y}, p1{b.x + n.x, b.y + n.y}, p2{b.x - n.x, b.y - n.y}, p3{a.x - n.x, a.y - n.y};
    triangle(p0, p1, p2, c);
    triangle(p0, p2, p3, c);
}

void UiCanvas::circle(Vec2 c, float r, Vec4 color, int seg) {
    for (int i = 0; i < seg; ++i) {
        float a0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg), a1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(seg);
        triangle(c, {c.x + std::cos(a0) * r, c.y + std::sin(a0) * r}, {c.x + std::cos(a1) * r, c.y + std::sin(a1) * r}, color);
    }
}

Vec2 UiCanvas::measure(const std::string& s, float size) const {
    if (m_font) return m_font->measure(s, size * 7.5f / m_font->lineHeight());
    return {static_cast<float>(stb_easy_font_width(const_cast<char*>(s.c_str()))) * size,
            static_cast<float>(stb_easy_font_height(const_cast<char*>(s.c_str()))) * size};
}

float UiCanvas::text(float x, float y, const std::string& s, Vec4 c, float size, bool shadow) {
    if (m_font) {  // TrueType: textured quads from the font atlas
        float scale = size * 7.5f / m_font->lineHeight();
        static thread_local std::vector<Font::Quad> quads;
        quads.clear();
        float w = m_font->layout(s.size() > 4000 ? std::string_view(s).substr(0, 4000) : std::string_view(s), x, y, scale, &quads);
        auto emit = [&](float ox, float oy, Vec4 col) {
            for (const auto& q : quads) {
                UiVertex a{{q.p0.x + ox, q.p0.y + oy}, col, q.uv0}, b{{q.p1.x + ox, q.p0.y + oy}, col, {q.uv1.x, q.uv0.y}},
                    e{{q.p1.x + ox, q.p1.y + oy}, col, q.uv1}, d{{q.p0.x + ox, q.p1.y + oy}, col, {q.uv0.x, q.uv1.y}};
                for (const UiVertex* v : {&a, &b, &e, &a, &e, &d}) m_v.push_back(*v);
            }
        };
        if (shadow) emit(size * 0.5f, size * 0.5f, {0, 0, 0, c.w * 0.6f});
        emit(0, 0, c);
        return w;
    }
    static thread_local std::vector<char> buf(256 * 1024);
    std::string clean;
    for (char ch : s) clean += (static_cast<unsigned char>(ch) < 128 && (ch >= 32 || ch == '\n')) ? ch : '?';
    if (clean.size() > 2000) clean.resize(2000);
    int quads = stb_easy_font_print(0, 0, clean.data(), nullptr, buf.data(), static_cast<int>(buf.size()));
    auto* f = reinterpret_cast<const float*>(buf.data());
    auto emit = [&](float ox, float oy, Vec4 col) {
        for (int q = 0; q < quads; ++q) {
            Vec2 p[4];
            for (int k = 0; k < 4; ++k) {
                const float* vert = f + (q * 4 + k) * 4;
                p[k] = {x + ox + vert[0] * size, y + oy + vert[1] * size};
            }
            for (int k : {0, 1, 2, 0, 2, 3}) m_v.push_back({p[k], col});
        }
    };
    if (shadow) emit(size * 0.5f, size * 0.5f, {0, 0, 0, c.w * 0.6f});
    emit(0, 0, c);
    return measure(clean, size).x;
}

void drawMinimap(UiCanvas& ui, Vec2 tl, float sz, Vec3 center, float yaw, float metersAcross, const std::vector<std::vector<Vec3>>& roads,
                 const std::vector<MinimapBlip>& blips, const Vec3* waypoint) {
    ui.rect(tl.x, tl.y, sz, sz, {0.05f, 0.07f, 0.09f, 0.75f});
    float ppm = sz / std::max(1.0f, metersAcross);
    float c = std::cos(yaw), s = std::sin(yaw);
    Vec2 mid{tl.x + sz * 0.5f, tl.y + sz * 0.5f};
    auto toMap = [&](Vec3 p) {
        float dx = p.x - center.x, dz = p.z - center.z;
        // rotate so the facing direction (yaw) points up the map
        float rx = dx * c - dz * s, rz = dx * s + dz * c;
        return Vec2{mid.x + rx * ppm, mid.y + rz * ppm};
    };
    auto inside = [&](Vec2 p) { return p.x >= tl.x && p.x <= tl.x + sz && p.y >= tl.y && p.y <= tl.y + sz; };
    for (auto& r : roads)
        for (size_t i = 1; i < r.size(); ++i) {
            Vec2 a = toMap(r[i - 1]), b = toMap(r[i]);
            if (!inside(a) && !inside(b)) continue;
            auto clampP = [&](Vec2 p) { return Vec2{std::clamp(p.x, tl.x, tl.x + sz), std::clamp(p.y, tl.y, tl.y + sz)}; };
            ui.line(clampP(a), clampP(b), std::max(2.0f, 6.0f * ppm), {0.55f, 0.58f, 0.62f, 0.9f});
        }
    for (auto& b : blips) {
        Vec2 p = toMap(b.position);
        p = {std::clamp(p.x, tl.x + 3, tl.x + sz - 3), std::clamp(p.y, tl.y + 3, tl.y + sz - 3)};
        ui.rect(p.x - b.size * 0.5f, p.y - b.size * 0.5f, b.size, b.size, b.color);
    }
    if (waypoint) {
        Vec2 p = toMap(*waypoint);
        p = {std::clamp(p.x, tl.x + 5, tl.x + sz - 5), std::clamp(p.y, tl.y + 5, tl.y + sz - 5)};
        ui.circle(p, 5, {1.0f, 0.85f, 0.1f, 1}, 12);
    }
    // the player: an arrow pointing up
    ui.triangle({mid.x, mid.y - 7}, {mid.x + 5, mid.y + 5}, {mid.x - 5, mid.y + 5}, {1, 1, 1, 1});
    ui.frame(tl.x, tl.y, sz, sz, 2, {1, 1, 1, 0.5f});
}

} // namespace fledge::gameplay
