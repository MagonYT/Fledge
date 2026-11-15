// FBX importer (ufbx v0.23.1, MIT / public domain): meshes, hierarchy, materials, textures, skin + animation.
// Output layout matches the glTF importer: <destDir>/<name>/{Meshes, Materials, Textures, source}/ + <name>.fmodel
// (+ <name>.fchar when skinned). ufbx converts the file's declared axes / units to Fledge space (right-handed, Y-up,
// metres) and never opens external files (load_external_files = false).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "fledge/core/log.h"
#include "fledge/importers/archive.h"
#include "fledge/importers/importers.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <ufbx.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fledge {

namespace {

using SceneGuard = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;

std::string str(const ufbx_string& s) { return std::string(s.data, s.length); }

SceneGuard loadScene(std::span<const uint8_t> bytes, std::string& error) {
    ufbx_load_opts o{};
    o.target_axes = ufbx_axes_right_handed_y_up;
    o.target_unit_meters = 1.0f;
    o.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    o.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    o.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    o.generate_missing_normals = true;
    o.load_external_files = false;  // never follow references to other files
    o.ignore_missing_external_files = true;
    o.clean_skin_weights = true;
    o.node_depth_limit = 512;
    o.temp_allocator.memory_limit = static_cast<size_t>(3ull << 30);   // hostile files cannot exhaust memory
    o.result_allocator.memory_limit = static_cast<size_t>(3ull << 30);
    o.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_REPLACEMENT_CHARACTER;
    ufbx_error err{};
    ufbx_scene* s = ufbx_load_memory(bytes.data(), bytes.size(), &o, &err);
    if (!s) {
        error = "not a readable FBX file: " + str(err.description);
        if (err.info_length) error += std::string(" (") + err.info + ")";
    }
    return SceneGuard(s, &ufbx_free_scene);
}

Vec3 v3(const ufbx_vec3& v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
Quat q4(const ufbx_quat& q) { return Quat{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)}.normalized(); }
Mat4 m4(const ufbx_matrix& m) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) r.c[c] = Vec4(v3(m.cols[c]), c == 3 ? 1.0f : 0.0f);
    return r;
}
Transform tr(const ufbx_transform& t) { return {v3(t.translation), q4(t.rotation), v3(t.scale)}; }

struct VertexKey {
    Vertex v;
    bool operator==(const VertexKey& o) const { return std::memcmp(&v, &o.v, sizeof(Vertex)) == 0; }
};
struct VertexHash {
    size_t operator()(const VertexKey& k) const {
        auto b = std::span(reinterpret_cast<const uint8_t*>(&k.v), sizeof(Vertex));
        return static_cast<size_t>(contentHash(b));
    }
};

// One triangulated mesh part. cornerToVertex (optional) receives the ufbx vertex index of every output vertex.
MeshData buildPart(const ufbx_mesh& m, std::span<const uint32_t> faces, std::vector<uint32_t>* outVertexOf = nullptr) {
    MeshData md;
    std::vector<uint32_t> tri(m.max_face_triangles * 3 + 3);
    std::unordered_map<VertexKey, uint32_t, VertexHash> dedupe;
    for (uint32_t fi : faces) {
        if (fi >= m.faces.count) continue;
        ufbx_face f = m.faces.data[fi];
        if (f.num_indices < 3) continue;
        uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), &m, f);
        for (uint32_t k = 0; k < n * 3; ++k) {
            uint32_t ix = tri[k];
            Vertex v{};
            v.position = v3(ufbx_get_vertex_vec3(&m.vertex_position, ix));
            v.normal = m.vertex_normal.exists ? normalize(v3(ufbx_get_vertex_vec3(&m.vertex_normal, ix))) : Vec3{0, 1, 0};
            if (m.vertex_uv.exists) {
                ufbx_vec2 uv = ufbx_get_vertex_vec2(&m.vertex_uv, ix);
                v.uv = {static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y)};  // FBX: bottom-left origin; Fledge/glTF: top-left
            }
            auto [it, fresh] = dedupe.try_emplace(VertexKey{v}, static_cast<uint32_t>(md.vertices.size()));
            if (fresh) {
                md.vertices.push_back(v);
                if (outVertexOf) outVertexOf->push_back(m.vertex_indices.data[ix]);
            }
            md.indices.push_back(it->second);
        }
    }
    return md;
}

struct FbxWriter {
    Vfs& vfs;
    AssetRegistry& reg;
    ImportResult& res;
    Json importInfo;
    AssetRecord write(const std::string& rel, std::span<const uint8_t> bytes, AssetType type, const Json& extraMeta = Json::object()) {
        if (!vfs.write("content:/" + rel, bytes)) throw std::runtime_error("cannot write content:/" + rel);
        AssetRecord rec;
        if (type != AssetType::Other) {
            Guid g = reg.add(rel, type);
            rec = *reg.find(g);
            Json info = importInfo;
            for (auto& [k, v] : extraMeta.items()) info[k] = v;
            reg.writeMeta(vfs, rec, info);
        } else {
            rec.path = rel;
            rec.type = type;
        }
        res.created.push_back(rec);
        return rec;
    }
};

} // namespace

Result<anim::Character> importFbxCharacter(std::span<const uint8_t> bytes, std::vector<std::string>* warnings, bool fixZeroWeights,
                                           bool fixZeroNormals) {
    using R = Result<anim::Character>;
    std::string err;
    SceneGuard scene = loadScene(bytes, err);
    if (!scene) return R::failure(err);
    const ufbx_scene& s = *scene;
    const ufbx_skin_deformer* skin = nullptr;
    const ufbx_node* meshNode = nullptr;
    for (size_t i = 0; i < s.nodes.count && !skin; ++i) {
        const ufbx_node* n = s.nodes.data[i];
        if (n->mesh && n->mesh->skin_deformers.count) {
            skin = n->mesh->skin_deformers.data[0];
            meshNode = n;
        }
    }
    if (!skin) return R::failure("the FBX file has no skinned mesh");
    // Skeleton: every cluster bone plus their ancestors below the scene root, parents before children.
    std::set<const ufbx_node*> boneSet;
    for (size_t c = 0; c < skin->clusters.count; ++c)
        for (const ufbx_node* n = skin->clusters.data[c]->bone_node; n && !n->is_root; n = n->parent) boneSet.insert(n);
    std::vector<const ufbx_node*> order;
    std::function<void(const ufbx_node*)> visit = [&](const ufbx_node* n) {
        if (boneSet.contains(n)) order.push_back(n);
        for (size_t c = 0; c < n->children.count; ++c) visit(n->children.data[c]);
    };
    visit(s.root_node);
    std::map<const ufbx_node*, int> boneOf;
    for (size_t i = 0; i < order.size(); ++i) boneOf[order[i]] = static_cast<int>(i);
    anim::Character ch;
    ch.name = meshNode->name.length ? str(meshNode->name) : "Character";
    for (const ufbx_node* n : order) {
        anim::Bone b;
        b.name = n->name.length ? str(n->name) : std::format("bone{}", ch.skeleton.bones.size());
        b.parent = boneOf.contains(n->parent) ? boneOf[n->parent] : -1;
        // top bones: relative to the scene root (world); others: local
        b.rest = b.parent < 0 ? decompose(m4(n->node_to_world)) : tr(n->local_transform);
        ch.skeleton.bones.push_back(b);
    }
    ch.skeleton.computeInverseBind();
    // Bind matrices from the clusters (mesh in world space -> bone), so a default pose that differs from the bind pose works.
    Mat4 geometryToWorld = m4(meshNode->geometry_to_world);
    Mat4 worldToGeometry = geometryToWorld.inverse();
    for (size_t c = 0; c < skin->clusters.count; ++c) {
        const ufbx_skin_cluster* cl = skin->clusters.data[c];
        if (cl->bone_node && boneOf.contains(cl->bone_node))
            ch.skeleton.bones[static_cast<size_t>(boneOf[cl->bone_node])].inverseBind = m4(cl->geometry_to_bone) * worldToGeometry;
    }
    // Skinned mesh (all parts), in world space.
    const ufbx_mesh& m = *meshNode->mesh;
    std::vector<uint32_t> vertexOf;
    if (m.material_parts.count > 1) {
        // 1.3.2 material slots: one part per material, concatenated (one skin, one pose); named after the material so importFbx
        // can give each slot the .fmat it wrote
        for (size_t pi = 0; pi < m.material_parts.count; ++pi) {
            const ufbx_mesh_part& part = m.material_parts.data[pi];
            if (part.num_triangles == 0) continue;
            std::vector<uint32_t> partOf;
            MeshData md = buildPart(m, std::span(part.face_indices.data, part.face_indices.count), &partOf);
            if (md.empty()) continue;
            const uint32_t baseV = static_cast<uint32_t>(ch.mesh.vertices.size()), first = static_cast<uint32_t>(ch.mesh.indices.size());
            ch.mesh.vertices.insert(ch.mesh.vertices.end(), md.vertices.begin(), md.vertices.end());
            for (uint32_t i : md.indices) ch.mesh.indices.push_back(baseV + i);
            vertexOf.insert(vertexOf.end(), partOf.begin(), partOf.end());
            anim::Submesh sm;
            const ufbx_material* mat = part.index < m.materials.count ? m.materials.data[part.index] : nullptr;
            sm.name = mat && mat->name.length ? str(mat->name) : std::format("slot{}", ch.submeshes.size());
            sm.firstIndex = first;
            sm.indexCount = static_cast<uint32_t>(md.indices.size());
            sm.sourceMaterial = static_cast<int>(part.index);
            ch.submeshes.push_back(sm);
        }
        if (ch.submeshes.size() < 2) ch.submeshes.clear();
    } else {
        std::vector<uint32_t> allFaces(m.faces.count);
        for (uint32_t i = 0; i < allFaces.size(); ++i) allFaces[i] = i;
        ch.mesh = buildPart(m, allFaces, &vertexOf);
    }
    Mat4 normalM = worldToGeometry.transposed();
    for (auto& v : ch.mesh.vertices) {
        v.position = geometryToWorld.transformPoint(v.position);
        v.normal = normalize((normalM * Vec4(v.normal, 0)).xyz());
    }
    // 1.3.2: up to 8 influences (ufbx lists a vertex's weights largest first); a character gets influences 5..8 only when a
    // vertex has more than 4, so 4-influence rigs import exactly as before.
    bool eight = false;
    for (uint32_t vi : vertexOf)
        if (vi < skin->vertices.count && skin->vertices.data[vi].num_weights > 4) eight = true;
    for (uint32_t vi : vertexOf) {
        std::array<uint16_t, 4> j{}, j1{};
        Vec4 w{0, 0, 0, 0}, w1{0, 0, 0, 0};
        if (vi < skin->vertices.count) {
            const ufbx_skin_vertex& sv = skin->vertices.data[vi];
            for (uint32_t k = 0; k < std::min<uint32_t>(sv.num_weights, eight ? 8 : 4); ++k) {
                if (static_cast<size_t>(sv.weight_begin) + k >= skin->weights.count) break;
                const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + k];
                const ufbx_node* bone = sw.cluster_index < skin->clusters.count ? skin->clusters.data[sw.cluster_index]->bone_node : nullptr;
                uint16_t b = static_cast<uint16_t>(bone && boneOf.contains(bone) ? boneOf[bone] : 0);
                float wv = std::isfinite(sw.weight) && sw.weight > 0 ? static_cast<float>(sw.weight) : 0.0f;
                if (k < 4) {
                    j[k] = b;
                    w[static_cast<int>(k)] = wv;
                } else {
                    j1[k - 4] = b;
                    w1[static_cast<int>(k - 4)] = wv;
                }
            }
        }
        float sum = w.x + w.y + w.z + w.w + (w1.x + w1.y + w1.z + w1.w);
        if (sum > 0) {
            w = w * (1.0f / sum);
            w1 = w1 * (1.0f / sum);
        } else {
            w = {1, 0, 0, 0};  // FBX: an unweighted vertex follows the first bone (as before 1.3.2)
        }
        ch.skin.joints.push_back(j);
        ch.skin.weights.push_back(w);
        if (eight) {
            ch.skin.joints1.push_back(j1);
            ch.skin.weights1.push_back(w1);
        }
    }
    if (eight && warnings) warnings->push_back(std::format("character '{}': up to 8 skin influences per vertex (the .fchar is version 2)", ch.name));
    if (warnings)
        for (auto& wmsg : skinWeightWarnings(ch, ch.name, fixZeroWeights)) warnings->push_back(wmsg);
    if (ch.mesh.empty()) return R::failure("the skinned mesh has no triangles");
    // Animation stacks -> clips (baked to linear keys by ufbx).
    for (size_t a = 0; a < s.anim_stacks.count; ++a) {
        const ufbx_anim_stack* stack = s.anim_stacks.data[a];
        ufbx_bake_opts bo{};
        ufbx_error e{};
        ufbx_baked_anim* baked = ufbx_bake_anim(&s, stack->anim, &bo, &e);
        if (!baked) {
            if (warnings) warnings->push_back("animation '" + str(stack->name) + "' could not be baked: " + str(e.description));
            continue;
        }
        anim::Clip clip;
        clip.name = stack->name.length ? str(stack->name) : std::format("Take{}", a);
        // the stack's playback range when it has one, else the range of the keys themselves
        bool range = baked->playback_duration > 1e-6;
        double t0 = range ? baked->playback_time_begin : baked->key_time_min;
        double dur = range ? baked->playback_duration : baked->key_time_max - baked->key_time_min;
        clip.duration = static_cast<float>(std::max(dur, 1.0 / 30.0));
        for (size_t i = 0; i < baked->nodes.count; ++i) {
            const ufbx_baked_node& bn = baked->nodes.data[i];
            if (bn.typed_id >= s.nodes.count) continue;
            const ufbx_node* node = s.nodes.data[bn.typed_id];
            auto it = boneOf.find(node);
            if (it == boneOf.end()) continue;
            anim::Track t;
            t.bone = it->second;
            bool top = ch.skeleton.bones[static_cast<size_t>(t.bone)].parent < 0;
            Mat4 parentWorld = top && node->parent ? m4(node->parent->node_to_world) : Mat4{};
            for (size_t k = 0; k < bn.translation_keys.count; ++k) {
                t.tTimes.push_back(static_cast<float>(bn.translation_keys.data[k].time - t0));
                Vec3 p = v3(bn.translation_keys.data[k].value);
                t.translations.push_back(top ? parentWorld.transformPoint(p) : p);
            }
            Quat parentRot = top ? decompose(parentWorld).rotation : Quat{};
            for (size_t k = 0; k < bn.rotation_keys.count; ++k) {
                t.rTimes.push_back(static_cast<float>(bn.rotation_keys.data[k].time - t0));
                Quat q = q4(bn.rotation_keys.data[k].value);
                t.rotations.push_back(top ? (parentRot * q).normalized() : q);
            }
            for (size_t k = 0; k < bn.scale_keys.count; ++k) {
                t.sTimes.push_back(static_cast<float>(bn.scale_keys.data[k].time - t0));
                t.scales.push_back(v3(bn.scale_keys.data[k].value));
            }
            clip.tracks.push_back(std::move(t));
        }
        ufbx_free_baked_anim(baked);
        anim::compressClip(clip);
        ch.clips.push_back(std::move(clip));
    }
    // 1.3.3: zero-length bind-mesh normals and clips keying a bone's scale to 0 (one warning each; the normal fix is opt-in)
    std::vector<std::string> robust = anim::robustnessWarnings(ch, ch.name, fixZeroNormals);
    if (warnings) warnings->insert(warnings->end(), robust.begin(), robust.end());
    return ch;
}

Result<ImportResult> importFbx(const fs::path& source, Vfs& vfs, AssetRegistry& reg, const ModelImportOptions& options) {
    std::string readErr;
    auto bytes = readFileBytes(source, &readErr);
    if (!bytes) return Result<ImportResult>::failure("cannot read " + readErr);
    std::string err;
    SceneGuard scene = loadScene(*bytes, err);
    if (!scene) return Result<ImportResult>::failure(err);
    const ufbx_scene& s = *scene;
    fs::path externalRoot = options.externalRoot.empty() ? source.parent_path() : options.externalRoot;

    std::string modelName = convert::sanitizeName(options.name.empty() ? narrow(source.stem().wstring()) : options.name, "Model", 48);
    std::string base = (options.destDir.empty() ? std::string("Models") : options.destDir) + "/" + modelName + "/";
    std::string srcName = convert::sanitizeName(narrow(source.filename().wstring()), "model.fbx");
    ImportResult res;
    FbxWriter w{vfs, reg, res, {{"importer", "fbx"}, {"source", base + "source/" + srcName}}};
    if (options.provenance.is_object() && !options.provenance.empty()) w.importInfo["provenance"] = options.provenance;
    if (options.fixZeroWeights) w.importInfo["fixZeroWeights"] = true;
    if (options.fixZeroNormals) w.importInfo["fixZeroNormals"] = true;
    std::set<std::string> usedNames;
    // 1.3.3: zero-length normals over every static mesh of the model, reported once (counts + examples)
    size_t zeroTotal = 0, zeroVerts = 0, zeroMeshes = 0;
    int zeroFixed = 0;
    std::string zeroExamples;
    auto unique = [&](const std::string& dir, std::string name, const std::string& ext) {
        std::string candidate = name;
        for (int i = 2; !usedNames.insert(toLower(dir + candidate + ext)).second; ++i) candidate = std::format("{}_{}", name, i);
        return dir + candidate + ext;
    };
    try {
        w.write(base + "source/" + srcName, *bytes, AssetType::Other);

        // ---- textures (embedded content, resolver, or a file below externalRoot)
        std::map<const ufbx_texture*, std::string> texGuid;
        std::map<const ufbx_texture*, ImageData> texImage;  // decoded, for channel packing
        auto loadTexture = [&](const ufbx_texture* t, bool linear) -> std::string {
            if (!t) return {};
            if (auto it = texGuid.find(t); it != texGuid.end()) return it->second;
            std::string file = str(t->relative_filename).empty() ? str(t->filename) : str(t->relative_filename);
            std::string leaf = narrow(pathFromUtf8(file).filename().wstring());
            if (options.textureResolver && !leaf.empty())
                if (std::string g = options.textureResolver(leaf); !g.empty()) return texGuid[t] = g;
            Bytes data;
            if (t->content.size > 0) {
                auto* p = static_cast<const uint8_t*>(t->content.data);
                data.assign(p, p + t->content.size);
            } else if (!file.empty()) {
                for (fs::path cand : {source.parent_path() / pathFromUtf8(file), source.parent_path() / pathFromUtf8(leaf),
                                      source.parent_path() / "textures" / pathFromUtf8(leaf), source.parent_path() / "Textures" / pathFromUtf8(leaf)}) {
                    std::error_code ec;
                    if (!fs::is_regular_file(osPath(cand), ec) || !archive::pathWithin(externalRoot, cand)) continue;
                    if (auto b = readFileBytes(cand)) {
                        data = std::move(*b);
                        break;
                    }
                }
            }
            auto img = data.empty() ? std::nullopt : decodeImage(data);
            if (!img) {
                res.warnings.push_back(std::format("texture '{}' not found or not decodable; skipped", file.empty() ? str(t->name) : file));
                return texGuid[t] = std::string();
            }
            std::string ext = toLower(narrow(pathFromUtf8(leaf).extension().wstring()));
            bool keepBytes = ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
            std::string stem = convert::sanitizeName(narrow(pathFromUtf8(leaf).stem().wstring()), std::format("texture{}", texGuid.size()));
            std::string rel = unique(base + "Textures/", stem, keepBytes ? ext : ".png");
            Bytes out = keepBytes ? data : encodePng(*img);
            texImage[t] = std::move(*img);
            return texGuid[t] = w.write(rel, out, AssetType::Texture, {{"colorSpace", linear ? "linear" : "srgb"}}).guid.toString();
        };
        auto writeImage = [&](const std::string& name, const ImageData& img) {
            return w.write(unique(base + "Textures/", name, ".png"), encodePng(img), AssetType::Texture, {{"colorSpace", "linear"}}).guid.toString();
        };

        // ---- materials
        std::map<const ufbx_material*, std::string> matGuid;
        for (size_t i = 0; i < s.materials.count; ++i) {
            const ufbx_material* fm = s.materials.data[i];
            std::string name = str(fm->name);
            if (options.materialResolver)
                if (std::string g = options.materialResolver(name); !g.empty()) {
                    matGuid[fm] = g;
                    continue;
                }
            const ufbx_material_pbr_maps& p = fm->pbr;
            Material m;
            if (p.base_color.has_value) m.baseColor = v3(p.base_color.value_vec3);
            if (p.base_factor.has_value && p.base_factor.value_real > 0) m.baseColor = m.baseColor * static_cast<float>(p.base_factor.value_real);
            m.baseColorTexture = loadTexture(p.base_color.texture, false);
            if (p.opacity.has_value && p.opacity.value_real < 0.999) {
                m.opacity = static_cast<float>(p.opacity.value_real);
                m.alphaMode = AlphaMode::Blend;
            }
            if (p.opacity.texture) res.warnings.push_back("material '" + name + "': separate opacity texture not mapped (use base colour alpha)");
            m.normalTexture = loadTexture(p.normal_map.texture, true);
            // roughness / metalness / AO: separate grayscale maps are packed into one ORM texture
            const ufbx_texture *roughT = p.roughness.texture, *glossT = p.glossiness.texture, *metalT = p.metalness.texture,
                               *aoT = p.ambient_occlusion.texture;
            m.roughness = p.roughness.has_value ? static_cast<float>(p.roughness.value_real)
                          : p.glossiness.has_value ? 1.0f - static_cast<float>(p.glossiness.value_real) : 0.5f;
            m.metallic = p.metalness.has_value ? static_cast<float>(p.metalness.value_real) : 0.0f;
            if (roughT || glossT || metalT || aoT) {
                for (const ufbx_texture* t : {roughT ? roughT : glossT, metalT, aoT}) loadTexture(t, true);
                const ufbx_texture* rt = roughT ? roughT : glossT;
                bool sameOrm = rt && rt == metalT;  // one texture holding several channels (ORM-style)
                if (sameOrm && texImage.contains(rt)) {
                    m.metallicRoughnessTexture = texGuid[rt];
                    if (aoT == rt) m.occlusionTexture = texGuid[rt];
                } else {
                    auto img = [&](const ufbx_texture* t) -> const ImageData* { return t && texImage.contains(t) ? &texImage[t] : nullptr; };
                    convert::Channel ao{img(aoT), 0, false, 1.0f, 1.0f};
                    convert::Channel rough{img(rt), 0, !roughT && glossT, 1.0f, m.roughness};
                    convert::Channel metal{img(metalT), 0, false, 1.0f, m.metallic};
                    std::string packed = writeImage(convert::sanitizeName(name, "material") + "_ORM", convert::packChannels(ao, rough, metal));
                    m.metallicRoughnessTexture = packed;
                    if (img(aoT)) m.occlusionTexture = packed;
                    if (img(rt)) m.roughness = 1.0f;  // the texture carries the value (glTF factors multiply)
                    if (img(metalT)) m.metallic = 1.0f;
                }
            }
            Vec3 emit = p.emission_color.has_value ? v3(p.emission_color.value_vec3) : Vec3{0, 0, 0};
            float emitFactor = p.emission_factor.has_value ? static_cast<float>(p.emission_factor.value_real) : 1.0f;
            m.emissive = emit * emitFactor;
            m.emissiveTexture = loadTexture(p.emission_color.texture, false);
            if (!m.emissiveTexture.empty() && m.emissive == Vec3{0, 0, 0}) m.emissive = {1, 1, 1};
            std::string matName = convert::sanitizeName(name, std::format("material{}", i), 48);
            std::string text = m.toJson().dump(2);
            matGuid[fm] = w.write(unique(base + "Materials/", matName, ".fmat"), std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
                                  AssetType::Material).guid.toString();
        }

        // ---- meshes: one .cmesh per (mesh, material part)
        std::map<const ufbx_mesh*, std::vector<ModelMeshRef>> meshRefs;
        for (size_t i = 0; i < s.meshes.count; ++i) {
            const ufbx_mesh* fm = s.meshes.data[i];
            std::string name = convert::sanitizeName(str(fm->name), std::format("mesh{}", i), 48);
            for (size_t pi = 0; pi < fm->material_parts.count; ++pi) {
                const ufbx_mesh_part& part = fm->material_parts.data[pi];
                if (part.num_triangles == 0) continue;
                MeshData md = buildPart(*fm, std::span(part.face_indices.data, part.face_indices.count));
                if (md.empty()) continue;
                if (options.scale != 1.0f) convert::convertMesh(md, convert::SourceSpace::Fledge, options.scale);
                if (std::vector<uint32_t> zero = zeroNormalVertices(md); !zero.empty()) {
                    ++zeroMeshes;
                    zeroTotal += zero.size();
                    if (zeroMeshes <= 3) {
                        std::string ex;
                        for (size_t k = 0; k < zero.size() && k < 4; ++k) ex += (k ? ", " : "") + std::to_string(zero[k]);
                        zeroExamples += std::format("{}'{}' ({} of {}: {}{})", zeroMeshes > 1 ? "; " : "", name, zero.size(), md.vertices.size(), ex,
                                                    zero.size() > 4 ? ", ..." : "");
                    }
                    if (options.fixZeroNormals) zeroFixed += fixZeroNormals(md);
                }
                zeroVerts += md.vertices.size();
                std::string rel = unique(base + "Meshes/", name + (fm->material_parts.count > 1 ? std::format("_{}", pi) : ""), ".cmesh");
                AssetRecord rec = w.write(rel, writeCookedMesh(md), AssetType::Mesh);
                const ufbx_material* mat = part.index < fm->materials.count ? fm->materials.data[part.index] : nullptr;
                meshRefs[fm].push_back({rec.guid.toString(), mat && matGuid.contains(mat) ? matGuid[mat] : ""});
            }
            if (fm->faces.count > 0 && meshRefs[fm].empty()) res.warnings.push_back("mesh '" + name + "' has no triangles; skipped");
        }
        if (zeroTotal > 0)
            res.warnings.push_back(zeroNormalSetWarning(modelName, zeroTotal, zeroVerts, zeroMeshes, zeroExamples, options.fixZeroNormals ? zeroFixed : -1));

        // ---- hierarchy -> .fmodel (the scene root is implicit)
        ModelAsset model;
        model.name = modelName;
        std::function<void(const ufbx_node*, int)> visit = [&](const ufbx_node* n, int parent) {
            int self = parent;
            if (!n->is_root) {
                ModelNode mn;
                mn.name = n->name.length ? str(n->name) : std::format("node{}", model.nodes.size());
                mn.parent = parent;
                Transform t = tr(n->local_transform);
                mn.position = t.position * options.scale;  // meshes are scaled too: the whole model scales uniformly
                mn.rotation = t.rotation;
                mn.scale = t.scale;
                if (n->mesh && meshRefs.contains(n->mesh)) mn.meshes = meshRefs[n->mesh];
                self = static_cast<int>(model.nodes.size());
                model.nodes.push_back(std::move(mn));
            }
            for (size_t c = 0; c < n->children.count; ++c) visit(n->children.data[c], self);
        };
        if (options.scale != 1.0f) res.warnings.push_back(std::format("extra scale {} applied", options.scale));
        visit(s.root_node, -1);
        std::string text = model.toJson().dump(2);
        res.primary = w.write(base + modelName + ".fmodel", std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), AssetType::Model);

        bool skinned = false;
        for (size_t i = 0; i < s.meshes.count; ++i) skinned |= s.meshes.data[i]->skin_deformers.count > 0;
        if (skinned) {
            auto ch = importFbxCharacter(*bytes, &res.warnings, options.fixZeroWeights, options.fixZeroNormals);
            if (ch) {
                if (auto wb = characterBoneWarning(*ch, modelName, currentEdition()); !wb.empty()) res.warnings.push_back(wb);
                // 1.3.2 material slots: the .fmat written above for the material of the same name
                for (auto& sm : ch->submeshes)
                    for (auto& [fm, guid] : matGuid)
                        if (fm && str(fm->name) == sm.name) {
                            sm.material = guid;
                            break;
                        }
                std::string ct = ch->toJson().dump();
                w.write(base + modelName + ".fchar", std::span(reinterpret_cast<const uint8_t*>(ct.data()), ct.size()), AssetType::Other);
            } else {
                res.warnings.push_back("skin not imported: " + ch.error());
            }
        } else if (s.anim_stacks.count > 0) {
            res.warnings.push_back(std::format("{} animation stack(s) on an unskinned model were not imported (node animation is not supported)",
                                               s.anim_stacks.count));
        }
        if (s.metadata.warnings.count > 0)
            res.warnings.push_back(std::format("ufbx reported {} recoverable problem(s) in the file", s.metadata.warnings.count));
    } catch (const std::exception& e) {
        return Result<ImportResult>::failure(e.what());
    }
    reg.save(vfs);
    FLEDGE_LOG_INFO("import", "FBX '{}' imported: {} assets, {} warnings", modelName, res.created.size(), res.warnings.size());
    for (auto& wmsg : res.warnings) FLEDGE_LOG_WARN("import", "{}", wmsg);
    return res;
}

} // namespace fledge
