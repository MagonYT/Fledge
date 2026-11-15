// Gameplay panels (docs/GAMEPLAY.md): the visual mission graph editor ("Missions") and "Gameplay" (one-click
// player / city / traffic / day-night / weather setup, live stats, navmesh view).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>

#include <imgui.h>

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/gameplay/mission.h"
#include "fledge/gameplay/traffic.h"

namespace fledge {
EntityId createTree(EditorSession& s, const Json& node, EntityId parent);
Result<EntityId> addCityDistrict(EditorSession& s, const Json& params);
EntityId makePlayer(EditorSession& s, EntityId e, Vec3 at);
} // namespace fledge

namespace fledge::editor {

namespace {
struct MissionUi {
    EntityId entity = kNullEntity;
    int selected = 0;
    int linkFrom = 0;
    bool linkFail = false;
    int dragging = 0;
    ImVec2 dragStart;
    Json dragGraph;
} g_mui;

Json starterMission() {
    return {{"name", "New Mission"},
            {"nodes", Json::array({{{"id", 1}, {"type", "Start"}, {"x", 30}, {"y", 60}},
                                   {{"id", 2}, {"type", "GoTo"}, {"target", "Player"}, {"radius", 5}, {"text", "Go somewhere"}, {"x", 220}, {"y", 60}},
                                   {{"id", 3}, {"type", "End"}, {"success", true}, {"text", "Mission complete"}, {"reward", 100}, {"x", 410}, {"y", 60}}})},
            {"links", Json::array({{{"from", 1}, {"to", 2}}, {{"from", 2}, {"to", 3}}})}};
}
ImU32 nodeColor(const std::string& t) {
    if (t == "Start") return IM_COL32(60, 140, 80, 255);
    if (t == "End") return IM_COL32(150, 60, 60, 255);
    if (t == "Timer" || t == "SetWanted" || t == "LoseWanted") return IM_COL32(150, 110, 40, 255);
    if (t == "Dialogue") return IM_COL32(90, 80, 150, 255);
    return IM_COL32(55, 85, 130, 255);
}
} // namespace

void EditorApp::drawMissionsPanel() {
    PanelDef* pd = panel("Missions");
    ImGui::SetNextWindowSize({900 * effectiveScale(), 460 * effectiveScale()}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Missions", pd ? &pd->open : nullptr)) {
        ImGui::End();
        return;
    }
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("Open a project.");
        ImGui::End();
        return;
    }
    float s = effectiveScale();
    Scene& scene = m_session.scene();
    std::vector<EntityId> missions;
    for (EntityId e : scene.entities())
        if (scene.get<MissionComponent>(e)) missions.push_back(e);
    EntityId sel = m_session.primarySelection();
    if (sel && scene.get<MissionComponent>(sel)) g_mui.entity = sel;
    if (g_mui.entity && !scene.isValid(g_mui.entity)) g_mui.entity = kNullEntity;
    if (!g_mui.entity && !missions.empty()) g_mui.entity = missions.front();
    // mission picker + add
    ImGui::SetNextItemWidth(200 * s);
    if (ImGui::BeginCombo("##mission", g_mui.entity ? scene.displayName(g_mui.entity).c_str() : "(no mission)")) {
        for (EntityId e : missions)
            if (ImGui::Selectable(scene.displayName(e).c_str(), e == g_mui.entity)) g_mui.entity = e;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_PLUS " New mission") && !m_session.playing()) {
        EntityId e = m_session.createEntity("Mission", kNullEntity, {{"Mission", {{"graph", starterMission()}}}});
        if (e) {
            g_mui.entity = e;
            m_session.select(e);
        }
    }
    trackRect("missions.new");
    if (!g_mui.entity) {
        ImGui::TextWrapped("A mission is a graph of objectives: go somewhere, get in a car, drive, collect, wait, talk, lose the cops... "
                           "Press New mission to start from a small example.");
        ImGui::End();
        return;
    }
    EntityId me = g_mui.entity;
    auto* mc = scene.get<MissionComponent>(me);
    Json graph = mc->graph.is_object() && mc->graph.contains("nodes") ? mc->graph : starterMission();
    gameplay::MissionGraph mg = gameplay::MissionGraph::fromJson(graph);
    auto commit = [&](const Json& g, const char* label) { (void)m_session.setProperty(me, "Mission", "graph", g, std::string("mission.") + label); };
    int live = m_session.playing() ? m_session.world().missionNode(me) : 0;
    ImGui::SameLine();
    ImGui::TextDisabled("%s  %s", mc->state.c_str(), mc->objective.c_str());
    auto problems = mg.validate();
    if (!problems.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.7f, 0.3f, 1));
        ImGui::TextWrapped("%s %s%s", ICON_WARNING, problems.front().c_str(), problems.size() > 1 ? std::format("  (+{} more)", problems.size() - 1).c_str() : "");
        ImGui::PopStyleColor();
    }
    // ---------------------------------------------------------------- canvas
    ImVec2 availAll = ImGui::GetContentRegionAvail();
    bool narrow = availAll.x < 560 * s;  // docked in a thin column: graph on top, node editor below
    ImGui::BeginChild("##mgraph", narrow ? ImVec2{0, std::max(160 * s, availAll.y * 0.5f)} : ImVec2{availAll.x - 280 * s, 0}, ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##mcanvas", size);
    trackRect("missions.graph");
    bool canvasHovered = ImGui::IsItemHovered();
    ImVec2 nodeSize{150 * s, 50 * s};
    auto posOf = [&](const gameplay::MissionNode& n) {
        return ImVec2{origin.x + n.params.value("x", 30.0f) * s, origin.y + n.params.value("y", 60.0f) * s};
    };
    for (auto& l : mg.links) {
        const auto *a = mg.node(l.from), *b = mg.node(l.to);
        if (!a || !b) continue;
        ImVec2 pa = posOf(*a), pb = posOf(*b);
        ImVec2 p0{pa.x + nodeSize.x, pa.y + nodeSize.y * (l.onFail ? 0.8f : 0.5f)}, p1{pb.x, pb.y + nodeSize.y * 0.5f};
        ImU32 col = l.onFail ? IM_COL32(220, 90, 70, 230) : IM_COL32(180, 180, 190, 220);
        dl->AddBezierCubic(p0, {p0.x + 50 * s, p0.y}, {p1.x - 50 * s, p1.y}, p1, col, 2 * s);
        dl->AddTriangleFilled(p1, {p1.x - 9 * s, p1.y - 5 * s}, {p1.x - 9 * s, p1.y + 5 * s}, col);
    }
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hoveredNode = 0;
    for (auto& n : mg.nodes) {
        ImVec2 p = posOf(n);
        bool hov = mouse.x >= p.x && mouse.x <= p.x + nodeSize.x && mouse.y >= p.y && mouse.y <= p.y + nodeSize.y && canvasHovered;
        if (hov) hoveredNode = n.id;
        ImU32 fill = nodeColor(n.type);
        dl->AddRectFilled(p, {p.x + nodeSize.x, p.y + nodeSize.y}, fill, 6 * s);
        if (n.id == live) dl->AddRect({p.x - 3 * s, p.y - 3 * s}, {p.x + nodeSize.x + 3 * s, p.y + nodeSize.y + 3 * s}, IM_COL32(90, 230, 120, 255), 8 * s, 0, 3 * s);
        if (n.id == g_mui.selected) dl->AddRect(p, {p.x + nodeSize.x, p.y + nodeSize.y}, IM_COL32(242, 135, 106, 255), 6 * s, 0, 2.5f * s);
        dl->AddText({p.x + 8 * s, p.y + 6 * s}, IM_COL32_WHITE, std::format("{}  #{}", n.type, n.id).c_str());
        std::string sub = n.params.value("text", n.params.value("target", std::string()));
        if (sub.size() > 22) sub = sub.substr(0, 21) + "...";
        dl->AddText({p.x + 8 * s, p.y + 26 * s}, IM_COL32(220, 220, 220, 200), sub.c_str());
    }
    if (g_mui.linkFrom) {
        if (const auto* a = mg.node(g_mui.linkFrom)) {
            ImVec2 pa = posOf(*a);
            dl->AddLine({pa.x + nodeSize.x, pa.y + nodeSize.y * 0.5f}, mouse, g_mui.linkFail ? IM_COL32(220, 90, 70, 200) : IM_COL32(200, 200, 200, 200), 2 * s);
        }
    }
    // interaction: click selects / finishes a link, drag moves (one undo step per drag)
    if (canvasHovered && ImGui::IsMouseClicked(0)) {
        if (g_mui.linkFrom && hoveredNode && hoveredNode != g_mui.linkFrom && !m_session.playing()) {
            Json g = mg.toJson();
            Json link = {{"from", g_mui.linkFrom}, {"to", hoveredNode}};
            if (g_mui.linkFail) link["on"] = "fail";
            Json links = Json::array();  // one success / one fail link per node
            for (auto& l : g["links"])
                if (!(l.value("from", 0) == g_mui.linkFrom && (l.value("on", std::string()) == "fail") == g_mui.linkFail)) links.push_back(l);
            links.push_back(link);
            g["links"] = links;
            commit(g, "link");
            g_mui.linkFrom = 0;
        } else {
            g_mui.linkFrom = 0;
            g_mui.selected = hoveredNode;
            if (hoveredNode && !m_session.playing()) {
                g_mui.dragging = hoveredNode;
                g_mui.dragStart = mouse;
                g_mui.dragGraph = mg.toJson();
            }
        }
    }
    if (g_mui.dragging && ImGui::IsMouseDown(0)) {
        ImVec2 d{(mouse.x - g_mui.dragStart.x) / s, (mouse.y - g_mui.dragStart.y) / s};
        Json g = g_mui.dragGraph;
        for (auto& n : g["nodes"])
            if (n.value("id", 0) == g_mui.dragging) {
                n["x"] = std::round(n.value("x", 30.0f) + d.x);
                n["y"] = std::round(n.value("y", 60.0f) + d.y);
            }
        mc->graph = g;  // live preview; committed on release
    } else if (g_mui.dragging) {
        Json moved = mc->graph;
        mc->graph = g_mui.dragGraph;
        if (moved != g_mui.dragGraph) commit(moved, "move");
        g_mui.dragging = 0;
    }
    ImGui::EndChild();
    if (!narrow) ImGui::SameLine();
    // ---------------------------------------------------------------- side panel
    ImGui::BeginChild("##mside", {0, 0});
    ImGui::BeginDisabled(m_session.playing());
    static int addType = 1;
    const auto& types = gameplay::MissionGraph::nodeTypes();
    ImGui::SetNextItemWidth(130 * s);
    if (ImGui::BeginCombo("##addtype", types[static_cast<size_t>(addType)].c_str())) {
        for (size_t i = 0; i < types.size(); ++i)
            if (ImGui::Selectable(types[i].c_str(), static_cast<int>(i) == addType)) addType = static_cast<int>(i);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_PLUS " Add node")) {
        Json g = mg.toJson();
        int id = 1;
        for (auto& n : mg.nodes) id = std::max(id, n.id + 1);
        float x = 30, y = 200;
        if (const auto* sn = mg.node(g_mui.selected)) {
            x = sn->params.value("x", 30.0f) + 190;
            y = sn->params.value("y", 60.0f);
        }
        Json node = {{"id", id}, {"type", types[static_cast<size_t>(addType)]}, {"x", x}, {"y", y}};
        g["nodes"].push_back(node);
        if (g_mui.selected && !mg.next(g_mui.selected, false) && mg.node(g_mui.selected) && mg.node(g_mui.selected)->type != "End")
            g["links"].push_back({{"from", g_mui.selected}, {"to", id}});  // chain after the selected node
        commit(g, "add");
        g_mui.selected = id;
    }
    trackRect("missions.addnode");
    if (const auto* n = mg.node(g_mui.selected)) {
        ImGui::Separator();
        ImGui::Text("%s #%d", n->type.c_str(), n->id);
        Json params = n->params;
        bool changed = false;
        auto textField = [&](const char* label, const char* key) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", params.value(key, std::string()).c_str());
            if (ImGui::InputText(label, buf, sizeof(buf))) {  // each keystroke merges into one undo step
                params[key] = std::string(buf);
                changed = true;
            }
        };
        auto numField = [&](const char* label, const char* key, float def, float mn, float mx) {
            float v = params.value(key, def);
            if (ImGui::DragFloat(label, &v, 0.5f, mn, mx)) {
                params[key] = v;
                changed = true;
            }
        };
        const std::string& t = n->type;
        if (t != "Start") textField("text", "text");
        if (t == "GoTo" || t == "DriveTo" || t == "EnterVehicle" || t == "Eliminate") {
            textField("target", "target");
            if (ImGui::BeginPopupContextItem("pick")) {
                for (EntityId e : scene.entities())
                    if (scene.parentOf(e) == kNullEntity && ImGui::Selectable(scene.displayName(e).c_str())) {
                        params["target"] = scene.displayName(e);
                        changed = true;
                    }
                ImGui::EndPopup();
            }
            ImGui::TextDisabled("right-click target for a list");
        }
        if (t == "GoTo" || t == "DriveTo") numField("radius", "radius", 5, 0.5f, 200);
        if (t == "Wait" || t == "Dialogue" || t == "Timer") numField("seconds", "seconds", t == "Timer" ? 60.0f : 3.0f, 0.1f, 3600);
        if (t == "Collect" || t == "Reward") {
            textField("item", "item");
            numField("count", "count", 1, 1, 1000);
        }
        if (t == "Reward") numField("money", "money", 0, -100000, 1000000);
        if (t == "SetWanted") numField("level", "level", 1, 0, 5);
        if (t == "End") {
            bool ok = params.value("success", true);
            if (ImGui::Checkbox("success", &ok)) {
                params["success"] = ok;
                changed = true;
            }
            numField("reward", "reward", 0, 0, 1000000);
        }
        if (changed) {
            Json g = mg.toJson();
            for (auto& gn : g["nodes"])
                if (gn.value("id", 0) == n->id) {
                    for (auto& [k, v] : params.items()) gn[k] = v;
                    for (const char* k : {"count", "money", "level", "reward"})  // integers
                        if (gn.contains(k) && gn[k].is_number()) gn[k] = static_cast<int>(std::lround(gn[k].get<double>()));
                }
            commit(g, "edit");
        }
        if (ImGui::Button(ICON_ARROW_RIGHT " Link to...")) {
            g_mui.linkFrom = n->id;
            g_mui.linkFail = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Fail link to...")) {
            g_mui.linkFrom = n->id;
            g_mui.linkFail = true;
        }
        if (g_mui.linkFrom) ImGui::TextDisabled("click the target node");
        if (ImGui::Button(ICON_TRASH " Delete node")) {
            Json g = mg.toJson(), nodes = Json::array(), links = Json::array();
            for (auto& gn : g["nodes"])
                if (gn.value("id", 0) != n->id) nodes.push_back(gn);
            for (auto& l : g["links"])
                if (l.value("from", 0) != n->id && l.value("to", 0) != n->id) links.push_back(l);
            g["nodes"] = nodes;
            g["links"] = links;
            commit(g, "delete");
            g_mui.selected = 0;
        }
    } else {
        ImGui::TextDisabled("Select a node to edit it.");
    }
    ImGui::EndDisabled();
    if (problems.size() > 1) {
        ImGui::Separator();
        for (auto& p : problems) ImGui::BulletText("%s", p.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

void EditorApp::drawGameplayPanel() {
    PanelDef* pd = panel("Gameplay");
    ImGui::SetNextWindowSize({520 * effectiveScale(), 360 * effectiveScale()}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Gameplay", pd ? &pd->open : nullptr)) {
        ImGui::End();
        return;
    }
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("Open a project.");
        ImGui::End();
        return;
    }
    float s = effectiveScale();
    Scene& scene = m_session.scene();
    GameWorld& w = m_session.world();
    bool editable = !m_session.playing();
    EntityId settings = kNullEntity;
    for (EntityId e : scene.entities())
        if (scene.get<TrafficComponent>(e) || scene.get<TimeOfDayComponent>(e) || scene.get<WeatherComponent>(e)) settings = e;
    if (!settings) settings = scene.findByName("World Settings");
    auto ensureSettings = [&]() {
        if (!settings) settings = m_session.createEntity("World Settings");
        return settings;
    };
    ImGui::SeparatorText("Setup (one click each)");
    ImGui::BeginDisabled(!editable);
    if (ImGui::Button(ICON_MAP " Add city district")) {
        Vec3 at = w.viewerOverride.value_or(Vec3{0, 0, 0});
        auto r = addCityDistrict(m_session, {{"blocksX", 4}, {"blocksZ", 4}, {"origin", {std::floor(at.x / 64) * 64 - 128, 0, std::floor(at.z / 64) * 64 - 128}}});
        if (r) toast(ToastLevel::Success, "Added a 4 x 4 block district (roads, buildings, parks)");
    }
    trackRect("gameplay.city");
    ImGui::SameLine();
    EntityId player = w.player();
    if (ImGui::Button(player ? ICON_USER " Player: OK" : ICON_USER " Make player")) {
        EntityId sel = m_session.primarySelection();
        Vec3 at = w.viewerOverride.value_or(Vec3{0, 0, 0});
        m_session.select(makePlayer(m_session, sel && !scene.get<VehicleComponent>(sel) ? sel : kNullEntity, {at.x, 0, at.z}));
    }
    trackRect("gameplay.player");
    auto* tc = settings ? scene.get<TrafficComponent>(settings) : nullptr;
    if (!tc && ImGui::Button(ICON_CUBES " Add traffic")) (void)m_session.applyComponents(ensureSettings(), {{"Traffic", Json::object()}}, "Add traffic");
    if (tc) {
        bool en = tc->enabled;
        if (ImGui::Checkbox("Traffic", &en)) (void)m_session.setProperty(settings, "Traffic", "enabled", en);
        ImGui::SameLine();
        int cars = tc->cars, peds = tc->pedestrians;
        ImGui::SetNextItemWidth(90 * s);
        if (ImGui::SliderInt("cars", &cars, 0, 200)) (void)m_session.setProperty(settings, "Traffic", "cars", cars, "gp.cars");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * s);
        if (ImGui::SliderInt("pedestrians", &peds, 0, 300)) (void)m_session.setProperty(settings, "Traffic", "pedestrians", peds, "gp.peds");
    }
    auto* tod = settings ? scene.get<TimeOfDayComponent>(settings) : nullptr;
    if (!tod && ImGui::Button(ICON_SUN " Add day / night")) (void)m_session.applyComponents(ensureSettings(), {{"TimeOfDay", Json::object()}}, "Add day/night");
    if (tod) {
        float h = tod->hour, dm = tod->dayMinutes;
        ImGui::SetNextItemWidth(160 * s);
        if (ImGui::SliderFloat("hour", &h, 0, 24, "%.1f h")) (void)m_session.setProperty(settings, "TimeOfDay", "hour", h, "gp.hour");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * s);
        if (ImGui::DragFloat("min / day", &dm, 0.5f, 0, 1440, "%.0f")) (void)m_session.setProperty(settings, "TimeOfDay", "dayMinutes", dm, "gp.day");
    }
    trackRect("gameplay.time");
    auto* wc = settings ? scene.get<WeatherComponent>(settings) : nullptr;
    const char* presets[] = {"Clear", "Cloudy", "Overcast", "Rain", "Storm", "Fog"};
    int cur = wc ? static_cast<int>(wc->preset) : 0;
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::Combo("weather", &cur, presets, 6)) (void)m_session.applyComponents(ensureSettings(), {{"Weather", {{"preset", presets[cur]}}}}, "Set weather");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Live");
    if (!m_session.playing()) {
        ImGui::TextDisabled("Press Play: traffic, police, missions and the HUD run while playing.");
    } else {
        auto st = w.gameplayStats();
        ImGui::Text("traffic %d cars / %d pedestrians   police %d   wanted %d", st.trafficCars, st.pedestrians, st.police, st.wanted);
        if (auto* t = w.traffic())
            ImGui::Text("junction crossings %d   conflicts %d   stuck %d   mean %.1f m/s", t->stats().junctionCrossings, t->stats().junctionConflicts,
                        t->stats().respawned, t->stats().avgSpeed);
        ImGui::Text("speed %.0f km/h   objective: %s", st.speedKmh, st.objective.c_str());
        ImGui::Text("gameplay logic %.2f ms", st.ms);
        ImGui::Checkbox("Show navmesh", &w.navDebug);
        if (w.navDebug && !w.navMesh()) ImGui::TextDisabled("(no navmesh: needs static colliders, terrain or roads)");
    }
    ImGui::End();
}

} // namespace fledge::editor
