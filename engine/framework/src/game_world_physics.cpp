// GameWorld <-> physics glue (docs/PHYSICS.md): providers (meshes, terrains, roads, skeletons), the fixed-step
// loop, contact events -> Fledge Script callbacks, ragdoll poses -> animation, Time Machine state, debug drawing.
#include <algorithm>
#include <cmath>

#include "fledge/core/log.h"
#include "fledge/framework/game_world.h"
#include "fledge/world/roads.h"

namespace fledge {

bool GameWorld::sceneNeedsPhysics(Scene& scene) {
    for (EntityId e : scene.entities()) {
        if (scene.get<ColliderComponent>(e) || scene.get<CharacterControllerComponent>(e) || scene.get<VehicleComponent>(e)) return true;
        if (auto* cloth = scene.get<ClothComponent>(e); cloth && cloth->enabled) return true;
        if (auto* a = scene.get<AnimatorComponent>(e); a && a->ragdoll) return true;
    }
    return false;
}

void GameWorld::startPhysics() {
    if (m_physics || !sceneNeedsPhysics(m_scene)) return;
    m_physics = std::make_unique<physics::PhysicsWorld>();
    m_physicsAccum = 0;
    physics::Providers& p = m_physics->providers;
    p.mesh = [this](const std::string& ref) { return cpuMesh(ref); };
    p.terrainHeights = [this](EntityId e, std::vector<float>& h, int& res, float& size) {
        const world::Heightfield* hf = heightfield(e);
        if (!hf) return false;
        h = hf->heights();
        res = hf->resolution();
        size = hf->size();
        return true;
    };
    p.roadMesh = [this](EntityId e) {
        MeshData out;
        auto* s = m_scene.get<SplineComponent>(e);
        if (!s) return out;
        world::RoadMeshes rm = world::buildRoadMeshes(*s);
        out = std::move(rm.road);
        uint32_t base = static_cast<uint32_t>(out.vertices.size());
        out.vertices.insert(out.vertices.end(), rm.sidewalks.vertices.begin(), rm.sidewalks.vertices.end());
        for (uint32_t i : rm.sidewalks.indices) out.indices.push_back(base + i);
        return out;
    };
    p.clothText = [this](const std::string& asset) -> std::optional<std::string> {
        std::string path = asset.find(":/") == std::string::npos ? "content:/" + asset : asset;
        return m_vfs.readText(path);
    };
    p.skeleton = [this](EntityId e) -> const void* {
        auto* ac = m_scene.get<AnimatorComponent>(e);
        const anim::Character* c = ac ? m_anim.character(ac->character) : nullptr;
        return c ? &c->skeleton : nullptr;
    };
    p.bonePose = [this](EntityId e) -> const std::vector<Mat4>* {
        static thread_local std::vector<Mat4> pose;
        pose = m_anim.boneMatrices(e);
        if (pose.empty()) {  // not animated yet: rest pose
            auto* ac = m_scene.get<AnimatorComponent>(e);
            if (const anim::Character* c = ac ? m_anim.character(ac->character) : nullptr) pose = anim::modelSpace(c->skeleton, anim::restPose(c->skeleton));
        }
        return pose.empty() ? nullptr : &pose;
    };
    m_anim.poseOverride = [this](EntityId e) { return m_physics ? m_physics->ragdollPose(e) : nullptr; };
    m_physics->sync(m_scene);
    FLEDGE_LOG_INFO("physics", "Physics started: {} bodies, {} characters, {} vehicles", m_physics->bodyCount(), m_physics->characterCount(),
                    m_physics->vehicleCount());
}

void GameWorld::stopPhysics() {
    m_anim.poseOverride = nullptr;
    m_physics.reset();
}

void GameWorld::stepPhysics(float dt) {
    if (!m_physics) {
        if (!m_playing || !sceneNeedsPhysics(m_scene)) return;
        startPhysics();  // a script / tool added the first physics component during play
        if (!m_physics) return;
    }
    m_physicsAccum = std::min(m_physicsAccum + dt, physicsStep * 4);  // never spiral: at most 4 steps per frame
    while (m_physicsAccum >= physicsStep * 0.999f) {
        m_physicsAccum -= physicsStep;
        for (auto& [n, sys] : m_systems) sys->fixedStep(*this, physicsStep);  // module systems (world_system.h)
        m_physics->sync(m_scene);  // streaming / scripts / editor may have added, moved or removed entities
        m_physics->step(m_scene, physicsStep);
        for (const physics::ContactEvent& ev : m_physics->takeEvents()) {
            int kind = static_cast<int>(ev.kind);  // matches FScriptCollision
            if (!m_scene.isValid(ev.a) || !m_scene.isValid(ev.b)) continue;
            m_script.notifyCollision(ev.a, ev.b, kind);
            m_script.notifyCollision(ev.b, ev.a, kind);
        }
    }
}

Bytes GameWorld::savePhysicsState() const { return m_physics ? m_physics->saveState() : Bytes{}; }

bool GameWorld::restorePhysicsState(const Bytes& state) {
    if (state.empty()) return false;
    if (!m_physics) startPhysics();
    if (!m_physics) return false;
    m_physics->sync(m_scene);
    return m_physics->restoreState(state);
}

void GameWorld::addPhysicsDebug(const std::vector<EntityId>& selection, std::unordered_map<EntityId, Mat4>& worlds) {
    if (!m_renderer) return;
    const Vec3 green{0.2f, 1.0f, 0.4f}, cyan{0.2f, 0.8f, 1.0f};
    auto gizmo = [&](EntityId e, Vec3 color) {
        auto* col = m_scene.get<ColliderComponent>(e);
        auto* cc = m_scene.get<CharacterControllerComponent>(e);
        if (!col && !cc) return;
        const Mat4& m = worlds[e];
        Transform wt = decompose(m);
        Mat4 rigid = Mat4::translation(wt.position) * Mat4::rotation(wt.rotation);
        auto circle = [&](const Mat4& frame, Vec3 c, float r, int axis) {
            constexpr int kSeg = 24;
            for (int i = 0; i < kSeg; ++i) {
                float a0 = 6.2831853f * i / kSeg, a1 = 6.2831853f * (i + 1) / kSeg;
                auto pt = [&](float a) {
                    Vec3 o{0, 0, 0};
                    o[(axis + 1) % 3] = std::cos(a) * r;
                    o[(axis + 2) % 3] = std::sin(a) * r;
                    return frame.transformPoint(c + o);
                };
                m_renderer->addLine(pt(a0), pt(a1), color);
            }
        };
        if (cc) {  // capsule from the feet up
            float r = cc->radius, h = cc->height;
            Mat4 f = Mat4::translation(wt.position);
            circle(f, {0, r, 0}, r, 1);
            circle(f, {0, h - r, 0}, r, 1);
            circle(f, {0, h * 0.5f, 0}, r, 0);
            circle(f, {0, h * 0.5f, 0}, r, 2);
            return;
        }
        Vec3 sc = wt.scale;
        Vec3 off = col->offset * sc;
        switch (col->shape) {
            case ColliderShape::Sphere: {
                float r = col->radius * std::max({sc.x, sc.y, sc.z});
                for (int a = 0; a < 3; ++a) circle(rigid, off, r, a);
                break;
            }
            case ColliderShape::Capsule: {
                float r = col->radius * std::max(sc.x, sc.z), hh = col->height * sc.y * 0.5f;
                circle(rigid, off + Vec3{0, hh, 0}, r, 1);
                circle(rigid, off - Vec3{0, hh, 0}, r, 1);
                for (float s : {-1.0f, 1.0f}) {
                    m_renderer->addLine(rigid.transformPoint(off + Vec3{s * r, -hh, 0}), rigid.transformPoint(off + Vec3{s * r, hh, 0}), color);
                    m_renderer->addLine(rigid.transformPoint(off + Vec3{0, -hh, s * r}), rigid.transformPoint(off + Vec3{0, hh, s * r}), color);
                }
                break;
            }
            default: {  // box (mesh / hull colliders show their mesh bounds)
                Vec3 mn = col->size * -0.5f, mx = col->size * 0.5f;
                if (col->shape != ColliderShape::Box) {
                    std::string ref = col->mesh;
                    if (ref.empty())
                        if (auto* mr = m_scene.get<MeshRendererComponent>(e)) ref = mr->mesh;
                    if (const MeshData* md = cpuMesh(ref)) meshBounds(*md, mn, mx);
                }
                Vec3 c[8];
                for (int k = 0; k < 8; ++k) c[k] = m.transformPoint(col->offset + Vec3{k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z});
                const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& ed : edges) m_renderer->addLine(c[ed[0]], c[ed[1]], color);
            }
        }
    };
    for (EntityId e : selection)
        if (m_scene.isValid(e)) gizmo(e, green);
    if (!physicsDebug) return;
    if (m_physics) {  // live simulation: world bounds of every body
        std::vector<std::pair<Vec3, Vec3>> lines;
        m_physics->debugLines(lines);
        for (auto& [a, b] : lines) m_renderer->addLine(a, b, cyan);
    } else {
        for (EntityId e : m_scene.entities())
            if (std::find(selection.begin(), selection.end(), e) == selection.end()) gizmo(e, cyan);
    }
}

} // namespace fledge
