#include "fledge/script/module_host.h"

#include <format>

#include "fledge/core/log.h"
#include "fledge/scene/components.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fledge {

struct ModuleHostCallbacks {
    static ModuleHost* self(void* h) { return static_cast<ModuleHost*>(h); }
    static TransformComponent* tf(void* h, FledgeEntity e) {
        auto* s = self(h)->m_scene;
        return s ? s->get<TransformComponent>(e) : nullptr;
    }
    static void log(void*, int level, const char* msg) {
        Logger::get().log(static_cast<LogLevel>(level < 0 ? 0 : (level > 4 ? 4 : level)), "module", msg ? msg : "");
    }
    static FledgeEntity find(void* h, const char* name) {
        auto* s = self(h)->m_scene;
        return s && name ? s->findByName(name) : 0;
    }
    static int getPos(void* h, FledgeEntity e, FledgeVec3* o) {
        auto* t = tf(h, e);
        if (!t || !o) return 0;
        *o = {t->position.x, t->position.y, t->position.z};
        return 1;
    }
    static int setPos(void* h, FledgeEntity e, FledgeVec3 v) {
        auto* t = tf(h, e);
        if (!t) return 0;
        t->position = {v.x, v.y, v.z};
        return 1;
    }
    static int getRot(void* h, FledgeEntity e, FledgeQuat* o) {
        auto* t = tf(h, e);
        if (!t || !o) return 0;
        *o = {t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w};
        return 1;
    }
    static int setRot(void* h, FledgeEntity e, FledgeQuat q) {
        auto* t = tf(h, e);
        if (!t) return 0;
        t->rotation = Quat{q.x, q.y, q.z, q.w}.normalized();
        return 1;
    }
    static int getScale(void* h, FledgeEntity e, FledgeVec3* o) {
        auto* t = tf(h, e);
        if (!t || !o) return 0;
        *o = {t->scale.x, t->scale.y, t->scale.z};
        return 1;
    }
    static int setScale(void* h, FledgeEntity e, FledgeVec3 v) {
        auto* t = tf(h, e);
        if (!t) return 0;
        t->scale = {v.x, v.y, v.z};
        return 1;
    }
    static double time(void* h) { return self(h)->m_time; }
};

ModuleHost::ModuleHost() {
    m_hostApi.version = FLEDGE_MODULE_API_VERSION;
    m_hostApi.host = this;
    m_hostApi.log = &ModuleHostCallbacks::log;
    m_hostApi.find_entity = &ModuleHostCallbacks::find;
    m_hostApi.get_position = &ModuleHostCallbacks::getPos;
    m_hostApi.set_position = &ModuleHostCallbacks::setPos;
    m_hostApi.get_rotation = &ModuleHostCallbacks::getRot;
    m_hostApi.set_rotation = &ModuleHostCallbacks::setRot;
    m_hostApi.get_scale = &ModuleHostCallbacks::getScale;
    m_hostApi.set_scale = &ModuleHostCallbacks::setScale;
    m_hostApi.time_seconds = &ModuleHostCallbacks::time;
}

ModuleHost::~ModuleHost() { unloadAll(); }

static void* openLib(const fs::path& p) {
#ifdef _WIN32
    return LoadLibraryW(p.wstring().c_str());
#else
    return dlopen(p.string().c_str(), RTLD_NOW);
#endif
}
static void* findSym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}
static void closeLib(void* lib) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

Status ModuleHost::load(const std::string& name) {
    for (auto& m : m_modules)
        if (m.name == name) return {};
#ifdef _WIN32
    const std::string candidates[] = {name + ".dll", "lib" + name + ".dll"};
#else
    const std::string candidates[] = {"lib" + name + ".so", name + ".so"};
#endif
    for (auto& dir : m_searchPaths) {
        for (auto& file : candidates) {
            fs::path p = dir / file;
            std::error_code ec;
            if (!fs::exists(p, ec)) continue;
            void* lib = openLib(p);
            if (!lib) return Status::failure(std::format("failed to load module '{}'", narrow(p.wstring())));
            auto fn = reinterpret_cast<FledgeGetModuleApiFn>(findSym(lib, "fledge_get_module_api"));
            const FledgeModuleApi* api = fn ? fn() : nullptr;
            if (!api || api->version != FLEDGE_MODULE_API_VERSION) {
                closeLib(lib);
                return Status::failure(std::format("module '{}' has no compatible fledge_get_module_api (ABI v{})", name,
                                                   FLEDGE_MODULE_API_VERSION));
            }
            if (api->on_load && api->on_load(&m_hostApi) != 0) {
                closeLib(lib);
                return Status::failure(std::format("module '{}' on_load failed", name));
            }
            m_modules.push_back({name, lib, api});
            FLEDGE_LOG_INFO("script", "Loaded gameplay module '{}' from {}", name, narrow(p.wstring()));
            return {};
        }
    }
    return Status::failure(std::format("gameplay module '{}' not found in search paths", name));
}

void ModuleHost::unloadAll() {
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
        if (it->api->on_unload) it->api->on_unload();
        closeLib(it->handle);
    }
    m_modules.clear();
}

std::vector<std::string> ModuleHost::loadedModules() const {
    std::vector<std::string> v;
    for (auto& m : m_modules) v.push_back(m.name);
    return v;
}

void ModuleHost::beginPlay(Scene& scene) {
    m_scene = &scene;
    m_time = 0;
    for (auto& m : m_modules)
        if (m.api->on_begin_play) m.api->on_begin_play();
}

void ModuleHost::tick(Scene& scene, float dt) {
    m_scene = &scene;
    m_time += dt;
    auto& pool = scene.pool<ScriptComponent>();
    for (auto& m : m_modules) {
        if (m.api->on_entity_tick) {
            for (size_t i = 0; i < pool.entities().size(); ++i) {
                const auto& sc = pool.components()[i];
                if (sc.module == m.name) m.api->on_entity_tick(pool.entities()[i], sc.behaviour.c_str(), sc.param, dt);
            }
        }
        if (m.api->on_tick) m.api->on_tick(dt);
    }
}

void ModuleHost::endPlay(Scene& scene) {
    m_scene = &scene;
    for (auto& m : m_modules)
        if (m.api->on_end_play) m.api->on_end_play();
    m_scene = nullptr;
}

} // namespace fledge
