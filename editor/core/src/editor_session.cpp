#include "fledge/core/legacy.h"
#include "fledge/editor/editor_session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

#include "fledge/core/log.h"
#include "fledge/core/paths.h"
#include "fledge/scene/components.h"

namespace fledge {

EditorSession::EditorSession()
    : toolsDir(executablePath().parent_path()),
      m_world(std::make_unique<GameWorld>(m_vfs, m_registry)),
      m_history(std::make_unique<CommandStack>(m_world->scene())) {
    m_history->onChanged = [this] { markDirty(); };
    autosaveClock = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    wallClock = [] { return editor::wallClockMs(); };
    installAssetHooks();
    // No project yet: content:/ is in-memory scratch space, so materials (and other generated assets) work before a project
    // exists. createProject moves them into the new project.
    m_scratch = std::make_shared<MemoryProvider>();
    m_vfs.mount("content", m_scratch);
    m_imports = std::make_unique<editor::ImportQueue>(storeImport, enginePaths().userData / "StoreDownloads");
}

Result<store::ImportReport> EditorSession::importStoreFile(const fs::path& source, store::ImportOptions options) {
    if (!m_project) return Result<store::ImportReport>::failure(kNoProjectHelp);
    if (options.destRoot.empty() || options.destRoot == "StoreImports") options.destRoot = storeImport.destination.empty() ? "StoreImports" : storeImport.destination;
    // staging lives in the project's Intermediate/ (never shipped), one folder per import, removed afterwards
    fs::path staging = m_project->rootDir() / "Intermediate" / "StoreImport" / std::format("{}-{}", ++m_importSerial,
                                                                                    std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
    std::error_code ec;
    fs::remove_all(osPath(staging), ec);
    fs::create_directories(osPath(staging), ec);
    options.staging = staging;
    auto r = store::importStoreFile(source, m_vfs, m_registry, options);
    fs::remove_all(osPath(staging), ec);
    if (r) refreshAssets({});
    return r;
}

// ------------------------------------------------------------------ assets --
namespace {
// Editor loader: parses SOURCE files (OBJ, images) so edits on disk show up
// immediately; already-cooked files (.cmesh/.ctex, e.g. from glTF) load as-is.
class SourceAssetLoader final : public AssetLoader {
public:
    explicit SourceAssetLoader(const Vfs& vfs) : m_vfs(vfs) {}
    std::optional<MeshData> loadMesh(const std::string& vpath, std::string* error) override {
        auto bytes = m_vfs.read(vpath);
        if (!bytes) {
            if (error) *error = "not found: " + vpath;
            return std::nullopt;
        }
        if (endsWith(toLower(vpath), ".cmesh")) return readCookedMesh(*bytes, error);
        return parseObj(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), error);
    }
    std::optional<TextureData> loadTexture(const std::string& vpath, std::string* error) override {
        auto bytes = m_vfs.read(vpath);
        if (!bytes) {
            if (error) *error = "not found: " + vpath;
            return std::nullopt;
        }
        std::string l = toLower(vpath);
        if (endsWith(l, ".ctex")) return readCookedTexture(*bytes, error);
        if (endsWith(l, ".hdr")) {
            auto hdr = decodeHdr(*bytes, error);
            if (!hdr) return std::nullopt;
            return hdrToTexture(*hdr);
        }
        auto img = decodeImage(*bytes, error);
        if (!img) return std::nullopt;
        std::string scheme, rel;
        bool srgb = Vfs::split(vpath, scheme, rel) ? textureIsSrgb(m_vfs, rel) : true;
        return uncompressedTexture(*img, true, srgb);
    }

private:
    const Vfs& m_vfs;
};
} // namespace

void EditorSession::installAssetHooks() {
    m_world->setAssetLoader(std::make_shared<SourceAssetLoader>(m_vfs));
    m_history->fileIO.read = [this](const std::string& vpath) { return m_vfs.readText(vpath); };
    m_history->fileIO.write = [this](const std::string& vpath, const std::optional<std::string>& content) {
        if (content) {
            m_vfs.writeText(vpath, *content);
            return;
        }
        std::string scheme, rel;
        if (!Vfs::split(vpath, scheme, rel)) return;
        if (auto* dir = dynamic_cast<DirectoryProvider*>(m_vfs.provider(scheme))) {
            std::error_code ec;
            fs::remove(dir->resolve(rel), ec);
        }
    };
    m_history->fileIO.applied = [this](const std::vector<std::string>& vpaths) {
        std::vector<std::string> rel;
        bool projectFile = false;
        for (auto& p : vpaths) {
            if (startsWith(p, "content:/")) rel.push_back(p.substr(9));
            if (startsWith(p, "project:/")) projectFile = true;
        }
        if (projectFile && m_project) {  // undo/redo of a graphics change: reload the settings blocks
            if (auto p = Project::load(m_project->file)) {
                m_project->graphics = p->graphics;
                m_project->render = p->render;
                m_world->graphicsSettings = p->graphics;
                m_world->renderSettings = p->render;
                m_world->uiSettings = p->ui;
            }
        }
        if (!rel.empty() || !projectFile) refreshAssets(rel);
    };
}

void EditorSession::refreshAssets(const std::vector<std::string>& changed) {
    (void)m_registry.load(m_vfs);
    m_registry.rescan(m_vfs);
    invalidateChangedAssets(changed);
}

void EditorSession::invalidateChangedAssets(const std::vector<std::string>& changed) {
    if (changed.empty()) m_world->invalidateAll();
    for (auto& rel : changed) m_world->invalidateAsset("content:/" + rel);
    ++m_assetGeneration;
}

void EditorSession::startWatching(bool preferNative) {
    if (!m_project) return;
    m_watcher = FileWatcher::create(m_project->contentDir(), preferNative);
    if (m_watcher) FLEDGE_LOG_INFO("editor", "Watching {} ({})", narrow(m_project->contentDir().wstring()), m_watcher->backend());
}

std::vector<std::string> EditorSession::pollAssetChanges() {
    if (!m_watcher) return {};
    auto now = std::chrono::steady_clock::now();
    for (auto& p : m_watcher->poll()) {
        // Our own bookkeeping files and editor-only sources don't need reloads.
        if (endsWith(p, AssetRegistry::kMetaExtension) || p == AssetRegistry::kFileName ||
            (startsWith(p, std::string(AssetRegistry::kFileName) + ".") && endsWith(p, ".tmp")) ||
            p.find("/source/") != std::string::npos) continue;
        m_pendingChanges.push_back(p);
        m_lastChange = now;
    }
    if (m_pendingChanges.empty() || now - m_lastChange < std::chrono::milliseconds(300)) return {};  // debounce bursts
    std::sort(m_pendingChanges.begin(), m_pendingChanges.end());
    m_pendingChanges.erase(std::unique(m_pendingChanges.begin(), m_pendingChanges.end()), m_pendingChanges.end());
    auto changed = std::move(m_pendingChanges);
    m_pendingChanges.clear();
    refreshAssets(changed);
    FLEDGE_LOG_INFO("editor", "Live reload: {} changed file(s), e.g. {}", changed.size(), changed.front());
    return changed;
}

Result<ImportResult> EditorSession::importAsset(const fs::path& source, const ModelImportOptions& options) {
    if (!m_project) return Result<ImportResult>::failure(kNoProjectHelp);
    auto r = fledge::importAsset(source, m_vfs, m_registry, options);
    if (r) {
        std::vector<std::string> rel;
        for (auto& c : r->created) rel.push_back(c.path);
        refreshAssets(rel);
    }
    return r;
}

Result<ImportResult> EditorSession::reimportAsset(const std::string& ref, std::optional<bool> fixZeroWeights, std::optional<bool> fixZeroNormals) {
    if (!m_project) return Result<ImportResult>::failure(kNoProjectHelp);
    std::string vpath = m_registry.resolve(ref);
    const AssetRecord* rec = startsWith(vpath, "content:/") ? m_registry.findByPath(vpath.substr(9)) : nullptr;
    if (!rec) return Result<ImportResult>::failure("unknown asset: " + ref);
    AssetRecord copy = *rec;
    auto r = fledge::reimportAsset(copy, m_vfs, m_registry, fixZeroWeights, fixZeroNormals);
    if (r) {
        std::vector<std::string> rel;
        for (auto& c : r->created) rel.push_back(c.path);
        refreshAssets(rel);
    }
    return r;
}

std::optional<Material> EditorSession::loadMaterial(const std::string& ref) {
    std::string vpath = m_registry.resolve(ref);
    auto text = vpath.empty() ? std::nullopt : m_vfs.readText(vpath);
    if (!text) return std::nullopt;
    auto m = Material::fromJson(Json::parse(*text, nullptr, false));
    if (!m) return std::nullopt;
    return m.value();
}

Result<AssetRecord> EditorSession::createMaterial(const std::string& name, const Json& properties) {
    // Without a project the material lives in the scratch content (memory) until createProject / save into a project.
    std::string safe;
    for (char c : name) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    if (safe.empty()) safe = "Material";
    std::string rel = "Materials/" + safe + ".fmat";
    for (int i = 2; m_vfs.exists("content:/" + rel); ++i) rel = std::format("Materials/{}_{}.fmat", safe, i);
    Material m;
    if (auto st = Material::typeInfo().fromJson(&m, properties.is_null() ? Json::object() : properties); !st)
        return Result<AssetRecord>::failure(st.error);
    m_history->begin("Create material " + safe);
    for (const std::string& f : {"content:/" + rel, "content:/" + rel + AssetRegistry::kMetaExtension,
                                 std::string("content:/") + AssetRegistry::kFileName})
        m_history->touchFile(f);
    m_vfs.writeText("content:/" + rel, m.toJson().dump(2));
    Guid g = m_registry.add(rel, AssetType::Material);
    m_registry.writeMeta(m_vfs, *m_registry.find(g));
    m_registry.save(m_vfs);
    m_history->commit();
    refreshAssets({rel});
    return *m_registry.find(g);
}

Status EditorSession::setMaterialProperty(const std::string& ref, const std::string& property, const Json& value, const std::string& mergeKey) {
    std::string vpath = m_registry.resolve(ref);
    auto mat = loadMaterial(ref);
    if (!mat) return Status::failure("material not found: " + ref);
    const PropertyInfo* p = Material::typeInfo().find(property);
    if (!p) return Status::failure("material has no property '" + property + "'");
    if (auto st = p->fromJson(&*mat, value); !st) return st;
    m_history->begin(std::format("Material {}", property), EditSource::User, mergeKey);
    m_history->touchFile(vpath);
    m_vfs.writeText(vpath, mat->toJson().dump(2));
    m_history->commit();
    m_world->invalidateAsset(vpath);
    ++m_assetGeneration;
    return {};
}

Status EditorSession::assignMaterial(EntityId e, const std::string& materialRef) {
    std::string vpath = m_registry.resolve(materialRef);
    const AssetRecord* rec = startsWith(vpath, "content:/") ? m_registry.findByPath(vpath.substr(9)) : nullptr;
    if (!rec || rec->type != AssetType::Material) return Status::failure("not a material asset: " + materialRef);
    // 1.3.0 fix: an animated character takes the material on its Animator (it has no MeshRenderer; adding one used to put a
    // 1 m builtin cube around the character)
    if (scene().get<AnimatorComponent>(e) && !scene().get<MeshRendererComponent>(e))
        return setProperty(e, "Animator", "material", rec->guid.toString());
    return setProperty(e, "MeshRenderer", "material", rec->guid.toString());
}

Result<EntityId> EditorSession::instantiateAsset(const std::string& ref, EntityId parent, Vec3 position) {
    std::string vpath = m_registry.resolve(ref);
    const AssetRecord* rec = startsWith(vpath, "content:/") ? m_registry.findByPath(vpath.substr(9)) : nullptr;
    if (!rec) return Result<EntityId>::failure("unknown asset: " + ref);
    std::string base = narrow(pathFromUtf8(rec->path).stem().wstring());
    if (rec->type == AssetType::Mesh) {
        EntityId e = createEntity(base, parent, {{"Transform", {{"position", position}}}, {"MeshRenderer", {{"mesh", rec->guid.toString()}}}});
        if (!e) return Result<EntityId>::failure("instantiate failed");
        return e;
    }
    if (rec->type != AssetType::Model) return Result<EntityId>::failure(std::format("cannot instantiate a {} asset", toString(rec->type)));
    auto text = m_vfs.readText(vpath);
    auto model = ModelAsset::fromJson(Json::parse(text.value_or("{}"), nullptr, false));
    if (!model) return Result<EntityId>::failure(model.error());
    m_history->begin("Instantiate " + model->name);
    EntityId root = scene().createEntity(model->name);
    if (parent && scene().isValid(parent)) (void)scene().setParent(root, parent, -1, false);
    scene().get<TransformComponent>(root)->position = position;
    std::vector<EntityId> ids;
    for (const auto& n : model->nodes) {
        EntityId e = scene().createEntity(n.name);
        (void)scene().setParent(e, n.parent >= 0 ? ids[static_cast<size_t>(n.parent)] : root, -1, false);
        auto* t = scene().get<TransformComponent>(e);
        t->position = n.position;
        t->rotation = n.rotation;
        t->scale = n.scale;
        for (size_t k = 0; k < n.meshes.size(); ++k) {
            EntityId target = e;
            if (k > 0) {  // extra primitives become children
                target = scene().createEntity(std::format("{} [{}]", n.name, k));
                (void)scene().setParent(target, e, -1, false);
            }
            MeshRendererComponent mr;
            mr.mesh = n.meshes[k].mesh;
            mr.material = n.meshes[k].material;
            mr.color = {1, 1, 1};
            scene().add<MeshRendererComponent>(target, mr);
        }
        ids.push_back(e);
    }
    m_history->commit();
    return root;
}

Status EditorSession::saveRenderSettingsToProject() {
    if (!m_project) return Status::failure(kNoProjectHelp);
    m_project->render = m_world->renderSettings;
    m_project->render.debugView = DebugView::None;  // debug views are a viewport tool, not a project default
    return m_project->save();
}

Status EditorSession::setGraphicsSettings(const GraphicsSettings& graphics, const RenderSettings* render, const std::string& label,
                                          EditSource source) {
    if (!m_project) return Status::failure(kNoProjectHelp);
    GraphicsSettings g = graphics;
    g.sanitize();
    std::string vpath = "project:/" + narrow(m_project->file.filename().wstring());
    beginEdit(label, source);
    m_history->touchFile(vpath);
    m_project->graphics = g;
    if (render) {
        m_project->render = *render;
        m_project->render.sanitize();
        m_project->render.debugView = DebugView::None;
    }
    Status st = m_project->save();
    if (!st) {
        cancelEdit();
        return st;
    }
    m_world->graphicsSettings = m_project->graphics;
    if (render) m_world->renderSettings = m_project->render;
    m_world->uiSettings = m_project->ui;
    endEdit();
    return {};
}

Result<CookReport> EditorSession::cook(bool force) {
    if (!m_project) return Result<CookReport>::failure(kNoProjectHelp);
    CookOptions opt;
    opt.force = force;
    return cookProject(*m_project, opt);
}

EditorSession::~EditorSession() {
    if (playing()) (void)stop();
}

fs::path EditorSession::runtimeExe() const {
    return toolsDir / "fledge_runtime.exe";
}

Status EditorSession::recordFeatureUse(const FeatureRequirement& r) {
    if (!m_project) return {};
    Project copy = *m_project;
    if (auto onDisk = Project::load(m_project->file)) copy.requiredFeatures = onDisk->requiredFeatures;  // keep what others added
    if (!copy.requireFeature(r)) return {};
    if (auto st = copy.save(); !st) return st;
    m_project->requiredFeatures = copy.requiredFeatures;
    FLEDGE_LOG_INFO("editor", "This project now uses {} ({}): it opens in {} from now on (docs/EDITIONS.md)", r.title, r.id,
                    editionName(m_project->minEdition()));
    return {};
}

Status EditorSession::openProject(const fs::path& fileIn) {
    // Editions (docs/EDITIONS.md): decline a project that needs features this executable lacks BEFORE anything can
    // touch it (no migration, no .meta rescan, no lock or temp files; the current project, if any, stays open).
    m_decline.reset();
    if (EditionCheck check = checkProjectEdition(fileIn); !check.ok) {
        FLEDGE_LOG_WARN("editor", "Declined {}: {}", narrow(fileIn.wstring()), check.summary());
        Status st = Status::failure(check.summary());
        m_decline = std::move(check);
        return st;
    }
    fs::path file = fileIn;
    if (legacy::isLegacyProjectFile(file)) {  // pre-0.8 project: work on a converted copy (the original is untouched)
        auto m = legacy::migrateProject(file);
        if (!m) return Status::failure("cannot convert the older project: " + m.error());
        FLEDGE_LOG_INFO("editor", "Converted an older project into {} (original unchanged)", narrow(m->projectFile.parent_path().wstring()));
        file = m->projectFile;
    }
    reportLoad("Reading the project file", 0.08f);
    auto p = Project::load(file);
    if (!p) return Status::failure(p.error());
    reportLoad("Closing the previous project", 0.14f);
    if (playing()) (void)stop();
    if (m_project) closeProjectRecovery();  // the project being replaced: a clean scene loses its snapshots, an unsaved one gets a final snapshot
    m_project = p.value();
    m_vfs.mount("content", std::make_shared<DirectoryProvider>(m_project->contentDir()));
    m_vfs.mount("scripts", std::make_shared<DirectoryProvider>(m_project->scriptsDir()));
    m_vfs.mount("project", std::make_shared<DirectoryProvider>(m_project->rootDir()));  // .fledge snapshots (undo)
    m_hasBuild = false;
    m_lastBuild = {};
    aiTouchedScripts.clear();
    m_world->scripts().unloadModule();
    if (auto st = m_world->scripts().input().load(m_project->input); !st) FLEDGE_LOG_WARN("editor", "{}", st.error);
    reportLoad("Loading scripts", 0.22f);
    if (fs::path dll = m_project->currentScriptModule(); !dll.empty()) (void)loadScriptModule(dll);
    reportLoad("Reading the asset index", 0.28f);
    if (auto st = m_registry.load(m_vfs); !st) FLEDGE_LOG_WARN("editor", "{}", st.error);
    m_registry.rescan(m_vfs, true, [&](size_t index, size_t count, const std::string& path) {
        if (count == 0) reportLoad("Listing content files", 0.32f);
        else reportLoad(std::format("Scanning assets {}/{}  {}", index, count, path), 0.32f + 0.40f * static_cast<float>(index) / static_cast<float>(count));
    });
    m_world->renderSettings = m_project->render;
    m_world->uiSettings = m_project->ui;
    m_world->graphicsSettings = m_project->graphics;
    selection.clear();
    FLEDGE_LOG_INFO("editor", "Opened project '{}' ({})", m_project->name, narrow(m_project->file.wstring()));
    reportLoad("Loading " + m_project->startupScene, 0.78f);
    auto st = loadScene("content:/" + m_project->startupScene);
    reportLoad("Scene loaded", 0.86f);
    if (!st) FLEDGE_LOG_WARN("editor", "{}", st.error);
    m_autosaveScheduler.reset();
    m_recoveryOffer.reset();
    m_keepRecoveryOnClose = false;
    m_lastSnapshotSerial.reset();
    computeRecoveryOffer();
    return {};
}

// ------------------------------------------------------------------ scripts --
ScriptBuildOptions EditorSession::scriptBuildOptions() const {
    ScriptBuildOptions o;
    if (!scriptSettings.compiler.empty())
        if (auto tc = findToolchain(scriptSettings.compiler)) o.toolchain = *tc;
    return o;
}

Status EditorSession::loadScriptModule(const fs::path& dll) {
    auto st = m_world->scripts().loadModule(dll);
    if (st) m_world->scripts().input().load(m_project ? m_project->input : Json());
    return st;
}

void EditorSession::applyScriptBuild(const ScriptBuildResult& r) {
    m_lastBuild = r;
    m_hasBuild = true;
    if (r.ok && !r.module.empty() && r.module != m_world->scripts().module().path) {
        if (auto st = loadScriptModule(r.module); !st) {
            m_lastBuild.ok = false;
            m_lastBuild.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, "module load failed: " + st.error});
        }
    }
}

ScriptBuildResult EditorSession::buildScripts(bool force) {
    ScriptBuildResult r;
    if (!m_project) {
        r.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, kNoProjectHelp});
        return r;
    }
    ScriptBuildOptions o = scriptBuildOptions();
    o.force = force;
    r = fledge::buildScripts(*m_project, o);
    applyScriptBuild(r);
    return m_lastBuild;
}

Status EditorSession::writeScriptFile(const std::string& relPath, const std::string& content, bool fromAi) {
    if (!m_project) return Status::failure(kNoProjectHelp);
    auto full = resolveScriptPath(*m_project, relPath);
    if (!full) return Status::failure(full.error());
    std::error_code ec;
    fs::create_directories(full->parent_path(), ec);
    std::string rel = narrow(full->lexically_relative(fs::weakly_canonical(m_project->scriptsDir(), ec)).generic_wstring());
    std::string vpath = "scripts:/" + rel;
    m_history->begin("Edit script " + rel, fromAi ? EditSource::AI : EditSource::User);
    m_history->touchFile(vpath);
    bool ok = writeFileText(*full, content);
    m_history->commit();
    if (!ok) return Status::failure("cannot write " + narrow(full->wstring()));
    if (fromAi) aiTouchedScripts.insert(rel);
    return {};
}

namespace {
Json& behaviourList(Scene& s, EntityId e, Json& storage) {
    auto* bc = s.get<BehavioursComponent>(e);
    storage = bc && bc->list.is_array() ? bc->list : Json::array();
    return storage;
}
int findBehaviour(const Json& list, const std::string& cls) {
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].is_object() && list[i].value("class", "") == cls) return static_cast<int>(i);
    return -1;
}
} // namespace

Status EditorSession::attachBehaviour(EntityId e, const std::string& cls, const Json& props) {
    if (!scene().isValid(e)) return Status::failure("invalid entity");
    if (cls.empty()) return Status::failure("class name is empty");
    const auto& mod = m_world->scripts().module();
    if (m_world->scripts().hasModule() && !mod.find(cls)) {
        std::string known;
        for (auto& c : mod.classes)
            if (!c.isService) known += (known.empty() ? "" : ", ") + c.name;
        return Status::failure("unknown behaviour class '" + cls + "' (loaded module has: " + (known.empty() ? "none" : known) +
                               "). Build the scripts after adding a class.");
    }
    if (const auto* cd = mod.find(cls); cd && cd->isService) return Status::failure(cls + " is a Service (global), not a Behaviour");
    Json tmp;
    Json list = behaviourList(scene(), e, tmp);
    if (findBehaviour(list, cls) >= 0) return Status::failure(cls + " is already attached to this entity");
    list.push_back({{"class", cls}, {"enabled", true}, {"properties", props.is_object() ? props : Json::object()}});
    beginEdit("Attach " + cls);
    if (!scene().get<BehavioursComponent>(e)) (void)addComponent(e, "Behaviours");
    auto st = setProperty(e, "Behaviours", "list", list);
    endEdit();
    if (st && playing()) m_world->scripts().attachLive(e, cls, props.is_object() ? props : Json::object());
    return st;
}

Status EditorSession::detachBehaviour(EntityId e, const std::string& cls) {
    if (!scene().isValid(e)) return Status::failure("invalid entity");
    Json tmp;
    Json list = behaviourList(scene(), e, tmp);
    int i = findBehaviour(list, cls);
    if (i < 0) return Status::failure(cls + " is not attached to this entity");
    list.erase(static_cast<size_t>(i));
    if (playing()) m_world->scripts().detachLive(e, cls);
    return setProperty(e, "Behaviours", "list", list);
}

Status EditorSession::setBehaviourEnabled(EntityId e, const std::string& cls, bool enabled) {
    Json tmp;
    Json list = behaviourList(scene(), e, tmp);
    int i = findBehaviour(list, cls);
    if (i < 0) return Status::failure(cls + " is not attached to this entity");
    list[static_cast<size_t>(i)]["enabled"] = enabled;
    return setProperty(e, "Behaviours", "list", list);
}

Status EditorSession::setBehaviourProperty(EntityId e, const std::string& cls, const std::string& prop, const Json& value,
                                           const std::string& mergeKey) {
    if (!scene().isValid(e)) return Status::failure("invalid entity");
    Json tmp;
    Json list = behaviourList(scene(), e, tmp);
    int i = findBehaviour(list, cls);
    if (i < 0) return Status::failure(cls + " is not attached to this entity");
    if (const auto* cd = m_world->scripts().module().find(cls); cd && !cd->find(prop)) {
        std::string known;
        for (auto& p : cd->properties) known += (known.empty() ? "" : ", ") + p.name;
        return Status::failure(std::format("{} has no property '{}' (properties: {})", cls, prop, known.empty() ? "none" : known));
    }
    if (playing()) {
        if (auto st = m_world->scripts().setLiveProperty(e, cls, prop, value); !st) return st;
    }
    Json& item = list[static_cast<size_t>(i)];
    if (!item.contains("properties") || !item["properties"].is_object()) item["properties"] = Json::object();
    item["properties"][prop] = value;
    return setProperty(e, "Behaviours", "list", list, mergeKey);
}

Result<Json> EditorSession::behaviourProperties(EntityId e, const std::string& cls) {
    if (!scene().isValid(e)) return Result<Json>::failure("invalid entity");
    Json tmp;
    Json list = behaviourList(scene(), e, tmp);
    int i = findBehaviour(list, cls);
    if (i < 0) return Result<Json>::failure(cls + " is not attached to this entity");
    if (playing())
        if (auto live = m_world->scripts().liveProperties(e, cls)) return *live;
    Json out = Json::object();
    if (const auto* cd = m_world->scripts().module().find(cls))
        for (auto& p : cd->properties) out[p.name] = p.defaultValue;
    const Json& item = list[static_cast<size_t>(i)];
    if (item.contains("properties") && item["properties"].is_object())
        for (auto& [k, v] : item["properties"].items()) out[k] = v;
    return out;
}

void EditorSession::simulateFrames(int frames, const InputFeed& feed) {
    // stepOnce, not tick(dt): tick() does nothing while paused (Time Machine scrubbing) and depends on the accumulator.
    for (int i = 0; i < frames && playing(); ++i) {
        if (feed) {
            script::InputFrame in;
            feed(i, in);
            liveInput = in;
        }
        stepOnce();
    }
    if (feed) liveInput = {};
    // Animation LOD skips far Animators on some frames (no camera in a headless session = the viewer is the origin); the
    // caller reads the scene right after this returns, so apply their pending time now (docs/MCP.md "play").
    if (frames > 0 && playing()) m_world->catchUpAnimation();
}

Status EditorSession::newProject(const fs::path& directory, const std::string& name) {
    auto p = Project::createNew(directory, name);
    if (!p) return Status::failure(p.error());
    return openProject(p->file);
}

Status EditorSession::createProject(const fs::path& directory, const std::string& name) {
    if (m_project) return newProject(directory, name);  // a project is open: plain new project
    // Adopt the work done without a project: the current scene becomes the startup scene, scratch assets keep their GUIDs.
    Json sceneJson = scene().toJson();
    bool keepScene = !scene().entities().empty();
    std::vector<std::pair<std::string, Bytes>> files;
    for (const std::string& rel : m_scratch->list())
        if (rel != AssetRegistry::kFileName)
            if (auto b = m_scratch->read(rel)) files.emplace_back(rel, std::move(*b));
    std::vector<AssetRecord> records;
    for (const std::string& rel : m_scratch->list())
        if (const AssetRecord* r = m_registry.findByPath(rel)) records.push_back(*r);
    if (playing()) (void)stop();
    auto p = Project::createNew(directory, name);
    if (!p) return Status::failure(p.error());
    if (auto st = openProject(p->file); !st) return st;
    for (auto& [rel, bytes] : files)
        if (!m_vfs.exists("content:/" + rel)) m_vfs.write("content:/" + rel, bytes);
    for (auto& r : records) m_registry.addWithGuid(r.path, r.type, r.guid);
    m_registry.rescan(m_vfs);
    m_registry.save(m_vfs);
    if (keepScene) {
        if (auto st = scene().fromJson(sceneJson); !st) return st;
        if (auto st = saveScene("content:/" + m_project->startupScene); !st) return st;
    }
    m_history->clear();
    m_scratch = std::make_shared<MemoryProvider>();
    FLEDGE_LOG_INFO("editor", "Created project '{}' with the current scene and {} scratch asset file(s)", name, files.size());
    return {};
}
Status EditorSession::loadScene(const std::string& vpath) {
    if (playing()) return Status::failure("stop play mode first");
    auto st = m_world->loadScene(vpath);
    if (st) {
        selection.clear();
        m_history->clear();  // loading a scene is not undoable
        dirty = false;
        m_autosaveScheduler.reset();
    }
    return st;
}

Status EditorSession::saveScene(const std::string& vpath) { return saveSceneImpl(vpath, true); }

Status EditorSession::saveSceneImpl(const std::string& vpath, bool manual) {
    std::string path = vpath.empty() ? m_world->currentScenePath() : vpath;
    if (path.empty()) path = "content:/Scenes/Untitled.fscene";
    if (!startsWith(path, "content:/")) return Status::failure("scene path must start with content:/");
    if (!endsWith(path, ".fscene")) return Status::failure("scene path must end with .fscene");
    auto st = m_world->saveScene(path);
    if (st) {
        dirty = false;
        // Recovery: the scene is saved, so its snapshots are obsolete (an undecided recovery offer stays untouched). A manual
        // save also ends the auto-save backup: the user has now seen and kept this state.
        if (m_project && !m_recoveryOffer) {
            if (auto store = recoveryStore()) store->clear();
            m_keepRecoveryOnClose = false;
        }
        m_lastSnapshotSerial.reset();
        m_autosaveScheduler.reset();
        if (manual && m_project) {
            std::error_code ec;
            fs::remove(osPath(autoSaveBackupFile(path)), ec);
        }
        // Editions: the first save that uses a component of an Advanced module records its feature in the manifest.
        for (const auto& [id, version] : scene().usedFeatures())
            if (auto fst = recordFeatureUse(requirementFor(FeatureRegistry::get(), id, version)); !fst) FLEDGE_LOG_WARN("editor", "{}", fst.error);
        std::string rel = path.substr(9);
        if (!m_registry.findByPath(rel)) {
            m_registry.add(rel, AssetType::Scene);
            m_registry.save(m_vfs);
        }
    }
    return st;
}

Status EditorSession::newScene(const std::string& name) {
    if (playing()) return Status::failure("stop play mode first");
    m_history->begin("New scene");
    m_history->touchAll();
    Scene& s = scene();
    for (EntityId e : s.roots()) s.destroyEntity(e);
    s.name = name;
    EntityId cam = s.createEntity("Camera");
    s.get<TransformComponent>(cam)->position = {0, 2, 6};
    s.add<CameraComponent>(cam);
    EntityId sun = s.createEntity("Sun");
    s.get<TransformComponent>(sun)->rotation = Quat::fromEuler({radians(-50), radians(30), 0});
    s.add<LightComponent>(sun);
    m_history->commit();
    selection.clear();
    markDirty();
    return {};
}

Status EditorSession::play() {
    if (playing()) return {};
    m_history->closeMerge();
    m_snapshot.capture(scene());
    m_history->setEnabled(false);  // play-mode changes are thrown away on stop: keep them out of history
    m_world->modules().addSearchPath(toolsDir);
    if (m_project) m_world->saveDirectory = m_project->rootDir() / "Saved";  // quick save / load while playing in the editor
    if (m_project)
        for (auto& m : m_project->modules)
            if (auto st = m_world->modules().load(m); !st) FLEDGE_LOG_WARN("editor", "{}", st.error);
    m_world->beginPlay();
    m_accum = 0;
    m_playFrames = 0;
    m_tweaks.clear();
    liveInput = {};
    deferredEdits.clear();
    lastDeferredReport = nullptr;
    timeMachine.beginPlay(*this);
    FLEDGE_LOG_INFO("editor", "Play");
    return {};
}

void EditorSession::stopNet() {
    m_netClient.reset();
    m_netServer.reset();
    m_netYaw = 0;
    m_netNow = 0;
    m_netStatus.clear();
}

net::NetInput EditorSession::liveNetInput() {
    net::NetInput in;
    m_netYaw -= liveInput.mouseDx * 0.003f;
    in.yaw = m_netYaw;
    in.moveZ = (liveInput.down[83] ? 1.0f : 0.0f) - (liveInput.down[87] ? 1.0f : 0.0f);  // S / W
    in.moveX = (liveInput.down[68] ? 1.0f : 0.0f) - (liveInput.down[65] ? 1.0f : 0.0f);  // D / A
    in.buttons = liveInput.down[32] ? net::kButtonJump : 0;                              // Space
    in.quantize();
    return in;
}

void EditorSession::tickNet() {
    m_netNow += TimeMachine::kStep;
    if (m_netServer) {
        m_netServer->tickHost(liveNetInput());
        m_netServer->update(m_netNow);
        m_netStatus = std::format("Hosting UDP {}  ·  {} client{}", m_netServer->address().toString(), m_netServer->clientCount(),
                                  m_netServer->clientCount() == 1 ? "" : "s");
    }
    if (m_netClient) {
        m_netClient->setInput(liveNetInput());
        m_netClient->update(m_netNow);
        m_netStatus = std::format("Joined {}  ·  {}", m_netClient->state() == net::ClientState::Connected ? "connected" : net::clientStateName(m_netClient->state()),
                                  m_netClient->localPlayer() != kNullEntity ? "in world" : "waiting");
    }
}

Status EditorSession::startListenHost(std::unique_ptr<net::Transport> transport) {
    if (!transport) return Status::failure("no network socket");
    if (!playing()) {
        if (auto st = play(); !st) return st;
    }
    m_netClient.reset();
    m_netServer = std::make_unique<net::NetServer>(scene(), std::move(transport));
    m_netServer->spawnListenHost();
    m_netNow = 0;
    m_netStatus = std::format("Hosting UDP {}", m_netServer->address().toString());
    FLEDGE_LOG_INFO("editor", "{}", m_netStatus);
    return {};
}

Status EditorSession::startJoin(std::unique_ptr<net::Transport> transport, net::NetAddress server) {
    if (!transport) return Status::failure("no network socket");
    if (!server.valid()) return Status::failure("join address needs a port");
    if (!playing()) {
        if (auto st = play(); !st) return st;
    }
    m_netServer.reset();
    std::vector<EntityId> replicated(scene().pool<NetIdentityComponent>().entities());
    for (EntityId e : replicated)
        if (scene().isValid(e)) scene().destroyEntity(e);
    m_netClient = std::make_unique<net::NetClient>(scene(), std::move(transport), server);
    m_netNow = 0;
    m_netClient->connect(m_netNow);
    m_netStatus = std::format("Joining {}", server.toString());
    FLEDGE_LOG_INFO("editor", "{}", m_netStatus);
    return {};
}

Status EditorSession::playHost(uint16_t port) {
    if (playing()) return Status::failure("Stop Play before hosting");
    std::string err;
    auto t = net::UdpTransport::open({0u, port}, &err);
    if (!t) return Status::failure(err.empty() ? "could not bind the host port" : err);
    return startListenHost(std::move(t));
}

Status EditorSession::playJoin(const std::string& hostPort) {
    if (playing()) return Status::failure("Stop Play before joining");
    auto addr = net::NetAddress::parse(hostPort);
    if (!addr || addr->host == 0) return Status::failure("join address looks like host:port (example 127.0.0.1:7777)");
    std::string err;
    bool loop = (addr->host >> 24) == 127;
    auto t = net::UdpTransport::open({loop ? 0x7F000001u : 0u, 0}, &err);
    if (!t) return Status::failure(err.empty() ? "could not open a client socket" : err);
    return startJoin(std::move(t), *addr);
}

std::optional<net::NetAddress> EditorSession::netAddress() const {
    if (m_netServer) return m_netServer->address();
    return std::nullopt;
}

EntityId EditorSession::netLocalPlayer() const {
    if (m_netServer) return m_netServer->hostPlayer();
    if (m_netClient) return m_netClient->localPlayer();
    return kNullEntity;
}

std::optional<ViewItem> EditorSession::netView(float aspect) {
    EntityId e = netLocalPlayer();
    if (e == kNullEntity) return std::nullopt;
    auto* t = scene().get<TransformComponent>(e);
    if (!t) return std::nullopt;
    Vec3 forward = Quat::fromAxisAngle({0, 1, 0}, m_netYaw).rotate({0, 0, -1});
    ViewItem v;
    v.position = t->position - forward * 6.0f + Vec3{0, 3, 0};
    v.view = Mat4::lookAt(v.position, t->position + Vec3{0, 1, 0}, {0, 1, 0});
    v.projection = Mat4::perspective(radians(60), aspect, 0.1f, 500);
    return v;
}

Status EditorSession::setProjectInput(const Json& input, bool save) {
    if (!m_project) return Status::failure("no project is open");
    m_project->input = input;
    return save ? m_project->save() : Status{};
}

Status EditorSession::stop() {
    if (!playing()) return {};
    stopNet();
    m_world->endPlay();  // also resets the script input map
    liveInput = {};       // a key held in the game view (or fed by MCP) must not survive Stop
    scriptedInputOnly = false;
    m_paused = false;
    m_stepQueued = 0;
    timeMachine.reset();
    m_world->modules().unloadAll();  // allows rebuilding the DLL between sessions
    auto st = m_snapshot.restore(scene());
    m_history->setEnabled(true);
    m_lastTweaks = std::move(m_tweaks);
    m_tweaks.clear();
    // drop tweaks of entities that only existed during play
    std::erase_if(m_lastTweaks, [this](const PlayTweak& t) { return !scene().isValid(t.entity) || scene().displayName(t.entity) != t.entityName; });
    // 1.3.2: MCP edits made during Play with target "edit" land in the restored edit scene now
    lastDeferredReport = nullptr;
    if (st && !deferredEdits.empty() && replayDeferredEdits) {
        std::vector<DeferredEdit> queue = std::move(deferredEdits);
        deferredEdits.clear();
        lastDeferredReport = replayDeferredEdits(queue);
    }
    deferredEdits.clear();
    pruneSelection();
    FLEDGE_LOG_INFO("editor", "Stop (scene restored)");
    return st;
}

void EditorSession::stepOnce() {
    m_world->scripts().setInput(liveInput);
    m_world->tick(TimeMachine::kStep);
    tickNet();
    ++m_playFrames;
    timeMachine.afterStep(*this, liveInput);
}

void EditorSession::tick(float dt) {
    if (!playing()) {
        m_paused = false;
        return;
    }
    if (paused()) {
        for (; m_stepQueued > 1e-5f; m_stepQueued -= TimeMachine::kStep) stepOnce();
        m_stepQueued = 0;
        return;
    }
    // Fixed steps: the same inputs always give the same result, so the Time Machine can replay them.
    m_accum += std::min(dt, 1.0f);
    for (int n = 0; m_accum >= TimeMachine::kStep - 1e-5f && n < 60; ++n) {
        stepOnce();
        m_accum -= TimeMachine::kStep;
    }
    if (m_accum < 0) m_accum = 0;
}

// --------------------------------------------------------------------- ops --
namespace {
template <typename F>
Status runEdit(CommandStack& h, const std::string& label, const std::string& mergeKey, F&& fn) {
    h.begin(label, EditSource::User, mergeKey);
    Status st = fn();
    if (st) h.commit();
    else h.cancel();
    return st;
}
} // namespace

EntityId EditorSession::createEntity(const std::string& name, EntityId parent, const Json& components) {
    EntityId created = kNullEntity;
    auto st = runEdit(*m_history, "Create " + name, {}, [&]() -> Status {
        if (parent != kNullEntity && !scene().isValid(parent)) return Status::failure(std::format("invalid parent {}", parent));
        created = scene().createEntity(name);
        if (parent != kNullEntity)
            if (auto s = scene().setParent(created, parent, -1, false); !s) return s;
        for (auto& [type, values] : components.items()) {
            auto* info = ComponentRegistry::get().find(type);
            if (!info) return Status::failure("unknown component type '" + type + "'");
            void* c = scene().addComponent(created, type);
            if (auto s = info->type.fromJson(c, values.is_null() ? Json::object() : values); !s) return s;
        }
        return {};
    });
    if (!st) {
        FLEDGE_LOG_WARN("editor", "create entity failed: {}", st.error);
        return kNullEntity;
    }
    return created;
}

Status EditorSession::destroyEntities(const std::vector<EntityId>& entities) {
    auto st = runEdit(*m_history, entities.size() == 1 ? "Delete " + scene().displayName(entities[0]) : std::format("Delete {} entities", entities.size()),
                      {}, [&]() -> Status {
                          for (EntityId e : entities) {
                              if (!scene().isValid(e)) continue;  // may already be gone as a descendant
                              m_history->touchSubtree(e);
                              scene().destroyEntity(e);
                          }
                          return {};
                      });
    pruneSelection();
    return st;
}

Status EditorSession::rename(EntityId e, const std::string& name) {
    return runEdit(*m_history, "Rename to " + name, {}, [&]() -> Status {
        auto* n = scene().get<NameComponent>(e);
        if (!n) return Status::failure(std::format("invalid entity {}", e));
        m_history->touch(e);
        n->name = name;
        return {};
    });
}

Status EditorSession::setProperty(EntityId e, const std::string& component, const std::string& property, const Json& value,
                                  const std::string& mergeKey) {
    if (playing() && scene().isValid(e)) {  // Edit-in-Play: remember the tweak for "Keep" after Stop
        Json comps = scene().entityToJson(e).value("components", Json::object());
        Json before = comps.contains(component) && comps[component].contains(property) ? comps[component][property] : Json();
        auto it = std::find_if(m_tweaks.begin(), m_tweaks.end(),
                               [&](const PlayTweak& t) { return t.entity == e && t.component == component && t.property == property; });
        if (it == m_tweaks.end()) m_tweaks.push_back({e, scene().displayName(e), component, property, before, value});
        else it->after = value;
    }
    return runEdit(*m_history, std::format("Set {}.{}", component, property), mergeKey, [&]() -> Status {
        if (!scene().isValid(e)) return Status::failure(std::format("invalid entity {}", e));
        auto* info = ComponentRegistry::get().find(component);
        if (!info) return Status::failure("unknown component type '" + component + "'");
        const PropertyInfo* p = info->type.find(property);
        if (!p) return Status::failure(std::format("component '{}' has no property '{}'", component, property));
        m_history->touch(e);
        return p->fromJson(scene().addComponent(e, component), value);
    });
}

Status EditorSession::keepPlayTweaks(const std::vector<size_t>& indices) {
    if (playing()) return Status::failure("stop play mode first");
    if (indices.empty()) return {};
    m_history->begin(std::format("Keep {} play-mode change{}", indices.size(), indices.size() == 1 ? "" : "s"), EditSource::User);
    for (size_t i : indices) {
        if (i >= m_lastTweaks.size()) continue;
        const PlayTweak& t = m_lastTweaks[i];
        if (auto st = setProperty(t.entity, t.component, t.property, t.after); !st) {
            m_history->cancel();
            return st;
        }
    }
    m_history->commit();
    m_lastTweaks.clear();
    return {};
}

Status EditorSession::applyComponents(EntityId e, const Json& components, const std::string& label) {
    return runEdit(*m_history, label, {}, [&]() -> Status {
        if (!scene().isValid(e)) return Status::failure(std::format("invalid entity {}", e));
        if (!components.is_object()) return Status::failure("'components' must be an object {ComponentName: {property: value}}");
        m_history->touch(e);
        for (auto& [type, values] : components.items()) {
            auto* info = ComponentRegistry::get().find(type);
            if (!info) return Status::failure("unknown component type '" + type + "'");
            if (auto s = info->type.fromJson(scene().addComponent(e, type), values.is_null() ? Json::object() : values); !s) return s;
        }
        return {};
    });
}

Status EditorSession::addComponent(EntityId e, const std::string& type) {
    return runEdit(*m_history, "Add " + type, {}, [&]() -> Status {
        m_history->touch(e);
        return scene().addComponent(e, type) ? Status::success() : Status::failure("cannot add component '" + type + "'");
    });
}

Status EditorSession::removeComponent(EntityId e, const std::string& type) {
    return runEdit(*m_history, "Remove " + type, {}, [&]() -> Status {
        m_history->touch(e);
        return scene().removeComponent(e, type) ? Status::success() : Status::failure("component not present or not removable");
    });
}

Status EditorSession::setParent(EntityId child, EntityId parent, int index, bool keepWorld) {
    std::string label = parent ? std::format("Parent {} to {}", scene().displayName(child), scene().displayName(parent))
                               : std::format("Unparent {}", scene().displayName(child));
    return runEdit(*m_history, label, {}, [&]() -> Status {
        // Sibling indices of the old and new siblings change too.
        m_history->touch(child);
        for (EntityId p : {scene().parentOf(child), parent}) {
            const auto sibs = p ? scene().childrenOf(p) : scene().roots();
            for (EntityId s : sibs) m_history->touch(s);
        }
        return scene().setParent(child, parent, index, keepWorld);
    });
}

Status EditorSession::setWorldTransform(EntityId e, const Transform& world, const std::string& mergeKey) {
    return runEdit(*m_history, "Transform " + scene().displayName(e), mergeKey, [&]() -> Status {
        if (!scene().isValid(e)) return Status::failure(std::format("invalid entity {}", e));
        m_history->touch(e);
        scene().setWorldTransform(e, world);
        return {};
    });
}

std::vector<EntityId> EditorSession::duplicate(const std::vector<EntityId>& entities) {
    std::vector<EntityId> out;
    m_history->begin(std::format("Duplicate {} entities", entities.size()));
    for (EntityId root : entities) {
        if (!scene().isValid(root)) continue;
        bool ancestorSelected = false;
        for (EntityId o : entities)
            if (o != root && scene().isAncestor(o, root)) ancestorSelected = true;
        if (ancestorSelected) continue;
        std::vector<EntityId> src = {root};
        auto d = scene().descendants(root);
        src.insert(src.end(), d.begin(), d.end());
        std::unordered_map<EntityId, EntityId> map;
        for (EntityId s : src) {
            Json comps = scene().entityToJson(s)["components"];
            EntityId n = scene().createEntity();
            for (auto& [type, values] : comps.items())
                (void)ComponentRegistry::get().find(type)->type.fromJson(scene().addComponent(n, type), values);
            map[s] = n;
            EntityId p = scene().parentOf(s);
            EntityId np = s == root ? p : map[p];
            if (np) (void)scene().setParent(n, np, -1, false);
        }
        if (auto* n = scene().get<NameComponent>(map[root])) n->name += " (copy)";
        out.push_back(map[root]);
    }
    m_history->commit();
    return out;
}

bool EditorSession::undo() {
    if (playing()) return false;
    bool ok = m_history->undo();
    pruneSelection();
    return ok;
}

bool EditorSession::redo() {
    if (playing()) return false;
    bool ok = m_history->redo();
    pruneSelection();
    return ok;
}

// --------------------------------------------------------------- selection --
void EditorSession::select(EntityId e, bool additive) {
    if (!additive) selection.clear();
    if (e == kNullEntity) return;
    auto it = std::find(selection.begin(), selection.end(), e);
    if (it != selection.end()) {
        if (additive) selection.erase(it);  // ctrl-click toggles
        else selection.push_back(e);
        return;
    }
    selection.push_back(e);
}

bool EditorSession::isSelected(EntityId e) const { return std::find(selection.begin(), selection.end(), e) != selection.end(); }

void EditorSession::pruneSelection() {
    std::erase_if(selection, [this](EntityId e) { return !scene().isValid(e); });
}

Status EditorSession::setBootSplash(bool enabled) {
    if (!m_project) return Status::failure(kNoProjectHelp);
    Project next = *m_project;
    next.splash = enabled;
    return commitProject(next, "Boot splash");
}

Status EditorSession::commitProject(const Project& next, const std::string& label) {
    if (!m_project) return Status::failure(kNoProjectHelp);
    if (next.name.empty()) return Status::failure("project name is empty");
    if (next.gameName.empty()) return Status::failure("game name is empty");
    for (unsigned char c : next.gameName)
        if (!(std::isalnum(c) || c == '_' || c == '-'))
            return Status::failure("game name may only contain letters, digits, '_' and '-'");
    if (next.startupScene.empty() || next.startupScene.find("..") != std::string::npos)
        return Status::failure("startup scene must be a path under Content");
    Project stored = next;
    stored.file = m_project->file;
    stored.windowWidth = std::clamp(stored.windowWidth, 320, 7680);
    stored.windowHeight = std::clamp(stored.windowHeight, 240, 4320);
    stored.ui.fontSize = std::clamp(stored.ui.fontSize, 8.0f, 96.0f);
    beginEdit(label);
    m_history->touchFile("project:/" + narrow(stored.file.filename().wstring()));
    *m_project = stored;
    Status st = m_project->save();
    if (!st) {
        cancelEdit();
        return st;
    }
    m_world->renderSettings = m_project->render;
    m_world->graphicsSettings = m_project->graphics;
    m_world->uiSettings = m_project->ui;
    endEdit();
    return {};
}

Result<PackageResult> EditorSession::package(bool sidecar) {
    if (!m_project) return Result<PackageResult>::failure(kNoProjectHelp);
    if (playing()) return Result<PackageResult>::failure("stop play mode first");
    if (dirty) {
        if (auto st = saveScene(); !st) return Result<PackageResult>::failure("saving scene failed: " + st.error);
    }
    PackageOptions opt;
    opt.projectFile = m_project->file;
    opt.runtimeExe = runtimeExe();
    opt.moduleDir = toolsDir;
    opt.embedPak = !sidecar;
    return packageProject(opt);
}

// ------------------------------------------------------ auto-save / recovery --
void EditorSession::markDirty() {
    dirty = true;
    ++m_changeSerial;
}

fs::path EditorSession::recoveryDir() const {
    return m_project ? m_project->rootDir() / "Intermediate" / "Recovery" : fs::path();
}

std::optional<editor::RecoveryStore> EditorSession::recoveryStore() const {
    if (!m_project) return std::nullopt;
    return editor::RecoveryStore(recoveryDir());
}

std::string EditorSession::activeScenePath() const {
    std::string p = m_world->currentScenePath();
    if (p.empty() && m_project) p = "content:/" + m_project->startupScene;
    return p;
}

fs::path EditorSession::autoSaveBackupFile(const std::string& scenePath) const {
    std::string name = scenePath.size() > 9 ? scenePath.substr(9) : scenePath;
    for (char& c : name)
        if (c == '/' || c == '\\' || c == ':') c = '_';
    return recoveryDir() / "AutoSaveBackup" / pathFromUtf8(name + ".bak");
}

bool EditorSession::writeRecoverySnapshot(std::string* error) {
    auto store = recoveryStore();
    if (!store || !autosave.recovery || !dirty || playing() || m_history->inTransaction() || m_recoveryOffer) return false;
    auto st = store->write(activeScenePath(), scene().toJson(), wallClock(), autosave.recoveryKeep);
    if (!st) {
        FLEDGE_LOG_WARN("editor", "{}", st.error);
        if (error) *error = st.error;
        return false;
    }
    m_lastSnapshotSerial = m_changeSerial;
    return true;
}

EditorSession::AutosaveResult EditorSession::pollAutosave(const AutosaveEnv& env) {
    AutosaveResult r;
    if (env.allowRealSave) m_realSaveSeen = true;
    editor::AutosaveContext c;
    c.hasProject = m_project.has_value();
    c.dirty = dirty;
    c.playing = playing();
    bool importing = false;
    for (const auto& it : m_imports->items())
        importing |= it.state == editor::ItemState::Importing || it.state == editor::ItemState::Downloading;
    c.transient = m_history->inTransaction() || importing;
    c.busyUi = env.busyUi;
    c.focused = env.focused;
    c.allowRealSave = env.allowRealSave;
    c.changeSerial = m_changeSerial;
    editor::AutosaveActions a = m_autosaveScheduler.tick(autosaveClock(), autosave, c);
    if (a.snapshot) r.snapshot = writeRecoverySnapshot(&r.error);
    if (a.autoSave) {
        if (auto st = autoSaveScene(); st) r.autoSaved = true;
        else r.error = st.error;
    }
    return r;
}

Status EditorSession::autoSaveScene() {
    if (!m_project) return Status::failure(kNoProjectHelp);
    if (playing()) return Status::failure("auto-save does not run during Play");
    if (m_recoveryOffer) return Status::failure("auto-save waits for the recovery decision");
    if (!dirty) return {};
    std::string path = activeScenePath();
    if (!startsWith(path, "content:/")) return Status::failure("scene path must start with content:/");
    fs::path real = m_project->contentDir() / pathFromUtf8(path.substr(9));
    fs::path bak = autoSaveBackupFile(path);
    std::error_code ec;
    // The first auto-save after a manual save/open keeps the file as it was; later ones leave that backup alone.
    if (fs::exists(osPath(real), ec) && !fs::exists(osPath(bak), ec)) {
        fs::create_directories(osPath(bak.parent_path()), ec);
        fs::copy_file(osPath(real), osPath(bak), fs::copy_options::overwrite_existing, ec);
        if (ec) return Status::failure("auto-save skipped: cannot keep a backup of " + narrow(real.wstring()) + " (" + ec.message() + ")");
    }
    return saveSceneImpl(path, false);
}

bool EditorSession::hasAutoSaveBackup() const {
    if (!m_project) return false;
    std::error_code ec;
    return fs::exists(osPath(autoSaveBackupFile(activeScenePath())), ec);
}

Status EditorSession::adoptSceneJson(const Json& sceneJson, const std::string& scenePath) {
    if (playing()) return Status::failure("stop play mode first");
    if (!scenePath.empty() && scenePath != m_world->currentScenePath() && m_vfs.exists(scenePath)) (void)loadScene(scenePath);
    if (auto st = scene().fromJson(sceneJson); !st) return st;
    selection.clear();
    m_history->clear();  // like loading a scene: not undoable, but the scene is unsaved from here on
    markDirty();
    return {};
}

Status EditorSession::restoreAutoSaveBackup() {
    if (!m_project) return Status::failure(kNoProjectHelp);
    auto text = readFileText(autoSaveBackupFile(activeScenePath()));
    if (!text) return Status::failure("there is no auto-save backup of this scene");
    Json j = Json::parse(*text, nullptr, false);
    if (!j.is_object()) return Status::failure("the auto-save backup is not valid JSON");
    return adoptSceneJson(j, activeScenePath());
}

void EditorSession::computeRecoveryOffer() {
    m_recoveryOffer.reset();
    auto store = recoveryStore();
    if (!store) return;
    std::vector<std::string> warnings;
    auto snaps = store->loadAll(&warnings);
    auto lookup = [this](const std::string& scenePath) {
        editor::SavedScene s;
        if (!startsWith(scenePath, "content:/")) return s;
        fs::path f = m_project->contentDir() / pathFromUtf8(scenePath.substr(9));
        std::error_code ec;
        if (!fs::exists(osPath(f), ec)) return s;
        s.exists = true;
        s.mtimeMs = editor::fileTimeMs(f);
        if (auto text = readFileText(f)) {
            Json j = Json::parse(*text, nullptr, false);
            if (j.is_object()) s.json = std::move(j);
        }
        return s;
    };
    m_recoveryOffer = editor::decideRecovery(snaps, lookup);
    if (m_recoveryOffer)
        FLEDGE_LOG_INFO("editor", "Unsaved work from an earlier session can be recovered ({} snapshot(s), newest: {})", m_recoveryOffer->snapshotCount,
                        m_recoveryOffer->summary);
}

Status EditorSession::restoreRecovery() {
    if (!m_recoveryOffer) return Status::failure("there is no recovery to restore");
    editor::RecoverySnapshot snap = m_recoveryOffer->snapshot;
    if (auto st = adoptSceneJson(snap.scene, snap.scenePath); !st) return st;
    m_recoveryOffer.reset();
    m_keepRecoveryOnClose = false;
    FLEDGE_LOG_INFO("editor", "Restored the unsaved scene from a recovery snapshot ({} entities)", snap.entities);
    return {};
}

void EditorSession::keepSavedVersion() {
    if (!m_recoveryOffer) return;
    m_recoveryOffer.reset();
    m_keepRecoveryOnClose = true;
}

void EditorSession::discardRecovery() {
    if (auto store = recoveryStore()) store->clear();
    m_recoveryOffer.reset();
    m_keepRecoveryOnClose = false;
    m_lastSnapshotSerial.reset();
}

void EditorSession::closeProjectRecovery() {
    auto store = recoveryStore();
    if (!store || m_recoveryOffer) return;  // an undecided offer is never touched
    if (playing()) (void)stop();
    if (dirty) {
        if (m_lastSnapshotSerial != m_changeSerial) (void)writeRecoverySnapshot();
    } else if (!m_keepRecoveryOnClose) {
        store->clear();
    }
    m_keepRecoveryOnClose = false;
}

Json EditorSession::autosaveState() const {
    Json j = {{"recovery", autosave.recovery},
              {"recoveryIntervalSeconds", autosave.recoverySeconds},
              {"recoveryKeep", autosave.recoveryKeep},
              {"autoSaveMode", editor::toString(autosave.mode)},
              {"autoSaveDelaySeconds", autosave.delaySeconds},
              // true auto-save of the real scene file runs only in the editor GUI session, never in a headless MCP server
              {"autoSaveActiveInThisSession", autosave.mode != editor::AutoSaveMode::Off && m_realSaveSeen},
              {"recoveryFolder", "Intermediate/Recovery"},
              {"snapshots", 0},
              {"recoveryPending", m_recoveryOffer.has_value()},
              {"autoSaveBackup", hasAutoSaveBackup()}};
    if (auto store = recoveryStore()) j["snapshots"] = store->count();
    if (m_recoveryOffer)
        j["pendingRecovery"] = {{"scene", m_recoveryOffer->snapshot.scenePath},
                                {"createdMs", m_recoveryOffer->snapshot.createdMs},
                                {"entities", m_recoveryOffer->snapshot.entities},
                                {"snapshots", m_recoveryOffer->snapshotCount}};
    return j;
}

} // namespace fledge
