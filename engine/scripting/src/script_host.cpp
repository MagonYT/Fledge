#include "fledge/script/script_host.h"
#include "fledge/core/legacy.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "fledge/core/log.h"
#include "fledge/scene/components.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fledge::script {

namespace {
constexpr float kFixedStep = 1.0f / 60.0f;
constexpr int kMaxFixedSteps = 5;

FScriptVec3 toC(Vec3 v) { return {v.x, v.y, v.z}; }
FScriptQuat toC(Quat q) { return {q.x, q.y, q.z, q.w}; }
Vec3 fromC(FScriptVec3 v) { return {v.x, v.y, v.z}; }
Quat fromC(FScriptQuat q) { return {q.x, q.y, q.z, q.w}; }

std::vector<std::string> splitTags(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == ',' || c == ';') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int writeIds(const std::vector<EntityId>& ids, FScriptEntity* out, int capacity) {
    for (int i = 0; i < capacity && i < static_cast<int>(ids.size()); ++i) out[i] = ids[static_cast<size_t>(i)];
    return static_cast<int>(ids.size());
}
} // namespace

const PropertyDesc* ClassDesc::find(std::string_view prop) const {
    for (auto& p : properties)
        if (p.name == prop) return &p;
    return nullptr;
}
const ClassDesc* ModuleInfo::find(std::string_view cls) const {
    for (auto& c : classes)
        if (c.name == cls) return &c;
    return nullptr;
}

// ------------------------------------------------------------ host callbacks --
struct ScriptHost::Impl {
    static ScriptHost& H(void* h) { return *static_cast<ScriptHost*>(h); }
    static Scene* S(void* h) { return H(h).m_scene; }
    static bool valid(void* h, FScriptEntity e) { return S(h) && e && S(h)->isValid(e); }

    static void log(void*, int level, const char* msg) {
        LogLevel l = level <= FSCRIPT_LOG_DEBUG ? LogLevel::Debug : level == FSCRIPT_LOG_INFO ? LogLevel::Info : level == FSCRIPT_LOG_WARN ? LogLevel::Warn : LogLevel::Error;
        Logger::get().log(l, "script", msg ? msg : "");
    }
    static int entityValid(void* h, FScriptEntity e) { return valid(h, e) ? 1 : 0; }
    static const char* entityName(void* h, FScriptEntity e) {
        if (!valid(h, e)) return "";
        H(h).m_scratch = S(h)->displayName(e);
        return H(h).m_scratch.c_str();
    }
    static void setEntityName(void* h, FScriptEntity e, const char* name) {
        if (!valid(h, e) || !name) return;
        if (auto* n = S(h)->get<NameComponent>(e)) n->name = name;
        else S(h)->add<NameComponent>(e, {name});
    }
    static int getTransform(void* h, FScriptEntity e, int space, FScriptVec3* pos, FScriptQuat* rot, FScriptVec3* scale) {
        if (!valid(h, e)) return 0;
        if (space == FSCRIPT_SPACE_WORLD) {
            Transform t = S(h)->worldTransform(e);
            if (pos) *pos = toC(t.position);
            if (rot) *rot = toC(t.rotation);
            if (scale) *scale = toC(t.scale);
        } else {
            auto* t = S(h)->get<TransformComponent>(e);
            if (!t) return 0;
            if (pos) *pos = toC(t->position);
            if (rot) *rot = toC(t->rotation);
            if (scale) *scale = toC(t->scale);
        }
        return 1;
    }
    static int setPosition(void* h, FScriptEntity e, int space, FScriptVec3 v) {
        if (!valid(h, e)) return 0;
        if (space == FSCRIPT_SPACE_WORLD && S(h)->parentOf(e)) {
            Transform t = S(h)->worldTransform(e);
            t.position = fromC(v);
            S(h)->setWorldTransform(e, t);
        } else if (auto* t = S(h)->get<TransformComponent>(e)) {
            t->position = fromC(v);
        }
        return 1;
    }
    static int setRotation(void* h, FScriptEntity e, int space, FScriptQuat q) {
        if (!valid(h, e)) return 0;
        Quat r = fromC(q).normalized();
        if (space == FSCRIPT_SPACE_WORLD && S(h)->parentOf(e)) {
            Transform t = S(h)->worldTransform(e);
            t.rotation = r;
            S(h)->setWorldTransform(e, t);
        } else if (auto* t = S(h)->get<TransformComponent>(e)) {
            t->rotation = r;
        }
        return 1;
    }
    static int setScale(void* h, FScriptEntity e, FScriptVec3 v) {
        if (!valid(h, e)) return 0;
        if (auto* t = S(h)->get<TransformComponent>(e)) t->scale = fromC(v);
        return 1;
    }
    static FScriptEntity parent(void* h, FScriptEntity e) { return valid(h, e) ? S(h)->parentOf(e) : 0; }
    static int setParent(void* h, FScriptEntity c, FScriptEntity p, int keepWorld) {
        if (!valid(h, c) || (p && !valid(h, p))) return 0;
        return S(h)->setParent(c, p, -1, keepWorld != 0).ok ? 1 : 0;
    }
    static int children(void* h, FScriptEntity e, FScriptEntity* out, int cap) {
        if (!valid(h, e)) return 0;
        return writeIds(S(h)->childrenOf(e), out, cap);
    }
    static const char* tags(void* h, FScriptEntity e) {
        H(h).m_scratch.clear();
        if (valid(h, e))
            if (auto* t = S(h)->get<TagsComponent>(e)) H(h).m_scratch = t->tags;
        return H(h).m_scratch.c_str();
    }
    static void setTags(void* h, FScriptEntity e, const char* tags) {
        if (!valid(h, e)) return;
        auto* t = S(h)->get<TagsComponent>(e);
        if (!t) t = &S(h)->add<TagsComponent>(e);
        t->tags = tags ? tags : "";
    }
    static int hasComponent(void* h, FScriptEntity e, const char* type) {
        return valid(h, e) && type && S(h)->getComponent(e, type) ? 1 : 0;
    }
    static int addComponent(void* h, FScriptEntity e, const char* type) {
        return valid(h, e) && type && S(h)->addComponent(e, type) ? 1 : 0;
    }
    static int removeComponent(void* h, FScriptEntity e, const char* type) {
        if (!valid(h, e) || !type) return 0;
        const auto* info = ComponentRegistry::get().find(type);
        if (!info || !info->removable || std::string_view(type) == "Behaviours") return 0;  // scripts cannot remove themselves this way
        return S(h)->removeComponent(e, type) ? 1 : 0;
    }
    static const char* getComponent(void* h, FScriptEntity e, const char* type) {
        if (!valid(h, e) || !type) return nullptr;
        void* c = S(h)->getComponent(e, type);
        const auto* info = ComponentRegistry::get().find(type);
        if (!c || !info) return nullptr;
        H(h).m_scratch = info->type.toJson(c).dump();
        return H(h).m_scratch.c_str();
    }
    static int setComponent(void* h, FScriptEntity e, const char* type, const char* json) {
        if (!valid(h, e) || !type || !json) return 0;
        const auto* info = ComponentRegistry::get().find(type);
        void* c = S(h)->getComponent(e, type);
        if (!info || !c) return 0;
        Json j = Json::parse(json, nullptr, false);
        if (!j.is_object()) return 0;
        auto st = info->type.fromJson(c, j);
        if (!st) FLEDGE_LOG_WARN("script", "set component {}: {}", type, st.error);
        return st.ok ? 1 : 0;
    }
    static FScriptEntity findByName(void* h, const char* name) { return S(h) && name ? S(h)->findByName(name) : 0; }
    static int findWithTag(void* h, const char* tag, FScriptEntity* out, int cap) {
        std::vector<EntityId> ids;
        if (S(h) && tag)
            for (EntityId e : S(h)->entities())
                if (auto* t = S(h)->get<TagsComponent>(e))
                    for (auto& x : splitTags(t->tags))
                        if (x == tag) {
                            ids.push_back(e);
                            break;
                        }
        return writeIds(ids, out, cap);
    }
    static int findWithComponent(void* h, const char* type, FScriptEntity* out, int cap) {
        std::vector<EntityId> ids;
        if (S(h) && type && ComponentRegistry::get().find(type))
            for (EntityId e : S(h)->entities())
                if (S(h)->getComponent(e, type)) ids.push_back(e);
        return writeIds(ids, out, cap);
    }
    static int raycast(void* h, FScriptVec3 o, FScriptVec3 d, float maxDist, FScriptRayHit* hit) {
        if (!H(h).raycast) return 0;
        auto r = H(h).raycast(fromC(o), fromC(d), maxDist);
        if (!r) return 0;
        if (hit) *hit = {r->entity, toC(r->point), toC(r->normal), r->distance};
        return 1;
    }
    // 1.3.2 filtered queries (Physics::Raycast / SphereCast)
    static int raycastFiltered(void* h, FScriptVec3 o, FScriptVec3 d, float maxDist, FScriptEntity ignore, uint32_t mask, int flags, FScriptRayHit* hit) {
        if (!H(h).raycastFiltered) return 0;
        auto r = H(h).raycastFiltered(fromC(o), fromC(d), maxDist, ignore, mask, (flags & FSCRIPT_QUERY_IGNORE_HIERARCHY) != 0);
        if (!r) return 0;
        if (hit) *hit = {r->entity, toC(r->point), toC(r->normal), r->distance};
        return 1;
    }
    static int sphereCast(void* h, FScriptVec3 o, float radius, FScriptVec3 d, float maxDist, FScriptEntity ignore, uint32_t mask, int flags,
                          FScriptRayHit* hit) {
        if (!H(h).sphereCast) return 0;
        auto r = H(h).sphereCast(fromC(o), radius, fromC(d), maxDist, ignore, mask, (flags & FSCRIPT_QUERY_IGNORE_HIERARCHY) != 0);
        if (!r) return 0;
        if (hit) *hit = {r->entity, toC(r->point), toC(r->normal), r->distance};
        return 1;
    }
    static FScriptEntity createEntity(void* h, const char* name, FScriptEntity parent) {
        if (!S(h)) return 0;
        EntityId e = S(h)->createEntity(name ? name : "Entity");
        if (parent && valid(h, parent)) (void)S(h)->setParent(e, parent, -1, false);
        return e;
    }
    static void destroyEntity(void* h, FScriptEntity e) {
        if (valid(h, e)) H(h).m_pendingDestroy.push_back(e);
    }
    static FScriptEntity spawnScene(void* h, const char* path, FScriptVec3 pos, FScriptQuat rot);
    static int addBehaviour(void* h, FScriptEntity e, const char* cls, const char* props) {
        if (!valid(h, e) || !cls || !H(h).m_info.find(cls)) return 0;
        Json p = props ? Json::parse(props, nullptr, false) : Json::object();
        if (!p.is_object()) p = Json::object();
        H(h).m_pendingAdd.push_back({e, {cls, p}});
        return 1;
    }
    static int inputAction(void* h, const char* a, int q) {
        return a && H(h).m_input.action(a, static_cast<InputQuery>(q < 0 || q > 2 ? 0 : q)) ? 1 : 0;
    }
    static float inputAxis(void* h, const char* a) { return a ? H(h).m_input.axis(a) : 0.0f; }
    static void mouseDelta(void* h, float* dx, float* dy) {
        if (dx) *dx = H(h).m_input.mouseDx();
        if (dy) *dy = H(h).m_input.mouseDy();
    }
    static int inputRebind(void* h, const char* a, const char* key) { return a && key && H(h).m_input.rebind(a, key) ? 1 : 0; }
    static void setTimeScale(void* h, float s) { H(h).m_frame.timeScale = std::clamp(s, 0.0f, 100.0f); }
    static int boneTransform(void* h, FScriptEntity e, const char* bone, FScriptVec3* pos, FScriptQuat* rot, FScriptVec3* scale) {
        if (!valid(h, e) || !bone || !H(h).boneModel) return 0;
        auto m = H(h).boneModel(e, bone);
        if (!m) return 0;
        Transform t = decompose(S(h)->worldMatrix(e) * *m);
        if (pos) *pos = toC(t.position);
        if (rot) *rot = toC(t.rotation);
        if (scale) *scale = toC(t.scale);
        return 1;
    }
    static const char* boneNames(void* h, FScriptEntity e) {
        H(h).m_scratch.clear();
        if (valid(h, e) && H(h).boneNames)
            for (auto& n : H(h).boneNames(e)) H(h).m_scratch += n + "\n";
        return H(h).m_scratch.c_str();
    }
};

// Instantiates a .fscene as a sub-tree ("prefab"). Entity ids are remapped,
// including entity-typed script properties inside "Behaviours".
FScriptEntity ScriptHost::Impl::spawnScene(void* h, const char* path, FScriptVec3 pos, FScriptQuat rot) {
    ScriptHost& self = H(h);
    if (!self.m_scene || !self.m_vfs || !path) return 0;
    std::string vpath = path;
    if (vpath.find(":/") == std::string::npos) vpath = "content:/" + vpath;
    auto text = self.m_vfs->readText(vpath);
    if (!text) {
        FLEDGE_LOG_WARN("script", "Spawn: cannot read scene '{}'", path);
        return 0;
    }
    Json j = Json::parse(*text, nullptr, false);
    if (!j.is_object() || !legacy::formatIs(j, "fscene")) {
        FLEDGE_LOG_WARN("script", "Spawn: '{}' is not a .fscene", path);
        return 0;
    }
    Scene& s = *self.m_scene;
    std::unordered_map<EntityId, EntityId> remap;
    std::vector<std::pair<EntityId, EntityId>> links;
    std::vector<EntityId> created;
    const Json ents = j.value("entities", Json::array());
    for (const auto& ej : ents) {
        EntityId e = s.createEntity("Entity");
        remap[ej.value("id", 0u)] = e;
        created.push_back(e);
    }
    size_t i = 0;
    for (const auto& ej : ents) {
        EntityId e = created[i++];
        if (ej.contains("parent") && ej["parent"].is_number_integer()) links.emplace_back(e, ej["parent"].get<EntityId>());
        const Json comps = ej.value("components", Json::object());
        for (auto& [type, value] : comps.items()) {
            const auto* info = ComponentRegistry::get().find(type);
            void* c = info ? s.addComponent(e, type) : nullptr;
            if (!c) continue;
            Json v = value;
            if (type == "Behaviours" && v.is_array())  // remap entity references
                for (auto& item : v) {
                    const ClassDesc* cd = self.m_info.find(item.value("class", ""));
                    if (!cd || !item.contains("properties")) continue;
                    for (auto& [pn, pv] : item["properties"].items()) {
                        const PropertyDesc* pd = cd->find(pn);
                        if (!pd || pd->type != "entity") continue;
                        auto fix = [&](Json& x) {
                            if (x.is_number_integer())
                                if (auto it = remap.find(x.get<EntityId>()); it != remap.end()) x = it->second;
                        };
                        if (pv.is_array())
                            for (auto& x : pv) fix(x);
                        else fix(pv);
                    }
                }
            (void)info->type.fromJson(c, v);
        }
    }
    for (auto [child, parent] : links)
        if (auto it = remap.find(parent); it != remap.end()) (void)s.setParent(child, it->second, -1, false);
    std::vector<EntityId> roots;
    for (EntityId e : created)
        if (!s.parentOf(e)) roots.push_back(e);
    EntityId root = roots.size() == 1 ? roots[0] : kNullEntity;
    if (!root) {
        root = s.createEntity(fs::path(path).stem().string());
        for (EntityId r : roots) (void)s.setParent(r, root, -1, false);
        created.push_back(root);
    }
    if (auto* t = s.get<TransformComponent>(root)) {
        t->position = fromC(pos);
        t->rotation = (fromC(rot) * t->rotation).normalized();
    }
    for (EntityId e : created) self.m_pendingCreate.push_back(e);
    return root;
}

// ------------------------------------------------------------------- module --
ScriptHost::ScriptHost() {
    m_api.abiVersion = FSCRIPT_ABI_VERSION;
    m_api.structSize = sizeof(FScriptHostApi);
    m_api.host = this;
    m_api.frameInfo = &m_frame;
    m_api.log = &Impl::log;
    m_api.entity_valid = &Impl::entityValid;
    m_api.entity_name = &Impl::entityName;
    m_api.set_entity_name = &Impl::setEntityName;
    m_api.get_transform = &Impl::getTransform;
    m_api.set_position = &Impl::setPosition;
    m_api.set_rotation = &Impl::setRotation;
    m_api.set_scale = &Impl::setScale;
    m_api.parent = &Impl::parent;
    m_api.set_parent = &Impl::setParent;
    m_api.children = &Impl::children;
    m_api.tags = &Impl::tags;
    m_api.set_tags = &Impl::setTags;
    m_api.has_component = &Impl::hasComponent;
    m_api.add_component = &Impl::addComponent;
    m_api.remove_component = &Impl::removeComponent;
    m_api.get_component = &Impl::getComponent;
    m_api.set_component = &Impl::setComponent;
    m_api.find_by_name = &Impl::findByName;
    m_api.find_with_tag = &Impl::findWithTag;
    m_api.find_with_component = &Impl::findWithComponent;
    m_api.raycast = &Impl::raycast;
    m_api.create_entity = &Impl::createEntity;
    m_api.destroy_entity = &Impl::destroyEntity;
    m_api.spawn_scene = &Impl::spawnScene;
    m_api.add_behaviour = &Impl::addBehaviour;
    m_api.input_action = &Impl::inputAction;
    m_api.input_axis = &Impl::inputAxis;
    m_api.mouse_delta = &Impl::mouseDelta;
    m_api.input_rebind = &Impl::inputRebind;
    m_api.set_time_scale = &Impl::setTimeScale;
    m_api.bone_transform = &Impl::boneTransform;
    m_api.raycast_filtered = &Impl::raycastFiltered;
    m_api.sphere_cast = &Impl::sphereCast;
    m_api.bone_names = &Impl::boneNames;
    m_frame.timeScale = 1.0f;
    m_frame.fixedDelta = kFixedStep;
}

ScriptHost::~ScriptHost() {
    if (playing()) endPlay();
    unloadModule();
}

Status ScriptHost::openModule(const fs::path& dll, Loaded& out, ModuleInfo& info) {
    std::error_code ec;
    if (!fs::is_regular_file(dll, ec)) return Status::failure("script module not found: " + narrow(dll.wstring()));
#ifdef _WIN32
    HMODULE h = LoadLibraryW(dll.wstring().c_str());
    if (!h) return Status::failure(std::format("cannot load script module {} (Windows error {})", narrow(dll.wstring()), GetLastError()));
    auto entry = reinterpret_cast<FScriptModuleEntryFn>(reinterpret_cast<void*>(GetProcAddress(h, "fledge_script_module_entry")));
    if (!entry)  // modules built before 1.0.1 (Perch): same ABI, old export name
        entry = reinterpret_cast<FScriptModuleEntryFn>(reinterpret_cast<void*>(GetProcAddress(h, "perch_module_entry")));
    auto unload = [h] { FreeLibrary(h); };
#else
    void* h = dlopen(dll.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return Status::failure(std::string("cannot load script module: ") + dlerror());
    auto entry = reinterpret_cast<FScriptModuleEntryFn>(dlsym(h, "fledge_script_module_entry"));
    if (!entry) entry = reinterpret_cast<FScriptModuleEntryFn>(dlsym(h, "perch_module_entry"));  // pre-1.0.1 (Perch) modules
    auto unload = [h] { dlclose(h); };
#endif
    if (!entry) {
        unload();
        return Status::failure("not a Fledge Script module (no fledge_script_module_entry export): " + narrow(dll.filename().wstring()));
    }
    const FScriptModuleApi* api = entry();
    if (!api || api->abiVersion != FSCRIPT_ABI_VERSION || api->structSize < sizeof(FScriptModuleApi)) {
        std::string why = api ? std::format("module ABI {} / size {}, engine expects ABI {} / size {}", api->abiVersion, api->structSize,
                                            FSCRIPT_ABI_VERSION, sizeof(FScriptModuleApi))
                              : "entry returned null";
        unload();
        return Status::failure("script module ABI mismatch (" + why + "): rebuild the scripts with this engine's SDK");
    }
    if (api->init(&m_api) != 0) {
        unload();
        return Status::failure("script module rejected the engine host API (version mismatch)");
    }
    Json d = Json::parse(api->describe(), nullptr, false);
    info = ModuleInfo{};
    info.path = dll;
    info.sdkVersion = api->sdkVersion ? api->sdkVersion : "";
    info.compiler = api->compiler ? api->compiler : "";
    for (auto& c : d.value("classes", Json::array())) {
        ClassDesc cd;
        cd.name = c.value("name", "");
        cd.isService = c.value("kind", "") == "service";
        for (auto& p : c.value("properties", Json::array())) {
            PropertyDesc pd;
            pd.name = p.value("name", "");
            pd.type = p.value("type", "");
            pd.isArray = p.value("array", false);
            pd.tooltip = p.value("tooltip", "");
            pd.assetKind = p.value("assetKind", "");
            pd.hidden = p.value("hidden", false);
            pd.readOnly = p.value("readOnly", false);
            pd.hasRange = p.contains("min");
            pd.min = p.value("min", 0.0f);
            pd.max = p.value("max", 0.0f);
            pd.defaultValue = p.value("default", Json());
            cd.properties.push_back(std::move(pd));
        }
        info.classes.push_back(std::move(cd));
    }
    if (const char* err = api->last_error(); err && *err) FLEDGE_LOG_WARN("script", "script module: {}", err);
    out.dll = reinterpret_cast<void*>(h);
    out.api = api;
    out.path = dll;
    return {};
}

void ScriptHost::closeModule(Loaded& m) {
    if (m.api) m.api->shutdown();
#ifdef _WIN32
    if (m.dll) FreeLibrary(static_cast<HMODULE>(m.dll));
#else
    if (m.dll) dlclose(m.dll);
#endif
    m = Loaded{};
}

Status ScriptHost::loadModule(const fs::path& dll) {
    auto fresh = std::make_unique<Loaded>();
    ModuleInfo info;
    if (auto st = openModule(dll, *fresh, info); !st) {
        FLEDGE_LOG_ERROR("script", "{}{}", st.error, m_module ? " - keeping the previously loaded scripts" : "");
        return st;
    }
    // Snapshot live objects (hot reload while playing).
    struct Saved {
        EntityId entity;
        std::string cls;
        Json props;
        bool enabled;
    };
    std::vector<Saved> saved;
    if (m_module && playing()) {
        for (auto& o : m_objects) {
            Json p = Json::parse(m_module->api->get_properties(o.handle), nullptr, false);
            saved.push_back({o.entity, o.cls, p.is_object() ? p : Json::object(), o.enabled});
        }
    }
    m_objects.clear();
    if (m_module) closeModule(*m_module);  // old objects dropped without lifecycle calls; coroutines/timers die with them
    m_module = std::move(fresh);
    m_info = std::move(info);
    ++m_generation;
    FLEDGE_LOG_INFO("script", "Loaded script module {} ({} classes, SDK {}, {})", narrow(dll.filename().wstring()), m_info.classes.size(),
                  m_info.sdkVersion, m_info.compiler);
    if (!saved.empty() || playing()) {
        ++m_reloads;
        m_module->api->set_reloading(1);
        for (auto& s : saved) {
            if (s.entity && !(m_scene && m_scene->isValid(s.entity))) continue;
            createObject(s.entity, s.cls, s.props, s.enabled, true);
        }
        // services that are new in this build
        for (auto& c : m_info.classes)
            if (c.isService && std::none_of(m_objects.begin(), m_objects.end(), [&](const Object& o) { return o.cls == c.name; }))
                createObject(kNullEntity, c.name, Json::object(), true, true);
        m_clearReloading = true;  // IsHotReloading() stays true through the first frame (OnStart)
        FLEDGE_LOG_INFO("script", "Hot reload #{}: {} objects recreated with their properties", m_reloads, m_objects.size());
    }
    return {};
}

void ScriptHost::unloadModule() {
    if (!m_module) return;
    m_objects.clear();
    closeModule(*m_module);
    m_module.reset();
    m_info = ModuleInfo{};
    ++m_generation;
}

std::string ScriptHost::lastModuleError() const {
    return m_module && m_module->api->last_error ? m_module->api->last_error() : "";
}

// -------------------------------------------------------------------- play --
void ScriptHost::createObject(EntityId e, const std::string& cls, const Json& props, bool enabled, bool reloading) {
    if (!m_module) return;
    const ClassDesc* cd = m_info.find(cls);
    if (!cd) {
        FLEDGE_LOG_WARN("script", "Entity {}: script class '{}' is not in the loaded module (build the scripts?)", e, cls);
        return;
    }
    const auto* api = m_module->api;
    FScriptInstance h = api->create(cls.c_str(), e);
    if (!h) {
        FLEDGE_LOG_ERROR("script", "Creating {} failed: {}", cls, api->last_error());
        return;
    }
    if (props.is_object() && !props.empty()) api->set_properties(h, props.dump().c_str());
    m_objects.push_back({e, cls, h, enabled});
    api->hook(h, FSCRIPT_HOOK_CREATE);
    if (enabled) api->hook(h, FSCRIPT_HOOK_ENABLE);
    if (reloading) api->hook(h, FSCRIPT_HOOK_HOT_RELOAD);
}

void ScriptHost::createForEntity(EntityId e, bool reloading) {
    auto* bc = m_scene ? m_scene->get<BehavioursComponent>(e) : nullptr;
    if (!bc || !bc->list.is_array()) return;
    for (const auto& item : bc->list) {
        if (!item.is_object()) continue;
        createObject(e, item.value("class", ""), item.value("properties", Json::object()), item.value("enabled", true), reloading);
    }
}

void ScriptHost::beginPlay(Scene& scene, Vfs* vfs) {
    if (playing()) endPlay();
    m_scene = &scene;
    m_vfs = vfs;
    m_frame = FScriptFrameInfo{};
    m_frame.timeScale = 1.0f;
    m_frame.fixedDelta = kFixedStep;
    m_fixedAccumulator = 0;
    resetInput();  // nothing held in the last session (or the editor) is still down
    m_pendingCreate.clear();
    m_pendingAdd.clear();
    m_pendingDestroy.clear();
    if (!m_module) return;
    for (auto& c : m_info.classes)
        if (c.isService) createObject(kNullEntity, c.name, Json::object(), true, false);
    for (EntityId e : scene.depthFirstOrder()) createForEntity(e, false);
    FLEDGE_LOG_INFO("script", "Play: {} services, {} behaviours", objectCount() - behaviourCount(), behaviourCount());
}

void ScriptHost::destroyObjectsOf(EntityId e) {
    if (!m_module) return;
    for (size_t i = 0; i < m_objects.size();) {
        if (m_objects[i].entity == e) {
            FScriptInstance h = m_objects[i].handle;
            m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(i));
            m_module->api->hook(h, FSCRIPT_HOOK_DESTROY);
        } else {
            ++i;
        }
    }
}

void ScriptHost::processDeferred() {
    for (int guard = 0; guard < 8 && (!m_pendingCreate.empty() || !m_pendingAdd.empty() || !m_pendingDestroy.empty()); ++guard) {
        auto create = std::move(m_pendingCreate);
        m_pendingCreate.clear();
        for (EntityId e : create)
            if (m_scene->isValid(e)) createForEntity(e, false);
        auto add = std::move(m_pendingAdd);
        m_pendingAdd.clear();
        for (auto& [e, cp] : add) {
            if (!m_scene->isValid(e)) continue;
            auto* bc = m_scene->get<BehavioursComponent>(e);
            if (!bc) bc = &m_scene->add<BehavioursComponent>(e);
            if (!bc->list.is_array()) bc->list = Json::array();
            bc->list.push_back({{"class", cp.first}, {"enabled", true}, {"properties", cp.second}});
            createObject(e, cp.first, cp.second, true, false);
        }
        auto destroy = std::move(m_pendingDestroy);
        m_pendingDestroy.clear();
        for (EntityId e : destroy) {
            if (!m_scene->isValid(e)) continue;
            std::vector<EntityId> all = m_scene->descendants(e);
            all.push_back(e);
            for (EntityId x : all) destroyObjectsOf(x);
            m_scene->destroyEntity(e);
        }
    }
    // Entities removed by someone else (editor, MCP) lose their objects too.
    for (size_t i = 0; i < m_objects.size();) {
        if (m_objects[i].entity && !m_scene->isValid(m_objects[i].entity)) {
            FScriptInstance h = m_objects[i].handle;
            m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(i));
            m_module->api->hook(h, FSCRIPT_HOOK_DESTROY);
        } else {
            ++i;
        }
    }
}

void ScriptHost::update(float dt) {
    updateEarly(dt);
    updateLate();
}

void ScriptHost::updateEarly(float dt) {
    m_lateDue = false;
    if (!playing()) return;
    m_input.beginFrame(m_pendingInput);
    m_pendingInput.mouseDx = m_pendingInput.mouseDy = 0;  // deltas are per frame
    m_frame.unscaledDelta = dt;
    m_frame.delta = dt * m_frame.timeScale;
    m_frame.elapsed += m_frame.delta;
    ++m_frame.frame;
    if (!m_module) return;
    const auto* api = m_module->api;
    m_fixedAccumulator += m_frame.delta;
    int steps = 0;
    while (m_fixedAccumulator >= kFixedStep && steps < kMaxFixedSteps) {
        api->frame(FSCRIPT_PHASE_FIXED, kFixedStep);
        m_fixedAccumulator -= kFixedStep;
        ++steps;
    }
    if (steps == kMaxFixedSteps) m_fixedAccumulator = 0;  // spiral-of-death guard
    api->frame(FSCRIPT_PHASE_UPDATE, m_frame.delta);
    m_lateDue = true;
}

void ScriptHost::updateLate() {
    if (!m_lateDue) return;
    m_lateDue = false;
    if (!playing() || !m_module) return;
    const auto* api = m_module->api;
    api->frame(FSCRIPT_PHASE_LATE, m_frame.delta);
    api->frame(FSCRIPT_PHASE_END_FRAME, m_frame.delta);
    processDeferred();
    if (m_clearReloading) {
        api->set_reloading(0);
        m_clearReloading = false;
    }
}

void ScriptHost::endPlay() {
    if (!playing()) return;
    if (m_module) {
        // behaviours first (reverse creation order), services last
        for (auto it = m_objects.rbegin(); it != m_objects.rend(); ++it)
            if (it->entity) m_module->api->hook(it->handle, FSCRIPT_HOOK_DESTROY);
        for (auto it = m_objects.rbegin(); it != m_objects.rend(); ++it)
            if (!it->entity) m_module->api->hook(it->handle, FSCRIPT_HOOK_DESTROY);
        m_module->api->shutdown();  // anything left (objects created by scripts' own bookkeeping)
        m_module->api->set_reloading(0);
    }
    m_objects.clear();
    m_scene = nullptr;
    m_vfs = nullptr;
    m_clearReloading = false;
    resetInput();  // a held key / axis (throttle) must not survive Stop
}

size_t ScriptHost::objectCount() const { return m_objects.size(); }
size_t ScriptHost::behaviourCount() const {
    return static_cast<size_t>(std::count_if(m_objects.begin(), m_objects.end(), [](const Object& o) { return o.entity != kNullEntity; }));
}

std::optional<Json> ScriptHost::liveProperties(EntityId e, const std::string& cls) {
    if (!m_module) return std::nullopt;
    for (auto& o : m_objects)
        if (o.entity == e && o.cls == cls) {
            Json j = Json::parse(m_module->api->get_properties(o.handle), nullptr, false);
            if (j.is_object()) return j;
        }
    return std::nullopt;
}

Status ScriptHost::setLiveProperty(EntityId e, const std::string& cls, const std::string& prop, const Json& value) {
    if (!m_module) return Status::failure("no script module loaded");
    for (auto& o : m_objects)
        if (o.entity == e && o.cls == cls) {
            Json j = {{prop, value}};
            if (m_module->api->set_properties(o.handle, j.dump().c_str()) < 2)
                return Status::failure(std::format("{}.{}: value {} not accepted", cls, prop, value.dump()));
            return {};
        }
    return Status::failure(std::format("no live {} on entity {}", cls, e));
}

std::vector<std::string> ScriptHost::liveClasses(EntityId e) const {
    std::vector<std::string> out;
    for (auto& o : m_objects)
        if (o.entity == e) out.push_back(o.cls);
    return out;
}

void ScriptHost::attachLive(EntityId e, const std::string& cls, const Json& props) {
    if (playing()) createObject(e, cls, props, true, false);
}

void ScriptHost::detachLive(EntityId e, const std::string& cls) {
    if (!m_module) return;
    for (size_t i = 0; i < m_objects.size(); ++i)
        if (m_objects[i].entity == e && m_objects[i].cls == cls) {
            FScriptInstance h = m_objects[i].handle;
            m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(i));
            m_module->api->hook(h, FSCRIPT_HOOK_DESTROY);
            return;
        }
}

void ScriptHost::notifyCollision(EntityId e, EntityId other, int kind) {
    if (!m_module) return;
    for (auto& o : m_objects)
        if (o.entity == e) m_module->api->collision(o.handle, kind, other);
}

} // namespace fledge::script
