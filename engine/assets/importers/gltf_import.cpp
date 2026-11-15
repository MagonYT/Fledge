// glTF 2.0 importer (cgltf v1.15, MIT) + generic importAsset/reimportAsset.
#include <cctype>
#include <cstdlib>
#include <functional>
#include <memory>
#include <cstring>
#include <format>
#include <map>
#include <set>

#include "fledge/assets/cooked.h"
#include "fledge/audio/audio.h"
#include "fledge/core/log.h"
#include "fledge/importers/archive.h"
#include "fledge/importers/importers.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fledge {

namespace {

std::string sanitize(std::string_view s, const std::string& fallback) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    while (out.size() > 48) out.pop_back();
    return out.empty() ? fallback : out;
}

// File callbacks so UTF-8 paths work on Windows (cgltf's default uses fopen). user_data = the folder external
// buffers may come from (store imports must not read "../../anything" a file points at).
cgltf_result readFileCb(const cgltf_memory_options*, const cgltf_file_options* fo, const char* path, cgltf_size* size, void** data) {
    if (fo && fo->user_data && !archive::pathWithin(*static_cast<const fs::path*>(fo->user_data), pathFromUtf8(path)))
        return cgltf_result_file_not_found;
    auto bytes = readFileBytes(pathFromUtf8(path));
    if (!bytes) return cgltf_result_file_not_found;
    void* mem = std::malloc(bytes->empty() ? 1 : bytes->size());
    if (!mem) return cgltf_result_out_of_memory;
    std::memcpy(mem, bytes->data(), bytes->size());
    *size = bytes->size();
    *data = mem;
    return cgltf_result_success;
}
void releaseFileCb(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

struct Writer {
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
    AssetRecord writeText(const std::string& rel, const std::string& text, AssetType type) {
        return write(rel, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), type);
    }
};

MeshData readPrimitive(const cgltf_primitive& prim, std::vector<std::string>& warnings) {
    MeshData m;
    const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr;
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const auto& a = prim.attributes[i];
        if (a.type == cgltf_attribute_type_position) pos = a.data;
        else if (a.type == cgltf_attribute_type_normal) nrm = a.data;
        else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv = a.data;
    }
    if (!pos) {
        warnings.push_back("primitive without POSITION skipped");
        return m;
    }
    m.vertices.resize(pos->count);
    for (cgltf_size i = 0; i < pos->count; ++i) {
        float v[4] = {};
        cgltf_accessor_read_float(pos, i, v, 3);
        m.vertices[i].position = {v[0], v[1], v[2]};
        if (nrm && i < nrm->count) {
            cgltf_accessor_read_float(nrm, i, v, 3);
            m.vertices[i].normal = {v[0], v[1], v[2]};
        }
        if (uv && i < uv->count) {
            cgltf_accessor_read_float(uv, i, v, 2);
            m.vertices[i].uv = {v[0], v[1]};
        }
    }
    if (prim.indices) {
        m.indices.resize(prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i) m.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
    } else {
        for (uint32_t i = 0; i < m.vertices.size(); ++i) m.indices.push_back(i);
    }
    for (uint32_t idx : m.indices)
        if (idx >= m.vertices.size()) {
            warnings.push_back("primitive with out-of-range index skipped");
            return {};
        }
    if (!nrm) {  // smooth normals from faces
        std::vector<Vec3> acc(m.vertices.size());
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            auto &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
            Vec3 n = cross(b.position - a.position, c.position - a.position);
            for (size_t k = 0; k < 3; ++k) acc[m.indices[i + k]] += n;
        }
        for (size_t i = 0; i < acc.size(); ++i) m.vertices[i].normal = normalize(acc[i]);
    }
    return m;
}

} // namespace

std::string zeroNormalSetWarning(const std::string& model, size_t zero, size_t vertices, size_t meshes, const std::string& examples, int fixed) {
    if (fixed >= 0)
        return std::format("model '{}': {} of {} mesh vertices in {} mesh{} had zero-length (or NaN) normals (e.g. {}); fixZeroNormals regenerated "
                           "{} of them from the geometry",
                           model, zero, vertices, meshes, meshes == 1 ? "" : "es", examples, fixed);
    return std::format("model '{}': {} of {} mesh vertices in {} mesh{} have zero-length (or NaN) normals (e.g. {}). Fledge 1.3.3 shades them "
                       "with the face normal; older versions and other engines can show black pixels and a bloom halo there. Re-export with "
                       "normals, or re-import with fixZeroNormals (regenerates them from the geometry; docs/ANIMATION.md \"Zero-length normals\")",
                       model, zero, vertices, meshes, meshes == 1 ? "" : "es", examples);
}

Result<ImportResult> importGltf(const fs::path& source, Vfs& vfs, AssetRegistry& reg, const ModelImportOptions& options) {
    std::string readErr;
    auto bytes = readFileBytes(source, &readErr);
    if (!bytes) return Result<ImportResult>::failure("cannot read " + readErr);
    // Store imports set externalRoot; a plain user import keeps reading references relative to the file as before.
    const fs::path& externalRoot = options.externalRoot;
    auto external = [&](const std::string& rel) -> std::optional<Bytes> {
        fs::path p = source.parent_path() / pathFromUtf8(rel);
        if (!externalRoot.empty() && !archive::pathWithin(externalRoot, p)) return std::nullopt;
        return readFileBytes(p);
    };
    cgltf_options opt{};
    opt.file.read = &readFileCb;
    opt.file.release = &releaseFileCb;
    if (!externalRoot.empty()) opt.file.user_data = const_cast<fs::path*>(&externalRoot);
    cgltf_data* data = nullptr;
    if (cgltf_parse(&opt, bytes->data(), bytes->size(), &data) != cgltf_result_success)
        return Result<ImportResult>::failure("not a valid glTF 2.0 file");
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> guard(data, &cgltf_free);
    std::string srcUtf8 = narrow(source.wstring());
    if (cgltf_load_buffers(&opt, data, srcUtf8.c_str()) != cgltf_result_success)
        return Result<ImportResult>::failure("glTF buffers could not be loaded (missing .bin?)");
    if (cgltf_validate(data) != cgltf_result_success) return Result<ImportResult>::failure("glTF validation failed");

    std::string modelName = sanitize(options.name.empty() ? narrow(source.stem().wstring()) : options.name, "Model");
    std::string base = (options.destDir.empty() ? std::string("Models") : options.destDir) + "/" + modelName + "/";
    ImportResult res;
    Writer w{vfs, reg, res, {{"importer", "gltf"}, {"source", base + "source/" + narrow(source.filename().wstring())}}};
    if (options.provenance.is_object() && !options.provenance.empty()) w.importInfo["provenance"] = options.provenance;
    if (options.fixZeroWeights) w.importInfo["fixZeroWeights"] = true;
    if (options.fixZeroNormals) w.importInfo["fixZeroNormals"] = true;
    // 1.3.3: zero-length normals over every static mesh of the model, reported once (counts + examples)
    size_t zeroTotal = 0, zeroVerts = 0, zeroMeshes = 0;
    int zeroFixed = 0;
    std::string zeroExamples;
    try {
        // Keep a copy of the source (editor-only) so reimport works without the original location.
        w.write(base + "source/" + narrow(source.filename().wstring()), *bytes, AssetType::Other);
        for (cgltf_size i = 0; i < data->buffers_count; ++i) {
            const char* uri = data->buffers[i].uri;
            if (uri && std::strncmp(uri, "data:", 5) != 0) {
                if (auto b = external(uri))
                    w.write(base + "source/" + narrow(pathFromUtf8(uri).filename().wstring()), *b, AssetType::Other);
            }
        }

        // Which images hold data (normal / metallic-roughness / occlusion) -> linear color space.
        std::set<const cgltf_image*> linearImages;
        for (cgltf_size i = 0; i < data->materials_count; ++i) {
            const cgltf_material& gm = data->materials[i];
            for (const cgltf_texture_view* v : {&gm.normal_texture, &gm.occlusion_texture, &gm.pbr_metallic_roughness.metallic_roughness_texture})
                if (v->texture && v->texture->image) linearImages.insert(v->texture->image);
        }
        // Images -> textures (original encoded bytes; cooked later).
        std::map<const cgltf_image*, std::string> imageGuid;
        for (cgltf_size i = 0; i < data->images_count; ++i) {
            const cgltf_image& img = data->images[i];
            Bytes imgBytes;
            std::string ext = ".png";
            if (img.buffer_view) {
                const uint8_t* p = cgltf_buffer_view_data(img.buffer_view);
                imgBytes.assign(p, p + img.buffer_view->size);
                if (img.mime_type && std::strstr(img.mime_type, "jpeg")) ext = ".jpg";
            } else if (img.uri && std::strncmp(img.uri, "data:", 5) == 0) {
                const char* comma = std::strchr(img.uri, ',');
                if (comma) imgBytes = base64Decode(comma + 1);
                if (std::strstr(img.uri, "image/jpeg")) ext = ".jpg";
            } else if (img.uri) {
                std::string decoded = img.uri;
                cgltf_decode_uri(decoded.data());
                decoded.resize(std::strlen(decoded.c_str()));
                if (auto b = external(decoded)) imgBytes = std::move(*b);
                std::string l = toLower(decoded);
                if (endsWith(l, ".jpg") || endsWith(l, ".jpeg")) ext = ".jpg";
            }
            if (imgBytes.empty() || !decodeImage(imgBytes)) {
                res.warnings.push_back(std::format("image {} could not be decoded; skipped", i));
                continue;
            }
            std::string name = sanitize(img.name ? img.name : "", std::format("image{}", i));
            Json colorSpace = {{"colorSpace", linearImages.contains(&img) ? "linear" : "srgb"}};
            imageGuid[&img] = w.write(base + "Textures/" + name + ext, imgBytes, AssetType::Texture, colorSpace).guid.toString();
        }
        auto texRef = [&](const cgltf_texture_view& v) -> std::string {
            if (!v.texture || !v.texture->image) return {};
            auto it = imageGuid.find(v.texture->image);
            return it == imageGuid.end() ? std::string() : it->second;
        };

        // Materials
        std::map<const cgltf_material*, std::string> matGuid;
        for (cgltf_size i = 0; i < data->materials_count; ++i) {
            const cgltf_material& gm = data->materials[i];
            Material m;
            if (gm.has_pbr_metallic_roughness) {
                const auto& pbr = gm.pbr_metallic_roughness;
                m.baseColor = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]};
                m.opacity = pbr.base_color_factor[3];
                m.baseColorTexture = texRef(pbr.base_color_texture);
                m.metallicRoughnessTexture = texRef(pbr.metallic_roughness_texture);
                m.metallic = pbr.metallic_factor;
                m.roughness = pbr.roughness_factor;
            }
            m.normalTexture = texRef(gm.normal_texture);
            m.normalScale = gm.normal_texture.texture ? gm.normal_texture.scale : 1.0f;
            m.occlusionTexture = texRef(gm.occlusion_texture);
            m.occlusionStrength = gm.occlusion_texture.texture ? gm.occlusion_texture.scale : 1.0f;
            m.emissive = {gm.emissive_factor[0], gm.emissive_factor[1], gm.emissive_factor[2]};
            m.emissiveTexture = texRef(gm.emissive_texture);
            if (m.emissiveTexture.size() && m.emissive == Vec3{0, 0, 0}) m.emissive = {1, 1, 1};
            if (gm.has_emissive_strength) m.emissiveIntensity = gm.emissive_strength.emissive_strength;
            m.twoSided = gm.double_sided;
            m.alphaMode = gm.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
                        : gm.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask
                                                                 : AlphaMode::Opaque;
            m.alphaCutoff = gm.alpha_cutoff;
            if (gm.unlit) m.shader = MaterialShader::Unlit;
            std::string name = sanitize(gm.name ? gm.name : "", std::format("material{}", i));
            matGuid[&gm] = w.writeText(base + "Materials/" + name + ".fmat", m.toJson().dump(2), AssetType::Material).guid.toString();
        }

        // Meshes (one .cmesh per primitive)
        std::map<const cgltf_mesh*, std::vector<ModelMeshRef>> meshRefs;
        for (cgltf_size i = 0; i < data->meshes_count; ++i) {
            const cgltf_mesh& gm = data->meshes[i];
            std::string name = sanitize(gm.name ? gm.name : "", std::format("mesh{}", i));
            for (cgltf_size p = 0; p < gm.primitives_count; ++p) {
                const cgltf_primitive& prim = gm.primitives[p];
                if (prim.type != cgltf_primitive_type_triangles) {
                    res.warnings.push_back(std::format("mesh '{}' primitive {}: only triangle lists are supported; skipped", name, p));
                    continue;
                }
                MeshData md = readPrimitive(prim, res.warnings);
                if (md.empty()) continue;
                if (std::vector<uint32_t> zero = zeroNormalVertices(md); !zero.empty()) {
                    ++zeroMeshes;
                    zeroTotal += zero.size();
                    if (zeroMeshes <= 3) {
                        std::string ex;
                        for (size_t k = 0; k < zero.size() && k < 4; ++k) ex += (k ? ", " : "") + std::to_string(zero[k]);
                        zeroExamples += std::format("{}'{}{}' ({} of {}: {}{})", zeroMeshes > 1 ? "; " : "", name,
                                                    gm.primitives_count > 1 ? std::format(" primitive {}", p) : "", zero.size(), md.vertices.size(), ex,
                                                    zero.size() > 4 ? ", ..." : "");
                    }
                    if (options.fixZeroNormals) zeroFixed += fixZeroNormals(md);
                }
                zeroVerts += md.vertices.size();
                std::string rel = base + "Meshes/" + name + (gm.primitives_count > 1 ? std::format("_{}", p) : "") + ".cmesh";
                AssetRecord rec = w.write(rel, writeCookedMesh(md), AssetType::Mesh);
                meshRefs[&gm].push_back({rec.guid.toString(), prim.material ? matGuid[prim.material] : ""});
            }
        }

        if (zeroTotal > 0)
            res.warnings.push_back(zeroNormalSetWarning(modelName, zeroTotal, zeroVerts, zeroMeshes, zeroExamples, options.fixZeroNormals ? zeroFixed : -1));

        // Node hierarchy -> .fmodel (parents before children)
        ModelAsset model;
        model.name = modelName;
        std::map<const cgltf_node*, int> index;
        std::function<void(const cgltf_node*, int)> visit = [&](const cgltf_node* n, int parent) {
            ModelNode mn;
            mn.name = n->name ? n->name : std::format("node{}", model.nodes.size());
            mn.parent = parent;
            float mtx[16];
            cgltf_node_transform_local(n, mtx);
            Mat4 m;
            std::memcpy(&m.c[0].x, mtx, sizeof(mtx));  // both column-major
            Transform t = decompose(m);
            mn.position = t.position;
            mn.rotation = t.rotation;
            mn.scale = t.scale;
            if (n->mesh) mn.meshes = meshRefs[n->mesh];
            int self = static_cast<int>(model.nodes.size());
            model.nodes.push_back(std::move(mn));
            for (cgltf_size c = 0; c < n->children_count; ++c) visit(n->children[c], self);
        };
        if (const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr)) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) visit(scene->nodes[i], -1);
        } else {
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
                if (!data->nodes[i].parent) visit(&data->nodes[i], -1);
        }
        res.primary = w.writeText(base + modelName + ".fmodel", model.toJson().dump(2), AssetType::Model);
        if (data->skins_count > 0) {  // skinned: also a character (skeleton + skinned mesh + clips) for Animator components
            auto ch = importGltfCharacter(*bytes, source.parent_path(), &res.warnings, options.fixZeroWeights, options.fixZeroNormals);
            if (ch) {
                if (auto wb = characterBoneWarning(*ch, modelName, currentEdition()); !wb.empty()) res.warnings.push_back(wb);
                // 1.3.2 material slots: the .fmat this import wrote for each primitive's material
                for (auto& sm : ch->submeshes)
                    if (sm.sourceMaterial >= 0 && static_cast<cgltf_size>(sm.sourceMaterial) < data->materials_count)
                        if (auto mg = matGuid.find(&data->materials[sm.sourceMaterial]); mg != matGuid.end()) sm.material = mg->second;
                if (ch->submeshes.size() >= 2)
                    FLEDGE_LOG_INFO("import", "character '{}': {} material slots (drawn per slot in Fledge Advanced; Fledge draws the whole character "
                                    "with one material)", modelName, ch->submeshes.size());
                std::string text = ch->toJson().dump();
                w.write(base + modelName + ".fchar", std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), AssetType::Other);
            } else {
                res.warnings.push_back("skin not imported: " + ch.error());
            }
        }
    } catch (const std::exception& e) {
        return Result<ImportResult>::failure(e.what());
    }
    reg.save(vfs);
    FLEDGE_LOG_INFO("import", "glTF '{}' imported: {} assets, {} warnings", modelName, res.created.size(), res.warnings.size());
    for (auto& wmsg : res.warnings) FLEDGE_LOG_WARN("import", "{}", wmsg);
    return res;
}

Result<ImportResult> importAsset(const fs::path& source, Vfs& vfs, AssetRegistry& reg, const ModelImportOptions& options) {
    std::string ext = toLower(narrow(source.extension().wstring()));
    if (ext == ".gltf" || ext == ".glb") return importGltf(source, vfs, reg, options);
    if (ext == ".fbx") return importFbx(source, vfs, reg, options);
    if (ext == ".uasset" || ext == ".umap" || ext == ".uexp" || ext == ".ubulk") return Result<ImportResult>::failure(kUnrealAssetHelp);
    std::string readErr;
    auto bytes = readFileBytes(source, &readErr);
    if (!bytes) return Result<ImportResult>::failure("cannot read the source file " + readErr);
    if (ext == ".bvh") {  // motion capture -> a character (skeleton + clip) in Characters/
        std::string err;
        auto ch = anim::parseBvh(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), 0.01f, &err);
        if (!ch) return Result<ImportResult>::failure("BVH import failed: " + err);
        std::string rel = "Characters/" + narrow(source.stem().wstring()) + ".fchar";
        std::string text = ch->toJson().dump();
        if (!vfs.write("content:/" + rel, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())))
            return Result<ImportResult>::failure("cannot write into Content/");
        Guid g = reg.add(rel, AssetType::Other);
        ImportResult res;
        res.primary = *reg.find(g);
        res.created.push_back(res.primary);
        reg.save(vfs);
        res.warnings.push_back("BVH has no mesh: use the clip through retargeting onto a character (Animation workspace)");
        return res;
    }
    AssetType type = assetTypeFromExtension(source);
    std::string err, sub;
    std::string objWarning;
    if (type == AssetType::Mesh && ext == ".obj") {
        auto obj = parseObj(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err);
        if (!obj) return Result<ImportResult>::failure("OBJ import failed: " + err);
        sub = "Meshes";
        // 1.3.3: reported only (an OBJ is copied as is and cooked later; fix the "vn" lines in the source)
        objWarning = zeroNormalWarning("mesh '" + narrow(source.filename().wstring()) + "'", obj->vertices.size(), zeroNormalVertices(*obj), -2);
    } else if (type == AssetType::Texture && ext != ".ctex") {
        if (!decodeImage(*bytes, &err)) return Result<ImportResult>::failure("image import failed: " + err);
        sub = "Textures";
    } else if (type == AssetType::Audio) {
        if (!audio::probeClip(*bytes, &err)) return Result<ImportResult>::failure("audio import failed: " + err);
        sub = "Audio";
    } else {
        return Result<ImportResult>::failure(
            "unsupported asset type (supported: .gltf, .glb, .fbx (incl. skins/animations), .bvh, .obj, .png/.jpg/.tga/.bmp, "
            ".wav/.mp3/.flac/.ogg; store packages .unitypackage / .zip through the Asset Store Import panel or import_asset_file)");
    }
    std::string rel = sub + "/" + narrow(source.filename().wstring());
    if (!vfs.write("content:/" + rel, *bytes)) return Result<ImportResult>::failure("cannot write into Content/");
    Guid g = reg.add(rel, type);
    ImportResult res;
    res.primary = *reg.find(g);
    res.created.push_back(res.primary);
    reg.writeMeta(vfs, res.primary, {{"importer", ext == ".obj" ? "obj" : type == AssetType::Audio ? "audio" : "image"}});
    reg.save(vfs);
    FLEDGE_LOG_INFO("import", "Imported {} as {} ({})", narrow(source.filename().wstring()), rel, g.toString());
    if (!objWarning.empty()) {
        res.warnings.push_back(objWarning);
        FLEDGE_LOG_WARN("import", "{}", objWarning);
    }
    return res;
}

Result<ImportResult> reimportAsset(const AssetRecord& asset, Vfs& vfs, AssetRegistry& reg, std::optional<bool> fixZeroWeights,
                                   std::optional<bool> fixZeroNormals) {
    Json meta = Json::parse(vfs.readText("content:/" + asset.path + AssetRegistry::kMetaExtension).value_or("{}"), nullptr, false);
    std::string importer = (!meta.is_discarded() && meta.contains("import")) ? meta["import"].value("importer", "") : "";
    if (importer == "gltf" || importer == "fbx") {
        std::string src = meta["import"].value("source", "");
        auto* dir = dynamic_cast<DirectoryProvider*>(vfs.provider("content"));
        if (src.empty() || !dir) return Result<ImportResult>::failure(importer + " source copy not found for re-import");
        fs::path tmp = dir->resolve(src);
        auto bytes = readFileBytes(tmp);
        if (!bytes) return Result<ImportResult>::failure(importer + " source copy not found: " + src);
        // Same folder and name as the first import (Models/<name>/ or a store import's folder), so GUIDs stay stable.
        ModelImportOptions o;
        std::string folder = src.substr(0, src.rfind("/source/"));  // <destDir>/<name>
        o.name = folder.substr(folder.rfind('/') + 1);
        o.destDir = folder.rfind('/') == std::string::npos ? "" : folder.substr(0, folder.rfind('/'));
        o.externalRoot = tmp.parent_path();
        if (meta["import"].contains("provenance")) o.provenance = meta["import"]["provenance"];
        // 1.3.2: the zero-weight fix sticks to the asset (recorded in its .meta) unless the caller says otherwise
        o.fixZeroWeights = fixZeroWeights.value_or(meta["import"].value("fixZeroWeights", false));
        o.fixZeroNormals = fixZeroNormals.value_or(meta["import"].value("fixZeroNormals", false));  // 1.3.3, same rule
        return importer == "fbx" ? importFbx(tmp, vfs, reg, o) : importGltf(tmp, vfs, reg, o);
    }
    // Plain files live in Content/ already: re-validate (callers then reload / re-cook).
    auto bytes = vfs.read("content:/" + asset.path);
    if (!bytes) return Result<ImportResult>::failure("asset file missing: " + asset.path);
    std::string err;
    std::string l = toLower(asset.path);
    if (endsWith(l, ".obj") && !parseObj(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err))
        return Result<ImportResult>::failure("OBJ invalid: " + err);
    if (asset.type == AssetType::Texture && !endsWith(l, ".ctex") && !decodeImage(*bytes, &err))
        return Result<ImportResult>::failure("image invalid: " + err);
    ImportResult res;
    res.primary = asset;
    res.created.push_back(asset);
    return res;
}

} // namespace fledge
