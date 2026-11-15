#include "fledge/assets/asset_registry.h"

#include <algorithm>
#include <format>
#include <set>

#include "fledge/core/log.h"

namespace fledge {

const char* toString(AssetType t) {
    switch (t) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Texture: return "Texture";
        case AssetType::Scene: return "Scene";
        case AssetType::Material: return "Material";
        case AssetType::Model: return "Model";
        case AssetType::Other: return "Other";
        case AssetType::Audio: return "Audio";
        default: return "Unknown";
    }
}

AssetType assetTypeFromString(std::string_view s) {
    for (AssetType t : {AssetType::Mesh, AssetType::Texture, AssetType::Scene, AssetType::Material, AssetType::Model, AssetType::Other, AssetType::Audio})
        if (s == toString(t)) return t;
    return AssetType::Unknown;
}

AssetType assetTypeFromExtension(const fs::path& p) {
    std::string ext = toLower(narrow(p.extension().wstring()));
    if (ext == ".obj" || ext == ".cmesh") return AssetType::Mesh;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".ctex" || ext == ".hdr")
        return AssetType::Texture;
    if (ext == ".fscene") return AssetType::Scene;
    if (ext == ".fmat") return AssetType::Material;
    if (ext == ".fmodel") return AssetType::Model;  // .gltf/.glb sources are editor-only (Other)
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") return AssetType::Audio;
    return AssetType::Other;
}

Guid AssetRegistry::add(const std::string& relPath, AssetType type) {
    if (auto* r = findByPath(relPath)) return r->guid;
    return addWithGuid(relPath, type, Guid::generate());
}

Guid AssetRegistry::addWithGuid(const std::string& relPath, AssetType type, Guid guid) {
    if (auto it = m_paths.find(relPath); it != m_paths.end() && it->second != guid) m_records.erase(it->second);
    if (auto it = m_records.find(guid); it != m_records.end() && it->second.path != relPath) m_paths.erase(it->second.path);
    m_records[guid] = {guid, relPath, type};
    m_paths.insert_or_assign(relPath, guid);
    return guid;
}

bool AssetRegistry::remove(const Guid& g) {
    auto it = m_records.find(g);
    if (it == m_records.end()) return false;
    m_paths.erase(it->second.path);
    m_records.erase(it);
    return true;
}

const AssetRecord* AssetRegistry::find(const Guid& g) const {
    auto it = m_records.find(g);
    return it == m_records.end() ? nullptr : &it->second;
}

const AssetRecord* AssetRegistry::findByPath(std::string_view relPath) const {
    auto it = m_paths.find(std::string(relPath));
    return it == m_paths.end() ? nullptr : find(it->second);
}

std::vector<AssetRecord> AssetRegistry::all() const {
    std::vector<AssetRecord> v;
    for (auto& [g, r] : m_records) v.push_back(r);
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.path < b.path; });
    return v;
}

std::string AssetRegistry::resolve(const std::string& ref) const {
    if (ref.empty() || startsWith(ref, "builtin:") || ref.find(":/") != std::string::npos) return ref;
    if (auto g = Guid::parse(ref)) {
        if (auto* r = find(*g)) return "content:/" + r->path;
        return {};
    }
    return "content:/" + ref;  // bare relative path
}

Json AssetRegistry::toJson() const {
    Json arr = Json::array();
    for (auto& r : all()) arr.push_back({{"guid", r.guid.toString()}, {"path", r.path}, {"type", toString(r.type)}});
    return {{"format", "fledgeassets"}, {"version", 1}, {"assets", arr}};
}

Status AssetRegistry::fromJson(const Json& j) {
    if (!j.is_object() || !j.contains("assets") || !j["assets"].is_array())
        return Status::failure("invalid asset registry");
    AssetRegistry loaded;
    for (auto& a : j["assets"]) {
        if (!a.is_object() || !a.contains("guid") || !a["guid"].is_string() ||
            !a.contains("path") || !a["path"].is_string() || !a.contains("type") || !a["type"].is_string())
            return Status::failure("invalid asset entry in registry");
        auto g = Guid::parse(a["guid"].get_ref<const std::string&>());
        if (!g) return Status::failure("invalid GUID in asset registry");
        loaded.addWithGuid(a["path"].get_ref<const std::string&>(), assetTypeFromString(a["type"].get_ref<const std::string&>()), *g);
    }
    m_records = std::move(loaded.m_records);
    m_paths = std::move(loaded.m_paths);
    return {};
}

Status AssetRegistry::load(const Vfs& vfs) {
    auto text = vfs.readText(std::string("content:/") + kFileName);
    if (!text) {
        m_records.clear();
        m_paths.clear();
        return {};
    }
    Json j = Json::parse(*text, nullptr, false);
    if (j.is_discarded()) return Status::failure("AssetRegistry.json is not valid JSON");
    return fromJson(j);
}

bool AssetRegistry::save(Vfs& vfs) const {
    if (m_deferIndexWrites) return true;
    const auto json = toJson().dump(2);
    auto* directory = dynamic_cast<DirectoryProvider*>(vfs.provider("content"));
    if (!directory) return vfs.writeText(std::string("content:/") + kFileName, json);
    const auto target = directory->resolve(kFileName);
    if (target.empty()) return false;
    const auto temporary = target.parent_path() / pathFromUtf8(std::string(kFileName) + "." + Guid::generate().toString() + ".tmp");
    std::error_code ec;
    if (writeFileText(temporary, json)) {
        fs::rename(osPath(temporary), osPath(target), ec);
        if (!ec) return true;
    }
    fs::remove(osPath(temporary), ec);
    return false;
}

bool AssetRegistry::writeMeta(Vfs& vfs, const AssetRecord& rec, const Json& extra) const {
    Json j = {{"format", "fledgemeta"}, {"version", 1}, {"guid", rec.guid.toString()}, {"type", toString(rec.type)}};
    if (!extra.empty()) j["import"] = extra;
    std::string path = "content:/" + rec.path + kMetaExtension;
    if (auto existing = vfs.readText(path)) {
        Json old = Json::parse(*existing, nullptr, false);
        if (old.is_object() && extra.empty() && old.contains("import")) j["import"] = old["import"];  // keep importer data
        if (!old.is_discarded() && old == j) return true;  // unchanged: avoid touching the file (watcher noise)
    }
    return vfs.writeText(path, j.dump(2));
}

bool AssetRegistry::rescan(Vfs& vfs, bool writeFiles, const std::function<void(size_t, size_t, const std::string&)>& progress) {
    AssetRegistry fresh;
    (void)fresh.load(vfs);
    if (progress) progress(0, 0, {});
    auto files = vfs.list("content");
    std::set<std::string> fileSet(files.begin(), files.end());
    auto tick = [&](size_t i) {
        if (!progress || (i != files.size() && (i % 16) != 0)) return;
        progress(i, files.size(), i < files.size() ? files[i] : std::string());
    };
    // .meta files win over the index.
    for (size_t fi = 0; fi < files.size(); ++fi) {
        tick(fi);
        const auto& f = files[fi];
        if (!endsWith(f, kMetaExtension)) continue;
        std::string asset = f.substr(0, f.size() - std::char_traits<char>::length(kMetaExtension));
        if (!fileSet.contains(asset)) continue;  // orphan meta
        Json m = Json::parse(vfs.readText("content:/" + f).value_or(""), nullptr, false);
        auto g = m.is_object() && m.contains("guid") && m["guid"].is_string()
                     ? Guid::parse(m["guid"].get_ref<const std::string&>()) : std::nullopt;
        if (!g) {
            FLEDGE_LOG_WARN("assets", "ignoring invalid meta file {}", f);
            continue;
        }
        const auto type = m.contains("type") && m["type"].is_string()
                              ? assetTypeFromString(m["type"].get_ref<const std::string&>()) : AssetType::Other;
        fresh.addWithGuid(asset, type, *g);
    }
    // Drop vanished files.
    for (auto it = fresh.m_records.begin(); it != fresh.m_records.end();) {
        if (fileSet.contains(it->second.path)) {
            ++it;
        } else {
            fresh.m_paths.erase(it->second.path);
            it = fresh.m_records.erase(it);
        }
    }
    // Register new asset files (anything typed except registry/meta/other).
    for (size_t fi = 0; fi < files.size(); ++fi) {
        tick(fi);
        const auto& f = files[fi];
        if (endsWith(f, kMetaExtension) || f == kFileName || (startsWith(f, "Models/") && f.find("/source/") != std::string::npos)) continue;
        AssetType t = assetTypeFromExtension(pathFromUtf8(f));
        if (t == AssetType::Other || fresh.findByPath(f)) continue;
        fresh.add(f, t);
    }
    // Every registered asset gets a meta file.
    if (writeFiles) {
        size_t i = 0, n = fresh.m_records.size();
        for (const auto& [guid, record] : fresh.m_records) {
            if (progress && (i % 16) == 0) progress(i, n, record.path);
            fresh.writeMeta(vfs, record);
            ++i;
        }
    }
    const bool changed = m_records != fresh.m_records;
    m_records = std::move(fresh.m_records);
    m_paths = std::move(fresh.m_paths);
    if (changed && writeFiles) save(vfs);
    return changed;
}

} // namespace fledge

namespace fledge {
bool textureIsSrgb(const Vfs& vfs, const std::string& rel) {
    if (auto text = vfs.readText("content:/" + rel + AssetRegistry::kMetaExtension)) {
        Json m = Json::parse(*text, nullptr, false);
        if (m.is_object() && m.contains("import") && m["import"].is_object() &&
            m["import"].contains("colorSpace") && m["import"]["colorSpace"].is_string())
            return m["import"]["colorSpace"].get_ref<const std::string&>() != "linear";
    }
    std::string l = toLower(rel);
    for (const char* hint : {"normal", "_nrm", "rough", "metal", "_orm", "occlusion", "_ao.", "_ao_", ".hdr"})
        if (l.find(hint) != std::string::npos) return false;
    return true;
}
} // namespace fledge
