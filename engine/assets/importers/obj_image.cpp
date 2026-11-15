// OBJ parser and stb-based image codec.
#include <charconv>
#include <sstream>

#include "fledge/importers/importers.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fledge {

namespace {
struct ObjIndex { int v = 0, vt = 0, vn = 0; };

bool parseIndex(std::string_view tok, ObjIndex& out) {
    int vals[3] = {0, 0, 0};
    int slot = 0;
    size_t start = 0;
    for (size_t i = 0; i <= tok.size(); ++i) {
        if (i == tok.size() || tok[i] == '/') {
            if (slot > 2) return false;
            if (i > start) {
                auto r = std::from_chars(tok.data() + start, tok.data() + i, vals[slot]);
                if (r.ec != std::errc()) return false;
            }
            ++slot;
            start = i + 1;
        }
    }
    out = {vals[0], vals[1], vals[2]};
    return out.v != 0;
}

int fixIndex(int idx, size_t count) { return idx > 0 ? idx - 1 : static_cast<int>(count) + idx; }
} // namespace

std::optional<MeshData> parseObj(std::string_view text, std::string* error) {
    std::vector<Vec3> pos, nrm;
    std::vector<Vec2> uvs;
    MeshData mesh;
    bool needNormals = false;
    std::istringstream in{std::string(text)};
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& msg) -> std::optional<MeshData> {
        if (error) *error = "line " + std::to_string(lineNo) + ": " + msg;
        return std::nullopt;
    };
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            Vec3 p; ls >> p.x >> p.y >> p.z;
            if (!ls) return fail("bad vertex");
            pos.push_back(p);
        } else if (tag == "vn") {
            Vec3 n; ls >> n.x >> n.y >> n.z;
            nrm.push_back(n);
        } else if (tag == "vt") {
            Vec2 t; ls >> t.x >> t.y;
            uvs.push_back(t);
        } else if (tag == "f") {
            std::vector<ObjIndex> face;
            std::string tok;
            while (ls >> tok) {
                ObjIndex idx;
                if (!parseIndex(tok, idx)) return fail("bad face index '" + tok + "'");
                face.push_back(idx);
            }
            if (face.size() < 3) return fail("face with < 3 vertices");
            auto base = static_cast<uint32_t>(mesh.vertices.size());
            for (auto& fi : face) {
                int vi = fixIndex(fi.v, pos.size());
                if (vi < 0 || vi >= static_cast<int>(pos.size())) return fail("vertex index out of range");
                Vertex v;
                v.position = pos[static_cast<size_t>(vi)];
                if (fi.vt) {
                    int ti = fixIndex(fi.vt, uvs.size());
                    if (ti >= 0 && ti < static_cast<int>(uvs.size())) v.uv = uvs[static_cast<size_t>(ti)];
                }
                if (fi.vn) {
                    int ni = fixIndex(fi.vn, nrm.size());
                    if (ni >= 0 && ni < static_cast<int>(nrm.size())) v.normal = normalize(nrm[static_cast<size_t>(ni)]);
                } else {
                    needNormals = true;
                }
                mesh.vertices.push_back(v);
            }
            for (uint32_t k = 1; k + 1 < face.size(); ++k) mesh.indices.insert(mesh.indices.end(), {base, base + k, base + k + 1});
        }
        // other tags (o, g, s, usemtl, mtllib) are ignored
    }
    if (mesh.empty()) return fail("no faces");
    if (needNormals) {  // flat normals per triangle
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            auto& a = mesh.vertices[mesh.indices[i]];
            auto& b = mesh.vertices[mesh.indices[i + 1]];
            auto& c = mesh.vertices[mesh.indices[i + 2]];
            Vec3 n = normalize(cross(b.position - a.position, c.position - a.position));
            for (Vertex* v : {&a, &b, &c})
                if (length(v->normal) < 0.5f) v->normal = n;
        }
    }
    return mesh;
}

std::optional<ImageData> decodeImage(std::span<const uint8_t> bytes, std::string* error) {
    int w, h, comp;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!px) {
        if (error) *error = stbi_failure_reason();
        return std::nullopt;
    }
    ImageData img;
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(px);
    return img;
}

std::optional<HdrImage> decodeHdr(std::span<const uint8_t> bytes, std::string* error) {
    int w, h, comp;
    float* px = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!px) {
        if (error) *error = stbi_failure_reason();
        return std::nullopt;
    }
    HdrImage img;
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(px);
    return img;
}

} // namespace fledge
