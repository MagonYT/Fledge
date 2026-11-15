// Texture channel packing, coordinate-space conversion, name sanitising and texture-role guessing (see convert.h).
#include "fledge/importers/convert.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>
#include <vector>

#include "fledge/core/utility.h"

namespace fledge::convert {

ImageData packChannels(const Channel& r, const Channel& g, const Channel& b, const Channel& a) {
    const Channel* ch[4] = {&r, &g, &b, &a};
    int w = 1, h = 1;
    for (auto* c : ch)
        if (c->image && c->image->width > 0 && c->image->height > 0) {
            w = std::max(w, c->image->width);
            h = std::max(h, c->image->height);
        }
    ImageData out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int k = 0; k < 4; ++k) {
        const Channel& c = *ch[k];
        bool hasImage = c.image && c.image->width > 0 && c.image->height > 0 &&
                        c.image->rgba.size() >= static_cast<size_t>(c.image->width) * c.image->height * 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float v = c.constant;
                if (hasImage) {
                    int sx = static_cast<int>(static_cast<int64_t>(x) * c.image->width / w);
                    int sy = static_cast<int>(static_cast<int64_t>(y) * c.image->height / h);
                    v = c.image->rgba[(static_cast<size_t>(sy) * c.image->width + sx) * 4 + static_cast<size_t>(std::clamp(c.channel, 0, 3))] / 255.0f;
                }
                v = std::clamp(v * c.scale, 0.0f, 1.0f);
                if (c.invert) v = 1.0f - v;
                out.rgba[(static_cast<size_t>(y) * w + x) * 4 + static_cast<size_t>(k)] = static_cast<uint8_t>(std::lround(v * 255.0f));
            }
    }
    return out;
}

ImageData unityMetallicSmoothnessToOrm(const ImageData* metallic, const ImageData* smoothnessSource, float glossScale,
                                        const ImageData* occlusion) {
    // roughness = 1 - smoothness * glossScale (scale is applied before the inversion)
    Channel roughness{smoothnessSource, 3, true, glossScale, 0.5f};
    return packChannels({occlusion, 1, false, 1.0f, 1.0f}, roughness, {metallic, 0, false, 1.0f, 0.0f}, {});
}

ImageData hdrpMaskToOrm(const ImageData& mask) {
    return packChannels({&mask, 1}, {&mask, 3, true}, {&mask, 0}, {});
}

// ------------------------------------------------------------------ spaces ----
const char* toString(SourceSpace s) {
    switch (s) {
        case SourceSpace::Unity: return "Unity (left-handed, Y-up, metres)";
        case SourceSpace::Unreal: return "Unreal (left-handed, Z-up, centimetres)";
        default: return "Fledge / glTF (right-handed, Y-up, metres)";
    }
}

namespace {
// Change of basis M (columns: where the source axes go). Both source spaces are mirrored relative to Fledge.
Mat4 basis(SourceSpace s) {
    Mat4 m;
    if (s == SourceSpace::Unity) {
        m.c[2] = {0, 0, -1, 0};  // Unity +Z (forward) -> Fledge -Z (forward)
    } else if (s == SourceSpace::Unreal) {
        m.c[0] = {0, 0, -1, 0};  // Unreal +X (forward) -> Fledge -Z
        m.c[1] = {1, 0, 0, 0};   // Unreal +Y (right)   -> Fledge +X
        m.c[2] = {0, 1, 0, 0};   // Unreal +Z (up)      -> Fledge +Y
    }
    return m;
}
bool mirrors(SourceSpace s) { return s != SourceSpace::Fledge; }
} // namespace

Vec3 toFledgeDirection(Vec3 d, SourceSpace s) { return (basis(s) * Vec4(d, 0.0f)).xyz(); }
Vec3 toFledgePoint(Vec3 p, SourceSpace s) { return toFledgeDirection(p, s) * (s == SourceSpace::Unreal ? 0.01f : 1.0f); }
Quat toFledgeRotation(Quat q, SourceSpace s) {
    if (s == SourceSpace::Fledge) return q;
    Mat4 m = basis(s);
    return quatFromMatrix(m * Mat4::rotation(q) * m.transposed()).normalized();
}

void convertMesh(MeshData& mesh, SourceSpace s, float extraScale) {
    for (auto& v : mesh.vertices) {
        v.position = toFledgePoint(v.position, s) * extraScale;
        v.normal = normalize(toFledgeDirection(v.normal, s));
    }
    for (auto& t : mesh.tangents) {
        Vec3 d = toFledgeDirection(t.xyz(), s);
        t = Vec4(d, mirrors(s) ? -t.w : t.w);
    }
    if (mirrors(s)) {
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
        for (auto& lod : mesh.lods)
            for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) std::swap(lod.indices[i + 1], lod.indices[i + 2]);
    }
}

// ------------------------------------------------------------------- names ----
std::string sanitizeName(std::string_view s, const std::string& fallback, size_t maxLen) {
    std::string out;
    for (unsigned char c : s) {
        bool ok = std::isalnum(c) || c == '_' || c == '-' || c == '.';
        out += ok ? static_cast<char>(c) : '_';
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());  // no hidden files, no "." / ".."
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.size() > maxLen) out.resize(maxLen);
    std::string stem = toLower(out.substr(0, out.find('.')));
    for (const char* r : {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "lpt1", "lpt2", "lpt3"})
        if (stem == r) out = "_" + out;
    return out.empty() ? fallback : out;
}

const char* toString(TextureRole r) {
    switch (r) {
        case TextureRole::BaseColor: return "BaseColor";
        case TextureRole::Normal: return "Normal";
        case TextureRole::Orm: return "ORM";
        case TextureRole::Roughness: return "Roughness";
        case TextureRole::Metallic: return "Metallic";
        case TextureRole::Occlusion: return "Occlusion";
        case TextureRole::Emissive: return "Emissive";
        case TextureRole::Opacity: return "Opacity";
        case TextureRole::Height: return "Height";
        case TextureRole::Smoothness: return "Smoothness";
        case TextureRole::MetallicSmoothness: return "MetallicSmoothness";
        default: return "Unknown";
    }
}

TextureRole guessTextureRole(std::string_view fileName, std::string* setName) {
    std::string name(fileName);
    if (auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    name = name.substr(0, name.rfind('.'));
    std::string l = toLower(name);
    // longest suffixes first; a suffix must follow a separator ('_', '-', ' ', '.') or a case change is not needed
    static const std::vector<std::pair<const char*, TextureRole>> kSuffixes = {
        {"occlusionroughnessmetallic", TextureRole::Orm}, {"metallicsmoothness", TextureRole::MetallicSmoothness},
        {"ambientocclusion", TextureRole::Occlusion}, {"metallicgloss", TextureRole::MetallicSmoothness},
        {"basecolor", TextureRole::BaseColor}, {"base_color", TextureRole::BaseColor}, {"basecolour", TextureRole::BaseColor},
        {"displacement", TextureRole::Height}, {"smoothness", TextureRole::Smoothness}, {"glossiness", TextureRole::Smoothness},
        {"roughness", TextureRole::Roughness}, {"metalness", TextureRole::Metallic}, {"occlusion", TextureRole::Occlusion},
        {"normalmap", TextureRole::Normal}, {"normalgl", TextureRole::Normal}, {"normaldx", TextureRole::Normal},
        {"emissive", TextureRole::Emissive}, {"emission", TextureRole::Emissive}, {"metallic", TextureRole::Metallic},
        {"diffuse", TextureRole::BaseColor}, {"opacity", TextureRole::Opacity}, {"albedo", TextureRole::BaseColor},
        {"normal", TextureRole::Normal}, {"height", TextureRole::Height}, {"colour", TextureRole::BaseColor},
        {"color", TextureRole::BaseColor}, {"alpha", TextureRole::Opacity}, {"gloss", TextureRole::Smoothness},
        {"rough", TextureRole::Roughness}, {"metal", TextureRole::Metallic}, {"emit", TextureRole::Emissive},
        {"diff", TextureRole::BaseColor}, {"disp", TextureRole::Height}, {"nrm", TextureRole::Normal}, {"nor", TextureRole::Normal},
        {"orm", TextureRole::Orm}, {"arm", TextureRole::Orm}, {"rgh", TextureRole::Roughness}, {"col", TextureRole::BaseColor},
        {"ao", TextureRole::Occlusion}, {"bc", TextureRole::BaseColor}, {"ms", TextureRole::MetallicSmoothness},
        {"d", TextureRole::BaseColor}, {"n", TextureRole::Normal}, {"r", TextureRole::Roughness}, {"m", TextureRole::Metallic},
        {"e", TextureRole::Emissive}, {"h", TextureRole::Height}};
    for (auto& [suffix, role] : kSuffixes) {
        std::string_view sv(suffix);
        if (l.size() <= sv.size() || l.compare(l.size() - sv.size(), sv.size(), sv) != 0) continue;
        char sep = l[l.size() - sv.size() - 1];
        bool separated = sep == '_' || sep == '-' || sep == ' ' || sep == '.';
        if (!separated && sv.size() <= 3) continue;  // short codes need a separator ("T_Rock_N", not "Iron")
        if (setName) {
            std::string base = name.substr(0, name.size() - sv.size());
            while (!base.empty() && (base.back() == '_' || base.back() == '-' || base.back() == ' ' || base.back() == '.')) base.pop_back();
            for (const char* prefix : {"T_", "t_", "TX_", "tx_", "Tex_", "tex_"})
                if (startsWith(base, prefix) && base.size() > std::char_traits<char>::length(prefix)) {
                    base = base.substr(std::char_traits<char>::length(prefix));
                    break;
                }
            *setName = base;
        }
        return role;
    }
    if (setName) *setName = name;
    return TextureRole::Unknown;
}

} // namespace fledge::convert
