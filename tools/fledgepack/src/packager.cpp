#include "fledge/packager/packager.h"

#include <cstring>
#include <format>
#include <fstream>
#include <unordered_map>

#include "fledge/assets/asset_registry.h"
#include "fledge/assets/cooked.h"
#include "fledge/assets/fpack.h"
#include "fledge/assets/pak.h"
#include "fledge/assets/lod.h"
#include "fledge/importers/importers.h"
#include "fledge/world/partition.h"
#include "fledge/packager/cooker.h"
#include "fledge/packager/script_build.h"
#include "fledge/core/features.h"
#include "fledge/core/log.h"
#include "fledge/core/paths.h"
#include "fledge/core/version.h"
#include "fledge/core/redact.h"
#include "fledge/core/sha256.h"
#include "fledge/core/vfs.h"
#include "fledge/framework/project.h"
#include "fledge/framework/edition_check.h"
#include "fledge/render/upscalers.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fledge {

static bool isTextAsset(const std::string& rel) {
    std::string l = toLower(rel);
    for (const char* ext : {".fscene", ".json", ".fledge", ".fmat", ".fmodel", ".fhtml", ".obj", ".txt", ".mtl", ".lua", ".ini", ".cfg"})
        if (endsWith(l, ext)) return true;
    return false;
}

// Scene data can require Advanced rendering even when requiredFeatures does not record it.
// Refuse unsupported content rather than silently package it with the Standard player.
static std::string sceneNeedsAdvancedPlayer(const fs::path& contentDir) {
    DirectoryProvider content(contentDir);
    for (const auto& rel : content.list()) {
        if (isEditorOnlyFile(rel) || !endsWith(toLower(rel), ".fscene")) continue;
        auto data = content.read(rel);
        if (!data) continue;
        Json sj = Json::parse(std::string(data->begin(), data->end()), nullptr, false);
        if (!sj.is_object()) continue;
        for (const auto& ent : sj.value("entities", Json::array())) {
            if (!ent.is_object() || !ent.contains("components")) continue;
            const Json& comps = ent["components"];
            if (comps.contains("Decal")) return "projected decals";
            if (!comps.contains("Animator") || !comps["Animator"].is_object()) continue;
            const Json& a = comps["Animator"];
            if (a.contains("leaderPose") && a["leaderPose"].is_string() && !a["leaderPose"].get<std::string>().empty())
                return "leader pose (clothing / extra meshes following a character)";
        }
    }
    return {};
}

// Everything that goes into a package: the entries plus counters.
struct CollectedEntries {
    std::map<std::string, Bytes> entries;  // pak path -> raw bytes
    size_t fileCount = 0;
    uint64_t contentBytes = 0;
};

// Collects the exact set of files that goes into a package (cooked outputs,
// passthrough content, world chunks, registry, project manifest). Shared by
// the in-memory v1/v2 builds and the streaming incremental v2 build.
static Result<CollectedEntries> collectProjectEntries(const fs::path& projectFile,
                                                      const std::function<void(size_t, size_t, const std::string&)>& onFile) {
    auto project = Project::load(projectFile);
    if (!project) return Result<CollectedEntries>::failure(project.error());
    // 1) incremental cook into <project>/Intermediate/Cooked — used textures only, plus the environment HDR (lighting).
    CookOptions cookOpt;
    cookOpt.usedTexturesOnly = true;
    auto cook = cookProject(*project, cookOpt, [&](const std::string& msg) {
        if (onFile) onFile(0, 1, msg);
    });
    if (!cook) return Result<CollectedEntries>::failure("cook failed: " + cook.error());
    auto usedTex = collectUsedTextures(*project);
    DirectoryProvider content(project->contentDir());
    DirectoryProvider cooked(project->cookDir());
    // Entries are collected first, then emitted by emitPackage() below: the
    // default is the v2 content-addressed chunked container ("CAGEPAK2");
    // --legacy-pak selects the v1 monolithic layout.
    std::map<std::string, Bytes> entries;
    auto addEntry = [&](std::string path, Bytes data) { entries[std::move(path)] = std::move(data); };
    uint64_t bytes = 0;
    size_t count = 0;
    size_t omittedTextures = 0;
    const auto listed = content.list();
    size_t listedIndex = 0;
    for (const auto& rel : listed) {
        ++listedIndex;
        if (onFile) onFile(listedIndex, listed.size(), rel);
        // Editor-only sources (.meta, glTF sources) are never shipped.
        if (isEditorOnlyFile(rel) || rel == AssetRegistry::kFileName) continue;
        if (isContentTexture(rel)) {
            std::string src = rel;
            std::string l = toLower(rel);
            if (endsWith(l, ".ctex") && rel.size() > 5) src = rel.substr(0, rel.size() - 5);
            if (!usedTex.contains(rel) && !usedTex.contains(src)) {
                ++omittedTextures;
                continue;
            }
        }
        std::string target = cookedPathFor(rel);
        std::optional<Bytes> data;
        std::string name = rel;
        if (!target.empty() && target != rel) {  // ship the cooked file instead of the source
            data = cooked.read(target);
            name = target;
            if (!data) return Result<CollectedEntries>::failure("cooked output missing for " + rel);
        } else {
            data = content.read(rel);
            if (!data) return Result<CollectedEntries>::failure("cannot read content file " + rel);
            // Safety net: never ship anything that looks like a credential.
            if (isTextAsset(rel)) {
                std::string_view text(reinterpret_cast<const char*>(data->data()), data->size());
                if (redact(text) != text)
                    return Result<CollectedEntries>::failure(std::format(
                        "content file '{}' contains something that looks like an API key or token; refusing to package it", rel));
            }
        }
        // Open worlds: the scene is split into the persistent part + one chunk file per cell (streamed at runtime).
        if (endsWith(toLower(rel), ".fscene")) {
            Json sj = Json::parse(std::string(data->begin(), data->end()), nullptr, false);
            bool partitioned = false;
            if (sj.is_object())
                for (auto& ent : sj.value("entities", Json::array()))
                    if (ent.contains("components") && ent["components"].contains("WorldPartition")) partitioned = true;
            if (partitioned) {
                auto cooked = world::cookWorld(sj);
                if (!cooked) return Result<CollectedEntries>::failure(rel + ": " + cooked.error());
                std::string dir = world::worldDirFor(rel);
                for (auto& [file, text] : cooked->chunks) {
                    bytes += text.size();
                    ++count;
                    addEntry(dir + "/" + file, Bytes(text.begin(), text.end()));
                }
                // HLOD: one merged, simplified proxy mesh per chunk (drawn for unloaded chunks in the distance)
                {
                    Vfs hv;
                    hv.mount("content", std::make_shared<DirectoryProvider>(project->contentDir()));
                    AssetRegistry hreg;
                    hreg.rescan(hv, false);
                    std::map<std::string, std::optional<MeshData>> meshCache;
                    auto meshFor = [&](const std::string& ref) -> const MeshData* {
                        auto it = meshCache.find(ref);
                        if (it == meshCache.end()) {
                            std::optional<MeshData> md;
                            std::string path = hreg.resolve(ref);
                            if (startsWith(path, "builtin:")) md = primitives::byName(path);
                            else if (endsWith(toLower(path), ".obj"))
                                if (auto t = hv.readText(path)) md = parseObj(*t, nullptr);
                            if (md && !md->empty()) generateLods(*md);
                            it = meshCache.emplace(ref, std::move(md)).first;
                        }
                        return it->second ? &*it->second : nullptr;
                    };
                    for (auto& ch : cooked->index["chunks"]) {
                        auto text = std::find_if(cooked->chunks.begin(), cooked->chunks.end(), [&](auto& c) { return c.first == ch.value("name", ""); });
                        if (text == cooked->chunks.end()) continue;
                        Scene tmp;
                        world::applyChunk(tmp, world::parseChunk(text->second), {}, {});
                        auto worlds = tmp.computeWorldMatrices();
                        std::vector<ProxyPart> parts;
                        Vec3 colour{0, 0, 0};
                        for (EntityId e : tmp.entities())
                            if (auto* mr = tmp.get<MeshRendererComponent>(e); mr && mr->visible)
                                if (const MeshData* md = meshFor(mr->mesh)) {
                                    parts.push_back({md, worlds[e]});
                                    colour += mr->color;
                                }
                        if (parts.empty()) continue;
                        MeshData proxy = buildProxyMesh(parts, 400);
                        if (proxy.empty()) continue;
                        Bytes hb = writeCookedMesh(proxy);
                        bytes += hb.size();
                        addEntry(dir + "/" + world::ChunkCoord{ch.value("x", 0), ch.value("z", 0)}.name() + ".hlod", std::move(hb));
                        colour = colour / static_cast<float>(parts.size());
                        ch["hlod"] = true;
                        ch["hlodColor"] = {colour.x, colour.y, colour.z};
                    }
                }
                std::string idx = cooked->index.dump();
                addEntry(dir + "/index.json", Bytes(idx.begin(), idx.end()));
                std::string s = cooked->scene.dump();
                *data = Bytes(s.begin(), s.end());
                FLEDGE_LOG_INFO("package", "Open world {}: {} chunks", rel, cooked->chunks.size());
            }
        }
        bytes += data->size();
        ++count;
        addEntry(name, std::move(*data));
    }
    if (!content.exists(project->startupScene))
        return Result<CollectedEntries>::failure("startup scene '" + project->startupScene + "' not found in Content/");
    // Registry index rebuilt in memory from the .meta files (nothing is written to the project).
    Vfs vfs;
    vfs.mount("content", std::make_shared<DirectoryProvider>(project->contentDir()));
    AssetRegistry registry;
    registry.rescan(vfs, false);
    for (const auto& rec : registry.all()) {
        if (!isContentTexture(rec.path)) continue;
        std::string src = rec.path;
        std::string l = toLower(rec.path);
        if (endsWith(l, ".ctex") && rec.path.size() > 5) src = rec.path.substr(0, rec.path.size() - 5);
        if (!usedTex.contains(rec.path) && !usedTex.contains(src)) registry.remove(rec.guid);
    }
    std::string reg = registry.toJson().dump(2);
    addEntry(AssetRegistry::kFileName, Bytes(reg.begin(), reg.end()));
    std::string manifest = project->toJson().dump(2);
    addEntry(Project::kPakManifestName, Bytes(manifest.begin(), manifest.end()));
    FLEDGE_LOG_INFO("package", "Pak '{}': {} files, {} unused textures left out, environment '{}'", project->name, count, omittedTextures,
                    project->render.environment.empty() ? "(procedural sky)" : project->render.environment);
    CollectedEntries out;
    out.entries = std::move(entries);
    out.fileCount = count;
    out.contentBytes = bytes;
    return out;
}

Result<Bytes> buildProjectPak(const fs::path& projectFile, bool compress, size_t* fileCount, uint64_t* contentBytes,
                              const std::function<void(size_t, size_t, const std::string&)>& onFile,
                              const std::function<void(size_t, size_t, const std::string&)>& onCompress, bool legacyPak) {
    auto collected = collectProjectEntries(projectFile, onFile);
    if (!collected) return Result<Bytes>::failure(collected.error());
    if (fileCount) *fileCount = collected->fileCount;
    if (contentBytes) *contentBytes = collected->contentBytes;
    if (legacyPak) {
        PakWriter writer;
        writer.setCompression(compress);
        for (auto& [path, data] : collected->entries) writer.add(path, std::move(data));
        return writer.build(onCompress);
    }
    FpackWriter writer;
    for (auto& [path, data] : collected->entries) writer.add(path, std::move(data));
    return writer.build(onCompress);
}

// Seeds a v2 writer with the chunk payloads of a previous archive (file or
// exe with an appended pak) so identical chunks keep their stored bytes
// instead of being re-compressed. Returns the number of chunks made
// available for reuse (0 when previousFile is empty/unreadable).
static size_t seedIncrementalReuse(FpackWriter& writer, const fs::path& previousFile) {
    if (previousFile.empty()) return 0;
    auto prevIdx = readFpackIndex(previousFile);
    if (!prevIdx) return 0;
    // Chunk offsets in the index are relative to the pak start; for an
    // appended pak the payloads live after the exe image, so locate the pak
    // base first.
    uint64_t base = 0;
    {
        std::error_code ec;
        uint64_t fileSize = fs::file_size(osPath(previousFile), ec);
        if (ec) return 0;
        if (fileSize >= 32) {
            std::ifstream tf(osPath(previousFile), std::ios::binary);
            tf.seekg(static_cast<std::streamoff>(fileSize - 32));
            uint8_t buf[32];
            if (tf.read(reinterpret_cast<char*>(buf), 32) && std::memcmp(buf, "CAGEFTR1", 8) == 0) {
                uint64_t off = 0, size = 0;
                for (int i = 0; i < 8; ++i) off |= uint64_t(buf[8 + i]) << (8 * i);
                for (int i = 0; i < 8; ++i) size |= uint64_t(buf[16 + i]) << (8 * i);
                uint32_t fver = uint32_t(buf[24]) | (uint32_t(buf[25]) << 8);
                if (fver == kFpackVersion && off + size + 32 <= fileSize) base = off;
            }
        }
    }
    std::ifstream in(osPath(previousFile), std::ios::binary);
    if (!in) return 0;
    std::unordered_map<uint64_t, Bytes> payloads;
    for (const FpackChunk& c : prevIdx->chunks) {
        Bytes blob(c.storedSize);
        in.seekg(static_cast<std::streamoff>(base + c.offset));
        if (!in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()))) {
            payloads.clear();
            break;
        }
        payloads.emplace(c.hash, std::move(blob));
    }
    if (payloads.empty()) return 0;
    writer.setChunks(prevIdx->chunks, payloads);
    FLEDGE_LOG_INFO("package", "Incremental: {} chunks available for reuse from {}", prevIdx->chunks.size(),
                    narrow(previousFile.filename().wstring()));
    return prevIdx->chunks.size();
}

// Streams the v2 image to file: header + chunks go to "<file>.tmp" first, the
// index (written last) is staged in "<file>.new" which is atomically renamed
// over the target. previousFile, when readable, seeds incremental chunk
// reuse: unchanged chunks are not re-compressed or re-stored.
Result<FpackIndex> buildProjectFpackFile(const fs::path& projectFile, const fs::path& file, const fs::path& previousFile,
                                         size_t* fileCount, uint64_t* contentBytes,
                                         const std::function<void(size_t, size_t, const std::string&)>& onFile,
                                         const std::function<void(size_t, size_t, const std::string&)>& onChunk) {
    auto collected = collectProjectEntries(projectFile, onFile);
    if (!collected) return Result<FpackIndex>::failure(collected.error());
    if (fileCount) *fileCount = collected->fileCount;
    if (contentBytes) *contentBytes = collected->contentBytes;

    FpackWriter writer;
    for (auto& [path, data] : collected->entries) writer.add(path, std::move(data));
    collected->entries.clear();
    seedIncrementalReuse(writer, previousFile);
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.wstring() + L".tmp";
    const fs::path fin = file.wstring() + L".new";
    FpackIndex idx;
    {
        std::ofstream out(osPath(tmp), std::ios::binary | std::ios::trunc);
        if (!out) return Result<FpackIndex>::failure("cannot open " + narrow(tmp.wstring()));
        auto r = writer.buildIndex(out, onChunk);
        if (!r) return Result<FpackIndex>::failure(r.error());
        idx = std::move(*r);
    }
    // Build the real header now that the index offset/size are known. The
    // staged file is placeholder header + chunk payloads, so its size is the
    // index offset; the real header overwrites the placeholder 1:1.
    uint64_t chunkBytes = fs::file_size(osPath(tmp), ec);
    if (ec) return Result<FpackIndex>::failure("cannot measure staged pak");
    Bytes indexBlob = serializeFpackIndex(idx);
    Bytes header;
    header.insert(header.end(), kFpackMagic, kFpackMagic + 8);
    for (uint32_t v : {kFpackVersion, 0u, kFpackChunkShift, 0u})  // version, flags, chunkShift, reserved
        for (int i = 0; i < 4; ++i) header.push_back(static_cast<uint8_t>(v >> (8 * i)));
    for (uint64_t v : {static_cast<uint64_t>(idx.entries.size()), static_cast<uint64_t>(idx.chunks.size()), chunkBytes,
                       static_cast<uint64_t>(indexBlob.size())})
        for (int i = 0; i < 8; ++i) header.push_back(static_cast<uint8_t>(v >> (8 * i)));
    if (header.size() != kFpackHeaderSize) return Result<FpackIndex>::failure("internal header size mismatch");
    {
        std::error_code ec2;
        fs::copy_file(tmp, fin, fs::copy_options::overwrite_existing, ec2);
        if (ec2) return Result<FpackIndex>::failure("cannot stage pak: " + ec2.message());
        std::ofstream out(osPath(fin), std::ios::binary | std::ios::in);
        if (!out) return Result<FpackIndex>::failure("cannot open " + narrow(fin.wstring()));
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        out.seekp(0, std::ios::end);
        out.write(reinterpret_cast<const char*>(indexBlob.data()), static_cast<std::streamsize>(indexBlob.size()));
        if (!out.good()) return Result<FpackIndex>::failure("cannot finalize pak");
    }
    fs::remove(file, ec);  // Windows rename cannot overwrite
    ec.clear();
    fs::rename(fin, file, ec);
    if (ec) return Result<FpackIndex>::failure("cannot replace " + narrow(file.wstring()) + ": " + ec.message());
    fs::remove(tmp, ec);
    return idx;
}

Result<PackageResult> packageProject(const PackageOptions& opt, const ProgressFn& progress) {
    auto say = [&](const std::string& s, float fraction = -1.0f) {
        FLEDGE_LOG_INFO("fledgepack", "{}", s);
        if (progress) progress(s, fraction);
    };
    const auto edition = checkProjectEdition(opt.projectFile);
    if (!edition.ok)
        return Result<PackageResult>::failure("This Standard-only source distribution cannot package this project: " + edition.summary());
    auto project = Project::load(opt.projectFile);
    if (!project) return Result<PackageResult>::failure(project.error());

    // Refuse unsupported projects before cooking or writing any package output.
    const FeatureRegistry& features = FeatureRegistry::get();
    if (auto missing = missingFeatures(project->requiredFeatures, features); !missing.empty()) {
        std::string list;
        for (const auto& m : missing) list += std::format("{}{} ({})", list.empty() ? "" : ", ", m.title, m.reason);
        return Result<PackageResult>::failure(std::format("this project needs features {} {} does not have: {}", editionName(currentEdition()),
                                                          FLEDGE_VERSION_STRING, list));
    }
    std::string contentWhy = sceneNeedsAdvancedPlayer(project->contentDir());
    if (!contentWhy.empty()) {
        return Result<PackageResult>::failure(
            std::format("this game uses {} — Advanced games are not supported by this Standard-only source distribution", contentWhy));
    }
    if (minimumEdition(project->requiredFeatures, features) == Edition::Advanced)
        return Result<PackageResult>::failure("Advanced games are not supported by this Standard-only source distribution");
    PackageResult res;
    fs::path runtime = opt.runtimeExe.empty() ? executablePath().parent_path() / "fledge_runtime.exe" : opt.runtimeExe;
    std::error_code ec;
    if (!fs::is_regular_file(runtime, ec))
        return Result<PackageResult>::failure("runtime executable not found: " + narrow(runtime.wstring()));
    say(std::format("Runtime: {} (Standard)", narrow(runtime.filename().wstring())));
    fs::path moduleDir = opt.moduleDir.empty() ? runtime.parent_path() : opt.moduleDir;
    res.outputDir = opt.outputDir.empty() ? project->buildDir() / project->gameName : opt.outputDir;

    say(std::format("Packaging '{}' -> {}", project->gameName, narrow(res.outputDir.wstring())), 0.02f);
    res.pakFormat = opt.legacyPak ? 1 : 2;
    const auto onFile = [&](size_t index, size_t total, const std::string& rel) {
        if (index != total && index % 32 != 0) return;
        float fraction = total ? 0.05f + 0.23f * static_cast<float>(index) / static_cast<float>(total) : 0.15f;
        say(std::format("Packing content {}/{}  {}", index, total, rel), fraction);
    };
    const auto onChunk = [&](size_t done, size_t total, const std::string& path) {
        float fraction = total ? 0.30f + 0.32f * static_cast<float>(done) / static_cast<float>(total) : 0.4f;
        say(std::format("Compressing pak {}/{}  {}", done, total, path), fraction);
    };
    // Legacy path keeps the historical in-memory image; v2 (embedded or
    // sidecar) seeds incremental chunk reuse from the previous package before
    // building. The sidecar is streamed to disk below; the embedded pak is
    // built in memory here (its size is needed at append).
    std::optional<Bytes> pak;
    std::optional<FpackIndex> sidecarIndex;
    if (opt.legacyPak) {
        auto built = buildProjectPak(opt.projectFile, opt.compress, &res.fileCount, &res.contentBytes, onFile, onChunk, true);
        if (!built) return Result<PackageResult>::failure(built.error());
        pak = std::move(*built);
        res.pakBytes = pak->size();
    } else if (opt.embedPak) {
        // v2 embedded: collect, seed reuse from the previous embedded pak,
        // build the image in memory.
        auto collected = collectProjectEntries(opt.projectFile, onFile);
        if (!collected) return Result<PackageResult>::failure(collected.error());
        res.fileCount = collected->fileCount;
        res.contentBytes = collected->contentBytes;
        FpackWriter writer;
        for (auto& [path, data] : collected->entries) writer.add(path, std::move(data));
        collected->entries.clear();
        if (opt.incremental) {
            fs::path prevPath = res.outputDir / (widen(project->gameName) + L".exe");
            if (fs::is_regular_file(prevPath, ec)) seedIncrementalReuse(writer, prevPath);
        }
        auto built = writer.build(onChunk);
        if (!built) return Result<PackageResult>::failure(built.error());
        pak = std::move(*built);
        res.pakBytes = pak->size();
    }
    if (pak) say(std::format("Built pak v{}: {} files, {} bytes content -> {} bytes packed", res.pakFormat, res.fileCount,
                             res.contentBytes, res.pakBytes),
                 0.64f);

    fs::create_directories(res.outputDir, ec);
    if (ec) return Result<PackageResult>::failure("cannot create output directory");
    res.executable = res.outputDir / (widen(project->gameName) + L".exe");

    fs::remove(res.executable, ec);  // libstdc++ on Windows fails overwrite_existing copies of existing files
    fs::copy_file(runtime, res.executable, fs::copy_options::overwrite_existing, ec);
    if (ec) return Result<PackageResult>::failure("cannot copy runtime: " + ec.message());

    // Icon must be applied before appending the pak (UpdateResource rewrites the PE file).
    if (opt.applyIcon && !project->icon.empty()) {
        fs::path ico = project->rootDir() / pathFromUtf8(project->icon);
        if (auto st = setExecutableIcon(res.executable, ico); st) {
            res.iconApplied = true;
            say("Applied icon " + project->icon, 0.66f);
        } else {
            say("Warning: icon not applied: " + st.error);
        }
    }

    res.subsystem = opt.subsystem > 0 ? opt.subsystem : (project->guiSubsystem ? 2 : 3);
    if (auto st = setPeSubsystem(res.executable, res.subsystem); !st) return Result<PackageResult>::failure(st.error);
    say(res.subsystem == 2 ? "Subsystem: GUI (no console window)" : "Subsystem: console", 0.68f);

    auto writeBlob = [&](const fs::path& dest, std::ios::openmode extra, std::span<const uint8_t> blob, float from, float to,
                         const char* label) -> bool {
        std::ofstream out;
#ifdef _WIN32
        out.open(dest.wstring().c_str(), std::ios::binary | extra);
#else
        out.open(dest, std::ios::binary | extra);
#endif
        if (!out) return false;
        constexpr size_t kChunk = 8u << 20;
        size_t written = 0;
        while (written < blob.size()) {
            size_t n = std::min(kChunk, blob.size() - written);
            out.write(reinterpret_cast<const char*>(blob.data() + written), static_cast<std::streamsize>(n));
            if (!out) return false;
            written += n;
            float t = blob.empty() ? 1.0f : static_cast<float>(written) / static_cast<float>(blob.size());
            say(std::format("{}  {:.0f} / {:.0f} MB", label, written / 1048576.0, blob.size() / 1048576.0), from + (to - from) * t);
        }
        return out.good();
    };

    if (opt.embedPak) {
        say("Appending the pak to the game executable", 0.69f);
        uint64_t offset = fs::file_size(res.executable, ec);
        if (ec) return Result<PackageResult>::failure("cannot measure the copied runtime");
        if (!writeBlob(res.executable, std::ios::app, *pak, 0.69f, 0.82f, "Writing the game executable"))
            return Result<PackageResult>::failure("cannot write game exe");
        Bytes footer = opt.legacyPak ? makePakFooter(offset, pak->size()) : makeFpackFooter(offset, pak->size());
        if (!writeBlob(res.executable, std::ios::app, footer, 0.82f, 0.83f, "Writing the pak footer"))
            return Result<PackageResult>::failure("cannot write game exe footer");
        say(std::format("Embedded pak v{} at offset {} in {}", res.pakFormat, offset, narrow(res.executable.filename().wstring())),
            0.83f);
    } else {
        fs::path side = res.outputDir / (widen(project->gameName) + L".fpak");
        say("Writing " + narrow(side.filename().wstring()), 0.69f);
        if (opt.legacyPak) {
            if (!writeBlob(side, std::ios::trunc, *pak, 0.69f, 0.83f, "Writing sidecar pak"))
                return Result<PackageResult>::failure("cannot write sidecar pak");
        } else {
            // Streamed + atomic: header/chunks go to a temp file, the index
            // (written last) is staged and renamed over the target. The
            // previous sidecar (still on disk until the rename) seeds reuse.
            fs::path prevSide = opt.incremental && fs::is_regular_file(side, ec) ? side : fs::path();
            auto built = buildProjectFpackFile(opt.projectFile, side, prevSide, &res.fileCount, &res.contentBytes, onFile, onChunk);
            if (!built) return Result<PackageResult>::failure(built.error());
            sidecarIndex = std::move(*built);
            res.pakBytes = fs::file_size(side, ec);
            if (ec) return Result<PackageResult>::failure("cannot measure sidecar pak");
            res.chunksWritten = sidecarIndex->chunks.size();
            // Report how many stored chunks carry content the previous archive
            // already had (these are the ones incremental reuse avoided
            // re-compressing).
            if (!prevSide.empty()) {
                std::unordered_map<uint64_t, bool> fresh;
                for (const FpackChunk& c : sidecarIndex->chunks) fresh[c.hash] = true;
                if (auto prevIdx = readFpackIndex(prevSide))
                    for (const FpackChunk& c : prevIdx->chunks)
                        if (fresh.contains(c.hash)) ++res.chunksReused;
            }
            say(std::format("Built pak v2: {} files, {} bytes content -> {} bytes packed ({} chunks, {} reused)", res.fileCount,
                            res.contentBytes, res.pakBytes, res.chunksWritten, res.chunksReused),
                0.83f);
        }
        say("Wrote sidecar " + narrow(side.filename().wstring()), 0.83f);
    }

    for (const auto& m : project->modules) {
        fs::path dll = moduleDir / (widen(m) + L".dll");
        if (!fs::is_regular_file(dll, ec)) {
            return Result<PackageResult>::failure(std::format("gameplay module '{}' not found at {} (build it first)", m,
                                                              narrow(dll.wstring())));
        }
        fs::remove(res.outputDir / dll.filename(), ec);
        fs::copy_file(dll, res.outputDir / dll.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) return Result<PackageResult>::failure("cannot copy module " + m);
        res.copiedModules.push_back(m);
        say("Copied module " + narrow(dll.filename().wstring()));
    }
    // Fledge Script files: release build (-O2), shipped as <GameName>Scripts.dll next to the exe.
    if (opt.buildScripts && project->hasScripts()) {
        ScriptBuildOptions so;
        so.release = true;
        if (!opt.compiler.empty()) {
            auto tc = findToolchain(opt.compiler);
            if (!tc) return Result<PackageResult>::failure(tc.error());
            so.toolchain = *tc;
        }
        say("Building scripts (release)", 0.78f);
        int scriptStep = 0;
        auto br = buildScripts(*project, so, [&](const std::string& s) {
            ++scriptStep;
            say(s, std::min(0.88f, 0.78f + scriptStep * 0.01f));
        });
        if (!br.ok) {
            std::string msg = std::format("script build failed ({} errors)", br.errors());
            for (auto& d : br.diagnostics)
                if (d.severity == Diagnostic::Severity::Error) {
                    msg += std::format("\n  {}:{}:{}: {}", d.file, d.line, d.column, d.message);
                    if (msg.size() > 2000) break;
                }
            return Result<PackageResult>::failure(msg);
        }
        fs::path dst = res.outputDir / br.module.filename();
        fs::remove(dst, ec);
        fs::copy_file(br.module, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return Result<PackageResult>::failure("cannot copy script module: " + ec.message());
        res.scriptModule = narrow(dst.filename().wstring());
        say("Copied scripts " + res.scriptModule);
    }
    // Vendor upscaler runtime (DLSS / XeSS): ship the project's selected version from the local cache,
    // re-verified by SHA-256, with its licence. Missing -> warning (the game falls back to FSR/TAA).
    if (project->graphics.upscaler == UpscalerKind::Dlss || project->graphics.upscaler == UpscalerKind::XeSS) {
        const upscalers::VersionEntry* v = upscalers::selectVersion(project->graphics.upscaler, project->graphics.upscalerVersion, opt.upscalerCache);
        if (!v) {
            std::string w = std::format("no {} version is installed - the game will fall back to FSR/TAA (install one in the Graphics panel)",
                                        toString(project->graphics.upscaler));
            res.warnings.push_back(w);
            say("Warning: " + w);
        } else {
            fs::path src = upscalers::versionDir(*v, opt.upscalerCache);
            for (const auto& f : v->files) {
                std::string hash = Sha256::fileHex(src / pathFromUtf8(f.name));
                if (hash != f.sha256) return Result<PackageResult>::failure(std::format("{}: SHA-256 mismatch in the version cache ({})", f.name, v->id));
                fs::remove(res.outputDir / pathFromUtf8(f.name), ec);
                fs::copy_file(src / pathFromUtf8(f.name), res.outputDir / pathFromUtf8(f.name), fs::copy_options::overwrite_existing, ec);
                if (ec) return Result<PackageResult>::failure("cannot copy " + f.name + ": " + ec.message());
            }
            if (!v->license.name.empty()) {
                fs::create_directories(res.outputDir / "licenses", ec);
                fs::path lic = res.outputDir / "licenses" / pathFromUtf8(v->id + "-" + v->license.name);
                fs::remove(lic, ec);
                fs::copy_file(src / pathFromUtf8(v->license.name), lic, fs::copy_options::overwrite_existing, ec);
            }
            res.upscalerVersion = v->id;
            say("Shipped upscaler runtime " + v->id);
        }
    }
    if (project->extra.contains("fhtmlPage") && project->extra["fhtmlPage"].is_string() &&
        !project->extra["fhtmlPage"].get_ref<const std::string&>().empty()) {
        const auto helper = runtime.parent_path() / "Fhtml";
        if (!fs::is_regular_file(helper / "FledgeFhtml.exe", ec) || !fs::is_regular_file(helper / "FledgeFhtml.dll", ec))
            return Result<PackageResult>::failure("the project uses FHTML but the selected runtime has no Fhtml helper");
        std::vector<fs::path> browserFiles;
        uint64_t browserBytes = 0;
        for (auto it = fs::recursive_directory_iterator(helper, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            browserFiles.push_back(it->path());
            browserBytes += it->file_size(ec);
        }
        if (ec) return Result<PackageResult>::failure("cannot read the FHTML browser runtime: " + ec.message());
        uint64_t copiedBytes = 0;
        for (size_t i = 0; i < browserFiles.size(); ++i) {
            const fs::path& src = browserFiles[i];
            fs::path rel = fs::relative(src, helper, ec);
            fs::path dst = res.outputDir / "Fhtml" / rel;
            fs::create_directories(dst.parent_path(), ec);
            uint64_t fileBytes = fs::file_size(src, ec);
            float fraction = browserBytes ? 0.88f + 0.09f * static_cast<float>(copiedBytes) / static_cast<float>(browserBytes) : 0.92f;
            say(std::format("Copying browser runtime {}/{}  {} ({:.0f} / {:.0f} MB)", i + 1, browserFiles.size(),
                            narrow(rel.wstring()), copiedBytes / 1048576.0, browserBytes / 1048576.0),
                fraction);
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) return Result<PackageResult>::failure("cannot copy the FHTML browser runtime: " + ec.message());
            copiedBytes += fileBytes;
        }
        say("Copied the FHTML browser runtime and its license notices", 0.97f);
    }
    // DirectX Shader Compiler: the Vulkan backend compiles HLSL to SPIR-V at runtime (the other APIs never load it).
    if (fs::path dxc = runtime.parent_path() / "dxcompiler.dll"; fs::is_regular_file(dxc, ec)) {
        fs::remove(res.outputDir / "dxcompiler.dll", ec);
        fs::copy_file(dxc, res.outputDir / "dxcompiler.dll", fs::copy_options::overwrite_existing, ec);
        if (ec) return Result<PackageResult>::failure("cannot copy dxcompiler.dll: " + ec.message());
        say("Copied dxcompiler.dll (Vulkan: HLSL -> SPIR-V)");
    } else {
        res.warnings.push_back("dxcompiler.dll not found next to the runtime - the game cannot use Vulkan (other APIs work)");
    }
    // Licence texts of the third-party code compiled into the runtime (from the install's licenses/ folder).
    {
        fs::path lic = enginePaths().licenses;
        if (fs::is_directory(lic, ec)) {
            fs::create_directories(res.outputDir / "licenses", ec);
            for (const char* f : {"Fledge-LICENSE.txt", "meshoptimizer-MIT.txt", "JoltPhysics-MIT.txt", "miniaudio-Unlicense-or-MIT0.txt", "RecastNavigation-zlib.txt", "GLFW-zlib.txt", "nlohmann_json-MIT.txt", "stb-MIT-or-PublicDomain.txt", "DirectX-Headers-MIT.txt",
                                  "Vulkan-Headers-Apache-2.0-MIT.txt", "FidelityFX-FSR1-MIT.txt", "DirectXShaderCompiler-NCSA-MIT.txt"}) {
                if (!fs::is_regular_file(lic / f, ec)) continue;
                fs::remove(res.outputDir / "licenses" / f, ec);
                fs::copy_file(lic / f, res.outputDir / "licenses" / f, fs::copy_options::overwrite_existing, ec);
            }
        }
    }
    // MinGW builds are linked statically (-static), so no runtime DLLs are needed.
    // MSVC builds would copy the VC++ redistributable DLLs here (TODO when MSVC is supported).
    say("Done: " + narrow(res.executable.wstring()), 1.0f);
    return res;
}

namespace {
// Offset of the Subsystem field: e_lfanew -> "PE\0\0" + COFF header (20) + optional header offset 68.
Result<size_t> peSubsystemOffset(const Bytes& b) {
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return Result<size_t>::failure("not a PE file (no MZ header)");
    uint32_t lfanew;
    std::memcpy(&lfanew, b.data() + 0x3C, 4);
    if (static_cast<uint64_t>(lfanew) + 24 + 70 > b.size()) return Result<size_t>::failure("truncated PE header");
    if (std::memcmp(b.data() + lfanew, "PE\0\0", 4) != 0) return Result<size_t>::failure("not a PE file (no PE signature)");
    uint16_t magic;
    std::memcpy(&magic, b.data() + lfanew + 24, 2);
    if (magic != 0x10B && magic != 0x20B) return Result<size_t>::failure("unknown PE optional header");
    return static_cast<size_t>(lfanew) + 24 + 68;
}
} // namespace

Result<int> readPeSubsystem(const fs::path& exe) {
    auto b = readFileBytes(exe);
    if (!b) return Result<int>::failure("cannot read " + narrow(exe.wstring()));
    auto off = peSubsystemOffset(*b);
    if (!off) return Result<int>::failure(off.error());
    uint16_t v;
    std::memcpy(&v, b->data() + *off, 2);
    return static_cast<int>(v);
}

Status setPeSubsystem(const fs::path& exe, int subsystem) {
    if (subsystem != 2 && subsystem != 3) return Status::failure("subsystem must be 2 (GUI) or 3 (console)");
    auto b = readFileBytes(exe);
    if (!b) return Status::failure("cannot read " + narrow(exe.wstring()));
    auto off = peSubsystemOffset(*b);
    if (!off) return Status::failure(off.error());
    uint16_t v = static_cast<uint16_t>(subsystem);
    std::memcpy(b->data() + *off, &v, 2);
    return writeFileBytes(exe, *b) ? Status::success() : Status::failure("cannot write " + narrow(exe.wstring()));
}

#ifdef _WIN32
#pragma pack(push, 2)
struct IcoDirEntry {
    BYTE width, height, colors, reserved;
    WORD planes, bitCount;
    DWORD bytesInRes, imageOffset;
};
struct GrpIconDirEntry {
    BYTE width, height, colors, reserved;
    WORD planes, bitCount;
    DWORD bytesInRes;
    WORD id;
};
#pragma pack(pop)

Status setExecutableIcon(const fs::path& exe, const fs::path& icoFile) {
    auto ico = readFileBytes(icoFile);
    if (!ico || ico->size() < 6) return Status::failure("cannot read icon file");
    uint16_t type, count;
    std::memcpy(&type, ico->data() + 2, 2);
    std::memcpy(&count, ico->data() + 4, 2);
    if (type != 1 || count == 0 || ico->size() < 6 + count * sizeof(IcoDirEntry)) return Status::failure("not a valid .ico");
    HANDLE h = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
    if (!h) return Status::failure("BeginUpdateResource failed");
    Bytes group(6);
    std::memcpy(group.data(), ico->data(), 6);
    for (uint16_t i = 0; i < count; ++i) {
        IcoDirEntry e;
        std::memcpy(&e, ico->data() + 6 + i * sizeof(IcoDirEntry), sizeof(e));
        if (static_cast<uint64_t>(e.imageOffset) + e.bytesInRes > ico->size()) {
            EndUpdateResourceW(h, TRUE);
            return Status::failure("corrupt .ico");
        }
        WORD id = static_cast<WORD>(i + 1);
        if (!UpdateResourceW(h, MAKEINTRESOURCEW(3) /*RT_ICON*/, MAKEINTRESOURCEW(id), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                             ico->data() + e.imageOffset, e.bytesInRes)) {
            EndUpdateResourceW(h, TRUE);
            return Status::failure("UpdateResource(RT_ICON) failed");
        }
        GrpIconDirEntry g{e.width, e.height, e.colors, e.reserved, e.planes, e.bitCount, e.bytesInRes, id};
        auto* gp = reinterpret_cast<uint8_t*>(&g);
        group.insert(group.end(), gp, gp + sizeof(g));
    }
    if (!UpdateResourceW(h, MAKEINTRESOURCEW(14) /*RT_GROUP_ICON*/, MAKEINTRESOURCEW(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         group.data(), static_cast<DWORD>(group.size()))) {
        EndUpdateResourceW(h, TRUE);
        return Status::failure("UpdateResource(RT_GROUP_ICON) failed");
    }
    if (!EndUpdateResourceW(h, FALSE)) return Status::failure("EndUpdateResource failed");
    return {};
}
#else
Status setExecutableIcon(const fs::path&, const fs::path&) { return Status::failure("icons are only supported on Windows"); }
#endif

} // namespace fledge
