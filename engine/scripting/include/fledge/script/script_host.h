#pragma once
// ScriptHost: the engine side of Fledge Script. Loads a compiled script module
// (DLL built from <project>/Scripts), creates script objects for the scene's
// "Behaviours" components while playing, drives their lifecycle, and hot
// reloads the module while preserving reflected property values.
//
// Part of the shipping runtime (no editor / AI code). Thread: main thread only.
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/core/vfs.h"
#include "fledge/script/abi.h"
#include "fledge/script/input_map.h"
#include "fledge/scene/scene.h"

namespace fledge::script {

struct PropertyDesc {
    std::string name, type, tooltip, assetKind;
    bool isArray = false, hidden = false, readOnly = false, hasRange = false;
    float min = 0, max = 0;
    Json defaultValue;
};

struct ClassDesc {
    std::string name;
    bool isService = false;
    std::vector<PropertyDesc> properties;
    const PropertyDesc* find(std::string_view prop) const;
};

struct ModuleInfo {
    fs::path path;
    std::string sdkVersion, compiler;
    std::vector<ClassDesc> classes;
    const ClassDesc* find(std::string_view cls) const;
};

struct RaycastHit {
    EntityId entity = kNullEntity;
    Vec3 point, normal;
    float distance = 0;
};

class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Loads a script module. If a module is already loaded, the new one replaces it only
    // after it loaded and passed the ABI check (otherwise the old one keeps running).
    // While playing this is a HOT RELOAD: every live object's properties are serialized,
    // the old objects are dropped without lifecycle calls (coroutines/timers stop), and new
    // objects are created for the same entities with the properties restored, then
    // OnCreate (IsHotReloading() == true), OnEnable and OnHotReload run.
    Status loadModule(const fs::path& dll);
    void unloadModule();
    bool hasModule() const { return m_module != nullptr; }
    const ModuleInfo& module() const { return m_info; }
    uint32_t moduleGeneration() const { return m_generation; }  // bumps on every (re)load

    // Play lifecycle. beginPlay creates services, then behaviours for every entity with a
    // "Behaviours" component. scene / vfs must outlive play.
    void beginPlay(::fledge::Scene& scene, Vfs* vfs);
    void update(float dt);  // updateEarly(dt) + updateLate()
    // The frame in two halves (GameWorld::tick): fixed steps (60 Hz) + OnUpdate, then - after physics and the animation
    // update, so OnLateUpdate sees this frame's bone poses and physics results - OnLateUpdate, end-of-frame work and the
    // deferred spawns / destroys. updateLate() does nothing unless updateEarly() ran a frame since the last call.
    void updateEarly(float dt);
    void updateLate();
    // Bone queries for Animator::BoneWorld* (GameWorld supplies them from the AnimationSystem): model-space matrix of a
    // bone of an entity's Animator (nullopt = unknown bone / no Animator) and the skeleton's bone names.
    std::function<std::optional<Mat4>(EntityId, const std::string&)> boneModel;
    std::function<std::vector<std::string>(EntityId)> boneNames;
    const FScriptHostApi& hostApi() const { return m_api; }  // tests drive the SDK wrappers against the real host
    void endPlay();
    bool playing() const { return m_scene != nullptr; }

    InputMap& input() { return m_input; }
    void setInput(const InputFrame& frame) { m_pendingInput = frame; }
    void resetInput() {  // nothing down, no pressed / released edges
        m_pendingInput = {};
        m_input.beginFrame({});
        m_input.beginFrame({});
    }
    // Raycast provider (GameWorld supplies a bounds-based one until physics exists).
    std::function<std::optional<RaycastHit>(Vec3 origin, Vec3 dir, float maxDist)> raycast;
    // 1.3.2 filtered queries (GameWorld::raycast / sphereCast with a QueryFilter): ignore entity (+ its children), layer mask.
    std::function<std::optional<RaycastHit>(Vec3 origin, Vec3 dir, float maxDist, EntityId ignore, uint32_t layerMask, bool ignoreHierarchy)>
        raycastFiltered;
    std::function<std::optional<RaycastHit>(Vec3 origin, float radius, Vec3 dir, float maxDist, EntityId ignore, uint32_t layerMask, bool ignoreHierarchy)>
        sphereCast;

    // Live objects (playing)
    size_t objectCount() const;
    size_t behaviourCount() const;
    std::optional<Json> liveProperties(EntityId e, const std::string& cls);
    Status setLiveProperty(EntityId e, const std::string& cls, const std::string& prop, const Json& value);
    std::vector<std::string> liveClasses(EntityId e) const;
    void attachLive(EntityId e, const std::string& cls, const Json& props);  // editor attach while playing
    void detachLive(EntityId e, const std::string& cls);
    int reloadCount() const { return m_reloads; }
    std::string lastModuleError() const;

    // Engine -> script collision events (GameWorld forwards the physics contact events).
    void notifyCollision(EntityId e, EntityId other, int kind);

    struct Impl;  // host callbacks (C ABI) need access

private:
    struct Object {
        EntityId entity = kNullEntity;  // 0 for services
        std::string cls;
        FScriptInstance handle = 0;
        bool enabled = true;
    };
    struct Loaded {
        void* dll = nullptr;
        const FScriptModuleApi* api = nullptr;
        fs::path path;
    };
    void createForEntity(EntityId e, bool reloading);
    void createObject(EntityId e, const std::string& cls, const Json& props, bool enabled, bool reloading);
    void destroyObjectsOf(EntityId e);
    void processDeferred();
    Status openModule(const fs::path& dll, Loaded& out, ModuleInfo& info);
    void closeModule(Loaded& m);

    std::unique_ptr<Loaded> m_module;
    ModuleInfo m_info;
    uint32_t m_generation = 0;
    int m_reloads = 0;
    FScriptHostApi m_api{};
    FScriptFrameInfo m_frame{};
    ::fledge::Scene* m_scene = nullptr;
    Vfs* m_vfs = nullptr;
    std::vector<Object> m_objects;
    std::vector<EntityId> m_pendingCreate;        // entities spawned this frame
    std::vector<std::pair<EntityId, std::pair<std::string, Json>>> m_pendingAdd;  // add_behaviour
    std::vector<EntityId> m_pendingDestroy;
    InputMap m_input;
    InputFrame m_pendingInput;
    float m_fixedAccumulator = 0;
    bool m_clearReloading = false;
    bool m_lateDue = false;
    std::string m_scratch;
    friend struct Impl;
};

} // namespace fledge::script
