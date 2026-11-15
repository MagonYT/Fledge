// Texture cooking: mip chain + block compression (stb_dxt, public domain/MIT,
// fetched pinned with the rest of stb). Only linked into tools/editor - the
// runtime never references these functions.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "fledge/assets/cooked.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fledge {

namespace {
struct SrgbTables {
    float toLinear[256];
    SrgbTables() {
        for (int i = 0; i < 256; ++i) {
            float c = static_cast<float>(i) / 255.0f;
            toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    }
};
const SrgbTables& srgbTables() {
    static const SrgbTables t;
    return t;
}
uint8_t linearToSrgb8(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(s * 255.0f));
}
} // namespace

std::vector<ImageData> generateMipChain(const ImageData& image, bool srgb) {
    const auto& lin = srgbTables().toLinear;
    std::vector<ImageData> chain{image};
    while (chain.back().width > 1 || chain.back().height > 1) {
        const ImageData& s = chain.back();
        ImageData d;
        d.width = std::max(1, s.width / 2);
        d.height = std::max(1, s.height / 2);
        d.rgba.resize(static_cast<size_t>(d.width) * static_cast<size_t>(d.height) * 4);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                for (int c = 0; c < 4; ++c) {
                    bool gamma = srgb && c < 3;  // alpha is always linear
                    float sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            int sx = std::min(s.width - 1, x * 2 + dx), sy = std::min(s.height - 1, y * 2 + dy);
                            uint8_t v = s.rgba[(static_cast<size_t>(sy) * static_cast<size_t>(s.width) + static_cast<size_t>(sx)) * 4 + static_cast<size_t>(c)];
                            sum += gamma ? lin[v] : static_cast<float>(v) / 255.0f;
                        }
                    sum *= 0.25f;
                    d.rgba[(static_cast<size_t>(y) * static_cast<size_t>(d.width) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] =
                        gamma ? linearToSrgb8(sum) : static_cast<uint8_t>(std::lround(sum * 255.0f));
                }
        chain.push_back(std::move(d));
    }
    return chain;
}

TextureData uncompressedTexture(const ImageData& image, bool mips, bool srgb) {
    TextureData t;
    t.format = TextureFormat::RGBA8;
    t.srgb = srgb;
    auto chain = mips ? generateMipChain(image, srgb) : std::vector<ImageData>{image};
    for (auto& m : chain) t.mips.push_back({m.width, m.height, std::move(m.rgba)});
    return t;
}

TextureData cookTexture(const ImageData& image, const TextureCookOptions& opt) {
    TextureCompression c = opt.compression;
    if (c == TextureCompression::Auto) c = image.hasAlpha() ? TextureCompression::BC3 : TextureCompression::BC1;
    if (c == TextureCompression::None) return uncompressedTexture(image, opt.mips, opt.srgb);
    TextureData t;
    t.srgb = opt.srgb;
    t.format = c == TextureCompression::BC1 ? TextureFormat::BC1 : TextureFormat::BC3;
    bool alpha = t.format == TextureFormat::BC3;
    size_t blockSize = alpha ? 16 : 8;
    auto chain = opt.mips ? generateMipChain(image, opt.srgb) : std::vector<ImageData>{image};
    for (const auto& m : chain) {
        TextureMip mip;
        mip.width = m.width;
        mip.height = m.height;
        int bw = (m.width + 3) / 4, bh = (m.height + 3) / 4;
        mip.data.resize(static_cast<size_t>(bw) * static_cast<size_t>(bh) * blockSize);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                uint8_t block[64];
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {  // replicate edge pixels for partial blocks
                        int sx = std::min(m.width - 1, bx * 4 + x), sy = std::min(m.height - 1, by * 4 + y);
                        std::memcpy(block + (y * 4 + x) * 4, &m.rgba[(static_cast<size_t>(sy) * static_cast<size_t>(m.width) + static_cast<size_t>(sx)) * 4], 4);
                    }
                stb_compress_dxt_block(mip.data.data() + (static_cast<size_t>(by) * static_cast<size_t>(bw) + static_cast<size_t>(bx)) * blockSize,
                                       block, alpha ? 1 : 0, STB_DXT_HIGHQUAL);
            }
        t.mips.push_back(std::move(mip));
    }
    return t;
}

} // namespace fledge
