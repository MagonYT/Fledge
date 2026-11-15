#include "fledge/packager/cooker.h"

#include <format>
#include <memory>
#include <set>

#include "fledge/assets/asset_registry.h"
#include "fledge/assets/cooked.h"
#include "fledge/assets/lod.h"
#include "fledge/core/guid.h"
#include "fledge/core/log.h"
#include "fledge/core/vfs.h"
#include "fledge/importers/importers.h"

namespace fledge {

namespace {
constexpr int kCookerVersion = 3;  // v3: mesh LOD chains  // v2: gamma-correct mips, color spaces, HDR  // bump to invalidate every cache entry

std::string hex64(uint64_t v) { return std::format("{:016x}", v); }

bool isUsageSource(const std::string& rel) {
    if (rel == AssetRegistry::kFileName) return false;
    std::string l = toLower(rel);
    for (const char* ext : {".fscene", ".fmat", ".fmodel", ".fchar", ".fui", ".fhtml", ".lua", ".json", ".fcloth"})
        if (endsWith(l, ext)) return true;
    return false;
}

std::string contentRel(std::string s) {
    if (startsWith(s, "content:/")) s = s.substr(9);
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

std::string sourceTextureRel(const std::string& rel) {
    std::string l = toLower(rel);
    if (endsWith(l, ".ctex") && rel.size() > 5) return rel.substr(0, rel.size() - 5);
    return rel;
}

void considerTexture(const std::string& ref, const AssetRegistry& reg, std::unordered_set<std::string>& used) {
    if (ref.empty() || startsWith(ref, "builtin:")) return;
    std::string s = contentRel(ref);
    if (auto g = Guid::parse(s))
        if (const AssetRecord* r = reg.find(*g)) s = r->path;
    s = sourceTextureRel(s);
    if (isContentTexture(s)) used.insert(s);
}

void harvestQuoted(std::string_view text, const std::function<void(const std::string&)>& consider) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        size_t start = ++i;
        bool esc = false;
        for (; i < text.size(); ++i) {
            if (esc) {
                esc = false;
                continue;
            }
            if (text[i] == '\\') {
                esc = true;
                continue;
            }
            if (text[i] == '"') break;
        }
        size_t n = i > start ? i - start : 0;
        if (n == 0 || n > 400) continue;
        consider(std::string(text.substr(start, n)));
    }
}
} // namespace

bool isContentTexture(const std::string& relPath) { return assetTypeFromExtension(relPath) == AssetType::Texture; }

std::unordered_set<std::string> collectUsedTextures(const Project& project) {
    DirectoryProvider content(project.contentDir());
    Vfs vfs;
    vfs.mount("content", std::make_shared<DirectoryProvider>(project.contentDir()));
    AssetRegistry reg;
    (void)reg.load(vfs);
    std::unordered_set<std::string> used;
    auto consider = [&](const std::string& ref) { considerTexture(ref, reg, used); };
    consider(project.render.environment);
    if (project.extra.contains("fhtmlPage") && project.extra["fhtmlPage"].is_string()) consider(project.extra["fhtmlPage"].get<std::string>());
    for (const auto& rel : content.list()) {
        if (isEditorOnlyFile(rel) || !isUsageSource(rel)) continue;
        auto data = content.read(rel);
        if (!data) continue;
        harvestQuoted(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), consider);
    }
    return used;
}

Result<CookReport> cookProject(const Project& project, const CookOptions& options, const std::function<void(const std::string&)>& progress) {
    DirectoryProvider content(project.contentDir());
    DirectoryProvider out(project.cookDir());
    Vfs vfs;  // for .meta lookups (texture color space)
    vfs.mount("content", std::make_shared<DirectoryProvider>(project.contentDir()));
    std::error_code ec;
    fs::create_directories(project.cookDir(), ec);

    std::string settings = std::format("v{};tex={};mips={};tangents={}", kCookerVersion, project.textureCompression, project.textureMips,
                                       project.meshTangents);
    uint64_t seed = contentHash(std::span(reinterpret_cast<const uint8_t*>(settings.data()), settings.size()));

    auto cacheBytes = out.read("CookCache.json");
    Json cache = Json::parse(cacheBytes ? std::string(cacheBytes->begin(), cacheBytes->end()) : std::string("{}"), nullptr, false);
    if (cache.is_discarded() || !cache.contains("entries")) cache = {{"entries", Json::object()}};
    Json& entries = cache["entries"];

    TextureCookOptions texOpt;
    texOpt.mips = project.textureMips;
    texOpt.compression = textureCompressionFromString(project.textureCompression);

    CookReport rep;
    std::unordered_set<std::string> usedTex;
    if (options.usedTexturesOnly) {
        usedTex = collectUsedTextures(project);
        if (progress) {
            if (!project.render.environment.empty())
                progress("Baking lighting from the scene environment HDR");
            else
                progress("Cooking used textures (library maps not in the scene are skipped)");
        }
    }
    std::set<std::string> live;
    for (const auto& rel : content.list()) {
        if (isEditorOnlyFile(rel)) continue;
        std::string cooked = cookedPathFor(rel);
        if (cooked.empty() || cooked == rel) {
            ++rep.passthrough;
            continue;
        }
        if (options.usedTexturesOnly && isContentTexture(rel)) {
            std::string src = sourceTextureRel(rel);
            if (!usedTex.contains(rel) && !usedTex.contains(src)) {
                ++rep.unusedTextures;
                continue;
            }
        }
        live.insert(rel);
        auto bytes = content.read(rel);
        if (!bytes) return Result<CookReport>::failure("cannot read " + rel);
        rep.sourceBytes += bytes->size();
        uint64_t fileSeed = seed;
        if (!endsWith(toLower(rel), ".obj")) fileSeed ^= textureIsSrgb(vfs, rel) ? 0x5A5A5A5Aull : 0xA5A5A5A5ull;  // color space is a cook input
        std::string h = hex64(contentHash(*bytes, fileSeed));
        if (!options.force && entries.contains(rel) && entries[rel].value("hash", "") == h && out.exists(cooked)) {
            ++rep.skipped;
            continue;
        }
        std::string err;
        Bytes result;
        if (endsWith(toLower(rel), ".obj")) {
            auto mesh = parseObj(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err);
            if (!mesh) return Result<CookReport>::failure(std::format("{}: {}", rel, err));
            if (project.meshTangents) generateTangents(*mesh);
            generateLods(*mesh);  // LOD chain at cook time (meshoptimizer)
            result = writeCookedMesh(*mesh);
        } else if (endsWith(toLower(rel), ".hdr")) {
            auto hdr = decodeHdr(*bytes, &err);
            if (!hdr) return Result<CookReport>::failure(std::format("{}: {}", rel, err));
            result = writeCookedTexture(hdrToTexture(*hdr));
        } else {
            auto img = decodeImage(*bytes, &err);
            if (!img) return Result<CookReport>::failure(std::format("{}: {}", rel, err));
            TextureCookOptions opt = texOpt;
            opt.srgb = textureIsSrgb(vfs, rel);
            result = writeCookedTexture(cookTexture(*img, opt));
        }
        if (!out.write(cooked, result)) return Result<CookReport>::failure("cannot write cooked file " + cooked);
        entries[rel] = {{"hash", h}, {"output", cooked}};
        rep.cookedBytes += result.size();
        rep.cookedFiles.push_back(cooked);
        ++rep.cooked;
        if (progress) progress(std::format("cooked {} -> {} ({} bytes)", rel, cooked, result.size()));
    }
    // Remove outputs whose source disappeared.
    for (auto it = entries.begin(); it != entries.end();) {
        if (!live.contains(it.key())) {
            fs::remove(out.resolve(it.value().value("output", "")), ec);
            ++rep.removed;
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    std::string text = cache.dump(2);
    out.write("CookCache.json", std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    FLEDGE_LOG_INFO("cook", "Cook '{}': {} cooked, {} up to date, {} removed, {} passthrough, {} unused textures skipped", project.name, rep.cooked,
                    rep.skipped, rep.removed, rep.passthrough, rep.unusedTextures);
    return rep;
}

} // namespace fledge
