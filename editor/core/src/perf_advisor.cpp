#include "fledge/editor/perf_advisor.h"

#include <algorithm>
#include <format>
#include <map>

#include "fledge/editor/editor_session.h"

namespace fledge {

PerfReport analyzePerformance(EditorSession& s, Vec3 cameraPos) {
    PerfReport r;
    Scene& scene = s.scene();
    float far = 0;
    std::vector<Vec3> cams;
    for (EntityId e : scene.entities())
        if (auto* c = scene.get<CameraComponent>(e)) {
            far = std::max(far, c->farPlane);
            cams.push_back(scene.worldTransform(e).position);
        }
    cams.push_back(cameraPos);  // the editor camera
    if (far <= 0) far = 500;
    std::map<std::pair<std::string, std::string>, std::vector<EntityId>> groups;
    std::vector<EntityId> beyond;
    std::vector<std::pair<float, EntityId>> shadowCasters;
    for (EntityId e : scene.entities()) {
        Vec3 p = scene.worldTransform(e).position;
        if (auto* mr = scene.get<MeshRendererComponent>(e); mr && mr->visible) {
            ++r.meshes;
            groups[{mr->mesh, mr->material}].push_back(e);
            uint64_t tris = 12;
            if (const MeshData* m = s.world().cpuMesh(mr->mesh)) tris = m->indices.size() / 3;
            r.triangles += tris;
            float nearest = 1e30f;
            for (Vec3 c : cams) nearest = std::min(nearest, length(p - c));
            if (nearest > far * 1.05f) beyond.push_back(e);
            else if (tris > 20000 && nearest > 50)
                r.suggestions.push_back({"lod", std::format("'{}' has {} triangles and is {:.0f} m from the nearest camera: raise its LOD bias so coarser "
                                                            "LODs (generated automatically) are used earlier",
                                                            scene.displayName(e), tris, nearest),
                                         {e}, true});
        }
        if (auto* l = scene.get<LightComponent>(e)) {
            ++r.lights;
            if (l->castShadows) {
                ++r.shadowLights;
                if (l->type != LightType::Directional) shadowCasters.push_back({l->intensity * l->range, e});
            }
        }
    }
    if (!beyond.empty())
        r.suggestions.push_back({"culling", std::format("{} object{} lie beyond every camera's far plane ({:.0f} m) and can never be seen: hide them", beyond.size(),
                                                        beyond.size() == 1 ? "" : "s", far),
                                 beyond, true});
    if (shadowCasters.size() > 2) {
        std::sort(shadowCasters.begin(), shadowCasters.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::vector<EntityId> off;
        for (size_t i = 2; i < shadowCasters.size(); ++i) off.push_back(shadowCasters[i].second);
        r.suggestions.push_back({"shadows", std::format("{} point/spot lights cast shadows (each one renders the scene again): keep shadows on the 2 strongest, "
                                                        "turn them off on {}",
                                                        shadowCasters.size(), off.size()),
                                 off, true});
    }
    for (auto& [key, ents] : groups)
        if (ents.size() >= 8)
            r.suggestions.push_back({"instancing", std::format("{} objects share mesh '{}'{}: they are drawn with GPU instancing automatically "
                                                               "(OpenGL / Direct3D 11; Direct3D 12 draws them one by one). Keep their materials identical.",
                                                               ents.size(), key.first, key.second.empty() ? "" : " and the same material"),
                                     ents, false});
    return r;
}

int applyPerformanceFixes(EditorSession& s, const PerfReport& r) {
    int changed = 0;
    s.history().begin("Make it run well", EditSource::User);
    for (auto& sg : r.suggestions) {
        if (!sg.fixable) continue;
        for (EntityId e : sg.entities) {
            Status st;
            if (sg.kind == "culling") st = s.setProperty(e, "MeshRenderer", "visible", false);
            else if (sg.kind == "lod") st = s.setProperty(e, "MeshRenderer", "lodBias", s.scene().get<MeshRendererComponent>(e)->lodBias + 1.0f);
            else st = s.setProperty(e, "Light", "castShadows", false);
            if (st) ++changed;
        }
    }
    if (changed) s.history().commit();
    else s.history().cancel();
    return changed;
}

} // namespace fledge
