#include "fledge/scene/scene.h"
#include "fledge/core/legacy.h"

#include <algorithm>
#include <format>

#include "fledge/core/assert.h"
#include "fledge/scene/components.h"

namespace fledge {

// ------------------------------------------------------- ComponentRegistry --
ComponentRegistry::ComponentRegistry() { registerBuiltinComponents(*this); }

ComponentRegistry& ComponentRegistry::get() {
    static ComponentRegistry r;
    return r;
}

void ComponentRegistry::add(ComponentTypeInfo info) {
    for (auto& t : m_types)
        if (t.name == info.name) return;  // idempotent
    m_types.push_back(std::move(info));
}

const ComponentTypeInfo* ComponentRegistry::find(std::string_view name) const {
    for (auto& t : m_types)
        if (t.name == name) return &t;
    return nullptr;
}
const ComponentTypeInfo* ComponentRegistry::find(std::type_index ti) const {
    for (auto& t : m_types)
        if (t.cppType == ti) return &t;
    return nullptr;
}
int ComponentRegistry::indexOf(std::type_index ti) const {
    for (size_t i = 0; i < m_types.size(); ++i)
        if (m_types[i].cppType == ti) return static_cast<int>(i);
    return -1;
}
int ComponentRegistry::indexOf(std::string_view name) const {
    for (size_t i = 0; i < m_types.size(); ++i)
        if (m_types[i].name == name) return static_cast<int>(i);
    return -1;
}

// ------------------------------------------------------------------ Scene --
Scene::Scene() { ComponentRegistry::get(); }

IComponentPool& Scene::poolAt(int idx) {
    auto& reg = ComponentRegistry::get();
    FLEDGE_ASSERT_MSG(idx >= 0, "component type not registered");
    while (m_pools.size() < reg.types().size()) m_pools.push_back(reg.types()[m_pools.size()].makePool());
    return *m_pools[static_cast<size_t>(idx)];
}

EntityId Scene::createEntity(const std::string& entityName, EntityId forcedId) {
    EntityId id = forcedId != kNullEntity ? forcedId : m_nextId;
    if (isValid(id)) id = m_nextId;
    m_nextId = std::max(m_nextId, id + 1);
    m_entities.push_back(id);
    m_alive.insert(id);
    add<NameComponent>(id, {entityName});
    add<TransformComponent>(id);
    markHierarchyDirty();
    return id;
}

void Scene::markHierarchyDirty() { m_dfsDirty = true; }

void Scene::detach(EntityId e) {
    auto it = m_parent.find(e);
    if (it == m_parent.end()) return;
    markHierarchyDirty();
    auto& siblings = m_children[it->second];
    std::erase(siblings, e);
    if (siblings.empty()) m_children.erase(it->second);
    m_parent.erase(it);
}

void Scene::destroySingle(EntityId e) {
    if (!isValid(e)) return;
    detach(e);
    if (auto it = m_children.find(e); it != m_children.end()) {
        for (EntityId c : it->second) m_parent.erase(c);
        m_children.erase(it);
    }
    for (size_t i = 0; i < ComponentRegistry::get().types().size(); ++i) poolAt(static_cast<int>(i)).remove(e);
    m_opaque.erase(e);
    std::erase(m_entities, e);
    m_alive.erase(e);
    markHierarchyDirty();
}

void Scene::destroyEntity(EntityId e) {
    if (!isValid(e)) return;
    auto desc = descendants(e);
    for (auto it = desc.rbegin(); it != desc.rend(); ++it) destroySingle(*it);
    destroySingle(e);
}

bool Scene::isValid(EntityId e) const { return e != kNullEntity && m_alive.contains(e); }

EntityId Scene::findByName(std::string_view n) const {
    auto* self = const_cast<Scene*>(this);
    for (EntityId e : m_entities)
        if (auto* nc = self->get<NameComponent>(e); nc && nc->name == n) return e;
    return kNullEntity;
}

void Scene::clear() {
    for (auto& p : m_pools) p->clear();
    m_entities.clear();
    m_alive.clear();
    m_parent.clear();
    m_children.clear();
    m_opaque.clear();
    m_nextId = 1;
    m_dfsCache.clear();
    markHierarchyDirty();
}

void* Scene::getComponent(EntityId e, std::string_view type) {
    int idx = ComponentRegistry::get().indexOf(type);
    if (idx < 0 || !isValid(e)) return nullptr;
    return poolAt(idx).getRaw(e);
}

void* Scene::addComponent(EntityId e, std::string_view type) {
    int idx = ComponentRegistry::get().indexOf(type);
    if (idx < 0 || !isValid(e)) return nullptr;
    auto& p = poolAt(idx);
    if (void* existing = p.getRaw(e)) return existing;
    return p.addDefault(e);
}

bool Scene::removeComponent(EntityId e, std::string_view type) {
    auto* info = ComponentRegistry::get().find(type);
    if (!info || !info->removable || !isValid(e)) return false;
    auto& p = poolAt(ComponentRegistry::get().indexOf(type));
    if (!p.has(e)) return false;
    p.remove(e);
    return true;
}

std::vector<std::string> Scene::componentNames(EntityId e) const {
    std::vector<std::string> out;
    auto& types = ComponentRegistry::get().types();
    for (size_t i = 0; i < types.size() && i < m_pools.size(); ++i)
        if (m_pools[i]->has(e)) out.push_back(types[i].name);
    return out;
}

std::string Scene::displayName(EntityId e) {
    if (auto* n = get<NameComponent>(e)) return n->name;
    return std::format("Entity {}", e);
}

// --------------------------------------------------------------- hierarchy --
EntityId Scene::parentOf(EntityId e) const {
    auto it = m_parent.find(e);
    return it == m_parent.end() ? kNullEntity : it->second;
}

const std::vector<EntityId>& Scene::childrenOf(EntityId e) const {
    static const std::vector<EntityId> kEmpty;
    auto it = m_children.find(e);
    return it == m_children.end() ? kEmpty : it->second;
}

std::vector<EntityId> Scene::roots() const {
    std::vector<EntityId> r;
    for (EntityId e : m_entities)
        if (!m_parent.contains(e)) r.push_back(e);
    return r;
}

bool Scene::isAncestor(EntityId ancestor, EntityId e) const {
    for (EntityId p = parentOf(e); p != kNullEntity; p = parentOf(p))
        if (p == ancestor) return true;
    return false;
}

int Scene::siblingIndex(EntityId e) const {
    EntityId p = parentOf(e);
    if (p != kNullEntity) {
        const auto& c = childrenOf(p);
        return static_cast<int>(std::find(c.begin(), c.end(), e) - c.begin());
    }
    auto r = roots();
    return static_cast<int>(std::find(r.begin(), r.end(), e) - r.begin());
}

Status Scene::setParent(EntityId child, EntityId parent, int index, bool keepWorld) {
    if (!isValid(child)) return Status::failure(std::format("invalid entity {}", child));
    if (parent != kNullEntity && !isValid(parent)) return Status::failure(std::format("invalid parent {}", parent));
    if (parent == child || (parent != kNullEntity && isAncestor(child, parent)))
        return Status::failure("cannot parent an entity to itself or to one of its descendants");
    Transform world = worldTransform(child);
    detach(child);
    if (parent != kNullEntity) {
        auto& sib = m_children[parent];
        if (index < 0 || index > static_cast<int>(sib.size())) sib.push_back(child);
        else sib.insert(sib.begin() + index, child);
        m_parent[child] = parent;
    } else if (index >= 0) {
        // Root ordering lives in m_entities: move child before the index-th root.
        std::erase(m_entities, child);
        auto r = roots();
        auto pos = index < static_cast<int>(r.size())
                       ? std::find(m_entities.begin(), m_entities.end(), r[static_cast<size_t>(index)])
                       : m_entities.end();
        m_entities.insert(pos, child);
    }
    if (keepWorld) setWorldTransform(child, world);
    return {};
}

std::vector<EntityId> Scene::descendants(EntityId e) const {
    std::vector<EntityId> out;
    std::vector<EntityId> stack(childrenOf(e).rbegin(), childrenOf(e).rend());
    while (!stack.empty()) {
        EntityId c = stack.back();
        stack.pop_back();
        out.push_back(c);
        const auto& cc = childrenOf(c);
        stack.insert(stack.end(), cc.rbegin(), cc.rend());
    }
    return out;
}

const std::vector<EntityId>& Scene::depthFirstOrder() const {
    if (!m_dfsDirty && m_dfsCache.size() == m_entities.size()) return m_dfsCache;
    m_dfsCache.clear();
    m_dfsCache.reserve(m_entities.size());
    std::vector<EntityId> stack;
    for (EntityId r : m_entities) {
        if (m_parent.contains(r)) continue;
        m_dfsCache.push_back(r);
        const auto& kids = childrenOf(r);
        stack.assign(kids.rbegin(), kids.rend());
        while (!stack.empty()) {
            EntityId c = stack.back();
            stack.pop_back();
            m_dfsCache.push_back(c);
            const auto& cc = childrenOf(c);
            stack.insert(stack.end(), cc.rbegin(), cc.rend());
        }
    }
    m_dfsDirty = false;
    return m_dfsCache;
}

namespace {
// Bone socket: parent world * bone model matrix (when attachBone is set and resolves).
Mat4 socketFrame(Scene& s, EntityId parent, const Mat4& parentWorld, const TransformComponent* t) {
    if (!t || t->attachBone.empty() || !s.boneResolver) return parentWorld;
    if (auto b = s.boneResolver(parent, t->attachBone)) return parentWorld * *b;
    return parentWorld;
}
} // namespace

Mat4 Scene::worldMatrix(EntityId e) {
    auto* t = get<TransformComponent>(e);
    Mat4 local = t ? t->matrix() : Mat4{};
    EntityId p = parentOf(e);
    return p != kNullEntity ? socketFrame(*this, p, worldMatrix(p), t) * local : local;
}

Mat4 Scene::parentFrame(EntityId e) {
    EntityId p = parentOf(e);
    if (p == kNullEntity) return Mat4{};
    return socketFrame(*this, p, worldMatrix(p), get<TransformComponent>(e));
}

Transform Scene::worldTransform(EntityId e) {
    if (parentOf(e) == kNullEntity) {
        auto* t = get<TransformComponent>(e);
        return t ? t->toTransform() : Transform{};
    }
    return decompose(worldMatrix(e));
}

void Scene::setWorldTransform(EntityId e, const Transform& world) {
    auto* t = get<TransformComponent>(e);
    if (!t) return;
    EntityId p = parentOf(e);
    Transform local = p == kNullEntity ? world : decompose(parentFrame(e).inverse() * world.matrix());
    t->position = local.position;
    t->rotation = local.rotation;
    t->scale = local.scale;
}

std::unordered_map<EntityId, Mat4> Scene::computeWorldMatrices() {
    std::unordered_map<EntityId, Mat4> out;
    out.reserve(m_entities.size());
    for (EntityId e : depthFirstOrder()) {
        auto* t = get<TransformComponent>(e);
        Mat4 local = t ? t->matrix() : Mat4{};
        EntityId p = parentOf(e);
        out[e] = p != kNullEntity ? socketFrame(*this, p, out[p], t) * local : local;
    }
    return out;
}

// ------------------------------------------------------ unknown components --
const Json* Scene::opaqueComponents(EntityId e) const {
    auto it = m_opaque.find(e);
    return it == m_opaque.end() ? nullptr : &it->second;
}

std::vector<std::string> Scene::unknownComponentTypes() const {
    std::vector<std::string> out;
    for (const auto& [e, comps] : m_opaque)
        for (auto& [type, v] : comps.items()) out.push_back(type);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::pair<std::string, int>> Scene::usedFeatures() const {
    std::vector<std::pair<std::string, int>> out;
    auto& types = ComponentRegistry::get().types();
    for (size_t i = 0; i < types.size() && i < m_pools.size(); ++i)
        if (!types[i].feature.empty() && m_pools[i]->size() > 0) out.emplace_back(types[i].feature, types[i].featureVersion);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ------------------------------------------------------------ serialization --
Json Scene::entityToJson(EntityId e) {
    Json comps = Json::object();
    auto& types = ComponentRegistry::get().types();
    for (size_t i = 0; i < types.size(); ++i) {
        auto& p = poolAt(static_cast<int>(i));
        if (void* c = p.getRaw(e)) comps[types[i].name] = types[i].type.toJson(c);
    }
    if (auto it = m_opaque.find(e); it != m_opaque.end())
        for (auto& [type, value] : it->second.items()) comps[type] = value;  // unknown types: written back unchanged
    Json j = {{"id", e}};
    if (EntityId p = parentOf(e); p != kNullEntity) j["parent"] = p;
    j["components"] = comps;
    return j;
}

Json Scene::entityState(EntityId e) {
    Json j = entityToJson(e);
    j["parent"] = parentOf(e);
    j["index"] = siblingIndex(e);
    return j;
}

Status Scene::applyEntityState(EntityId e, const Json& state) {
    if (!isValid(e)) {
        EntityId got = createEntity("Entity", e);
        if (got != e) return Status::failure(std::format("cannot recreate entity {}", e));
    }
    const Json comps = state.value("components", Json::object());
    for (const auto& n : componentNames(e))
        if (!comps.contains(n)) removeComponent(e, n);
    m_opaque.erase(e);
    for (auto& [type, value] : comps.items()) {
        if (!ComponentRegistry::get().find(type)) {  // unknown type: keep it verbatim (never dropped)
            m_opaque[e][type] = value;
            continue;
        }
        void* c = addComponent(e, type);
        if (!c) return Status::failure("unknown component type '" + type + "'");
        const TypeInfo& ti = ComponentRegistry::get().find(type)->type;
        ti.clearOmitted(c, value);  // a state saved without an optional field (Transform.attachBone) means "not set"
        if (auto st = ti.fromJson(c, value); !st) return st;
    }
    EntityId parent = state.value("parent", kNullEntity);
    if (parent != kNullEntity && !isValid(parent)) parent = kNullEntity;
    int index = state.value("index", -1);
    if (parentOf(e) != parent || siblingIndex(e) != index) return setParent(e, parent, index, false);
    return {};
}

Json Scene::toJson() {
    Json ents = Json::array();
    for (EntityId e : depthFirstOrder()) ents.push_back(entityToJson(e));  // parents precede children
    return {{"format", "fscene"}, {"version", 2}, {"name", name}, {"entities", ents}};
}

Status Scene::fromJson(const Json& j) {
    if (!j.is_object() || !legacy::formatIs(j, "fscene"))
        return Status::failure("not a .fscene document (missing \"format\": \"fscene\")");
    int version = j.value("version", 0);
    // v1 = flat scenes (still loadable), v2 = optional "parent" per entity.
    if (version != 1 && version != 2) return Status::failure("unsupported .fscene version");
    clear();
    name = j.value("name", "Untitled");
    std::vector<std::pair<EntityId, EntityId>> links;
    for (const auto& ej : j.value("entities", Json::array())) {
        EntityId id = ej.value("id", 0u);
        EntityId e = createEntity("Entity", id);
        if (ej.contains("parent") && ej["parent"].is_number_integer()) links.emplace_back(e, ej["parent"].get<EntityId>());
        const Json& comps = ej.contains("components") ? ej["components"] : Json::object();
        for (auto& [type, value] : comps.items()) {
            if (!ComponentRegistry::get().find(type)) {  // unknown type (Advanced module / newer Fledge): preserved
                m_opaque[e][type] = value;
                continue;
            }
            void* c = addComponent(e, type);
            if (!c) return Status::failure(std::format("entity {}: cannot add component type '{}'", id, type));
            if (auto st = ComponentRegistry::get().find(type)->type.fromJson(c, value); !st)
                return Status::failure(std::format("entity {}: {}", id, st.error));
        }
    }
    for (auto [child, parent] : links) {
        if (parent == kNullEntity) continue;
        if (auto st = setParent(child, parent, -1, false); !st)
            return Status::failure(std::format("entity {}: bad parent {}: {}", child, parent, st.error));
    }
    return {};
}

} // namespace fledge
