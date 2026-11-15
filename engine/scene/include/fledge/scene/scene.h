#pragma once
// ECS. Entities are stable 32-bit ids (never reused within a scene, so MCP
// clients and serialized scenes can refer to them). Components live in
// sparse-set pools; every component type is registered with reflection so the
// editor, serializer and MCP tools can work with any of them generically.
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/math.h"
#include "fledge/core/reflection.h"
#include "fledge/core/utility.h"

namespace fledge {

using EntityId = uint32_t;
constexpr EntityId kNullEntity = 0;

class Scene;

// ----------------------------------------------------------------- pools ---
class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual bool has(EntityId e) const = 0;
    virtual void* getRaw(EntityId e) = 0;
    virtual void* addDefault(EntityId e) = 0;
    virtual void remove(EntityId e) = 0;
    virtual size_t size() const = 0;
    virtual void clear() = 0;
};

template <typename T>
class ComponentPool final : public IComponentPool {
public:
    bool has(EntityId e) const override { return m_sparse.contains(e); }
    T* get(EntityId e) {
        auto it = m_sparse.find(e);
        return it == m_sparse.end() ? nullptr : &m_dense[it->second];
    }
    void* getRaw(EntityId e) override { return get(e); }
    T& add(EntityId e, T value) {
        if (T* existing = get(e)) { *existing = std::move(value); return *existing; }
        m_sparse[e] = static_cast<uint32_t>(m_dense.size());
        m_dense.push_back(std::move(value));
        m_entities.push_back(e);
        return m_dense.back();
    }
    void* addDefault(EntityId e) override { return &add(e, T{}); }
    void remove(EntityId e) override {
        auto it = m_sparse.find(e);
        if (it == m_sparse.end()) return;
        uint32_t idx = it->second, last = static_cast<uint32_t>(m_dense.size() - 1);
        if (idx != last) {
            m_dense[idx] = std::move(m_dense[last]);
            m_entities[idx] = m_entities[last];
            m_sparse[m_entities[idx]] = idx;
        }
        m_dense.pop_back();
        m_entities.pop_back();
        m_sparse.erase(it);
    }
    size_t size() const override { return m_dense.size(); }
    void clear() override { m_dense.clear(); m_entities.clear(); m_sparse.clear(); }
    const std::vector<EntityId>& entities() const { return m_entities; }
    std::vector<T>& components() { return m_dense; }

private:
    std::vector<T> m_dense;
    std::vector<EntityId> m_entities;
    std::unordered_map<EntityId, uint32_t> m_sparse;
};

// ------------------------------------------------------------- registry ---
struct ComponentTypeInfo {
    std::string name;
    TypeInfo type;
    std::type_index cppType = typeid(void);
    std::function<std::unique_ptr<IComponentPool>()> makePool;
    bool removable = true;  // Name/Transform are not removable
    // Edition feature this component belongs to ("" = core). Components of Advanced modules set it; the editor records
    // the feature in the project manifest the first time a scene using it is saved (docs/EDITIONS.md).
    std::string feature;
    int featureVersion = 1;
};

class ComponentRegistry {
public:
    static ComponentRegistry& get();  // pre-populated with built-in components

    template <typename T>
    void registerType(TypeInfo type, bool removable = true, std::string feature = {}, int featureVersion = 1) {
        ComponentTypeInfo info;
        info.name = type.name;
        info.type = std::move(type);
        info.cppType = typeid(T);
        info.makePool = [] { return std::make_unique<ComponentPool<T>>(); };
        info.removable = removable;
        info.feature = std::move(feature);
        info.featureVersion = featureVersion;
        add(std::move(info));
    }
    const ComponentTypeInfo* find(std::string_view name) const;
    const ComponentTypeInfo* find(std::type_index t) const;
    int indexOf(std::type_index t) const;
    int indexOf(std::string_view name) const;
    const std::vector<ComponentTypeInfo>& types() const { return m_types; }

private:
    ComponentRegistry();
    friend void registerBuiltinComponents(ComponentRegistry&);
    void add(ComponentTypeInfo info);
    std::vector<ComponentTypeInfo> m_types;
};

// ----------------------------------------------------------------- scene ---
class Scene {
public:
    Scene();
    std::string name = "Untitled";

    EntityId createEntity(const std::string& name = "Entity", EntityId forcedId = kNullEntity);
    void destroyEntity(EntityId e);  // also destroys all descendants
    void destroySingle(EntityId e);  // destroys only e; its children become roots
    bool isValid(EntityId e) const;
    const std::vector<EntityId>& entities() const { return m_entities; }
    EntityId findByName(std::string_view name) const;
    void clear();
    // Ids below next are never handed out by createEntity (streamed-out chunk entities keep theirs).
    void reserveIds(EntityId next) { if (next > m_nextId) m_nextId = next; }
    EntityId nextId() const { return m_nextId; }

    template <typename T>
    T& add(EntityId e, T value = {}) { return pool<T>().add(e, std::move(value)); }
    template <typename T>
    T* get(EntityId e) { return pool<T>().get(e); }
    template <typename T>
    bool has(EntityId e) { return pool<T>().has(e); }
    template <typename T>
    void remove(EntityId e) { pool<T>().remove(e); }
    template <typename T>
    ComponentPool<T>& pool() {
        int idx = ComponentRegistry::get().indexOf(typeid(T));
        return static_cast<ComponentPool<T>&>(poolAt(idx));
    }
    // Iterate all entities having T and U.
    template <typename T, typename U, typename F>
    void each(F&& fn) {
        auto& a = pool<T>();
        auto& b = pool<U>();
        for (size_t i = 0; i < a.entities().size(); ++i) {
            EntityId e = a.entities()[i];
            if (U* u = b.get(e)) fn(e, a.components()[i], *u);
        }
    }

    // Type-erased access by component name (editor, MCP).
    void* getComponent(EntityId e, std::string_view type);
    void* addComponent(EntityId e, std::string_view type);
    bool removeComponent(EntityId e, std::string_view type);
    std::vector<std::string> componentNames(EntityId e) const;

    Json entityToJson(EntityId e);
    Json toJson();
    Status fromJson(const Json& j);  // replaces the whole scene

    // ---- unknown components (second safety net, docs/EDITIONS.md) ----------------------------------------
    // A component type this executable does not know (an Advanced module that is not linked, or a newer Fledge) is
    // never dropped: fromJson() / applyEntityState() keep its JSON verbatim and entityToJson() writes it back, so a
    // load + save round trip preserves it. The editor shows these entities as read-only.
    const Json* opaqueComponents(EntityId e) const;          // {"TypeName": value, ...} or nullptr
    std::vector<std::string> unknownComponentTypes() const;  // sorted, unique, over the whole scene
    bool hasUnknownComponents() const { return !m_opaque.empty(); }
    // Edition features used by the components of this scene (ComponentTypeInfo::feature), sorted, unique.
    std::vector<std::pair<std::string, int>> usedFeatures() const;

    std::string displayName(EntityId e);

    // ---- hierarchy -------------------------------------------------------
    // TransformComponent is LOCAL (relative to the parent). Roots keep scene
    // order (order of entities()); children keep their sibling order.
    EntityId parentOf(EntityId e) const;
    const std::vector<EntityId>& childrenOf(EntityId e) const;
    std::vector<EntityId> roots() const;
    bool isAncestor(EntityId ancestor, EntityId e) const;  // true if ancestor is a (grand)parent of e
    int siblingIndex(EntityId e) const;
    // Re-parents child (parent = kNullEntity -> root). index < 0 appends.
    // keepWorld recomputes the local transform so the world transform is unchanged.
    // Fails on cycles / invalid ids.
    Status setParent(EntityId child, EntityId parent, int index = -1, bool keepWorld = true);
    std::vector<EntityId> descendants(EntityId e) const;  // depth-first, excluding e
    const std::vector<EntityId>& depthFirstOrder() const;  // all entities, parents before children (cached until hierarchy changes)

    Mat4 worldMatrix(EntityId e);
    Transform worldTransform(EntityId e);  // decomposed worldMatrix (exact without shear)
    void setWorldTransform(EntityId e, const Transform& world);
    // World matrices for all entities in one pass.
    std::unordered_map<EntityId, Mat4> computeWorldMatrices();
    // The space e's local Transform is relative to: the parent's world matrix, times the socket bone's model-space matrix
    // when Transform.attachBone is set and resolves (identity for roots).
    Mat4 parentFrame(EntityId e);

    // ---- bone sockets (Transform.attachBone, docs/ANIMATION.md "Bone sockets") --------------------------------------
    // Model-space matrix of bone `bone` of the Animator on `owner` (the socket child's parent), or nullopt when it does
    // not resolve (no Animator, unknown bone, character not loaded): the child then follows the parent itself. Installed by
    // GameWorld (AnimationSystem::socketBone); a bare Scene has none, so attachBone is ignored there.
    std::function<std::optional<Mat4>(EntityId owner, const std::string& bone)> boneResolver;

    // Full state of one entity (components + parent + sibling index); used by
    // the undo system. applyEntityState creates the entity if needed.
    Json entityState(EntityId e);
    Status applyEntityState(EntityId e, const Json& state);

private:
    IComponentPool& poolAt(int idx);  // asserts idx is a registered type; creates pools lazily
    std::vector<std::unique_ptr<IComponentPool>> m_pools;
    std::vector<EntityId> m_entities;
    EntityId m_nextId = 1;
    std::unordered_set<EntityId> m_alive;
    std::unordered_map<EntityId, EntityId> m_parent;
    std::unordered_map<EntityId, std::vector<EntityId>> m_children;
    std::unordered_map<EntityId, Json> m_opaque;  // unknown component types, kept verbatim
    void detach(EntityId e);
    void markHierarchyDirty();
    mutable std::vector<EntityId> m_dfsCache;
    mutable bool m_dfsDirty = true;
};


// Play-in-editor support: snapshot scene state on Play, restore on Stop.
class SceneSnapshot {
public:
    void capture(Scene& scene) { m_data = scene.toJson(); m_valid = true; }
    Status restore(Scene& scene) {
        if (!m_valid) return Status::failure("no snapshot");
        auto st = scene.fromJson(m_data);
        m_valid = false;
        return st;
    }
    bool valid() const { return m_valid; }

private:
    Json m_data;
    bool m_valid = false;
};

} // namespace fledge
