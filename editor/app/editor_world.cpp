// Open world editor UI: World Streaming panel (minimap + chunk states + budgets), terrain sculpt/paint
// brushes, spline/road tools, chunk grid overlay. See docs/WORLD_STREAMING.md.
#include <algorithm>
#include <cmath>
#include <format>

#include <imgui.h>

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/world/partition.h"
#include "fledge/world/roads.h"
#include "fledge/world/terrain.h"

namespace fledge {
Status convertSceneToOpenWorld(EditorSession& s, float cellSize, size_t* chunks);
}

namespace fledge::editor {

namespace {
Vec3 focusPoint(Vec3 camPos, float yaw, float pitch, float dist) {
    Vec3 fwd{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    return camPos + fwd * dist;
}
} // namespace

void EditorApp::worldFrameHooks() {
    GameWorld& w = m_session.world();
    if (m_session.playing()) {
        Vec3 d = w.lastRebase();
        if (d.x != 0 || d.z != 0) m_camPos -= d;  // the floating origin moved: keep the editor camera on the same spot
        w.viewerOverride = m_streamAtCamera ? std::optional<Vec3>(m_camPos) : std::nullopt;
    } else {
        w.viewerOverride = m_camPos;  // terrain LOD around the editor camera
    }
    const WorldPartitionComponent* wp = m_session.hasProject() ? world::findPartitionSettings(m_session.scene()) : nullptr;
    if (m_showChunkGrid && wp && m_renderer) {
        double cell = wp->cellSize;
        world::DVec3 origin = w.streaming() ? w.streaming()->origin() : world::DVec3{};
        double cx = m_camPos.x + origin.x, cz = m_camPos.z + origin.z;
        int n = 8;
        world::ChunkCoord c0 = world::chunkOf(cx, cz, cell);
        for (int i = -n; i <= n + 1; ++i) {
            float x = static_cast<float>((c0.x + i) * cell - origin.x), z = static_cast<float>((c0.z + i) * cell - origin.z);
            float a = static_cast<float>((c0.z - n) * cell - origin.z), b = static_cast<float>((c0.z + n + 1) * cell - origin.z);
            float c = static_cast<float>((c0.x - n) * cell - origin.x), d2 = static_cast<float>((c0.x + n + 1) * cell - origin.x);
            m_renderer->addLine({x, 0.1f, a}, {x, 0.1f, b}, {0.95f, 0.55f, 0.4f});
            m_renderer->addLine({c, 0.1f, z}, {d2, 0.1f, z}, {0.95f, 0.55f, 0.4f});
        }
    }
    // brush cursor
    if (m_brush.active && m_brush.hasHit && m_renderer) {
        const int seg = 32;
        for (int i = 0; i < seg; ++i) {
            float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
            Vec3 p0 = m_brush.lastHit + Vec3{std::cos(a0) * m_brush.radius, 0.3f, std::sin(a0) * m_brush.radius};
            Vec3 p1 = m_brush.lastHit + Vec3{std::cos(a1) * m_brush.radius, 0.3f, std::sin(a1) * m_brush.radius};
            if (auto h0 = w.terrainHeight(p0.x, p0.z)) p0.y = *h0 + 0.3f;
            if (auto h1 = w.terrainHeight(p1.x, p1.z)) p1.y = *h1 + 0.3f;
            m_renderer->addLine(p0, p1, {1.0f, 0.8f, 0.2f});
        }
    }
}

bool EditorApp::updateTerrainBrush(const ViewItem& view, ImVec2 origin, ImVec2 size) {
    m_brush.hasHit = false;
    if (!m_brush.active) return false;
    Scene& scene = m_session.scene();
    auto* tc = scene.isValid(m_brush.terrain) ? scene.get<TerrainComponent>(m_brush.terrain) : nullptr;
    if (!tc) {
        m_brush.active = false;
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    Mat4 m = scene.worldMatrix(m_brush.terrain), inv = m.inverse();
    world::Heightfield hf = world::Heightfield::fromComponent(*tc);
    std::optional<Vec3> hitLocal;
    if (m_vpHovered) {
        Ray r = mouseRay(view, origin, size);
        Vec3 lo = inv.transformPoint(r.origin), ld = inv.transformPoint(r.origin + r.dir) - lo;
        hitLocal = hf.raycast(lo, ld, 5000);
    }
    if (hitLocal) {
        m_brush.hasHit = true;
        m_brush.lastHit = m.transformPoint(*hitLocal);
    }
    if (hitLocal && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        m_brush.stroking = true;
        m_brush.beforeHeights = tc->heights;
        m_brush.beforeSplat = tc->splat;
        if (m_brush.mode == static_cast<int>(world::BrushMode::Flatten)) m_brush.flattenHeight = hitLocal->y;
    }
    if (m_brush.stroking) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (hitLocal) {
                auto mode = static_cast<world::BrushMode>(m_brush.mode);
                float k = (mode == world::BrushMode::Raise || mode == world::BrushMode::Lower) ? m_brush.strength * io.DeltaTime * 8.0f : m_brush.strength * 0.25f;
                hf.brush(*hitLocal, m_brush.radius, k, mode, m_brush.layer, m_brush.flattenHeight);
                hf.toComponent(*tc);  // live preview (no history yet)
            }
        } else {  // release: one undo step from the state before the stroke
            std::string afterH = tc->heights, afterS = tc->splat;
            tc->heights = m_brush.beforeHeights;
            tc->splat = m_brush.beforeSplat;
            (void)m_session.applyComponents(m_brush.terrain, {{"Terrain", {{"heights", afterH}, {"splat", afterS}}}}, "Sculpt terrain");
            m_brush.stroking = false;
        }
    }
    return hitLocal.has_value() || m_brush.stroking;
}

void EditorApp::drawComponentTools(EntityId e, const std::string& comp) {
    Scene& scene = m_session.scene();
    float s = effectiveScale();
    if (comp == "Terrain") {
        auto* tc = scene.get<TerrainComponent>(e);
        if (!tc) return;
        ImGui::SeparatorText("Terrain tools");
        bool on = m_brush.active && m_brush.terrain == e;
        if (ImGui::Checkbox("Sculpt / paint in the viewport (LMB)", &on)) {
            m_brush.active = on;
            m_brush.terrain = e;
        }
        trackRect("terrain.brush");
        const char* modes[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint"};
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine();
            ImGui::RadioButton(modes[i], &m_brush.mode, i);
        }
        ImGui::SetNextItemWidth(140 * s);
        ImGui::SliderFloat("Radius", &m_brush.radius, 1, 100, "%.0f m");
        ImGui::SetNextItemWidth(140 * s);
        ImGui::SliderFloat("Strength", &m_brush.strength, 0.05f, 2.0f);
        if (m_brush.mode == 4) {
            const char* layers[] = {"0 grass", "1 dirt", "2 rock", "3 sand"};
            ImGui::SetNextItemWidth(140 * s);
            ImGui::Combo("Layer", &m_brush.layer, layers, 4);
        }
        if (ImGui::Button("Generate hills (new seed)")) {
            TerrainComponent t = *tc;
            t.seed = tc->seed + 1;
            world::Heightfield h(t.resolution, t.size);
            h.generateHills(t.seed, t.maxHeight);
            h.toComponent(t);
            (void)m_session.applyComponents(e, {{"Terrain", {{"seed", t.seed}, {"heights", t.heights}, {"splat", t.splat}}}}, "Generate terrain");
        }
        ImGui::SameLine();
        if (ImGui::Button("Flatten all")) {
            world::Heightfield h(tc->resolution, tc->size);
            TerrainComponent t = *tc;
            h.toComponent(t);
            (void)m_session.applyComponents(e, {{"Terrain", {{"heights", t.heights}, {"splat", t.splat}}}}, "Flatten terrain");
        }
        if (const world::Heightfield* hf = m_session.world().heightfield(e))
            ImGui::TextDisabled("%d x %d samples, %.2f m spacing, %d tiles drawn", hf->resolution(), hf->resolution(), hf->spacing(),
                                m_session.world().openWorldStats().terrainTiles);
    } else if (comp == "Spline") {
        auto* sc = scene.get<SplineComponent>(e);
        if (!sc) return;
        ImGui::SeparatorText("Spline tools");
        auto pts = world::splineControlPoints(*sc);
        ImGui::TextDisabled("%zu points", pts.size());
        auto set = [&](const std::vector<Vec3>& p) {
            Json arr = Json::array();
            for (auto& v : p) arr.push_back({v.x, v.y, v.z});
            (void)m_session.setProperty(e, "Spline", "points", arr);
        };
        if (ImGui::Button(ICON_PLUS " Add point")) {
            Vec3 next = pts.empty() ? Vec3{0, 0, 0} : pts.size() == 1 ? pts[0] + Vec3{0, 0, 20} : pts.back() + normalize(pts.back() - pts[pts.size() - 2]) * 20.0f;
            pts.push_back(next);
            set(pts);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add point at view focus")) {
            Vec3 f = focusPoint(m_camPos, m_camYaw, m_camPitch, m_camDist);
            pts.push_back(scene.worldMatrix(e).inverse().transformPoint(f));
            set(pts);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pts.empty());
        if (ImGui::Button("Remove last")) {
            pts.pop_back();
            set(pts);
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Snap points to terrain")) {
            Mat4 m = scene.worldMatrix(e), inv = m.inverse();
            for (auto& p : pts) {
                Vec3 w = m.transformPoint(p);
                if (auto h = m_session.world().terrainHeight(w.x, w.z)) {
                    w.y = *h;
                    p = inv.transformPoint(w);
                }
            }
            set(pts);
        }
        if (sc->kind == "road") {
            auto g = world::RoadGraph::build(scene);
            ImGui::TextDisabled("Road graph: %zu nodes, %zu edges, %d intersections", g.nodes.size(), g.edges.size(), g.intersections());
        }
    } else if (comp == "WorldPartition") {
        if (ImGui::Button(ICON_MAP " Open the World Streaming panel")) {
            if (PanelDef* p = panel("World Streaming")) p->open = true;
            m_focusQueue.push_back("World Streaming");
        }
    } else if (comp == "Foliage") {
        auto* f = scene.get<FoliageComponent>(e);
        if (f) ImGui::TextDisabled("about %.0f instances (drawn within 300 m)", static_cast<double>(f->area) * f->area / 100.0 * f->density);
    }
}

void EditorApp::drawStreamingPanel() {
    PanelDef* pd = panel("World Streaming");
    if (!ImGui::Begin("World Streaming", pd ? &pd->open : nullptr)) {
        ImGui::End();
        return;
    }
    float s = effectiveScale();
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("Open a project.");
        ImGui::End();
        return;
    }
    Scene& scene = m_session.scene();
    GameWorld& gw = m_session.world();
    const WorldPartitionComponent* wp = world::findPartitionSettings(scene);
    Vec3 focus = focusPoint(m_camPos, m_camYaw, m_camPitch, m_camDist);
    // ---- quick actions
    bool editable = !m_session.playing();
    ImGui::BeginDisabled(!editable);
    if (!wp) {
        ImGui::TextWrapped("This scene is not an open world yet. Converting adds World Settings: the scene is then stored and cooked as a "
                           "grid of chunks and streamed around the camera while playing.");
        if (ImGui::Button(ICON_MAP " Convert scene to open world")) {
            size_t n = 0;
            if (auto st = convertSceneToOpenWorld(m_session, 128, &n); st) toast(ToastLevel::Success, std::format("Open world: {} chunks of 128 m", n));
            else toast(ToastLevel::Error, st.error);
        }
        trackRect("world.convert");
    }
    if (ImGui::Button(ICON_MAP " Terrain")) {
        TerrainComponent t;
        t.seed = 1;
        world::Heightfield h = world::Heightfield::fromComponent(t);
        h.toComponent(t);
        Json c = ComponentRegistry::get().find("Terrain")->type.toJson(&t);
        m_session.select(m_session.createEntity("Terrain", kNullEntity, {{"Terrain", c}, {"Transform", {{"position", {focus.x, 0, focus.z}}}}}));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a 256 m terrain with hills at the view focus");
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROUTE " Road")) {
        Json pts = Json::array({{focus.x - 40, focus.y, focus.z}, {focus.x, focus.y, focus.z}, {focus.x + 40, focus.y, focus.z + 10}});
        m_session.select(m_session.createEntity("Road", kNullEntity, {{"Spline", {{"points", pts}, {"kind", "road"}}}}));
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_LAYERS " Foliage")) {
        m_session.select(m_session.createEntity("Foliage", kNullEntity, {{"Foliage", {{"area", 64.0}, {"density", 1.0}}}, {"Transform", {{"position", {focus.x, 0, focus.z}}}}}));
    }
    ImGui::EndDisabled();
    if (!wp) {
        ImGui::End();
        return;
    }
    world::StreamingWorld* sw = gw.streaming();
    double cell = wp->cellSize;
    world::DVec3 origin = sw ? sw->origin() : world::DVec3{};
    // ---- stats
    if (sw) {
        const auto& st = sw->stats();
        ImGui::Text("Streaming: %d/%d chunks loaded, %d loading, %.0f / %.0f KB, apply %.2f ms (max %.2f, budget %.1f)", st.loaded, st.known, st.loading,
                    st.residentBytes / 1024.0, wp->memoryBudgetMB * 1024.0, st.applyMsLast, st.applyMsMax, wp->frameBudgetMs);
        ImGui::TextDisabled("loads %d, unloads %d, evictions %d, floating origin (%.0f, %.0f) after %d rebases", st.loadsTotal, st.unloadsTotal, st.evictions,
                            st.origin.x, st.origin.z, st.rebases);
        if (ImGui::SmallButton("Load all chunks")) sw->loadAll();
    } else {
        ImGui::TextDisabled("Cell %.0f m, load %.0f m / unload %.0f m, budget %.0f MB / %.1f ms per frame. Streaming runs while playing.", cell,
                            wp->loadRadius, wp->unloadRadius, wp->memoryBudgetMB, wp->frameBudgetMs);
    }
    ImGui::Checkbox("Chunk grid in the viewport", &m_showChunkGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Stream around the editor camera", &m_streamAtCamera);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("While playing without a game camera, chunks load around the editor camera");
    // ---- minimap
    static float metersPerPixel = 4.0f;
    ImGui::SetNextItemWidth(160 * s);
    ImGui::SliderFloat("Zoom (m/px)", &metersPerPixel, 0.5f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(avail.y, 120.0f * s);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##map", avail);
    trackRect("world.map");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, {p0.x + avail.x, p0.y + avail.y}, IM_COL32(18, 20, 24, 255));
    dl->PushClipRect(p0, {p0.x + avail.x, p0.y + avail.y}, true);
    double camX = m_camPos.x + origin.x, camZ = m_camPos.z + origin.z;
    ImVec2 centre{p0.x + avail.x * 0.5f, p0.y + avail.y * 0.5f};
    auto toScreen = [&](double x, double z) { return ImVec2{centre.x + static_cast<float>((x - camX) / metersPerPixel), centre.y + static_cast<float>((z - camZ) / metersPerPixel)}; };
    // chunk occupancy (edit mode: from root positions; play: scheduler states)
    std::map<world::ChunkCoord, int> occ;
    auto worlds = scene.computeWorldMatrices();
    for (EntityId r : scene.roots()) {
        Vec3 p = worlds[r].c[3].xyz();
        occ[world::chunkOf(p.x + origin.x, p.z + origin.z, cell)]++;
    }
    std::map<world::ChunkCoord, world::ChunkState> states;
    if (sw)
        for (auto& [c, ch] : sw->scheduler().chunks()) states[c] = ch.state;
    std::set<world::ChunkCoord> cells;
    for (auto& [c, n] : occ) cells.insert(c);
    for (auto& [c, st] : states) cells.insert(c);
    for (auto& c : cells) {
        ImVec2 a = toScreen(c.x * cell, c.z * cell), b = toScreen((c.x + 1) * cell, (c.z + 1) * cell);
        ImU32 col = IM_COL32(60, 64, 72, 255);
        if (auto it = states.find(c); it != states.end())
            col = it->second == world::ChunkState::Loaded ? IM_COL32(60, 150, 90, 255) : it->second == world::ChunkState::Loading ? IM_COL32(200, 170, 60, 255)
                                                                                                                     : IM_COL32(70, 70, 80, 255);
        dl->AddRectFilled({a.x + 1, a.y + 1}, {b.x - 1, b.y - 1}, col);
        if (auto it = occ.find(c); it != occ.end() && b.x - a.x > 18) dl->AddText({a.x + 3, a.y + 2}, IM_COL32(230, 230, 230, 200), std::to_string(it->second).c_str());
    }
    // roads
    auto graph = world::RoadGraph::build(scene);
    for (auto& e : graph.edges)
        for (size_t i = 0; i + 1 < e.polyline.size(); ++i) {
            ImVec2 a = toScreen(e.polyline[i].x + origin.x, e.polyline[i].z + origin.z), b = toScreen(e.polyline[i + 1].x + origin.x, e.polyline[i + 1].z + origin.z);
            dl->AddLine(a, b, IM_COL32(220, 220, 210, 255), std::max(1.0f, e.width / metersPerPixel));
        }
    // radii + camera
    dl->AddCircle(centre, static_cast<float>(wp->loadRadius / metersPerPixel), IM_COL32(242, 135, 106, 220), 64, 1.5f);
    dl->AddCircle(centre, static_cast<float>(wp->unloadRadius / metersPerPixel), IM_COL32(242, 135, 106, 90), 64, 1.0f);
    ImVec2 fwd{-std::sin(m_camYaw) * 12 * s, -std::cos(m_camYaw) * 12 * s};
    dl->AddTriangleFilled({centre.x + fwd.x, centre.y + fwd.y}, {centre.x - fwd.y * 0.5f, centre.y + fwd.x * 0.5f}, {centre.x + fwd.y * 0.5f, centre.y - fwd.x * 0.5f},
                          IM_COL32(255, 255, 255, 255));
    dl->PopClipRect();
    ImGui::TextDisabled("Grey: chunk (number = root entities), green: loaded, yellow: loading. Click the map to move the camera there.");
    if (ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(p0, {p0.x + avail.x, p0.y + avail.y})) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            double wx = camX + (mp.x - centre.x) * metersPerPixel, wz = camZ + (mp.y - centre.y) * metersPerPixel;
            m_camPos.x = static_cast<float>(wx - origin.x);
            m_camPos.z = static_cast<float>(wz - origin.z);
        }
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) metersPerPixel = std::clamp(metersPerPixel * (wheel > 0 ? 0.8f : 1.25f), 0.5f, 64.0f);
    }
    ImGui::End();
}

} // namespace fledge::editor
