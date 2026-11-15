#include "fledge/gameplay/city.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace fledge::gameplay {

namespace {
struct Rng {
    uint32_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float f() { return static_cast<float>(next() >> 8) / 16777216.0f; }
    float range(float a, float b) { return a + (b - a) * f(); }
};
Json v3(Vec3 v) { return Json::array({v.x, v.y, v.z}); }
Json node(const std::string& name, Json components) { return {{"name", name}, {"components", std::move(components)}, {"children", Json::array()}}; }
} // namespace

EntityId spawnTree(Scene& scene, const Json& n, EntityId parent) {
    EntityId e = scene.createEntity(n.value("name", std::string("Entity")));
    if (n.contains("components") && n["components"].is_object())
        for (auto& [type, fields] : n["components"].items()) {
            const ComponentTypeInfo* info = ComponentRegistry::get().find(type);
            if (!info) continue;
            void* c = scene.getComponent(e, type);
            if (!c) c = scene.addComponent(e, type);
            if (c) (void)info->type.fromJson(c, fields);
        }
    if (parent) (void)scene.setParent(e, parent, -1, false);
    if (n.contains("children"))
        for (auto& ch : n["children"]) spawnTree(scene, ch, e);
    return e;
}

Json vehicleNode(const std::string& preset, Vec3 position, float yaw, const std::string& name, Vec3 color) {
    if (preset == "bike") {  // motorbike: narrow chassis, Wheel0 front / Wheel1 rear (the physics poses them)
        const Vec3 size{0.4f, 0.6f, 1.0f};
        const float wr = 0.31f, ww = 0.08f, susp = 0.2f;
        Quat q = Quat::fromAxisAngle({0, 1, 0}, yaw);
        Json root = node(name.empty() ? "Bike" : name,
                         {{"Transform", {{"position", v3(position + Vec3{0, wr + size.y * 0.5f + 0.35f, 0})}, {"rotation", Json::array({q.x, q.y, q.z, q.w})}}},
                          {"Collider", {{"shape", "Box"}, {"size", v3(size)}}},
                          {"RigidBody", {{"motion", "Dynamic"}, {"mass", 240}, {"friction", 0.5f}}},
                          {"Vehicle", {{"preset", "bike"}, {"maxEngineTorque", 150}, {"maxRpm", 10000}, {"wheelRadius", wr}, {"wheelWidth", ww}, {"suspension", susp}}},
                          {"Tags", {{"tags", "vehicle"}}}});
        root["children"].push_back(node("Body", {{"Transform", {{"scale", v3({0.3f, 0.45f, 1.1f})}}}, {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3(color)}}}}));
        root["children"].push_back(node("Tank", {{"Transform", {{"position", v3({0, 0.3f, -0.2f})}, {"scale", v3({0.34f, 0.2f, 0.45f})}}},
                                                 {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3(color)}}}}));
        root["children"].push_back(node("Seat", {{"Transform", {{"position", v3({0, 0.28f, 0.25f})}, {"scale", v3({0.28f, 0.1f, 0.5f})}}},
                                                 {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3({0.08f, 0.08f, 0.09f})}}}}));
        root["children"].push_back(node("Handlebar", {{"Transform", {{"position", v3({0, 0.45f, -0.5f})}, {"scale", v3({0.7f, 0.05f, 0.05f})}}},
                                                      {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3({0.2f, 0.2f, 0.22f})}}}}));
        float bz = size.z * 0.5f + wr * 0.8f, by = -0.9f * size.y * 0.5f;
        for (int i = 0; i < 2; ++i)
            root["children"].push_back(node(std::format("Wheel{}", i), {{"Transform", {{"position", v3({0, by, i == 0 ? -bz : bz})}, {"scale", v3({ww, wr * 2, wr * 2})}}},
                                                                        {"MeshRenderer", {{"mesh", "builtin:sphere"}, {"color", v3({0.08f, 0.08f, 0.08f})}}}}));
        return root;
    }
    bool truck = preset == "truck";
    Vec3 size = truck ? Vec3{2.4f, 1.4f, 7.0f} : Vec3{1.8f, 0.7f, 4.2f};
    float mass = truck ? 6000 : 1400, torque = truck ? 2500 : 500, rpm = truck ? 3500 : 6000;
    float wr = truck ? 0.55f : 0.38f, ww = truck ? 0.4f : 0.25f, susp = truck ? 0.3f : 0.25f;
    Quat q = Quat::fromAxisAngle({0, 1, 0}, yaw);
    Json root = node(name.empty() ? (truck ? "Truck" : "Car") : name,
                     {{"Transform", {{"position", v3(position + Vec3{0, wr + size.y * 0.5f + 0.3f, 0})}, {"rotation", Json::array({q.x, q.y, q.z, q.w})}}},
                      {"Collider", {{"shape", "Box"}, {"size", v3(size)}}},
                      {"RigidBody", {{"motion", "Dynamic"}, {"mass", mass}, {"friction", 0.5f}}},
                      {"Vehicle", {{"preset", truck ? "truck" : "car"}, {"maxEngineTorque", torque}, {"maxRpm", rpm}, {"wheelRadius", wr}, {"wheelWidth", ww},
                                   {"suspension", susp}}},
                      {"Tags", {{"tags", "vehicle"}}}});
    root["children"].push_back(node("Body", {{"Transform", {{"scale", v3(size)}}}, {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3(color)}}}}));
    Vec3 cabin{size.x * 0.85f, size.y * 0.8f, size.z * (truck ? 0.25f : 0.45f)};
    float cabinZ = truck ? -size.z * 0.35f : size.z * 0.05f;
    root["children"].push_back(node("Cabin", {{"Transform", {{"position", v3({0, size.y * 0.5f + cabin.y * 0.5f, cabinZ})}, {"scale", v3(cabin)}}},
                                              {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3({0.15f, 0.18f, 0.22f})}}}}));
    float wx = size.x * 0.5f - ww * 0.5f, wz = size.z * 0.5f - wr * 1.2f, wy = -size.y * 0.5f;
    const Vec3 wp[4] = {{-wx, wy, -wz}, {wx, wy, -wz}, {-wx, wy, wz}, {wx, wy, wz}};
    for (int i = 0; i < 4; ++i)
        root["children"].push_back(node(std::format("Wheel{}", i), {{"Transform", {{"position", v3(wp[i])}, {"scale", v3({ww, wr * 2, wr * 2})}}},
                                                                    {"MeshRenderer", {{"mesh", "builtin:sphere"}, {"color", v3({0.08f, 0.08f, 0.08f})}}}}));
    return root;
}

CityParams cityParamsFromJson(const Json& j) {
    CityParams p;
    if (!j.is_object()) return p;
    p.blocksX = std::clamp(j.value("blocksX", p.blocksX), 1, 32);
    p.blocksZ = std::clamp(j.value("blocksZ", p.blocksZ), 1, 32);
    p.blockSize = std::clamp(j.value("blockSize", p.blockSize), 24.0f, 400.0f);
    p.roadWidth = std::clamp(j.value("roadWidth", p.roadWidth), 4.0f, 40.0f);
    p.sidewalk = std::clamp(j.value("sidewalk", p.sidewalk), 0.0f, 10.0f);
    p.minHeight = j.value("minHeight", p.minHeight);
    p.maxHeight = j.value("maxHeight", p.maxHeight);
    p.parkChance = j.value("parkChance", p.parkChance);
    p.seed = j.value("seed", p.seed);
    p.name = j.value("name", p.name);
    p.ground = j.value("ground", p.ground);
    if (j.contains("origin") && j["origin"].is_array() && j["origin"].size() == 3)
        p.origin = {j["origin"][0].get<float>(), j["origin"][1].get<float>(), j["origin"][2].get<float>()};
    return p;
}

void applyGenerateSpec(Scene& scene, const Json& g) {
    if (!g.is_object()) return;
    if (g.contains("city")) spawnTree(scene, generateCity(cityParamsFromJson(g["city"])).root);
    if (g.contains("vehicles") && g["vehicles"].is_array())
        for (auto& v : g["vehicles"]) {
            if (!v.is_object()) continue;
            Vec3 pos{0, 0, 0}, col{0.75f, 0.12f, 0.1f};
            if (v.contains("position") && v["position"].is_array() && v["position"].size() == 3)
                pos = {v["position"][0].get<float>(), v["position"][1].get<float>(), v["position"][2].get<float>()};
            if (v.contains("color") && v["color"].is_array() && v["color"].size() == 3)
                col = {v["color"][0].get<float>(), v["color"][1].get<float>(), v["color"][2].get<float>()};
            spawnTree(scene, vehicleNode(v.value("preset", std::string("car")), pos, v.value("yaw", 0.0f), v.value("name", std::string()), col));
        }
}

int countNodes(const Json& n) {
    int c = 1;
    if (n.contains("children"))
        for (auto& ch : n["children"]) c += countNodes(ch);
    return c;
}

CityResult generateCity(const CityParams& p) {
    CityResult r;
    Rng rng{p.seed * 2654435761u | 1u};
    const float B = p.blockSize, W = p.roadWidth, S = p.sidewalk;
    const float sizeX = B * static_cast<float>(p.blocksX), sizeZ = B * static_cast<float>(p.blocksZ);
    r.center = p.origin + Vec3{sizeX * 0.5f, 0, sizeZ * 0.5f};
    r.root = node(p.name, {{"Transform", {{"position", v3(p.origin)}}}, {"Tags", {{"tags", "city"}}}});
    Json& kids = r.root["children"];
    if (p.ground) {
        float pad = B * 0.5f;
        kids.push_back(node("Ground", {{"Transform", {{"position", v3({sizeX * 0.5f, -0.5f, sizeZ * 0.5f})}, {"scale", v3({sizeX + 2 * pad, 1, sizeZ + 2 * pad})}}},
                                       {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3({0.32f, 0.36f, 0.3f})}}},
                                       {"Collider", {{"shape", "Box"}, {"size", v3({1, 1, 1})}}}}));
    }
    // roads: one spline per block edge (intersections at every grid node)
    Json roads = node("Roads", {{"Transform", Json::object()}});
    auto road = [&](Vec3 a, Vec3 b, const std::string& name) {
        Json spline = {{"points", Json::array({v3(a), v3(b)})}, {"kind", "road"}, {"width", W}, {"sidewalk", S}, {"lanes", 2}, {"speedLimit", 13.9f},
                       {"sidewalkTrim", W * 0.5f + S}};
        roads["children"].push_back(node(name, {{"Transform", Json::object()}, {"Spline", spline}}));
        ++r.roads;
    };
    for (int j = 0; j <= p.blocksZ; ++j)
        for (int i = 0; i < p.blocksX; ++i)
            road({B * static_cast<float>(i), 0, B * static_cast<float>(j)}, {B * static_cast<float>(i + 1), 0, B * static_cast<float>(j)}, std::format("Street {}-{}", j, i));
    for (int i = 0; i <= p.blocksX; ++i)
        for (int j = 0; j < p.blocksZ; ++j)
            road({B * static_cast<float>(i), 0, B * static_cast<float>(j)}, {B * static_cast<float>(i), 0, B * static_cast<float>(j + 1)}, std::format("Avenue {}-{}", i, j));
    kids.push_back(roads);
    // blocks: buildings on 1..4 lots, or a park
    Json blocks = node("Blocks", {{"Transform", Json::object()}});
    const Vec3 palette[] = {{0.62f, 0.6f, 0.56f}, {0.55f, 0.5f, 0.46f}, {0.7f, 0.68f, 0.62f}, {0.45f, 0.48f, 0.52f}, {0.66f, 0.55f, 0.45f}, {0.5f, 0.55f, 0.6f}};
    for (int i = 0; i < p.blocksX; ++i)
        for (int j = 0; j < p.blocksZ; ++j) {
            float x0 = B * static_cast<float>(i) + W * 0.5f + S, x1 = B * static_cast<float>(i + 1) - W * 0.5f - S;
            float z0 = B * static_cast<float>(j) + W * 0.5f + S, z1 = B * static_cast<float>(j + 1) - W * 0.5f - S;
            if (x1 - x0 < 6 || z1 - z0 < 6) continue;
            Vec3 c{(x0 + x1) * 0.5f, 0, (z0 + z1) * 0.5f};
            if (rng.f() < p.parkChance) {
                Json park = node(std::format("Park {}-{}", i, j),
                                 {{"Transform", {{"position", v3({c.x, 0.05f, c.z})}, {"scale", v3({x1 - x0, 0.1f, z1 - z0})}}},
                                  {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3({0.25f, 0.5f, 0.22f})}}}});
                blocks["children"].push_back(park);
                Json trees = node(std::format("Trees {}-{}", i, j),
                                  {{"Transform", {{"position", v3({c.x, 0.1f, c.z})}}},
                                   {"Foliage", {{"mesh", "builtin:sphere"}, {"color", v3({0.18f, 0.42f, 0.16f})}, {"area", std::min(x1 - x0, z1 - z0) * 0.8f},
                                                {"density", 1.5f}, {"seed", static_cast<int>(rng.next() % 10000)}, {"minScale", 2.0f}, {"maxScale", 4.0f}}}});
                blocks["children"].push_back(trees);
                ++r.parks;
                continue;
            }
            int lotsX = 1 + static_cast<int>(rng.next() % 2), lotsZ = 1 + static_cast<int>(rng.next() % 2);
            float lw = (x1 - x0) / static_cast<float>(lotsX), ld = (z1 - z0) / static_cast<float>(lotsZ);
            for (int a = 0; a < lotsX; ++a)
                for (int b = 0; b < lotsZ; ++b) {
                    float h = rng.range(p.minHeight, p.maxHeight);
                    if (rng.f() < 0.25f) h = std::min(p.maxHeight * 1.6f, h * 1.6f);  // the odd tower
                    float inset = 1.5f;
                    Vec3 pos{x0 + lw * (static_cast<float>(a) + 0.5f), h * 0.5f, z0 + ld * (static_cast<float>(b) + 0.5f)};
                    Vec3 col = palette[rng.next() % 6];
                    blocks["children"].push_back(node(std::format("Building {}-{}-{}", i, j, a * 2 + b),
                                                      {{"Transform", {{"position", v3(pos)}, {"scale", v3({lw - 2 * inset, h, ld - 2 * inset})}}},
                                                       {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", v3(col)}}},
                                                       {"Collider", {{"shape", "Box"}, {"size", v3({1, 1, 1})}}},
                                                       {"Tags", {{"tags", "building"}}}}));
                    ++r.buildings;
                }
            // spawn points on this block's sidewalks, parking spots along the kerb (x, z, yaw)
            r.spawnPoints.push_back(p.origin + Vec3{x0 - S * 0.5f, 0.2f, c.z});
            r.spawnPoints.push_back(p.origin + Vec3{c.x, 0.2f, z0 - S * 0.5f});
            // kerbside on the street north of the block (+Z side of the road = right lane for +X travel), facing +X
            r.parkingSpots.push_back(p.origin + Vec3{c.x, -1.5707963f, B * static_cast<float>(j) + W * 0.5f - 1.0f});
        }
    kids.push_back(blocks);
    return r;
}

} // namespace fledge::gameplay
