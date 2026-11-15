// GameWorld gameplay runtime (docs/GAMEPLAY.md): the built-in PlayerController (walk / drive / carjack / shoot),
// traffic + pedestrian entities from TrafficSim, police and the wanted level, NavAgents, pickups, missions,
// time of day + weather, camera rigs + sequences, the HUD, and save / load.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <set>

#include "fledge/core/log.h"
#include "fledge/core/paths.h"
#include "fledge/core/utility.h"
#include "fledge/framework/game_world.h"
#include "fledge/gameplay/city.h"
#include "fledge/gameplay/font.h"
#include "fledge/gameplay/localization.h"
#include "fledge/gameplay/ui.h"
#include "gameplay_state.h"

namespace fledge {

using namespace gameplay;

namespace {
constexpr const char* kTrafficTag = "traffic-agent";
constexpr const char* kPoliceTag = "police-unit";

Vec3 posOf(Scene& s, EntityId e) { return s.worldMatrix(e).c[3].xyz(); }
float yawOfQuat(Quat q) {
    Vec3 f = q.rotate({0, 0, -1});
    return std::atan2(-f.x, -f.z);
}
Vec3 forwardOfYaw(float yaw) { return {-std::sin(yaw), 0, -std::cos(yaw)}; }

Inventory loadInv(const InventoryComponent& c) {
    Inventory inv;
    inv.money = c.money;
    if (c.items.is_object())
        for (auto& [k, v] : c.items.items())
            if (v.is_number()) inv.items[k] = v.get<int>();
    if (c.ammo.is_object())
        for (auto& [k, v] : c.ammo.items())
            if (v.is_number()) inv.ammo[k] = v.get<int>();
    inv.current = c.weapon;
    return inv;
}
void storeInv(InventoryComponent& c, const Inventory& inv) {
    c.money = inv.money;
    c.items = Json::object();
    for (auto& [k, v] : inv.items) c.items[k] = v;
    c.ammo = Json::object();
    for (auto& [k, v] : inv.ammo) c.ammo[k] = v;
    c.weapon = inv.current;
}
bool hasTag(Scene& s, EntityId e, std::string_view tag) {
    auto* t = s.get<TagsComponent>(e);
    if (!t) return false;
    std::string_view all = t->tags;
    size_t p = all.find(tag);
    return p != std::string_view::npos && (p == 0 || all[p - 1] == ' ') && (p + tag.size() == all.size() || all[p + tag.size()] == ' ');
}
void appendTris(std::vector<Vec3>& v, std::vector<uint32_t>& idx, const MeshData& m, const Mat4& world) {
    uint32_t base = static_cast<uint32_t>(v.size());
    for (auto& vert : m.vertices) v.push_back(world.transformPoint(vert.position));
    for (uint32_t i : m.indices) idx.push_back(base + i);
}
// ParticleEmitter component -> simulation parameters (a preset replaces every field but enabled / burst).
EmitterParams emitterParams(const ParticleEmitterComponent& c) {
    static const char* kPresets[] = {"Custom", "Smoke", "Fire", "Sparks", "Dust", "Steam", "Rain", "Snow"};
    int preset = static_cast<int>(c.preset);
    if (preset > 0 && preset < 8) return particlePreset(kPresets[preset]);
    EmitterParams p;
    p.rate = c.rate;
    p.maxParticles = std::clamp(c.maxParticles, 1, 20000);
    p.lifetime = std::max(c.lifetime, 0.01f);
    p.direction = c.direction;
    p.speed = c.speed;
    p.spreadDeg = c.spread;
    p.shape = static_cast<EmitterShape>(std::clamp(static_cast<int>(c.shape), 0, 2));
    p.extent = c.extent;
    p.gravity = c.gravity;
    p.drag = c.drag;
    p.startSize = c.startSize;
    p.endSize = c.endSize;
    p.startColor = c.startColor;
    p.endColor = c.endColor;
    p.startOpacity = c.startOpacity;
    p.endOpacity = c.endOpacity;
    p.intensity = c.intensity;
    p.stretch = c.stretch;
    p.additive = c.additive;
    p.worldSpace = c.worldSpace;
    return p;
}
const Vec3 kCarColors[8] = {{0.75f, 0.12f, 0.1f}, {0.1f, 0.25f, 0.65f}, {0.85f, 0.85f, 0.82f}, {0.08f, 0.08f, 0.09f},
                            {0.2f, 0.45f, 0.25f}, {0.85f, 0.65f, 0.1f}, {0.45f, 0.45f, 0.48f}, {0.5f, 0.15f, 0.4f}};
} // namespace

// ------------------------------------------------------------------ lookups --
EntityId GameWorld::player() {
    for (EntityId e : m_scene.entities())
        if (m_scene.get<PlayerControllerComponent>(e)) return e;
    return kNullEntity;
}

const world::RoadGraph* GameWorld::roadGraph() {
    if (!m_gp) return nullptr;
    if (!m_gp->roadsBuilt) {
        m_gp->roads = world::RoadGraph::build(m_scene);
        m_gp->roadsBuilt = true;
    }
    return &m_gp->roads;
}

gameplay::TrafficSim* GameWorld::traffic() { return m_gp ? m_gp->traffic.get() : nullptr; }

gameplay::NavMesh* GameWorld::navMesh() {
    if (!m_gp) return nullptr;
    if (m_gp->nav || m_gp->navTried) return m_gp->nav.get();
    m_gp->navTried = true;
    std::vector<Vec3> v;
    std::vector<uint32_t> idx;
    auto worlds = m_scene.computeWorldMatrices();
    MeshData cube = primitives::byName("builtin:cube");
    for (EntityId e : m_scene.entities()) {
        if (hasTag(m_scene, e, kTrafficTag) || hasTag(m_scene, e, kPoliceTag)) continue;
        if (auto* col = m_scene.get<ColliderComponent>(e)) {
            auto* rb = m_scene.get<RigidBodyComponent>(e);
            if ((rb && rb->motion != MotionType::Static) || col->isTrigger) continue;
            Mat4 m = worlds[e] * Mat4::translation(col->offset);
            if (col->shape == ColliderShape::Box) appendTris(v, idx, cube, m * Mat4::scale(col->size));
            else if (auto* mr = m_scene.get<MeshRendererComponent>(e))
                if (const MeshData* md = cpuMesh(col->mesh.empty() ? mr->mesh : col->mesh)) appendTris(v, idx, *md, worlds[e]);
        }
        if (m_scene.get<TerrainComponent>(e))
            if (const world::Heightfield* hf = heightfield(e)) {
                int tiles = hf->tilesPerEdge(32);
                for (int tz = 0; tz < tiles; ++tz)
                    for (int tx = 0; tx < tiles; ++tx) appendTris(v, idx, hf->tileMesh(tx, tz, 1, 32), worlds[e]);
            }
        if (auto* sp = m_scene.get<SplineComponent>(e); sp && sp->kind == "road") {
            auto rm = world::buildRoadMeshes(*sp);
            appendTris(v, idx, rm.road, worlds[e]);
            appendTris(v, idx, rm.sidewalks, worlds[e]);
        }
    }
    NavSettings ns;
    ns.cellSize = 0.4f;
    ns.cellHeight = 0.2f;
    auto nav = std::make_unique<NavMesh>();
    if (nav->build(v, idx, ns, &m_gp->navError)) {
        FLEDGE_LOG_INFO("gameplay", "Navmesh: {} polygons from {} triangles in {:.0f} ms", nav->stats().polygons, nav->stats().inputTriangles, nav->stats().buildMs);
        m_gp->nav = std::move(nav);
    } else {
        FLEDGE_LOG_WARN("gameplay", "Navmesh not built: {}", m_gp->navError);
    }
    return m_gp->nav.get();
}

void GameWorld::showMessage(const std::string& text, float seconds, Vec3 color) {
    if (!m_gp) return;
    m_gp->bigMessage = text;
    m_gp->bigMessageLeft = seconds;
    m_gp->bigColor = {color.x, color.y, color.z, 1};
}

void GameWorld::commitCrime(int crime) {
    if (!m_gp) return;
    EntityId p = player();
    if (auto* w = p ? m_scene.get<WantedComponent>(p) : nullptr; !w || !w->enabled) return;
    m_gp->wanted.commit(static_cast<Crime>(crime));
}

int GameWorld::missionNode(EntityId e) const {
    if (!m_gp) return 0;
    auto it = m_gp->missions.find(e);
    return it == m_gp->missions.end() || it->second.state() != MissionState::Running ? 0 : it->second.currentNode();
}

GameWorld::GameplayStats GameWorld::gameplayStats() const {
    GameplayStats s;
    if (!m_gp) return s;
    s.trafficCars = m_gp->traffic ? static_cast<int>(m_gp->traffic->cars().size()) : 0;
    s.pedestrians = m_gp->traffic ? static_cast<int>(m_gp->traffic->pedestrians().size()) : 0;
    s.police = static_cast<int>(m_gp->police.size());
    s.policeCars = static_cast<int>(m_gp->policeCars.size());
    s.shotsAtPlayer = m_gp->shotsAtPlayer;
    s.hitsOnPlayer = m_gp->hitsOnPlayer;
    for (auto& [e, r] : m_gp->missions) s.missionsRunning += r.state() == MissionState::Running ? 1 : 0;
    s.wanted = m_gp->wanted.level;
    s.navMesh = m_gp->nav != nullptr;
    s.ms = m_gp->ms;
    s.speedKmh = m_gp->speedKmh;
    s.rainDrops = static_cast<int>(m_gp->rain.size());
    s.particles = s.rainDrops;
    for (auto& [e, em] : m_gp->emitters) s.particles += static_cast<int>(em.size());
    s.fogDensity = renderSettings.fog ? renderSettings.fogDensity : 0.0f;
    for (auto& [e, r] : m_gp->missions)
        if (r.state() == MissionState::Running && !r.objective().empty()) {
            s.objective = r.objective();
            break;
        }
    return s;
}

// ------------------------------------------------------------- play start --
void GameWorld::beginGameplay() {
    m_gp = std::make_unique<GameplayState>();
    reloadLocalization();
    // leftovers from a snapshot (Time Machine) are rebuilt from scratch
    std::vector<EntityId> stale;
    for (EntityId e : m_scene.entities())
        if (hasTag(m_scene, e, kTrafficTag) || hasTag(m_scene, e, kPoliceTag)) stale.push_back(e);
    for (EntityId e : stale)
        if (m_scene.isValid(e)) m_scene.destroyEntity(e);
    EntityId p = player();
    if (p) {
        auto* pc = m_scene.get<PlayerControllerComponent>(p);
        if (length(pc->spawnPoint) < 1e-4f) pc->spawnPoint = posOf(m_scene, p);
        if (!m_scene.get<CharacterControllerComponent>(p)) m_scene.add<CharacterControllerComponent>(p, CharacterControllerComponent{});
        if (!m_scene.get<HealthComponent>(p)) m_scene.add<HealthComponent>(p, HealthComponent{100, 100, "player", false});
        if (!m_scene.get<InventoryComponent>(p)) m_scene.add<InventoryComponent>(p, InventoryComponent{});
        if (auto* tc = m_scene.get<TransformComponent>(p)) m_gp->camYaw = yawOfQuat(tc->rotation);
        pc->vehicle.clear();
    }
    // traffic
    for (EntityId e : m_scene.entities()) {
        auto* tc = m_scene.get<TrafficComponent>(e);
        if (!tc || !tc->enabled) continue;
        const world::RoadGraph* g = roadGraph();
        if (!g || g->edges.empty()) {
            FLEDGE_LOG_WARN("gameplay", "Traffic: the scene has no road splines");
            break;
        }
        TrafficSettings ts;
        ts.cars = tc->cars;
        ts.pedestrians = tc->pedestrians;
        ts.radius = tc->radius;
        ts.seed = static_cast<uint32_t>(tc->seed);
        m_gp->traffic = std::make_unique<TrafficSim>(*g, ts);
        for (auto& c : m_gp->traffic->cars()) {
            Vec3 col = kCarColors[c.colorIndex % 8];
            Json n = {{"name", std::format("Traffic Car {}", c.id)},
                      {"components",
                       {{"Transform", {{"position", {c.position.x, c.position.y + 0.75f, c.position.z}}}},
                        {"Collider", {{"shape", "Box"}, {"size", {1.8f, 1.3f, 4.4f}}}},
                        {"RigidBody", {{"motion", "Kinematic"}}},
                        {"Tags", {{"tags", kTrafficTag}}}}},
                      {"children",
                       Json::array({{{"name", "Body"},
                                     {"components", {{"Transform", {{"scale", {1.8f, 0.75f, 4.4f}}, {"position", {0, -0.15f, 0}}}},
                                                     {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", {col.x, col.y, col.z}}}}}}},
                                    {{"name", "Cabin"},
                                     {"components", {{"Transform", {{"scale", {1.6f, 0.55f, 2.1f}}, {"position", {0, 0.45f, 0.2f}}}},
                                                     {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", {0.12f, 0.14f, 0.18f}}}}}}}})}};
            m_gp->trafficCars.push_back(spawnTree(m_scene, n));
        }
        uint32_t seed = 12345;
        for (auto& pd : m_gp->traffic->pedestrians()) {
            seed = seed * 1664525u + 1013904223u;
            float shade = 0.35f + static_cast<float>(seed >> 24) / 255.0f * 0.5f;
            Vec3 col = (seed & 1) ? Vec3{shade, shade * 0.8f, shade * 0.6f} : Vec3{shade * 0.5f, shade * 0.6f, shade};
            Json n = {{"name", std::format("Pedestrian {}", pd.id)},
                      {"components",
                       {{"Transform", {{"position", {pd.position.x, pd.position.y + 0.2f, pd.position.z}}}},
                        {"Animator", {{"character", "builtin:mannequin"}, {"crowd", true}, {"rootMotion", false}, {"params", {{"speed", pd.speed}}}, {"color", {col.x, col.y, col.z}}}},
                        {"Tags", {{"tags", kTrafficTag}}}}}};
            m_gp->trafficPeds.push_back(spawnTree(m_scene, n));
        }
        FLEDGE_LOG_INFO("gameplay", "Traffic: {} cars, {} pedestrians on {} roads ({} intersections)", m_gp->trafficCars.size(), m_gp->trafficPeds.size(),
                        g->edges.size(), g->intersections());
        break;
    }
    // missions
    for (EntityId e : m_scene.entities())
        if (auto* mc = m_scene.get<MissionComponent>(e)) {
            mc->state = "Idle";
            mc->objective.clear();
            if (mc->autoStart) m_gp->missions[e].start(MissionGraph::fromJson(mc->graph));
        }
    // pickups
    for (EntityId e : m_scene.entities())
        if (m_scene.get<PickupComponent>(e)) m_gp->pickupHome[e] = posOf(m_scene, e);
    m_gp->baseEnvIntensity = renderSettings.environmentIntensity;
    m_gp->baseExposure = renderSettings.exposure;
    m_gp->baseFog = renderSettings;
    m_gp->envSaved = true;
}

void GameWorld::endGameplay() {
    if (!m_gp) return;
    if (m_gp->envSaved) {
        renderSettings.environmentIntensity = m_gp->baseEnvIntensity;
        renderSettings.exposure = m_gp->baseExposure;
        const RenderSettings& b = m_gp->baseFog;
        renderSettings.fog = b.fog;
        renderSettings.fogDensity = b.fogDensity;
        renderSettings.fogHeightFalloff = b.fogHeightFalloff;
        renderSettings.fogColor = b.fogColor;
        renderSettings.wetness = b.wetness;
    }
    if (m_gp->rainVoice) m_mixer.stop(m_gp->rainVoice);
    for (EntityId e : m_gp->trafficCars)
        if (m_scene.isValid(e)) m_scene.destroyEntity(e);
    for (EntityId e : m_gp->trafficPeds)
        if (m_scene.isValid(e)) m_scene.destroyEntity(e);
    for (EntityId e : m_gp->police)
        if (m_scene.isValid(e)) m_scene.destroyEntity(e);
    for (EntityId e : m_gp->policeCars)
        if (m_scene.isValid(e)) m_scene.destroyEntity(e);
    m_gp.reset();
}

// ------------------------------------------------------- player + traffic --
void GameWorld::updateGameplay(float dt) {
    if (!m_gp) return;
    auto t0 = std::chrono::steady_clock::now();
    GameplayState& g = *m_gp;
    g.time += dt;
    script::InputMap& in = m_script.input();
    EntityId p = player();
    auto* pc = p ? m_scene.get<PlayerControllerComponent>(p) : nullptr;
    EntityId car = pc && !pc->vehicle.empty() ? m_scene.findByName(pc->vehicle) : kNullEntity;
    if (pc && !pc->vehicle.empty() && !car) pc->vehicle.clear();  // the car went away
    auto* health = p ? m_scene.get<HealthComponent>(p) : nullptr;
    bool dead = health && health->dead;

    if (pc && !dead) {
        // camera orbit input (mouse / right stick)
        g.camYaw -= std::clamp(in.axis("LookX") * pc->mouseSensitivity, -0.3f, 0.3f);  // clamp: no jumps from cursor warps
        g.camPitch = std::clamp(g.camPitch - std::clamp(in.axis("LookY") * pc->mouseSensitivity, -0.3f, 0.3f), -1.2f, 0.35f);
        auto* cc = m_scene.get<CharacterControllerComponent>(p);
        if (!car) {
            Vec3 fwd = forwardOfYaw(g.camYaw), right{-fwd.z, 0, fwd.x};
            Vec3 move = fwd * in.axis("MoveY") + right * in.axis("MoveX");
            float len = length(move);
            if (len > 1) move = move / len;
            float speed = in.action("Sprint", script::InputQuery::Down) ? pc->runSpeed : pc->walkSpeed;
            if (cc) {
                cc->move = move * speed;
                if (in.action("Jump", script::InputQuery::Pressed)) cc->jump = true;
            }
            if (len > 0.1f)
                if (auto* tc = m_scene.get<TransformComponent>(p)) {  // face the movement
                    float want = std::atan2(-move.x, -move.z), cur = yawOfQuat(tc->rotation);
                    float d = std::remainder(want - cur, 6.2831853f);
                    tc->rotation = Quat::fromAxisAngle({0, 1, 0}, cur + d * std::min(1.0f, dt * 12));
                }
        }
        // enter / exit vehicles (Interact)
        if (in.action("Interact", script::InputQuery::Pressed)) {
            if (car) {  // exit on the left side
                auto* vc = m_scene.get<VehicleComponent>(car);
                if (vc) {
                    vc->driver.clear();
                    vc->throttle = 0;
                    vc->steer = 0;
                    vc->brake = 1;
                    vc->handbrake = 1;
                }
                Transform ct = m_scene.worldTransform(car);
                Vec3 out = ct.position - ct.right() * 2.2f;
                out.y = ct.position.y + 0.3f;
                if (auto* tc = m_scene.get<TransformComponent>(p)) tc->position = out;
                if (cc) cc->enabled = true;
                if (auto* an = m_scene.get<AnimatorComponent>(p)) an->visible = true;
                pc->vehicle.clear();
                car = kNullEntity;
            } else {
                Vec3 pp = posOf(m_scene, p);
                EntityId best = kNullEntity;
                float bd = pc->enterRadius;
                for (EntityId e : m_scene.entities())
                    if (auto* vc = m_scene.get<VehicleComponent>(e); vc && vc->driver.empty() && vc->health > 0)
                        if (float d = length(posOf(m_scene, e) - pp); d < bd) {
                            bd = d;
                            best = e;
                        }
                if (!best && g.traffic) {  // carjack a traffic car
                    int bestId = -1;
                    for (auto& c : g.traffic->cars())
                        if (float d = length(c.position - pp); d < bd) {
                            bd = d;
                            bestId = c.id;
                        }
                    if (bestId > 0) {
                        const TrafficCar& tcar = g.traffic->cars()[static_cast<size_t>(bestId - 1)];
                        Json n = vehicleNode("car", tcar.position, tcar.yaw, std::format("Stolen Car {}", bestId), kCarColors[tcar.colorIndex % 8]);
                        best = spawnTree(m_scene, n);
                        g.traffic->respawnCar(bestId, pp);
                        commitCrime(static_cast<int>(Crime::VehicleTheft));
                        g.toast = "Carjacked!";
                        g.toastLeft = 2;
                    }
                }
                if (best) {
                    auto* vc = m_scene.get<VehicleComponent>(best);
                    vc->driver = m_scene.displayName(p);
                    vc->brake = 0;
                    vc->handbrake = 0;
                    pc->vehicle = m_scene.displayName(best);
                    if (cc) {
                        cc->enabled = false;
                        cc->move = {0, 0, 0};
                    }
                    if (auto* an = m_scene.get<AnimatorComponent>(p)) an->visible = false;
                    car = best;
                    g.camYaw = yawOfQuat(m_scene.worldTransform(best).rotation);
                }
            }
        }
        // driving
        if (car)
            if (auto* vc = m_scene.get<VehicleComponent>(car)) {
                vc->throttle = std::clamp(in.axis("MoveY"), -1.0f, 1.0f);
                vc->steer = std::clamp(in.axis("MoveX"), -1.0f, 1.0f);
                vc->handbrake = in.action("Jump", script::InputQuery::Down) ? 1.0f : 0.0f;
                vc->brake = 0;
                g.speedKmh = vc->speedKmh;
                Vec3 cp = posOf(m_scene, car);
                if (auto* tc = m_scene.get<TransformComponent>(p)) tc->position = cp + Vec3{0, 0.5f, 0};  // the player rides along
                if (g.traffic && std::fabs(vc->speedKmh) > 15) {
                    Transform ct = m_scene.worldTransform(car);
                    int hit = g.traffic->knockDown(ct.position + ct.forward() * 2.2f, 2.0f);
                    for (int i = 0; i < hit; ++i) commitCrime(static_cast<int>(Crime::HitPedestrian));
                }
            }
        if (!car) g.speedKmh = 0;
        // weapons
        auto* invc = m_scene.get<InventoryComponent>(p);
        if (invc && !car && !invc->weapon.empty() && in.action("Fire", script::InputQuery::Down)) {
            Inventory inv = loadInv(*invc);
            inv.lastShot = g.lastShot;
            if (inv.tryFire(g.time)) {
                g.lastShot = g.time;
                storeInv(*invc, inv);
                const WeaponDef* wd = weaponDef(inv.current);
                Vec3 origin = g.camPose.position, dir = g.camPose.rotation.rotate({0, 0, -1});
                if (!g.camInit) {
                    origin = posOf(m_scene, p) + Vec3{0, 1.5f, 0};
                    dir = forwardOfYaw(g.camYaw);
                }
                float range = wd ? wd->range : 50;
                float hitDist = range;
                EntityId hitEntity = kNullEntity;
                if (m_physics)
                    if (auto h = m_physics->raycast(origin, dir, range, p)) {
                        hitDist = h->distance;
                        hitEntity = h->entity;
                    }
                // people (no physics bodies): police / NPC entities with Health, then traffic pedestrians
                for (EntityId e : m_scene.entities()) {
                    auto* hc = m_scene.get<HealthComponent>(e);
                    if (!hc || hc->dead || e == p) continue;
                    Vec3 c = posOf(m_scene, e) + Vec3{0, 0.9f, 0};
                    float t = dot(c - origin, dir);
                    if (t < 0 || t > hitDist) continue;
                    Vec3 off = c - (origin + dir * t);
                    if (std::fabs(off.y) < 0.95f && std::sqrt(off.x * off.x + off.z * off.z) < 0.45f) {
                        hitDist = t;
                        hitEntity = e;
                    }
                }
                if (auto* hc = hitEntity ? m_scene.get<HealthComponent>(hitEntity) : nullptr) {
                    hc->current -= wd ? wd->damage * static_cast<float>(wd->pellets) * 0.6f : 20;
                    if (hc->current <= 0 && !hc->dead) {
                        hc->dead = true;
                        if (auto* an = m_scene.get<AnimatorComponent>(hitEntity)) an->ragdoll = true;
                        commitCrime(static_cast<int>(hc->faction == "police" ? Crime::KillPolice : Crime::KillCivilian));
                    } else {
                        commitCrime(static_cast<int>(Crime::Assault));
                    }
                } else if (g.traffic) {
                    int ped = g.traffic->pedestrianOnRay(origin, dir, hitDist);
                    if (ped >= 0 && g.traffic->knockDownIndex(ped)) commitCrime(static_cast<int>(Crime::KillCivilian));
                }
                if (g.time - g.lastShotCrime > 2) {
                    commitCrime(static_cast<int>(Crime::Shooting));
                    g.lastShotCrime = g.time;
                }
                if (auto clip = audioClip("builtin:shot")) {
                    audio::VoiceParams vp;
                    vp.spatial = true;
                    vp.position = origin;
                    vp.minDistance = 5;
                    vp.maxDistance = 200;
                    m_mixer.play(clip, vp);
                }
            }
        }
    }
    // death / respawn
    if (pc && health) {
        if (health->current <= 0 && !health->dead) {
            health->dead = true;
            g.deadTimer = 4;
            showMessage("WASTED", 4, {0.85f, 0.15f, 0.1f});
        }
        if (health->dead) {
            g.deadTimer -= dt;
            if (g.deadTimer <= 0) {
                health->dead = false;
                health->current = health->max;
                if (auto* tc = m_scene.get<TransformComponent>(p)) tc->position = pc->spawnPoint;
                if (auto* cc = m_scene.get<CharacterControllerComponent>(p)) cc->enabled = true;
                if (auto* an = m_scene.get<AnimatorComponent>(p)) an->visible = true;
                if (auto* inv = m_scene.get<InventoryComponent>(p)) inv->money = std::max(0, inv->money - 100);
                pc->vehicle.clear();
                g.wanted.setLevel(0);
            }
        }
    }
    // traffic
    if (g.traffic) {
        Vec3 focus = p ? posOf(m_scene, p) : Vec3{0, 0, 0};
        struct Obstacle {
            Vec3 pos;
            float along, halfWidth;
        };
        std::vector<Obstacle> obstacles;
        for (EntityId e : m_scene.entities())
            if (m_scene.get<VehicleComponent>(e)) {
                auto* col = m_scene.get<ColliderComponent>(e);
                Vec3 half = col ? col->size * 0.5f : Vec3{0.9f, 0.5f, 2.2f};
                obstacles.push_back({posOf(m_scene, e), half.z, half.x});
            }
        if (p && !car) obstacles.push_back({focus, 0.4f, 0.4f});
        g.traffic->step(dt, focus, [&](Vec3 pos, Vec3 dir, float range) {
            float best = 1e9f;
            for (auto& o : obstacles) {
                Vec3 d = o.pos - pos;
                float t = dot(d, dir);
                if (t < 0 || t > range) continue;
                Vec3 lat = d - dir * t;
                lat.y = 0;
                if (length(lat) < o.halfWidth + 1.0f) best = std::min(best, t - o.along);  // 1.0 = traffic car half width + margin
            }
            return best;
        });
        auto& cars = g.traffic->cars();
        for (size_t i = 0; i < cars.size() && i < g.trafficCars.size(); ++i)
            if (auto* tc = m_scene.get<TransformComponent>(g.trafficCars[i])) {
                tc->position = cars[i].position + Vec3{0, 0.75f, 0};
                tc->rotation = Quat::fromAxisAngle({0, 1, 0}, cars[i].yaw);
            }
        auto& peds = g.traffic->pedestrians();
        for (size_t i = 0; i < peds.size() && i < g.trafficPeds.size(); ++i)
            if (auto* tc = m_scene.get<TransformComponent>(g.trafficPeds[i])) {
                tc->position = peds[i].position + Vec3{0, peds[i].down ? 0.25f : 0.2f, 0};
                Quat r = Quat::fromAxisAngle({0, 1, 0}, peds[i].yaw);
                tc->rotation = peds[i].down ? r * Quat::fromAxisAngle({1, 0, 0}, -1.5707963f) : r;
                if (auto* an = m_scene.get<AnimatorComponent>(g.trafficPeds[i])) an->params["speed"] = peds[i].down ? 0.0f : peds[i].speed;
            }
    }
    // pickups
    if (p) {
        Vec3 pp = posOf(m_scene, p);
        std::vector<EntityId> taken;
        for (EntityId e : m_scene.entities()) {
            auto* pk = m_scene.get<PickupComponent>(e);
            if (!pk || g.pickupRespawn.count(e)) continue;
            if (length(posOf(m_scene, e) - pp) > pk->radius + (car ? 1.5f : 0.0f)) continue;
            if (auto* invc = m_scene.get<InventoryComponent>(p)) {
                Inventory inv = loadInv(*invc);
                inv.money += pk->money;
                inv.add(pk->item, pk->item.empty() ? 0 : pk->count);
                if (!pk->weapon.empty()) {
                    inv.giveWeapon(pk->weapon, pk->ammo);
                    inv.current = pk->weapon;
                }
                storeInv(*invc, inv);
                g.toast = pk->money ? std::format("+${}", pk->money) : !pk->weapon.empty() ? pk->weapon : pk->item;
                g.toastLeft = 2;
                if (auto clip = audioClip("builtin:beep")) m_mixer.play(clip, {});
            }
            taken.push_back(e);
        }
        for (EntityId e : taken) {
            auto* pk = m_scene.get<PickupComponent>(e);
            if (pk->respawnSeconds > 0) {
                g.pickupRespawn[e] = pk->respawnSeconds;
                if (auto* tc = m_scene.get<TransformComponent>(e)) tc->position.y -= 1000;  // hidden until it respawns
            } else {
                m_scene.destroyEntity(e);
            }
        }
        for (auto it = g.pickupRespawn.begin(); it != g.pickupRespawn.end();) {
            it->second -= dt;
            if (it->second <= 0) {
                if (auto* tc = m_scene.get<TransformComponent>(it->first)) tc->position.y += 1000;
                it = g.pickupRespawn.erase(it);
            } else {
                ++it;
            }
        }
    }
    // police + NavAgents
    auto* wc = p ? m_scene.get<WantedComponent>(p) : nullptr;
    int stars = wc && wc->enabled ? g.wanted.level : 0;
    std::erase_if(g.police, [&](EntityId e) { return !m_scene.isValid(e); });
    Vec3 pp = p ? posOf(m_scene, p) : Vec3{0, 0, 0};
    if (p) {  // player speed (aiming penalty for shooters)
        if (g.hasLastPlayerPos && dt > 0) g.playerSpeed = length(pp - g.lastPlayerPos) / dt;
        g.lastPlayerPos = pp;
        g.hasLastPlayerPos = true;
    }
    PoliceResponse response = wc && wc->enabled ? policeResponse(stars, wc->maxPolice) : PoliceResponse{};
    auto spawnOfficer = [&](Vec3 at) {
        Json n = {{"name", std::format("Police {}", ++g.policeSerial)},
                  {"components",
                   {{"Transform", {{"position", {at.x, at.y + 0.1f, at.z}}}},
                    {"Animator", {{"character", "builtin:mannequin"}, {"rootMotion", false}, {"color", {0.12f, 0.2f, 0.55f}}}},
                    {"Health", {{"max", 100}, {"current", 100}, {"faction", "police"}}},
                    {"NavAgent", {{"speed", 4.2f}, {"target", p ? m_scene.displayName(p) : ""}, {"arriveRadius", 1.2f}}},
                    {"Tags", {{"tags", kPoliceTag}}}}}};
        g.police.push_back(spawnTree(m_scene, n));
    };
    int onFoot = 0;  // spawned on foot (officers out of cruisers come on top)
    for (EntityId e : g.police)
        if (!hasTag(m_scene, e, "police-deployed")) ++onFoot;
    g.policeSpawnCooldown -= dt;
    if (onFoot < response.onFoot && g.policeSpawnCooldown <= 0) {
        g.policeSpawnCooldown = 1.0f;
        if (NavMesh* nav = navMesh()) {
            for (int attempt = 0; attempt < 30; ++attempt) {
                auto rp = nav->randomPoint(static_cast<uint32_t>(g.time * 1000) + static_cast<uint32_t>(attempt) * 7919u);
                if (!rp) break;
                float d = length(*rp - pp);
                if (d < 30 || d > 90) continue;
                spawnOfficer(*rp);
                break;
            }
        }
    }
    if (stars == 0)  // stand down: officers far away leave
        for (EntityId e : g.police)
            if (length(posOf(m_scene, e) - pp) > 35 && m_scene.isValid(e)) m_scene.destroyEntity(e);
    std::erase_if(g.police, [&](EntityId e) { return !m_scene.isValid(e); });
    // cruisers: spawn on the road graph, chase along the roads, officers get out next to an on-foot player
    updatePoliceCars(dt, stars > 0 ? response.cars : 0, p, car);
    for (auto& [e, cr] : g.cruisers) {
        if (cr.deployed || !m_scene.isValid(e) || !p || car || stars == 0) continue;
        auto* vc = m_scene.get<VehicleComponent>(e);
        Transform ct = m_scene.worldTransform(e);
        if (!vc || length(ct.position - pp) > 14 || std::fabs(vc->speedKmh) > 8) continue;
        cr.deployed = true;
        vc->driver.clear();  // parked and empty: the player can take it
        for (float side : {-1.0f, 1.0f}) {
            Vec3 at = ct.position + ct.right() * (2.2f * side);
            at.y = pp.y;
            spawnOfficer(at);
            if (auto* t = m_scene.get<TagsComponent>(g.police.back())) t->tags += " police-deployed";
        }
    }
    const ShooterParams shooterParams;
    for (EntityId e : m_scene.entities()) {
        auto* na = m_scene.get<NavAgentComponent>(e);
        if (!na) continue;
        if (auto* hc = m_scene.get<HealthComponent>(e); hc && hc->dead) continue;
        GameplayState::Agent& ag = g.agents[e];
        ag.repath -= dt;
        Vec3 pos = posOf(m_scene, e);
        std::optional<Vec3> goal;
        if (!na->target.empty())
            if (EntityId t = m_scene.findByName(na->target)) goal = posOf(m_scene, t);
        if (!goal && na->hasDestination) goal = na->destination;
        NavMesh* nav = navMesh();
        if (!goal && na->wander && nav && (ag.path.empty() || ag.next >= ag.path.size()))
            if (auto rp = nav->randomPoint(ag.rng = ag.rng * 1664525u + 1013904223u + e)) goal = rp;
        if (!goal) continue;
        na->arrived = length(*goal - pos) <= na->arriveRadius;
        if (nav && (ag.repath <= 0 || ag.next >= ag.path.size())) {
            ag.path = nav->findPath(pos, *goal);
            ag.next = 1;
            ag.repath = 1.0f;
        }
        // armed police (3+ stars): shoot with line of sight; hold position once in comfortable range
        bool hold = false;
        if (response.armed && p && health && !health->dead && hasTag(m_scene, e, kPoliceTag)) {
            if (!ag.shooterSeeded) {
                ag.shooter.rng = e * 2654435761u + 0x9e3779b9u;
                ag.shooterSeeded = true;
            }
            Vec3 eye = pos + Vec3{0, 1.5f, 0}, aimAt = pp + Vec3{0, car ? 0.6f : 1.2f, 0};
            float d = length(aimAt - eye);
            bool los = d <= shooterParams.range && lineOfSight(eye, aimAt, e, p, car);
            ShotResult shot = updateShooter(ag.shooter, shooterParams, dt, los, d, g.playerSpeed);
            hold = los && d < shooterParams.range * 0.6f;
            if (shot.fired) {
                ++g.shotsAtPlayer;
                if (auto clip = audioClip("builtin:shot")) {
                    audio::VoiceParams vp;
                    vp.spatial = true;
                    vp.position = eye;
                    vp.minDistance = 5;
                    vp.maxDistance = 200;
                    vp.volume = 0.8f;
                    m_mixer.play(clip, vp);
                }
            }
            if (shot.hit) {
                ++g.hitsOnPlayer;
                if (auto* vc = car ? m_scene.get<VehicleComponent>(car) : nullptr) {  // the car soaks most of it
                    vc->health = std::max(0.0f, vc->health - shot.damage);
                    health->current -= shot.damage * 0.25f;
                } else {
                    health->current -= shot.damage;
                }
            }
            if (hold)
                if (auto* tc = m_scene.get<TransformComponent>(e)) {  // face the player while shooting
                    Vec3 d2 = pp - pos;
                    tc->rotation = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-d2.x, -d2.z));
                }
        }
        Vec3 step{0, 0, 0};
        if (!na->arrived && !hold) {
            Vec3 target = ag.next < ag.path.size() ? ag.path[ag.next] : *goal;
            Vec3 d = target - pos;
            d.y = 0;
            if (length(d) < 0.4f && ag.next < ag.path.size()) ++ag.next;
            else if (length(d) > 1e-3f) step = normalize(d) * std::min(na->speed * dt, length(d));
        }
        if (auto* tc = m_scene.get<TransformComponent>(e)) {
            tc->position += step;
            if (ag.next < ag.path.size() + 1 && length(step) > 1e-4f) tc->position.y = (ag.next < ag.path.size() ? ag.path[ag.next].y : tc->position.y) + 0.05f;
            if (length(step) > 1e-4f) tc->rotation = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-step.x, -step.z));
        }
        if (auto* an = m_scene.get<AnimatorComponent>(e)) an->params["speed"] = length(step) / std::max(dt, 1e-4f);
        // police arrest an on-foot player who stays within reach
        if (hasTag(m_scene, e, kPoliceTag) && pc && !car && health && !health->dead && stars > 0) {
            if (length(pos - pp) < 1.8f) ag.arrestTimer += dt;
            else ag.arrestTimer = std::max(0.0f, ag.arrestTimer - dt);
            if (ag.arrestTimer > 2) {
                showMessage("BUSTED", 4, {0.3f, 0.5f, 1.0f});
                g.wanted.setLevel(0);
                if (auto* inv = m_scene.get<InventoryComponent>(p)) inv->money = std::max(0, inv->money - 200);
                if (auto* tc = m_scene.get<TransformComponent>(p)) tc->position = pc->spawnPoint;
                for (auto& [me, r] : g.missions)
                    if (r.state() == MissionState::Running) r.stop();
                ag.arrestTimer = 0;
            }
        }
    }
    g.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------------------------ police --
bool GameWorld::lineOfSight(Vec3 from, Vec3 to, EntityId ignore, EntityId target, EntityId targetVehicle) {
    if (!m_physics) return true;  // no colliders: nothing can block
    Vec3 d = to - from;
    float len = length(d);
    if (len < 1e-3f) return true;
    auto h = m_physics->raycast(from, d / len, len, ignore);
    if (!h || h->distance >= len - 0.75f) return true;
    if (h->entity == target || (targetVehicle && h->entity == targetVehicle)) return true;
    return hasTag(m_scene, h->entity, kPoliceTag);  // other police cars / officers do not hide the player
}

void GameWorld::updatePoliceCars(float dt, int wantedCars, EntityId p, EntityId playerCar) {
    GameplayState& g = *m_gp;
    std::erase_if(g.policeCars, [&](EntityId e) { return !m_scene.isValid(e) || !m_scene.get<VehicleComponent>(e); });
    for (auto it = g.cruisers.begin(); it != g.cruisers.end();)
        it = std::find(g.policeCars.begin(), g.policeCars.end(), it->first) != g.policeCars.end() ? std::next(it) : g.cruisers.erase(it);
    if (!p) return;
    Vec3 target = posOf(m_scene, playerCar ? playerCar : p);
    const world::RoadGraph* roads = roadGraph();
    // spawn: a road node 60..160 m away, facing the player (needs roads and running physics for the car body)
    g.policeCarCooldown -= dt;
    if (static_cast<int>(g.policeCars.size()) < wantedCars && g.policeCarCooldown <= 0 && m_physics && roads && !roads->nodes.empty()) {
        g.policeCarCooldown = 2.5f;
        size_t n = roads->nodes.size(), start = static_cast<size_t>(g.time * 7.0f) % n;
        for (size_t k = 0; k < n; ++k) {
            Vec3 at = roads->nodes[(start + k) % n].position;
            float d = length(at - target);
            if (d < 60 || d > 160) continue;
            bool blocked = false;  // keep spawn points clear of other vehicles
            for (EntityId e : m_scene.entities())
                if (m_scene.get<VehicleComponent>(e) && length(posOf(m_scene, e) - at) < 6) blocked = true;
            if (blocked) continue;
            Vec3 to = target - at;
            Json car = vehicleNode("car", at, std::atan2(-to.x, -to.z), std::format("Police Car {}", ++g.policeCarSerial), {0.06f, 0.08f, 0.2f});
            car["components"]["Tags"]["tags"] = std::string("vehicle ") + kPoliceTag + " police-car";
            car["components"]["Vehicle"]["driver"] = "Police";
            car["components"]["Vehicle"]["maxEngineTorque"] = 650;  // a little more punch than a civilian car
            car["children"].push_back({{"name", "Lightbar"},
                                       {"components",
                                        {{"Transform", {{"position", {0.0f, 0.75f, 0.2f}}, {"scale", {1.2f, 0.12f, 0.25f}}}},
                                         {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", {0.9f, 0.1f, 0.1f}}}}}},
                                       {"children", Json::array()}});
            EntityId e = spawnTree(m_scene, car);
            g.policeCars.push_back(e);
            g.cruisers[e] = {};
            break;
        }
    }
    const PursuitParams params;
    for (EntityId e : g.policeCars) {
        if (e == playerCar) continue;  // stolen: the player drives it
        auto* vc = m_scene.get<VehicleComponent>(e);
        GameplayState::Cruiser& cr = g.cruisers[e];
        Transform ct = m_scene.worldTransform(e);
        if (wantedCars == 0 || cr.deployed) {  // stand down / officers out: parked
            vc->throttle = 0;
            vc->steer = 0;
            vc->brake = 1;
            vc->handbrake = 1;
            continue;
        }
        cr.repath -= dt;
        if (cr.repath <= 0 || cr.route.empty()) {
            cr.route = roads ? roadRoute(*roads, ct.position, target) : std::vector<Vec3>{ct.position, target};
            cr.pursuit.next = 0;
            cr.repath = 1.0f;
        }
        DriveCommand cmd = pursuitDrive(cr.pursuit, params, cr.route, ct.position, yawOfQuat(ct.rotation), vc->speedKmh / 3.6f, target, dt);
        vc->throttle = cmd.throttle;
        vc->steer = cmd.steer;
        vc->brake = cmd.brake;
        vc->handbrake = cmd.handbrake;
    }
    if (wantedCars == 0)  // stand down: cruisers out of sight go home (a stolen one stays the player's)
        for (EntityId e : g.policeCars)
            if (e != playerCar && length(posOf(m_scene, e) - target) > 60) m_scene.destroyEntity(e);
    std::erase_if(g.policeCars, [&](EntityId e) { return !m_scene.isValid(e); });
}

// ------------------------------------------------- camera, missions, world --
void GameWorld::lateGameplay(float dt) {
    if (!m_gp) return;
    GameplayState& g = *m_gp;
    EntityId p = player();
    auto* pc = p ? m_scene.get<PlayerControllerComponent>(p) : nullptr;
    EntityId car = pc && !pc->vehicle.empty() ? m_scene.findByName(pc->vehicle) : kNullEntity;
    auto collide = [this](EntityId ignore) {
        return [this, ignore](Vec3 from, Vec3 dir, float maxD) -> std::optional<float> {
            if (!m_physics) return std::nullopt;
            if (auto h = m_physics->raycast(from, dir, maxD, ignore)) return h->distance;
            return std::nullopt;
        };
    };
    // sequences (cutscenes) own their camera while playing
    std::set<EntityId> sequenced;
    for (EntityId e : m_scene.entities())
        if (auto* sq = m_scene.get<SequenceComponent>(e)) {
            if (sq->play || (sq->playOnStart && !g.seqTime.count(e))) {
                g.seqTime[e] = 0;
                sq->playing = true;
                sq->play = false;
            }
            if (!sq->playing) continue;
            Sequence s = Sequence::fromJson({{"keys", sq->keys}});
            float& t = g.seqTime[e];
            t += dt;
            if (t > s.duration()) {
                if (sq->loop && s.duration() > 0) t = std::fmod(t, s.duration());
                else sq->playing = false;
            }
            sq->time = t;
            CameraPose pose = s.sample(t);
            if (auto* tc = m_scene.get<TransformComponent>(e)) {
                tc->position = pose.position;
                tc->rotation = pose.rotation;
            }
            if (auto* cam = m_scene.get<CameraComponent>(e)) cam->fovDegrees = pose.fovDegrees;
            if (sq->playing) sequenced.insert(e);
        }
    // player camera
    if (pc) {
        EntityId cam = pc->camera.empty() ? kNullEntity : m_scene.findByName(pc->camera);
        if (!cam)
            for (EntityId e : m_scene.entities())
                if (auto* c = m_scene.get<CameraComponent>(e); c && c->primary) cam = e;
        if (cam && !sequenced.count(cam)) {
            RigParams rp;
            rp.mode = RigMode::Orbit;
            rp.yaw = g.camYaw;
            rp.pitch = g.camPitch;
            rp.distance = car ? pc->cameraDistance * 1.6f : pc->cameraDistance;
            rp.height = pc->cameraHeight;
            rp.lookHeight = car ? 1.2f : 1.5f;
            Vec3 target = car ? posOf(m_scene, car) : posOf(m_scene, p);
            CameraPose want = rigPose(rp, target, 0, collide(car ? car : p));
            g.camPose = g.camInit ? smoothPose(g.camPose, want, 14, dt) : want;
            g.camInit = true;
            EntityId parent = m_scene.parentOf(cam);
            Transform world{g.camPose.position, g.camPose.rotation, {1, 1, 1}};
            if (parent) m_scene.setWorldTransform(cam, world);
            else if (auto* tc = m_scene.get<TransformComponent>(cam)) {
                tc->position = world.position;
                tc->rotation = world.rotation;
            }
        }
    }
    // camera rigs (non-player cameras)
    for (EntityId e : m_scene.entities()) {
        auto* rig = m_scene.get<CameraRigComponent>(e);
        if (!rig || sequenced.count(e) || rig->target.empty()) continue;
        EntityId t = m_scene.findByName(rig->target);
        if (!t) continue;
        RigParams rp;
        rp.mode = static_cast<RigMode>(rig->mode);
        rp.distance = rig->distance;
        rp.height = rig->height;
        rp.lookHeight = rig->lookHeight;
        Transform tw = m_scene.worldTransform(t);
        CameraPose want = rigPose(rp, tw.position, yawOfQuat(tw.rotation), rig->collision ? collide(t) : CameraCollision{});
        CameraPose& cur = g.rigPoses.try_emplace(e, want).first->second;
        cur = smoothPose(cur, want, rig->smoothing, dt);
        if (auto* tc = m_scene.get<TransformComponent>(e)) {
            tc->position = cur.position;
            tc->rotation = cur.rotation;
        }
    }
    // missions
    MissionContext ctx;
    ctx.positionOf = [this](const std::string& n) -> std::optional<Vec3> {
        EntityId e = m_scene.findByName(n);
        if (!e) return std::nullopt;
        return posOf(m_scene, e);
    };
    ctx.playerPosition = [this, p] { return p ? posOf(m_scene, p) : Vec3{0, 0, 0}; };
    ctx.playerVehicle = [pc] { return pc ? pc->vehicle : std::string(); };
    ctx.itemCount = [this, p](const std::string& item) {
        auto* inv = p ? m_scene.get<InventoryComponent>(p) : nullptr;
        return inv && inv->items.is_object() && inv->items.contains(item) && inv->items[item].is_number() ? inv->items[item].get<int>() : 0;
    };
    ctx.isDead = [this](const std::string& n) {
        EntityId e = m_scene.findByName(n);
        if (!e) return true;
        auto* hc = m_scene.get<HealthComponent>(e);
        return hc && hc->dead;
    };
    ctx.wantedLevel = [&g] { return g.wanted.level; };
    ctx.playerDead = [this, p] {
        auto* hc = p ? m_scene.get<HealthComponent>(p) : nullptr;
        return hc && hc->dead;
    };
    ctx.setWanted = [&g](int level) { g.wanted.setLevel(level); };
    ctx.reward = [this, p](int money, const std::string& item, int count) {
        auto* invc = p ? m_scene.get<InventoryComponent>(p) : nullptr;
        if (!invc) return;
        Inventory inv = loadInv(*invc);
        inv.money += money;
        inv.add(item, count);
        storeInv(*invc, inv);
    };
    for (EntityId e : m_scene.entities()) {
        auto* mc = m_scene.get<MissionComponent>(e);
        if (!mc) continue;
        MissionRunner& r = g.missions[e];
        if (mc->start) {
            r.start(MissionGraph::fromJson(mc->graph));
            mc->start = false;
        }
        MissionState before = r.state();
        r.update(dt, ctx);
        mc->state = toString(r.state());
        mc->objective = r.objective();
        if (before == MissionState::Running && r.state() == MissionState::Succeeded) showMessage("MISSION PASSED", 4, {1.0f, 0.8f, 0.2f});
        if (before == MissionState::Running && r.state() == MissionState::Failed) showMessage("MISSION FAILED", 4, {0.9f, 0.2f, 0.15f});
    }
    // wanted: seen when an officer or cruiser within 60 m has a clear line of sight
    if (auto* wc = p ? m_scene.get<WantedComponent>(p) : nullptr) {
        if (wc->setLevel >= 0) {
            g.wanted.setLevel(wc->setLevel);
            wc->setLevel = -1;
        }
        bool seen = false;
        Vec3 pp = posOf(m_scene, p);
        Vec3 eyeTarget = pp + Vec3{0, 1.2f, 0};
        for (EntityId e : g.police)
            if (!seen && m_scene.isValid(e) && length(posOf(m_scene, e) - pp) < 60)
                seen = lineOfSight(posOf(m_scene, e) + Vec3{0, 1.6f, 0}, eyeTarget, e, p, car);
        for (EntityId e : g.policeCars)
            if (!seen && e != car && m_scene.isValid(e) && length(posOf(m_scene, e) - pp) < 60)
                seen = lineOfSight(posOf(m_scene, e) + Vec3{0, 1.4f, 0}, eyeTarget, e, p, car);
        int before = g.wanted.level;
        g.wanted.update(dt, seen);
        if (before > 0 && g.wanted.level == 0) {
            g.toast = "Lost the cops";
            g.toastLeft = 3;
        }
        wc->level = g.wanted.level;
        wc->heat = g.wanted.heat;
    }
    // time of day + weather
    for (EntityId e : m_scene.entities()) {
        auto* tod = m_scene.get<TimeOfDayComponent>(e);
        if (!tod) continue;
        if (tod->dayMinutes > 0) tod->hour = std::fmod(tod->hour + dt * 24.0f / (tod->dayMinutes * 60.0f), 24.0f);
        SkyLight sky = skyAt(tod->hour, tod->latitude, 172);
        int preset = 0;
        float transition = 10;
        for (EntityId w : m_scene.entities())
            if (auto* wc = m_scene.get<WeatherComponent>(w)) {
                preset = static_cast<int>(wc->preset);
                transition = wc->transition;
            }
        if (preset != g.weatherPreset) {
            g.weatherFrom = g.weatherPreset < 0 ? weatherPreset(static_cast<WeatherKind>(preset)) : g.weatherNow;
            g.weatherTo = weatherPreset(static_cast<WeatherKind>(preset));
            g.weatherT = g.weatherPreset < 0 || transition <= 0 ? 1.0f : 0.0f;
            g.weatherPreset = preset;
        }
        g.weatherT = std::min(1.0f, g.weatherT + (transition > 0 ? dt / transition : 1.0f));
        g.weatherNow = blend(g.weatherFrom, g.weatherTo, g.weatherT);
        sky = applyWeather(sky, g.weatherNow);
        EntityId sun = tod->sun.empty() ? kNullEntity : m_scene.findByName(tod->sun);
        if (!sun)
            for (EntityId l : m_scene.entities())
                if (auto* lc = m_scene.get<LightComponent>(l); lc && lc->type == LightType::Directional) {
                    sun = l;
                    break;
                }
        if (auto* lc = sun ? m_scene.get<LightComponent>(sun) : nullptr) {
            lc->color = sky.color;
            lc->intensity = sky.intensity;
            if (auto* tc = m_scene.get<TransformComponent>(sun)) tc->rotation = lookRotation({0, 0, 0}, sky.lightDirection);
        }
        renderSettings.environmentIntensity = g.baseEnvIntensity * sky.environment;
        renderSettings.exposure = g.baseExposure + sky.exposure;
        g.skyBrightness = g.baseEnvIntensity * sky.environment;
        // fog: weather haze / rain on top of the project's own fog (the denser wins, colour and falloff blend by density)
        WeatherFog wf = weatherFog(g.weatherNow, sky);
        const RenderSettings& bf = g.baseFog;
        float baseDensity = bf.fog ? bf.fogDensity : 0.0f;
        renderSettings.fog = baseDensity + wf.density > 1e-5f;
        if (renderSettings.fog) {
            float t = wf.density / (baseDensity + wf.density);
            renderSettings.fogDensity = std::max(baseDensity, wf.density);
            renderSettings.fogHeightFalloff = bf.fog ? bf.fogHeightFalloff + (wf.heightFalloff - bf.fogHeightFalloff) * t : wf.heightFalloff;
            renderSettings.fogColor = lerp(bf.fogColor * sky.environment, wf.color * g.baseEnvIntensity, t);
        }
        // wet surfaces: the ground soaks while it rains and dries slowly afterwards (wetnessStep(), docs/GRAPHICS.md)
        if (g.wetness < 0) g.wetness = g.baseFog.wetness;
        g.wetness = wetnessStep(g.wetness, g.weatherNow.rain, dt);
        renderSettings.wetness = std::max(g.baseFog.wetness, g.wetness);
        // rain sound
        if (g.weatherNow.rain > 0.02f) {
            audio::VoiceParams vp;
            vp.bus = audio::Bus::Ambient;
            vp.loop = true;
            vp.volume = g.weatherNow.rain * 0.8f;
            if (!g.rainVoice || !m_mixer.isPlaying(g.rainVoice)) {
                if (auto clip = audioClip("builtin:rain")) g.rainVoice = m_mixer.play(clip, vp);
            } else {
                m_mixer.setParams(g.rainVoice, vp);
            }
        } else if (g.rainVoice) {
            m_mixer.stop(g.rainVoice);
            g.rainVoice = 0;
        }
        break;
    }
    // particles: weather rain around the viewer, ParticleEmitter components (wind from the weather)
    if (m_lastRebase != Vec3{0, 0, 0}) {  // floating origin moved the world last tick: move world-space particles too
        g.rain.shift(m_lastRebase * -1.0f);
        for (auto& [e, em] : g.emitters)
            if (auto* pc = m_scene.get<ParticleEmitterComponent>(e); pc && emitterParams(*pc).worldSpace) em.shift(m_lastRebase * -1.0f);
    }
    if (g.weatherNow.rain > 0.001f || g.rain.size() > 0)
        if (auto viewer = streamingViewer()) g.rain.update(dt, g.weatherNow, *viewer);
    Vec3 wind = Precipitation::windVelocity(g.weatherNow);
    for (auto it = g.emitters.begin(); it != g.emitters.end();)  // destroyed entities / removed components
        it = m_scene.isValid(it->first) && m_scene.get<ParticleEmitterComponent>(it->first) ? std::next(it) : g.emitters.erase(it);
    for (EntityId e : m_scene.entities()) {
        auto* pc = m_scene.get<ParticleEmitterComponent>(e);
        if (!pc) continue;
        auto [it, fresh] = g.emitters.try_emplace(e, ParticleEmitter(static_cast<uint32_t>(e) * 2654435761u + 1));
        EmitterParams ep = emitterParams(*pc);
        Mat4 world = m_scene.worldMatrix(e);
        if (pc->burst > 0) {
            it->second.burst(pc->burst, ep, world);
            pc->burst = 0;
        }
        if (!pc->enabled) ep.rate = 0;  // disabled: live particles finish, no new ones
        it->second.update(dt, ep, world, wind);
        pc->count = static_cast<int>(it->second.size());
    }
    if (g.bigMessageLeft > 0) g.bigMessageLeft -= dt;
    if (g.toastLeft > 0) g.toastLeft -= dt;
    // quick save / load
    if (!saveDirectory.empty()) {
        script::InputMap& in = m_script.input();
        if (in.action("QuickSave", script::InputQuery::Pressed)) {
            auto st = saveGame(saveDirectory / "quicksave.json");
            g.toast = st ? "Game saved" : "Save failed: " + st.error;
            g.toastLeft = 2;
        }
        if (in.action("QuickLoad", script::InputQuery::Pressed)) {
            auto st = loadGame(saveDirectory / "quicksave.json");
            g.toast = st ? "Game loaded" : "Load failed: " + st.error;
            g.toastLeft = 2;
        }
    }
}

// -------------------------------------------------------------- UI: text --
gameplay::Localization& GameWorld::localization() {
    if (!m_loc) m_loc = std::make_unique<Localization>();
    return *m_loc;
}

void GameWorld::reloadLocalization() {
    Localization& loc = localization();
    loc.clear();
    for (std::string f : m_vfs.list("content")) {  // Content/Localization/<language>.json
        if (startsWith(f, "content:/")) f = f.substr(9);
        if (!startsWith(f, "Localization/") || f.size() < 6 || f.substr(f.size() - 5) != ".json") continue;
        auto text = m_vfs.readText("content:/" + f);
        Json j = text ? Json::parse(*text, nullptr, false) : Json();
        if (!j.is_object()) {
            FLEDGE_LOG_WARN("ui", "Localization file '{}' is not a JSON object", f);
            continue;
        }
        std::string lang = f.substr(13, f.size() - 13 - 5);
        loc.addTable(lang, j);
    }
    loc.setLanguage(uiSettings.language.empty() ? "en" : uiSettings.language);
    loc.setFallback(uiSettings.fallbackLanguage.empty() ? "en" : uiSettings.fallbackLanguage);
}

const gameplay::Font* GameWorld::uiFont() {
    std::string key = uiSettings.font + "@" + std::to_string(uiSettings.fontSize);
    if (key != m_uiFontKey) {  // (re)load when the setting changes
        m_uiFontKey = key;
        m_uiFont.reset();
        if (m_renderer && m_uiFontTexture) {
            m_renderer->setOverlayTexture({});
            m_renderer->device().destroyTexture(m_uiFontTexture);
        }
        m_uiFontTexture = {};
        std::string err = "file not found";
        if (!uiSettings.font.empty()) {
            std::string path = m_registry.resolve(uiSettings.font);
            if (path.empty()) path = startsWith(uiSettings.font, "content:/") ? uiSettings.font : "content:/" + uiSettings.font;
            if (auto bytes = m_vfs.read(path)) m_uiFont = Font::load(std::move(*bytes), uiSettings.fontSize, &err);
            if (!m_uiFont) FLEDGE_LOG_WARN("ui", "UI font '{}': {} - using bundled Roboto", uiSettings.font, err);
        }
        if (!m_uiFont) {
            auto bundled = enginePaths().resources / "fonts" / "Roboto-Medium.ttf";
            if (auto bytes = readFileBytes(bundled, &err)) m_uiFont = Font::load(std::move(*bytes), uiSettings.fontSize, &err);
            if (!m_uiFont) FLEDGE_LOG_WARN("ui", "Bundled Roboto '{}': {} - using the built-in pixel font", bundled.string(), err);
        }
    }
    if (m_uiFont && m_renderer && !m_uiFontTexture) {  // the atlas becomes the overlay texture
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA8;
        d.width = m_uiFont->atlasWidth();
        d.height = m_uiFont->atlasHeight();
        d.sampler = {rhi::Filter::Linear, false, rhi::Wrap::Clamp, false};
        std::vector<uint8_t> rgba = m_uiFont->atlasRgba();
        rhi::TextureSubresource sub{0, 0, d.width, d.height, rgba.data(), rgba.size()};
        m_uiFontTexture = m_renderer->device().createTexture(d, std::span(&sub, 1));
        m_renderer->setOverlayTexture(m_uiFontTexture);
    }
    // without a renderer (tests, headless tools) the layout still uses the font; with one, only once it is uploaded
    return m_uiFont && (!m_renderer || m_uiFontTexture) ? m_uiFont.get() : nullptr;
}

std::string GameWorld::uiStateJson() {
    Json weapons = Json::array();
    Json primary = Json::object();
    if (const ComponentTypeInfo* info = ComponentRegistry::get().find("Weapon")) {
        EntityId p = player();
        for (EntityId e : m_scene.entities()) {
            void* c = m_scene.getComponent(e, "Weapon");
            if (!c) continue;
            Json w = info->type.toJson(c);
            Json row = {{"entity", m_scene.displayName(e)},
                        {"ammo", w.value("ammo", 0)},
                        {"reserve", w.value("reserve", 0)},
                        {"state", w.value("state", std::string())},
                        {"fireMode", w.value("fireMode", std::string())},
                        {"ads", w.value("adsT", 0.0f)},
                        {"definition", w.value("definition", std::string())}};
            weapons.push_back(row);
            std::string owner = w.value("owner", std::string());
            bool mine = owner.empty() || (p && (e == p || owner == m_scene.displayName(p)));
            std::string st = row.value("state", std::string());
            if (primary.empty() && mine && st != "Holstered") primary = row;
            else if (primary.empty() && mine) primary = row;
        }
        if (primary.empty() && !weapons.empty()) primary = weapons.front();
    }
    EntityId p = player();
    auto* health = p ? m_scene.get<HealthComponent>(p) : nullptr;
    auto* inv = p ? m_scene.get<InventoryComponent>(p) : nullptr;
    int ammo = 0;
    std::string weaponName;
    if (inv) {
        weaponName = inv->weapon;
        if (inv->ammo.is_object() && inv->ammo.contains(inv->weapon) && inv->ammo[inv->weapon].is_number())
            ammo = inv->ammo[inv->weapon].get<int>();
    }
    if (!primary.empty()) {
        if (weaponName.empty()) weaponName = primary.value("definition", std::string());
        if (ammo == 0) ammo = primary.value("ammo", 0);
    }
    Json j = {{"health", health ? health->current : 0},
              {"healthMax", health ? health->max : 0},
              {"money", inv ? inv->money : 0},
              {"wanted", m_gp ? m_gp->wanted.level : 0},
              {"ammo", ammo},
              {"weapon", weaponName},
              {"reserve", primary.value("reserve", 0)},
              {"weaponState", primary.value("state", std::string())},
              {"fireMode", primary.value("fireMode", std::string())},
              {"ads", primary.value("ads", 0.0f)},
              {"weapons", weapons}};
    if (inv) {
        j["inventory"] = {{"items", inv->items}, {"ammo", inv->ammo}, {"weapon", inv->weapon}};
    }
    if (m_gp) {
        j["speed"] = m_gp->speedKmh;
        std::string objective;
        for (auto& [e, r] : m_gp->missions)
            if (r.state() == gameplay::MissionState::Running && objective.empty()) objective = r.objective();
        j["objective"] = objective;
        j["hour"] = -1;
        for (EntityId e : m_scene.entities())
            if (auto* tod = m_scene.get<TimeOfDayComponent>(e)) j["hour"] = tod->hour;
    }
    return j.dump();
}

// --------------------------------------------------------------- particles --
std::vector<ParticleItem> GameWorld::particleItems() {
    std::vector<ParticleItem> out;
    if (!m_gp) return out;
    std::vector<ParticleSprite> sprites;
    m_gp->rain.sprites(sprites, m_gp->skyBrightness);
    for (auto& [e, em] : m_gp->emitters) {
        auto* pc = m_scene.get<ParticleEmitterComponent>(e);
        if (!pc || !m_scene.isValid(e)) continue;
        em.sprites(emitterParams(*pc), m_scene.worldMatrix(e), sprites);
    }
    out.reserve(sprites.size());
    for (const auto& s : sprites) out.push_back({s.position, s.velocity, s.size, s.stretch, s.color, s.additive});
    return out;
}

// --------------------------------------------------------------------- HUD --
void GameWorld::addHud(int width, int height) {
    if (!m_renderer) return;
    if (auto particles = particleItems(); !particles.empty()) m_renderer->addParticles(particles);
    if (navDebug && m_gp && m_gp->nav) {
        std::vector<Vec3> tris;
        m_gp->nav->debugTriangles(tris);
        for (size_t i = 0; i + 2 < tris.size() && i < 60000; i += 3)
            for (int k = 0; k < 3; ++k) m_renderer->addLine(tris[i + static_cast<size_t>(k)] + Vec3{0, 0.08f, 0}, tris[i + static_cast<size_t>((k + 1) % 3)] + Vec3{0, 0.08f, 0}, {0.2f, 0.7f, 1.0f});
    }
    auto tris = hudOverlay(width, height);
    if (!tris.empty()) m_renderer->addOverlay(tris, true);
}

bool GameWorld::wantsCursor() {
    if (!playing()) return false;
    auto& pool = m_scene.pool<UiScreenComponent>();
    for (auto& us : pool.components())
        if (us.visible && us.interactive && !us.document.empty() && !endsWith(toLower(us.document), ".fhtml")) return true;
    return false;
}

std::vector<ForwardRenderer::OverlayVertex> GameWorld::hudOverlay(int width, int height) {
    if (!m_gp || width <= 0 || height <= 0) return {};
    GameplayState& g = *m_gp;
    float scale = std::max(1.0f, static_cast<float>(height) / 720.0f);
    UiCanvas ui(static_cast<float>(width), static_cast<float>(height), scale);
    if (const Font* font = uiFont()) ui.setFont(font);
    EntityId p = player();
    auto* pc = p ? m_scene.get<PlayerControllerComponent>(p) : nullptr;
    auto* health = p ? m_scene.get<HealthComponent>(p) : nullptr;
    auto* inv = p ? m_scene.get<InventoryComponent>(p) : nullptr;
    std::string objective;
    std::optional<Vec3> waypoint;
    std::string subtitle;
    for (auto& [e, r] : g.missions)
        if (r.state() == MissionState::Running) {
            if (objective.empty()) objective = r.objective();
            if (!waypoint) waypoint = r.waypoint();
            if (subtitle.empty()) subtitle = r.subtitle();
        }
    float hour = -1;
    for (EntityId e : m_scene.entities())
        if (auto* tod = m_scene.get<TimeOfDayComponent>(e)) hour = tod->hour;
    auto expand = [&](std::string s) {
        s = localization().text(s);  // "@key" -> the current language
        auto rep = [&](const std::string& key, const std::string& val) {
            for (size_t pos; (pos = s.find(key)) != std::string::npos;) s.replace(pos, key.size(), val);
        };
        rep("{money}", std::to_string(inv ? inv->money : 0));
        rep("{health}", std::to_string(health ? static_cast<int>(health->current) : 0));
        rep("{wanted}", std::to_string(g.wanted.level));
        rep("{objective}", objective);
        rep("{speed}", std::to_string(static_cast<int>(std::fabs(g.speedKmh))));
        rep("{hour}", hour >= 0 ? std::format("{:02}:{:02}", static_cast<int>(hour), static_cast<int>(std::fmod(hour, 1.0f) * 60)) : "");
        int ammo = inv && inv->ammo.is_object() && inv->ammo.contains(inv->weapon) && inv->ammo[inv->weapon].is_number() ? inv->ammo[inv->weapon].get<int>() : 0;
        std::string weapon = inv ? inv->weapon : "";
        if (const ComponentTypeInfo* info = ComponentRegistry::get().find("Weapon")) {
            for (EntityId e : m_scene.entities()) {
                void* c = m_scene.getComponent(e, "Weapon");
                if (!c) continue;
                Json w = info->type.toJson(c);
                if (ammo == 0) ammo = w.value("ammo", 0);
                if (weapon.empty()) weapon = w.value("definition", std::string());
                if (ammo > 0 || !weapon.empty()) break;
            }
        }
        rep("{ammo}", std::to_string(ammo));
        rep("{weapon}", weapon);
        return s;
    };
    auto minimap = [&](Vec2 tl, float size) {
        if (const world::RoadGraph* rg = roadGraph()) {
            size_t points = 0;
            for (auto& edge : rg->edges) points += edge.polyline.size();
            if (rg != m_minimapGraph || rg->edges.size() != m_minimapEdges || points != m_minimapPoints) {
                m_minimapGraph = rg;
                m_minimapEdges = rg->edges.size();
                m_minimapPoints = points;
                m_minimapRoads.clear();
                m_minimapRoads.reserve(rg->edges.size());
                for (auto& edge : rg->edges) m_minimapRoads.push_back(edge.polyline);
            }
        } else if (m_minimapGraph) {
            m_minimapGraph = nullptr;
            m_minimapEdges = m_minimapPoints = 0;
            m_minimapRoads.clear();
        }
        const std::vector<std::vector<Vec3>>& roads = m_minimapRoads;
        std::vector<MinimapBlip> blips;
        for (EntityId e : g.police)
            if (m_scene.isValid(e)) blips.push_back({posOf(m_scene, e), {0.3f, 0.5f, 1.0f, 1}, 5});
        for (EntityId e : g.policeCars)  // cruisers: flashing red / blue
            if (m_scene.isValid(e)) blips.push_back({posOf(m_scene, e), std::fmod(g.time, 0.5f) < 0.25f ? Vec4{1, 0.2f, 0.2f, 1} : Vec4{0.3f, 0.5f, 1, 1}, 7});
        for (EntityId e : m_scene.entities())
            if (m_scene.get<PickupComponent>(e) && !g.pickupRespawn.count(e)) blips.push_back({posOf(m_scene, e), {0.3f, 0.9f, 0.4f, 1}, 4});
        float yaw = p ? (pc && !pc->vehicle.empty() && m_scene.findByName(pc->vehicle) ? yawOfQuat(m_scene.worldTransform(m_scene.findByName(pc->vehicle)).rotation)
                                                                                         : g.camYaw)
                      : 0;
        Vec3 c = p ? posOf(m_scene, p) : Vec3{0, 0, 0};
        Vec3 wp = waypoint.value_or(Vec3{});
        drawMinimap(ui, tl, size, c, yaw, 220, roads, blips, waypoint ? &wp : nullptr);
    };
    auto stars = [&](Vec2 tl, float s) {
        for (int i = 0; i < 5; ++i) {
            Vec4 col = i < g.wanted.level ? Vec4{1, 0.85f, 0.2f, 1} : Vec4{0.3f, 0.3f, 0.3f, 0.6f};
            Vec2 c{tl.x + static_cast<float>(i) * s * 1.2f + s * 0.5f, tl.y + s * 0.5f};
            ui.triangle({c.x, c.y - s * 0.5f}, {c.x + s * 0.45f, c.y + s * 0.4f}, {c.x - s * 0.45f, c.y + s * 0.4f}, col);
            ui.triangle({c.x, c.y + s * 0.55f}, {c.x - s * 0.45f, c.y - s * 0.25f}, {c.x + s * 0.45f, c.y - s * 0.25f}, col);
        }
    };
    if (pc && pc->defaultHud) {
        float s = scale;
        minimap({20 * s, static_cast<float>(height) - 220 * s}, 180 * s);
        if (health) ui.bar(20 * s, static_cast<float>(height) - 34 * s, 180 * s, 12 * s, health->current / std::max(1.0f, health->max), {0.25f, 0.85f, 0.35f, 1}, {0, 0, 0, 0.6f});
        std::string money = std::format("${}", inv ? inv->money : 0);
        ui.text(static_cast<float>(width) - ui.measure(money, 3 * s).x - 20 * s, 18 * s, money, {0.55f, 0.95f, 0.55f, 1}, 3 * s);
        stars({static_cast<float>(width) - 150 * s, 50 * s}, 22 * s);
        if (hour >= 0) {
            std::string clock = expand("{hour}");
            ui.text(static_cast<float>(width) - ui.measure(clock, 2 * s).x - 20 * s, 82 * s, clock, {1, 1, 1, 0.9f}, 2 * s);
        }
        auto fit = [&](const std::string& t, float size) {  // shrink lines that would not fit the screen
            float w1 = ui.measure(t, 1).x;
            return w1 > 0 ? std::min(size, (static_cast<float>(width) - 40 * s) / w1) : size;
        };
        if (!objective.empty()) {
            float ts = fit(objective, 2.5f * s);
            Vec2 sz = ui.measure(objective, ts);
            ui.text((static_cast<float>(width) - sz.x) * 0.5f, 24 * s, objective, {1, 0.95f, 0.75f, 1}, ts);
        }
        if (!subtitle.empty()) {
            float ts = fit(subtitle, 2.5f * s);
            Vec2 sz = ui.measure(subtitle, ts);
            ui.text((static_cast<float>(width) - sz.x) * 0.5f, static_cast<float>(height) - 90 * s, subtitle, {1, 1, 1, 1}, ts);
        }
        if (!pc->vehicle.empty()) {
            std::string sp = expand("{speed} km/h");
            ui.text(static_cast<float>(width) - ui.measure(sp, 3 * s).x - 24 * s, static_cast<float>(height) - 50 * s, sp, {1, 1, 1, 1}, 3 * s);
        } else if (inv && !inv->weapon.empty()) {
            float cx = static_cast<float>(width) * 0.5f, cy = static_cast<float>(height) * 0.5f;
            ui.rect(cx - 8 * s, cy - 1 * s, 6 * s, 2 * s, {1, 1, 1, 0.9f});
            ui.rect(cx + 2 * s, cy - 1 * s, 6 * s, 2 * s, {1, 1, 1, 0.9f});
            ui.rect(cx - 1 * s, cy - 8 * s, 2 * s, 6 * s, {1, 1, 1, 0.9f});
            ui.rect(cx - 1 * s, cy + 2 * s, 2 * s, 6 * s, {1, 1, 1, 0.9f});
            std::string ammo = expand("{weapon} {ammo}");
            ui.text(static_cast<float>(width) - ui.measure(ammo, 2.5f * s).x - 24 * s, static_cast<float>(height) - 46 * s, ammo, {1, 1, 1, 1}, 2.5f * s);
        }
    }
    // custom HudElements
    for (EntityId e : m_scene.entities()) {
        auto* h = m_scene.get<HudElementComponent>(e);
        if (!h || !h->visible) continue;
        Vec4 col{h->color.x, h->color.y, h->color.z, h->opacity};
        Vec2 size{h->size.x * scale, h->size.y * scale};
        float textScale = h->textSize * scale;
        std::string text = expand(h->text);
        if (h->type == HudType::Text || h->type == HudType::Money || h->type == HudType::Clock || h->type == HudType::Objective || h->type == HudType::Subtitle ||
            h->type == HudType::Speedometer) {
            if (text.empty())
                text = h->type == HudType::Money ? expand("${money}") : h->type == HudType::Clock ? expand("{hour}") : h->type == HudType::Objective ? objective
                     : h->type == HudType::Subtitle                                                                                 ? subtitle
                     : h->type == HudType::Speedometer                                                                              ? expand("{speed} km/h")
                                                                                                                                    : text;
            float w1 = ui.measure(text, 1).x;
            if (w1 > 0) textScale = std::min(h->textSize * scale, (static_cast<float>(width) - 20) / w1);
            size = ui.measure(text, textScale);
        }
        Vec2 at = ui.place(static_cast<Anchor>(h->anchor), {h->offset.x * scale, h->offset.y * scale}, size);
        if (h->align != HudAlign::Auto || h->verticalAlign != HudVAlign::Auto) {  // 1.3.2: box edge / centre on the anchor point
            Vec2 p = ui.place(static_cast<Anchor>(h->anchor), {h->offset.x * scale, h->offset.y * scale}, {0, 0});  // the point itself
            if (h->align != HudAlign::Auto) at.x = p.x - size.x * (h->align == HudAlign::Left ? 0.0f : h->align == HudAlign::Center ? 0.5f : 1.0f);
            if (h->verticalAlign != HudVAlign::Auto)
                at.y = p.y - size.y * (h->verticalAlign == HudVAlign::Top ? 0.0f : h->verticalAlign == HudVAlign::Middle ? 0.5f : 1.0f);
        }
        switch (h->type) {
            case HudType::Panel: ui.rect(at.x, at.y, size.x, size.y, col); break;
            case HudType::Bar:
            case HudType::Health: {
                float f = health ? health->current / std::max(1.0f, health->max) : 0;
                if (h->type == HudType::Bar && !h->binding.empty()) {  // Entity/Component.field[/maxField]
                    auto slash = h->binding.find('/'), dotp = h->binding.find('.');
                    if (slash != std::string::npos && dotp != std::string::npos && dotp > slash) {
                        EntityId be = m_scene.findByName(h->binding.substr(0, slash));
                        std::string comp = h->binding.substr(slash + 1, dotp - slash - 1), rest = h->binding.substr(dotp + 1);
                        std::string field = rest.substr(0, rest.find('/')), maxField = rest.find('/') != std::string::npos ? rest.substr(rest.find('/') + 1) : "";
                        if (be) {
                            Json cj = m_scene.entityToJson(be)["components"].value(comp, Json::object());
                            float v = cj.value(field, 0.0f), mx = maxField.empty() ? 1.0f : cj.value(maxField, 1.0f);
                            f = v / std::max(1e-4f, mx);
                        }
                    }
                }
                ui.bar(at.x, at.y, size.x, size.y, f, col, {0, 0, 0, 0.6f});
                break;
            }
            case HudType::Minimap: minimap(at, size.x); break;
            case HudType::Wanted: stars(at, size.y); break;
            case HudType::Crosshair:
                ui.rect(at.x + size.x * 0.5f - 1, at.y, 2, size.y, col);
                ui.rect(at.x, at.y + size.y * 0.5f - 1, size.x, 2, col);
                break;
            default: ui.text(at.x, at.y, text, col, textScale); break;
        }
    }
    // retained UI documents (UiScreen): layout, input, draw
    bool pointerClick = uiPointer.down && !g.pointerWasDown;
    g.pointerWasDown = uiPointer.down;
    float uiAnimDt = 0;
    if (g.uiAnimAt >= 0) uiAnimDt = std::max(0.0f, g.time - g.uiAnimAt);
    g.uiAnimAt = g.time;
    script::InputMap& keys = m_script.input();
    for (EntityId e : m_scene.entities()) {
        auto* us = m_scene.get<UiScreenComponent>(e);
        if (!us || !us->visible || us->document.empty()) continue;
        if (endsWith(toLower(us->document), ".fhtml")) continue; // the standalone runtime owns the browser surface
        auto [it, fresh] = g.uiDocs.try_emplace(us->document);
        if (fresh) {
            std::string path = m_registry.resolve(us->document);
            if (path.empty()) path = startsWith(us->document, "content:/") ? us->document : "content:/" + us->document;
            auto text = m_vfs.readText(path);
            auto doc = text ? UiDocument::fromJson(Json::parse(*text, nullptr, false)) : Result<UiDocument>::failure("not found");
            if (doc) it->second = std::move(*doc);
            else FLEDGE_LOG_WARN("ui", "UiScreen '{}': {}", us->document, doc.error());
        }
        if (!it->second) continue;
        UiDocument& doc = *it->second;
        doc.layout(ui, expand);
        if (us->interactive) {
            UiInput in;
            in.mouse = uiPointer.pos;
            in.click = pointerClick;
            in.held = uiPointer.down;
            in.up = keys.action("UiUp", script::InputQuery::Pressed);
            in.down = keys.action("UiDown", script::InputQuery::Pressed);
            in.accept = keys.action("UiAccept", script::InputQuery::Pressed);
            for (const std::string& id : doc.handleInput(in)) {
                us->clicked = id;
                ++us->clicks;
            }
            us->focus = doc.focus();
        }
        doc.tick(uiAnimDt);
        doc.draw(ui, expand);
    }
    if (g.toastLeft > 0 && !g.toast.empty()) {
        Vec2 sz = ui.measure(g.toast, 2.5f * scale);
        ui.text((static_cast<float>(width) - sz.x) * 0.5f, static_cast<float>(height) * 0.30f, g.toast, {1, 1, 1, std::min(1.0f, g.toastLeft)}, 2.5f * scale);
    }
    if (g.bigMessageLeft > 0 && !g.bigMessage.empty()) {
        Vec2 sz = ui.measure(g.bigMessage, 7 * scale);
        Vec4 c = g.bigColor;
        c.w = std::min(1.0f, g.bigMessageLeft);
        ui.text((static_cast<float>(width) - sz.x) * 0.5f, (static_cast<float>(height) - sz.y) * 0.45f, g.bigMessage, c, 7 * scale);
    }
    std::vector<ForwardRenderer::OverlayVertex> tris;
    tris.reserve(ui.vertices().size());
    for (auto& v : ui.vertices()) tris.push_back({v.pos, v.color, v.uv});
    return tris;
}

// ---------------------------------------------------------------- save/load --
Status GameWorld::saveGame(const fs::path& file) {
    if (!m_playing || !m_gp) return Status::failure("not playing");
    Json j = {{"format", "fledge-save"}, {"version", 1}, {"scene", m_scenePath}};
    EntityId p = player();
    if (p) {
        Transform t = m_scene.worldTransform(p);
        j["player"] = {{"name", m_scene.displayName(p)}, {"position", {t.position.x, t.position.y, t.position.z}}, {"yaw", yawOfQuat(t.rotation)}};
        if (auto* h = m_scene.get<HealthComponent>(p)) j["player"]["health"] = h->current;
        if (auto* inv = m_scene.get<InventoryComponent>(p)) j["player"]["inventory"] = {{"money", inv->money}, {"items", inv->items}, {"ammo", inv->ammo}, {"weapon", inv->weapon}};
    }
    j["wanted"] = {{"level", m_gp->wanted.level}, {"heat", m_gp->wanted.heat}};
    Json missions = Json::object();
    for (auto& [e, r] : m_gp->missions)
        if (m_scene.isValid(e)) missions[m_scene.displayName(e)] = r.saveState();
    j["missions"] = missions;
    for (EntityId e : m_scene.entities())
        if (auto* tod = m_scene.get<TimeOfDayComponent>(e)) j["hour"] = tod->hour;
    Json vehicles = Json::object();
    for (EntityId e : m_scene.entities())
        if (auto* vc = m_scene.get<VehicleComponent>(e)) {
            Transform t = m_scene.worldTransform(e);
            vehicles[m_scene.displayName(e)] = {{"position", {t.position.x, t.position.y, t.position.z}}, {"yaw", yawOfQuat(t.rotation)}, {"health", vc->health}};
        }
    j["vehicles"] = vehicles;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!writeFileText(file, j.dump(2))) return Status::failure("cannot write " + file.string());
    return {};
}

Status GameWorld::loadGame(const fs::path& file) {
    if (!m_playing || !m_gp) return Status::failure("not playing");
    auto text = readFileText(file);
    if (!text) return Status::failure("no save at " + file.string());
    Json j = Json::parse(*text, nullptr, false);
    if (!j.is_object() || j.value("format", std::string()) != "fledge-save") return Status::failure("not a Fledge save");
    EntityId p = player();
    if (p && j.contains("player")) {
        auto& pj = j["player"];
        auto pos = pj.value("position", std::vector<float>{0, 0, 0});
        if (auto* pc = m_scene.get<PlayerControllerComponent>(p); pc && !pc->vehicle.empty()) {  // step out first
            if (EntityId car = m_scene.findByName(pc->vehicle))
                if (auto* vc = m_scene.get<VehicleComponent>(car)) vc->driver.clear();
            pc->vehicle.clear();
            if (auto* cc = m_scene.get<CharacterControllerComponent>(p)) cc->enabled = true;
            if (auto* an = m_scene.get<AnimatorComponent>(p)) an->visible = true;
        }
        if (auto* tc = m_scene.get<TransformComponent>(p)) {
            tc->position = {pos[0], pos[1], pos[2]};
            tc->rotation = Quat::fromAxisAngle({0, 1, 0}, pj.value("yaw", 0.0f));
        }
        if (auto* h = m_scene.get<HealthComponent>(p)) {
            h->current = pj.value("health", h->max);
            h->dead = false;
        }
        if (auto* inv = m_scene.get<InventoryComponent>(p); inv && pj.contains("inventory")) {
            inv->money = pj["inventory"].value("money", 0);
            inv->items = pj["inventory"].value("items", Json::object());
            inv->ammo = pj["inventory"].value("ammo", Json::object());
            inv->weapon = pj["inventory"].value("weapon", std::string());
        }
    }
    m_gp->wanted.setLevel(j.value("wanted", Json::object()).value("level", 0));
    if (j.contains("missions"))
        for (auto& [name, st] : j["missions"].items())
            if (EntityId e = m_scene.findByName(name))
                if (auto* mc = m_scene.get<MissionComponent>(e)) m_gp->missions[e].loadState(MissionGraph::fromJson(mc->graph), st);
    if (j.contains("hour"))
        for (EntityId e : m_scene.entities())
            if (auto* tod = m_scene.get<TimeOfDayComponent>(e)) tod->hour = j["hour"].get<float>();
    if (j.contains("vehicles"))
        for (auto& [name, vj] : j["vehicles"].items())
            if (EntityId e = m_scene.findByName(name)) {
                auto pos = vj.value("position", std::vector<float>{0, 0, 0});
                if (auto* tc = m_scene.get<TransformComponent>(e)) {
                    tc->position = {pos[0], pos[1], pos[2]};
                    tc->rotation = Quat::fromAxisAngle({0, 1, 0}, vj.value("yaw", 0.0f));
                }
                if (auto* rb = m_scene.get<RigidBodyComponent>(e)) rb->velocity = {0, 0, 0};
                if (auto* vc = m_scene.get<VehicleComponent>(e)) vc->health = vj.value("health", 100.0f);
            }
    return {};
}

} // namespace fledge
