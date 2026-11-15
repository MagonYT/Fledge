#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "fledge/assets/cooked.h"

namespace fledge {

namespace {
struct Writer {
    Bytes b;
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
    void raw(const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
};
struct Reader {
    std::span<const uint8_t> d;
    size_t pos = 0;
    bool ok = true;
    uint32_t u32() {
        if (pos + 4 > d.size()) { ok = false; return 0; }
        uint32_t v = static_cast<uint32_t>(d[pos]) | (static_cast<uint32_t>(d[pos + 1]) << 8) | (static_cast<uint32_t>(d[pos + 2]) << 16) |
                     (static_cast<uint32_t>(d[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    float f32() {
        uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    size_t remaining() const { return pos <= d.size() ? d.size() - pos : 0; }
    bool raw(void* out, size_t n) {
        if (pos + n > d.size()) { ok = false; return false; }
        std::memcpy(out, d.data() + pos, n);
        pos += n;
        return true;
    }
    bool magic(const char* m) {
        if (pos + 4 > d.size() || std::memcmp(d.data() + pos, m, 4) != 0) return ok = false;
        pos += 4;
        return true;
    }
};
template <typename T>
std::optional<T> fail(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return std::nullopt;
}
} // namespace

// ------------------------------------------------------------------ mesh ---
Bytes writeCookedMesh(const MeshData& mesh) {
    bool tangents = mesh.tangents.size() == mesh.vertices.size() && !mesh.tangents.empty();
    Vec3 mn, mx;
    meshBounds(mesh, mn, mx);
    Writer w;
    w.raw("CMSH", 4);
    w.u32(1);
    w.u32(static_cast<uint32_t>(mesh.vertices.size()));
    w.u32(static_cast<uint32_t>(mesh.indices.size()));
    w.u32(tangents ? 1u : 0u);
    for (int i = 0; i < 3; ++i) w.f32(mn[i]);
    for (int i = 0; i < 3; ++i) w.f32(mx[i]);
    w.u32(tangents ? 48u : 32u);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vertex& v = mesh.vertices[i];
        for (float f : {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y}) w.f32(f);
        if (tangents)
            for (int k = 0; k < 4; ++k) w.f32(mesh.tangents[i][k]);
    }
    for (uint32_t idx : mesh.indices) w.u32(idx);
    if (!mesh.lods.empty()) {  // optional trailer (older readers stop before it): LOD index lists
        w.raw("LODS", 4);
        w.u32(static_cast<uint32_t>(mesh.lods.size()));
        for (const MeshLod& l : mesh.lods) {
            w.f32(l.error);
            w.u32(static_cast<uint32_t>(l.indices.size()));
            for (uint32_t idx : l.indices) w.u32(idx);
        }
    }
    return w.b;
}

std::optional<MeshData> readCookedMesh(std::span<const uint8_t> bytes, std::string* error) {
    Reader r{bytes};
    if (!r.magic("CMSH")) return fail<MeshData>(error, "not a .cmesh file");
    if (r.u32() != 1) return fail<MeshData>(error, "unsupported .cmesh version");
    uint32_t vc = r.u32(), ic = r.u32(), flags = r.u32();
    for (int i = 0; i < 6; ++i) r.f32();  // bounds (recomputable; kept for tools)
    uint32_t stride = r.u32();
    bool tangents = flags & 1;
    if (!r.ok || stride != (tangents ? 48u : 32u)) return fail<MeshData>(error, "corrupt .cmesh header");
    if (static_cast<uint64_t>(vc) * stride + static_cast<uint64_t>(ic) * 4 > bytes.size()) return fail<MeshData>(error, "truncated .cmesh");
    MeshData m;
    m.vertices.resize(vc);
    if (tangents) m.tangents.resize(vc);
    for (uint32_t i = 0; i < vc; ++i) {
        Vertex& v = m.vertices[i];
        v.position = {r.f32(), r.f32(), r.f32()};
        v.normal = {r.f32(), r.f32(), r.f32()};
        v.uv = {r.f32(), r.f32()};
        if (tangents) m.tangents[i] = {r.f32(), r.f32(), r.f32(), r.f32()};
    }
    m.indices.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        m.indices[i] = r.u32();
        if (m.indices[i] >= vc) return fail<MeshData>(error, "index out of range in .cmesh");
    }
    if (!r.ok) return fail<MeshData>(error, "truncated .cmesh");
    if (r.remaining() >= 8 && r.magic("LODS")) {
        uint32_t n = r.u32();
        for (uint32_t l = 0; l < n && r.ok; ++l) {
            MeshLod lod;
            lod.error = r.f32();
            uint32_t c = r.u32();
            if (static_cast<uint64_t>(c) * 4 > r.remaining()) return fail<MeshData>(error, "truncated .cmesh LODs");
            lod.indices.resize(c);
            for (uint32_t i = 0; i < c; ++i) {
                lod.indices[i] = r.u32();
                if (lod.indices[i] >= vc) return fail<MeshData>(error, "LOD index out of range in .cmesh");
            }
            m.lods.push_back(std::move(lod));
        }
    }
    return m;
}

// --------------------------------------------------------------- texture ---
const char* toString(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8: return "RGBA8";
        case TextureFormat::BC1: return "BC1";
        case TextureFormat::BC3: return "BC3";
        case TextureFormat::RGBA16F: return "RGBA16F";
    }
    return "?";
}

TextureCompression textureCompressionFromString(std::string_view s) {
    if (s == "none") return TextureCompression::None;
    if (s == "bc1") return TextureCompression::BC1;
    if (s == "bc3") return TextureCompression::BC3;
    return TextureCompression::Auto;
}
const char* toString(TextureCompression c) {
    switch (c) {
        case TextureCompression::None: return "none";
        case TextureCompression::BC1: return "bc1";
        case TextureCompression::BC3: return "bc3";
        case TextureCompression::Auto: return "auto";
    }
    return "?";
}

static size_t mipBytes(TextureFormat f, int w, int h) {
    size_t bw = static_cast<size_t>((w + 3) / 4), bh = static_cast<size_t>((h + 3) / 4);
    switch (f) {
        case TextureFormat::RGBA8: return static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        case TextureFormat::BC1: return bw * bh * 8;
        case TextureFormat::BC3: return bw * bh * 16;
        case TextureFormat::RGBA16F: return static_cast<size_t>(w) * static_cast<size_t>(h) * 8;
    }
    return 0;
}

Bytes writeCookedTexture(const TextureData& t) {
    Writer w;
    w.raw("CTEX", 4);
    w.u32(1);
    w.u32(static_cast<uint32_t>(t.format));
    w.u32(static_cast<uint32_t>(t.width()));
    w.u32(static_cast<uint32_t>(t.height()));
    w.u32(static_cast<uint32_t>(t.mips.size()));
    w.u32(t.srgb ? 1u : 0u);
    for (auto& m : t.mips) {
        w.u32(static_cast<uint32_t>(m.width));
        w.u32(static_cast<uint32_t>(m.height));
        w.u32(static_cast<uint32_t>(m.data.size()));
        w.raw(m.data.data(), m.data.size());
    }
    return w.b;
}

std::optional<TextureData> readCookedTexture(std::span<const uint8_t> bytes, std::string* error) {
    Reader r{bytes};
    if (!r.magic("CTEX")) return fail<TextureData>(error, "not a .ctex file");
    if (r.u32() != 1) return fail<TextureData>(error, "unsupported .ctex version");
    TextureData t;
    uint32_t fmt = r.u32();
    if (fmt > 3) return fail<TextureData>(error, "unknown texture format");
    t.format = static_cast<TextureFormat>(fmt);
    r.u32();
    r.u32();
    uint32_t mipCount = r.u32();
    t.srgb = r.u32() & 1;
    if (!r.ok || mipCount == 0 || mipCount > 16) return fail<TextureData>(error, "corrupt .ctex header");
    for (uint32_t i = 0; i < mipCount; ++i) {
        TextureMip m;
        m.width = static_cast<int>(r.u32());
        m.height = static_cast<int>(r.u32());
        uint32_t size = r.u32();
        if (!r.ok || m.width <= 0 || m.height <= 0 || size != mipBytes(t.format, m.width, m.height))
            return fail<TextureData>(error, std::format("corrupt mip {} in .ctex", i));
        m.data.resize(size);
        if (!r.raw(m.data.data(), size)) return fail<TextureData>(error, "truncated .ctex");
        t.mips.push_back(std::move(m));
    }
    return t;
}

namespace {
void decodeColorBlock(const uint8_t* b, uint8_t out[16][4], bool forceFourColor) {
    uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8)), c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    auto expand = [](uint16_t c, uint8_t rgb[3]) {
        rgb[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
        rgb[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        rgb[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
    };
    uint8_t pal[4][4] = {};
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    bool four = forceFourColor || c0 > c1;
    for (int k = 0; k < 3; ++k) {
        if (four) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = four ? 255 : 0;
    uint32_t idx = static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) | (static_cast<uint32_t>(b[6]) << 16) |
                   (static_cast<uint32_t>(b[7]) << 24);
    for (int p = 0; p < 16; ++p) std::memcpy(out[p], pal[(idx >> (2 * p)) & 3], 4);
}

void decodeAlphaBlock(const uint8_t* b, uint8_t out[16][4]) {
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; ++i) a[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
        for (int i = 1; i < 5; ++i) a[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    for (int p = 0; p < 16; ++p) out[p][3] = a[(bits >> (3 * p)) & 7];
}
} // namespace

TextureData decompressToRgba8(const TextureData& tex) {
    if (tex.format == TextureFormat::RGBA8 || tex.format == TextureFormat::RGBA16F) return tex;
    TextureData out;
    out.format = TextureFormat::RGBA8;
    out.srgb = tex.srgb;
    size_t blockSize = tex.format == TextureFormat::BC1 ? 8 : 16;
    for (const auto& m : tex.mips) {
        TextureMip o;
        o.width = m.width;
        o.height = m.height;
        o.data.assign(static_cast<size_t>(m.width) * static_cast<size_t>(m.height) * 4, 0);
        int bw = (m.width + 3) / 4, bh = (m.height + 3) / 4;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* blk = m.data.data() + (static_cast<size_t>(by) * static_cast<size_t>(bw) + static_cast<size_t>(bx)) * blockSize;
                uint8_t px[16][4];
                if (tex.format == TextureFormat::BC1) decodeColorBlock(blk, px, false);
                else {
                    decodeColorBlock(blk + 8, px, true);
                    decodeAlphaBlock(blk, px);
                }
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        int X = bx * 4 + x, Y = by * 4 + y;
                        if (X < m.width && Y < m.height)
                            std::memcpy(&o.data[(static_cast<size_t>(Y) * static_cast<size_t>(m.width) + static_cast<size_t>(X)) * 4], px[y * 4 + x], 4);
                    }
            }
        out.mips.push_back(std::move(o));
    }
    return out;
}

// --------------------------------------------------------------- cooking ---
uint64_t contentHash(std::span<const uint8_t> data, uint64_t seed) {
    uint64_t h = seed;
    for (uint8_t b : data) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::string cookedPathFor(const std::string& relPath) {
    std::string l = toLower(relPath);
    if (endsWith(l, ".cmesh") || endsWith(l, ".ctex")) return relPath;
    if (endsWith(l, ".obj")) return relPath + ".cmesh";
    for (const char* e : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr"})
        if (endsWith(l, e)) return relPath + ".ctex";
    return {};
}

TextureData hdrToTexture(const HdrImage& img, int maxWidth) {
    HdrImage src = img;
    while (src.width > maxWidth && src.width > 1 && src.height > 1) {  // 2x box downscale
        HdrImage d;
        d.width = src.width / 2;
        d.height = std::max(1, src.height / 2);
        d.rgba.resize(static_cast<size_t>(d.width) * static_cast<size_t>(d.height) * 4);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                for (int c = 0; c < 4; ++c) {
                    float sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            sum += src.rgba[(static_cast<size_t>(std::min(src.height - 1, y * 2 + dy)) * static_cast<size_t>(src.width) +
                                             static_cast<size_t>(x * 2 + dx)) * 4 + static_cast<size_t>(c)];
                    d.rgba[(static_cast<size_t>(y) * static_cast<size_t>(d.width) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] = sum * 0.25f;
                }
        src = std::move(d);
    }
    TextureData t;
    t.format = TextureFormat::RGBA16F;
    t.srgb = false;
    TextureMip m;
    m.width = src.width;
    m.height = src.height;
    m.data.resize(src.rgba.size() * 2);
    for (size_t i = 0; i < src.rgba.size(); ++i) {
        uint16_t h = floatToHalf(src.rgba[i]);
        m.data[i * 2] = static_cast<uint8_t>(h & 0xFF);
        m.data[i * 2 + 1] = static_cast<uint8_t>(h >> 8);
    }
    t.mips.push_back(std::move(m));
    return t;
}

HdrImage textureToHdr(const TextureData& tex) {
    HdrImage out;
    if (tex.mips.empty()) return out;
    TextureData src = tex.format == TextureFormat::BC1 || tex.format == TextureFormat::BC3 ? decompressToRgba8(tex) : tex;
    const TextureMip& m = src.mips[0];
    out.width = m.width;
    out.height = m.height;
    size_t n = static_cast<size_t>(m.width) * static_cast<size_t>(m.height) * 4;
    out.rgba.resize(n);
    for (size_t i = 0; i < n; ++i) {
        if (src.format == TextureFormat::RGBA16F) {
            out.rgba[i] = halfToFloat(static_cast<uint16_t>(m.data[i * 2] | (m.data[i * 2 + 1] << 8)));
        } else {
            float v = static_cast<float>(m.data[i]) / 255.0f;
            out.rgba[i] = (i % 4 == 3 || !src.srgb) ? v : std::pow(v, 2.2f);
        }
    }
    return out;
}

bool isEditorOnlyFile(const std::string& relPath) {
    std::string l = toLower(relPath);
    // .fbx: model sources (imported to .cmesh / .fmodel); .fimport: store import reports and provenance (1.1.0)
    return endsWith(l, ".meta") || endsWith(l, ".gltf") || endsWith(l, ".glb") || endsWith(l, ".bin") || endsWith(l, ".fbx") ||
           endsWith(l, ".fimport");
}

} // namespace fledge
