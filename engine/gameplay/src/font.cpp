#include "fledge/gameplay/font.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>

namespace fledge::gameplay {

namespace {
struct Range {
    char32_t first;
    int count;
};
// ASCII, Latin-1 Supplement, Latin Extended-A, Greek and Coptic, Cyrillic: the European languages
constexpr Range kRanges[] = {{32, 95}, {160, 96}, {256, 128}, {880, 144}, {1024, 256}};
constexpr int kSolidRows = 4;  // rows 0..3 are full coverage: untextured UI shapes sample uv (0, 0)
} // namespace

std::u32string decodeUtf8(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        bool ok = n >= 0 && i + static_cast<size_t>(n) < s.size();
        char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1Fu) : n == 2 ? (c & 0x0Fu) : (c & 0x07u);
        for (int k = 1; ok && k <= n; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) {
            out += U'\uFFFD';
            ++i;
            continue;
        }
        out += cp;
        i += static_cast<size_t>(n) + 1;
    }
    return out;
}
struct Font::Impl {
    Bytes ttf;
    stbtt_fontinfo info{};
    float scale = 1;  // stbtt scale for pixelHeight
    std::vector<stbtt_packedchar> chars[std::size(kRanges)];
    bool find(char32_t c, int& range, int& index) const {
        for (size_t r = 0; r < std::size(kRanges); ++r)
            if (c >= kRanges[r].first && c < kRanges[r].first + static_cast<char32_t>(kRanges[r].count)) {
                if (stbtt_FindGlyphIndex(&info, static_cast<int>(c)) == 0) return false;  // not in the font (would pack .notdef)
                range = static_cast<int>(r);
                index = static_cast<int>(c - kRanges[r].first);
                return true;
            }
        return false;
    }
};

Font::~Font() = default;

std::shared_ptr<Font> Font::load(Bytes ttf, float pixelHeight, std::string* error) {
    std::shared_ptr<Font> f(new Font());
    f->m_impl = std::make_unique<Impl>();
    Impl& im = *f->m_impl;
    im.ttf = std::move(ttf);
    int offset = im.ttf.empty() ? -1 : stbtt_GetFontOffsetForIndex(im.ttf.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&im.info, im.ttf.data(), offset)) {
        if (error) *error = "not a TrueType / OpenType font";
        return nullptr;
    }
    f->m_px = std::clamp(pixelHeight, 6.0f, 128.0f);
    im.scale = stbtt_ScaleForPixelHeight(&im.info, f->m_px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&im.info, &asc, &desc, &gap);
    f->m_ascent = static_cast<float>(asc) * im.scale;
    f->m_lineHeight = static_cast<float>(asc - desc + gap) * im.scale;
    // grow the atlas until everything fits
    for (int step = 0; step < 12; ++step) {  // 256x256, 512x256, 512x512, 1024x512 ... up to 4096x4096
        int w = 256 << ((step + 1) / 2), h = 256 << (step / 2);
        std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, pixels.data() + static_cast<size_t>(kSolidRows) * static_cast<size_t>(w), w, h - kSolidRows, w, 1, nullptr)) continue;
        stbtt_PackSetOversampling(&pc, 2, 1);  // horizontal oversampling: smooth sub-pixel placement along lines
        stbtt_pack_range ranges[std::size(kRanges)];
        for (size_t r = 0; r < std::size(kRanges); ++r) {
            im.chars[r].assign(static_cast<size_t>(kRanges[r].count), stbtt_packedchar{});
            ranges[r] = {};
            ranges[r].font_size = f->m_px;
            ranges[r].first_unicode_codepoint_in_range = static_cast<int>(kRanges[r].first);
            ranges[r].num_chars = kRanges[r].count;
            ranges[r].chardata_for_range = im.chars[r].data();
        }
        int ok = stbtt_PackFontRanges(&pc, im.ttf.data(), 0, ranges, static_cast<int>(std::size(kRanges)));
        stbtt_PackEnd(&pc);
        if (!ok) continue;  // too small (missing glyphs alone do not fail)
        std::fill(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(kSolidRows * w), uint8_t{255});
        f->m_w = w;
        f->m_h = h;
        f->m_atlas = std::move(pixels);
        return f;
    }
    if (error) *error = "font atlas would exceed 4096 x 4096 (pixel height too large)";
    return nullptr;
}

std::vector<uint8_t> Font::atlasRgba() const {
    std::vector<uint8_t> out(m_atlas.size() * 4, 255);
    for (size_t i = 0; i < m_atlas.size(); ++i) out[i * 4 + 3] = m_atlas[i];
    return out;
}

bool Font::hasGlyph(char32_t c) const {
    int r, i;
    return m_impl->find(c, r, i);
}

int Font::glyphCount() const {
    int n = 0;
    for (const auto& r : kRanges)
        for (int k = 0; k < r.count; ++k) n += hasGlyph(r.first + static_cast<char32_t>(k)) ? 1 : 0;
    return n;
}

float Font::layout(std::string_view utf8, float x, float y, float scale, std::vector<Quad>* out) const {
    const Impl& im = *m_impl;
    std::u32string text = decodeUtf8(utf8);
    float penX = 0, baseline = m_ascent, widest = 0;
    int prevGlyph = 0;
    const float subH = static_cast<float>(m_h - kSolidRows);
    for (char32_t c : text) {
        if (c == U'\n') {
            widest = std::max(widest, penX);
            penX = 0;
            baseline += m_lineHeight;
            prevGlyph = 0;
            continue;
        }
        int r = 0, i = 0;
        if (!im.find(c, r, i) && !im.find(U'?', r, i)) continue;
        char32_t shown = kRanges[r].first + static_cast<char32_t>(i);
        int glyph = stbtt_FindGlyphIndex(&im.info, static_cast<int>(shown));
        if (prevGlyph) penX += static_cast<float>(stbtt_GetGlyphKernAdvance(&im.info, prevGlyph, glyph)) * im.scale;
        prevGlyph = glyph;
        float qx = penX, qy = 0;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(im.chars[r].data(), m_w, m_h - kSolidRows, i, &qx, &qy, &q, 0);
        if (out && q.x1 > q.x0 && c != U' ' && c != U'\u00A0') {  // spaces advance only
            auto v = [&](float t) { return (t * subH + static_cast<float>(kSolidRows)) / static_cast<float>(m_h); };  // sub-image -> full atlas
            out->push_back({{x + q.x0 * scale, y + (baseline + q.y0) * scale}, {x + q.x1 * scale, y + (baseline + q.y1) * scale}, {q.s0, v(q.t0)}, {q.s1, v(q.t1)}});
        }
        penX = qx;
    }
    return std::max(widest, penX) * scale;
}

Vec2 Font::measure(std::string_view utf8, float scale) const {
    int lines = 1;
    for (char c : utf8) lines += c == '\n' ? 1 : 0;
    return {layout(utf8, 0, 0, scale, nullptr), static_cast<float>(lines) * m_lineHeight * scale};
}

} // namespace fledge::gameplay
