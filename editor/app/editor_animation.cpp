// Animation workspace: skeleton viewer, clip preview scrubber + event editor, state machine / blend tree graph
// editor, retarget map. Works on the selected entity's Animator component. See docs/ANIMATION.md.
#include <algorithm>
#include <cmath>
#include <format>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/anim/anim.h"

namespace fledge::editor {

namespace {
struct AnimUi {
    EntityId entity = kNullEntity;
    int tab = 0;
    std::string clip;
    float time = 0;
    bool playing = true, scrub = false, showSkeleton = true;
    std::string selectedState;
    int selectedTransition = -1;
    std::string newEvent = "footstep";
    std::string retargetSource = "builtin:mannequin";
    std::map<std::string, std::string> retargetMap;
    Json dragGraph;  // graph being edited while dragging a node (committed on release)
    std::string dragging;
} g_ui;
} // namespace

void EditorApp::drawAnimationPanel() {
    PanelDef* pd = panel("Animation");
    if (!ImGui::Begin("Animation", pd ? &pd->open : nullptr)) {
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
    // ---- target entity
    EntityId e = !m_session.selection.empty() && scene.get<AnimatorComponent>(m_session.selection.front()) ? m_session.selection.front() : g_ui.entity;
    if (!scene.isValid(e) || !scene.get<AnimatorComponent>(e)) e = kNullEntity;
    if (e == kNullEntity)
        for (EntityId x : scene.entities())
            if (scene.get<AnimatorComponent>(x)) {
                e = x;
                break;
            }
    g_ui.entity = e;
    if (!e) {
        ImGui::TextWrapped("No animated character in this scene. Add one: it uses the built-in mannequin (Idle / Walk / Run / Jump / Wave) until "
                           "you import a skinned glTF (Content Browser) or a BVH.");
        if (ImGui::Button(ICON_RUNNING " Add animated character") && !m_session.playing()) {
            EntityId c = m_session.createEntity("Character", kNullEntity, {{"Animator", Json::object()}});
            m_session.select(c);
        }
        trackRect("anim.add");
        ImGui::End();
        return;
    }
    AnimatorComponent* ac = scene.get<AnimatorComponent>(e);
    const anim::Character* ch = m_session.world().animation().character(ac->character);
    ImGui::Text("%s %s", ICON_RUNNING, scene.displayName(e).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("character %s%s", ac->character.c_str(), ch ? "" : " (missing!)");
    ImGui::SameLine();
    if (ImGui::Checkbox("Preview in edit mode", &ac->preview)) (void)m_session.setProperty(e, "Animator", "preview", ac->preview);
    if (!ch) {
        ImGui::End();
        return;
    }
    {  // 1.3.1: where this character is skinned while playing, and why (same as MCP get_animation_info "skinning")
        Json sk = m_session.world().skinningInfo(e);
        std::string path = sk.value("path", std::string());
        std::string line = std::format("Skinning while playing: {} - {}", path == "gpu" ? "GPU" : path == "cpu" ? "CPU" : "none",
                                       sk.value("reason", std::string()));
        ImGui::TextDisabled("%s", line.c_str());
    }
    const char* tabs[] = {"State machine", "Clips & events", "Skeleton", "Retarget"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(tabs[i], g_ui.tab == i)) g_ui.tab = i;
        trackRect(std::string("anim.tab.") + std::to_string(i));
    }
    ImGui::Separator();
    auto commitGraph = [&](const Json& j) { (void)m_session.setProperty(e, "Animator", "graph", j); };
    Json graphJ = ac->graph.is_object() && !ac->graph.empty() ? ac->graph : anim::AnimGraph::defaultLocomotion();
    anim::AnimGraph graph = anim::AnimGraph::fromJson(graphJ);
    if (graph.layers.empty()) graph.layers.push_back({});
    std::vector<std::string> clipNames;
    for (auto& c : ch->clips) clipNames.push_back(c.name);

    // clip preview only on the clips tab
    if (g_ui.tab != 1) m_session.world().animation().setClipPreview(kNullEntity, "", 0);

    if (g_ui.tab == 0) {
        // ================================================================ state machine graph
        auto& L = graph.layers[0];
        ImGui::BeginChild("##graph", {ImGui::GetContentRegionAvail().x * 0.62f, 0}, ImGuiChildFlags_Borders);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##canvas", size);
        trackRect("anim.graph");
        Json& posJ = graphJ["editor"]["pos"];
        auto nodePos = [&](size_t i) -> ImVec2 {
            const std::string& n = L.states[i].name;
            if (posJ.is_object() && posJ.contains(n)) return {origin.x + posJ[n][0].get<float>() * s, origin.y + posJ[n][1].get<float>() * s};
            return {origin.x + (40 + static_cast<float>(i % 3) * 190) * s, origin.y + (40 + static_cast<float>(i / 3) * 110) * s};
        };
        ImVec2 nodeSize{150 * s, 46 * s};
        std::string current = m_session.world().animation().animator(e) ? m_session.world().animation().animator(e)->currentState() : "";
        // transitions
        for (size_t t = 0; t < L.transitions.size(); ++t) {
            auto& tr = L.transitions[t];
            int a = L.find(tr.from), b = L.find(tr.to);
            if (b < 0) continue;
            ImVec2 pb = nodePos(static_cast<size_t>(b));
            ImVec2 pa = a >= 0 ? nodePos(static_cast<size_t>(a)) : ImVec2{origin.x + 10 * s, pb.y};
            ImVec2 ca{pa.x + nodeSize.x * 0.5f, pa.y + nodeSize.y * 0.5f}, cb{pb.x + nodeSize.x * 0.5f, pb.y + nodeSize.y * 0.5f};
            ImVec2 dir{cb.x - ca.x, cb.y - ca.y};
            float len = std::sqrt(dir.x * dir.x + dir.y * dir.y) + 1e-3f;
            ImVec2 n{-dir.y / len * 6 * s, dir.x / len * 6 * s};  // offset so A->B and B->A don't overlap
            ImVec2 p0{ca.x + n.x, ca.y + n.y}, p1{cb.x + n.x - dir.x / len * 40 * s, cb.y + n.y - dir.y / len * 26 * s};
            bool sel = g_ui.selectedTransition == static_cast<int>(t);
            ImU32 col = sel ? IM_COL32(242, 135, 106, 255) : IM_COL32(170, 170, 180, 200);
            dl->AddLine(p0, p1, col, sel ? 2.5f * s : 1.5f * s);
            ImVec2 ad{dir.x / len, dir.y / len};
            dl->AddTriangleFilled(p1, {p1.x - ad.x * 9 * s - ad.y * 5 * s, p1.y - ad.y * 9 * s + ad.x * 5 * s},
                                  {p1.x - ad.x * 9 * s + ad.y * 5 * s, p1.y - ad.y * 9 * s - ad.x * 5 * s}, col);
            ImVec2 mid{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
            if (ImGui::IsMouseClicked(0) && ImGui::IsItemHovered() && std::fabs(ImGui::GetIO().MousePos.x - mid.x) < 10 * s &&
                std::fabs(ImGui::GetIO().MousePos.y - mid.y) < 10 * s)
                g_ui.selectedTransition = static_cast<int>(t);
            dl->AddCircleFilled(mid, 4 * s, col);
        }
        // nodes
        for (size_t i = 0; i < L.states.size(); ++i) {
            const auto& st = L.states[i];
            ImVec2 p = nodePos(i);
            bool isCur = st.name == current, isSel = st.name == g_ui.selectedState, isDef = st.name == L.defaultState;
            dl->AddRectFilled(p, {p.x + nodeSize.x, p.y + nodeSize.y}, isCur ? IM_COL32(60, 120, 80, 255) : IM_COL32(45, 48, 56, 255), 6 * s);
            dl->AddRect(p, {p.x + nodeSize.x, p.y + nodeSize.y}, isSel ? IM_COL32(242, 135, 106, 255) : IM_COL32(90, 95, 105, 255), 6 * s, 0, isSel ? 2.5f : 1.0f);
            dl->AddText({p.x + 8 * s, p.y + 5 * s}, IM_COL32(240, 240, 240, 255), (st.name + (isDef ? "  (default)" : "")).c_str());
            std::string sub = st.blend == 0 ? "clip " + st.clip : st.blend == 1 ? "blend 1D: " + st.paramX : "blend 2D: " + st.paramX + "/" + st.paramY;
            dl->AddText({p.x + 8 * s, p.y + 24 * s}, IM_COL32(170, 170, 180, 255), sub.c_str());
            ImVec2 mp = ImGui::GetIO().MousePos;
            bool over = mp.x >= p.x && mp.x <= p.x + nodeSize.x && mp.y >= p.y && mp.y <= p.y + nodeSize.y;
            if (over && ImGui::IsMouseClicked(0)) {
                g_ui.selectedState = st.name;
                g_ui.selectedTransition = -1;
                g_ui.dragging = st.name;
                g_ui.dragGraph = graphJ;
            }
        }
        if (!g_ui.dragging.empty()) {  // move the node; one undo step on release
            if (ImGui::IsMouseDown(0)) {
                Json& pj = g_ui.dragGraph["editor"]["pos"];
                int idx = L.find(g_ui.dragging);
                ImVec2 cur = idx >= 0 ? nodePos(static_cast<size_t>(idx)) : ImVec2{origin.x, origin.y};
                if (pj.contains(g_ui.dragging)) cur = {origin.x + pj[g_ui.dragging][0].get<float>() * s, origin.y + pj[g_ui.dragging][1].get<float>() * s};
                ImVec2 d = ImGui::GetIO().MouseDelta;
                pj[g_ui.dragging] = {(cur.x + d.x - origin.x) / s, (cur.y + d.y - origin.y) / s};
                ac->graph = g_ui.dragGraph;  // live (no history yet)
            } else {
                Json final = g_ui.dragGraph;
                ac->graph = graphJ;  // restore, then commit as one step
                if (final != graphJ) commitGraph(final);
                g_ui.dragging.clear();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        // ---- inspector for the selection
        ImGui::BeginChild("##ginsp", {0, 0}, ImGuiChildFlags_Borders);
        bool changed = false;
        ImGui::SeparatorText("Parameters");
        for (size_t i = 0; i < graph.params.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            auto& p = graph.params[i];
            float live = m_session.world().animation().animator(e) ? m_session.world().animation().animator(e)->param(p.name) : p.value;
            ImGui::Text("%s (%s) = %.2f", p.name.c_str(), p.type.c_str(), live);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                graph.params.erase(graph.params.begin() + static_cast<long>(i));
                changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        static std::string newParam = "speed";
        static int newType = 0;
        ImGui::SetNextItemWidth(90 * s);
        ImGui::InputText("##np", &newParam);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80 * s);
        const char* types[] = {"float", "bool", "trigger"};
        ImGui::Combo("##nt", &newType, types, 3);
        ImGui::SameLine();
        if (ImGui::SmallButton("Add param") && !newParam.empty()) {
            graph.params.push_back({newParam, types[newType], 0});
            changed = true;
        }
        ImGui::SeparatorText("State");
        int si = L.find(g_ui.selectedState);
        if (si >= 0) {
            auto& st = L.states[static_cast<size_t>(si)];
            ImGui::Text("%s", st.name.c_str());
            const char* kinds[] = {"Clip", "Blend 1D", "Blend 2D"};
            int k = st.blend;
            ImGui::SetNextItemWidth(120 * s);
            if (ImGui::Combo("Motion", &k, kinds, 3)) {
                st.blend = k;
                changed = true;
            }
            if (st.blend == 0) {
                ImGui::SetNextItemWidth(160 * s);
                if (ImGui::BeginCombo("Clip", st.clip.c_str())) {
                    for (auto& n : clipNames)
                        if (ImGui::Selectable(n.c_str(), n == st.clip)) {
                            st.clip = n;
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::SetNextItemWidth(120 * s);
                changed |= ImGui::InputText("Param X", &st.paramX, ImGuiInputTextFlags_EnterReturnsTrue);
                if (st.blend == 2) {
                    ImGui::SetNextItemWidth(120 * s);
                    changed |= ImGui::InputText("Param Y", &st.paramY, ImGuiInputTextFlags_EnterReturnsTrue);
                }
                for (size_t c = 0; c < st.children.size(); ++c) {
                    ImGui::PushID(static_cast<int>(c));
                    auto& chd = st.children[c];
                    ImGui::SetNextItemWidth(100 * s);
                    if (ImGui::BeginCombo("##cc", chd.clip.c_str())) {
                        for (auto& n : clipNames)
                            if (ImGui::Selectable(n.c_str(), n == chd.clip)) {
                                chd.clip = n;
                                changed = true;
                            }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60 * s);
                    if (st.blend == 1) changed |= ImGui::InputFloat("pos", &chd.pos, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
                    else {
                        changed |= ImGui::InputFloat("x", &chd.x, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(60 * s);
                        changed |= ImGui::InputFloat("y", &chd.y, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("+ child")) {
                    st.children.push_back({clipNames.empty() ? "" : clipNames[0], 0, 0, 0});
                    changed = true;
                }
            }
            ImGui::SetNextItemWidth(80 * s);
            changed |= ImGui::InputFloat("Speed", &st.speed, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::SmallButton("Make default")) {
                L.defaultState = st.name;
                changed = true;
            }
            ImGui::SameLine();
            static int toIdx = 0;
            ImGui::SetNextItemWidth(100 * s);
            if (ImGui::BeginCombo("##to", toIdx < static_cast<int>(L.states.size()) ? L.states[static_cast<size_t>(toIdx)].name.c_str() : "")) {
                for (size_t i = 0; i < L.states.size(); ++i)
                    if (ImGui::Selectable(L.states[i].name.c_str(), static_cast<int>(i) == toIdx)) toIdx = static_cast<int>(i);
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ transition") && toIdx < static_cast<int>(L.states.size())) {
                anim::AnimGraph::Transition t;
                t.from = st.name;
                t.to = L.states[static_cast<size_t>(toIdx)].name;
                t.conditions.push_back({graph.params.empty() ? "speed" : graph.params[0].name, ">", 0.5f});
                L.transitions.push_back(t);
                g_ui.selectedTransition = static_cast<int>(L.transitions.size()) - 1;
                changed = true;
            }
            if (ImGui::SmallButton(ICON_TRASH " Delete state")) {
                std::string n = st.name;
                L.states.erase(L.states.begin() + si);
                std::erase_if(L.transitions, [&](auto& t) { return t.from == n || t.to == n; });
                g_ui.selectedState.clear();
                changed = true;
            }
        } else {
            ImGui::TextDisabled("Click a state.");
        }
        static std::string newState = "NewState";
        ImGui::SetNextItemWidth(110 * s);
        ImGui::InputText("##ns", &newState);
        ImGui::SameLine();
        if (ImGui::SmallButton("+ state") && L.find(newState) < 0) {
            anim::AnimGraph::State st;
            st.name = newState;
            st.clip = clipNames.empty() ? "" : clipNames[0];
            L.states.push_back(st);
            g_ui.selectedState = newState;
            changed = true;
        }
        trackRect("anim.addstate");
        if (g_ui.selectedTransition >= 0 && g_ui.selectedTransition < static_cast<int>(L.transitions.size())) {
            auto& t = L.transitions[static_cast<size_t>(g_ui.selectedTransition)];
            ImGui::SeparatorText(("Transition " + t.from + " -> " + t.to).c_str());
            ImGui::SetNextItemWidth(80 * s);
            changed |= ImGui::InputFloat("Duration", &t.duration, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SetNextItemWidth(80 * s);
            changed |= ImGui::InputFloat("Exit time (-1 off)", &t.exitTime, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
            int curve = t.curve == "ease" ? 1 : 0;
            const char* curves[] = {"linear", "ease"};
            ImGui::SetNextItemWidth(80 * s);
            if (ImGui::Combo("Curve", &curve, curves, 2)) {
                t.curve = curves[curve];
                changed = true;
            }
            for (size_t c = 0; c < t.conditions.size(); ++c) {
                ImGui::PushID(static_cast<int>(c));
                auto& cd = t.conditions[c];
                ImGui::SetNextItemWidth(80 * s);
                changed |= ImGui::InputText("##p", &cd.param, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                const char* ops[] = {">", "<", ">=", "<=", "==", "!=", "true", "false", "trigger"};
                int oi = 0;
                for (int k = 0; k < 9; ++k)
                    if (cd.op == ops[k]) oi = k;
                ImGui::SetNextItemWidth(70 * s);
                if (ImGui::Combo("##op", &oi, ops, 9)) {
                    cd.op = ops[oi];
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60 * s);
                changed |= ImGui::InputFloat("##v", &cd.value, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    t.conditions.erase(t.conditions.begin() + static_cast<long>(c));
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ condition")) {
                t.conditions.push_back({graph.params.empty() ? "speed" : graph.params[0].name, ">", 0});
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_TRASH " Delete transition")) {
                L.transitions.erase(L.transitions.begin() + g_ui.selectedTransition);
                g_ui.selectedTransition = -1;
                changed = true;
            }
        }
        if (changed && !m_session.playing()) {
            Json out = graph.toJson();
            out["editor"] = graphJ.value("editor", Json::object());
            commitGraph(out);
        }
        ImGui::EndChild();
    } else if (g_ui.tab == 1) {
        // ================================================================ clips & events
        if (g_ui.clip.empty() || !ch->clip(g_ui.clip)) g_ui.clip = clipNames.empty() ? "" : clipNames[0];
        const anim::Clip* clip = ch->clip(g_ui.clip);
        ImGui::SetNextItemWidth(160 * s);
        if (ImGui::BeginCombo("Clip", g_ui.clip.c_str())) {
            for (auto& n : clipNames)
                if (ImGui::Selectable(n.c_str(), n == g_ui.clip)) {
                    g_ui.clip = n;
                    g_ui.time = 0;
                }
            ImGui::EndCombo();
        }
        trackRect("anim.clip");
        if (!clip) {
            ImGui::End();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button(g_ui.playing ? ICON_PAUSE : ICON_PLAY)) g_ui.playing = !g_ui.playing;
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f s, %s, %zu keys (compressed), %zu tracks", clip->duration, clip->loop ? "loop" : "once", clip->keyCount(), clip->tracks.size());
        if (g_ui.playing) {
            g_ui.time += ImGui::GetIO().DeltaTime;
            if (g_ui.time > clip->duration) g_ui.time = clip->loop ? std::fmod(g_ui.time, clip->duration) : clip->duration;
        }
        // timeline with events (clip + authored)
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 46 * s;
        ImGui::InvisibleButton("##timeline", {w, h});
        trackRect("anim.timeline");
        if (ImGui::IsItemActive()) {
            g_ui.time = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / w * clip->duration, 0.0f, clip->duration);
            g_ui.playing = false;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, {p0.x + w, p0.y + h}, ImGui::GetColorU32(ImGuiCol_FrameBg), 4 * s);
        for (int i = 0; i <= 10; ++i) {
            float x = p0.x + w * static_cast<float>(i) / 10.0f;
            dl->AddLine({x, p0.y + h - 10 * s}, {x, p0.y + h}, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
        auto evX = [&](float t) { return p0.x + w * t / std::max(1e-4f, clip->duration); };
        for (auto& ev : clip->events) {
            dl->AddTriangleFilled({evX(ev.time), p0.y + 4 * s}, {evX(ev.time) - 5 * s, p0.y + 14 * s}, {evX(ev.time) + 5 * s, p0.y + 14 * s}, IM_COL32(120, 180, 255, 255));
            dl->AddText({evX(ev.time) + 6 * s, p0.y + 2 * s}, IM_COL32(150, 190, 255, 255), ev.name.c_str());
        }
        auto& authored = graph.events[g_ui.clip];
        for (auto& ev : authored) {
            dl->AddTriangleFilled({evX(ev.time), p0.y + 18 * s}, {evX(ev.time) - 5 * s, p0.y + 28 * s}, {evX(ev.time) + 5 * s, p0.y + 28 * s}, IM_COL32(242, 135, 106, 255));
            dl->AddText({evX(ev.time) + 6 * s, p0.y + 16 * s}, IM_COL32(242, 160, 130, 255), ev.name.c_str());
        }
        dl->AddLine({evX(g_ui.time), p0.y}, {evX(g_ui.time), p0.y + h}, IM_COL32(255, 255, 255, 230), 2 * s);
        ImGui::TextDisabled("t = %.2f s   blue: events in the clip   orange: events you added (stored in the Animator graph)", g_ui.time);
        ImGui::SetNextItemWidth(140 * s);
        ImGui::InputText("##evname", &g_ui.newEvent);
        ImGui::SameLine();
        bool evChanged = false;
        if (ImGui::Button("Add event at playhead") && !g_ui.newEvent.empty()) {
            authored.push_back({g_ui.time, g_ui.newEvent});
            evChanged = true;
        }
        trackRect("anim.addevent");
        for (size_t i = 0; i < authored.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BulletText("%.2f s  %s", authored[i].time, authored[i].name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                authored.erase(authored.begin() + static_cast<long>(i));
                evChanged = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (evChanged) {
            if (authored.empty()) graph.events.erase(g_ui.clip);
            Json out = graph.toJson();
            out["editor"] = graphJ.value("editor", Json::object());
            commitGraph(out);
        }
        if (!m_session.playing()) m_session.world().animation().setClipPreview(e, g_ui.clip, g_ui.time);
    } else if (g_ui.tab == 2) {
        // ================================================================ skeleton
        ImGui::Checkbox("Draw the skeleton in the viewport", &g_ui.showSkeleton);
        ImGui::TextDisabled("%zu bones, %zu vertices, %zu clips", ch->skeleton.bones.size(), ch->mesh.vertices.size(), ch->clips.size());
        ImGui::BeginChild("##bones");
        std::function<void(int, int)> tree = [&](int b, int depth) {
            ImGui::Indent(static_cast<float>(depth) * 0 + 12 * s);
            ImGui::BulletText("%s", ch->skeleton.bones[static_cast<size_t>(b)].name.c_str());
            for (size_t c = 0; c < ch->skeleton.bones.size(); ++c)
                if (ch->skeleton.bones[c].parent == b) tree(static_cast<int>(c), depth + 1);
            ImGui::Unindent(12 * s);
        };
        for (size_t b = 0; b < ch->skeleton.bones.size(); ++b)
            if (ch->skeleton.bones[b].parent < 0) tree(static_cast<int>(b), 0);
        ImGui::EndChild();
    } else {
        // ================================================================ retarget
        ImGui::TextWrapped("Copy clips from another character (e.g. an imported mocap BVH or a glTF) onto this one. Bones are matched by name "
                           "(Mixamo / Bip01 prefixes ignored); change any row below.");
        ImGui::SetNextItemWidth(260 * s);
        ImGui::InputText("Source character", &g_ui.retargetSource);
        const anim::Character* src = m_session.world().animation().character(g_ui.retargetSource);
        if (!src) {
            ImGui::TextColored({1, 0.45f, 0.45f, 1}, "Source not found (builtin:mannequin or a .fchar path / GUID)");
        } else {
            if (ImGui::Button("Auto-map bones")) g_ui.retargetMap = anim::autoBoneMap(src->skeleton, ch->skeleton);
            trackRect("anim.automap");
            ImGui::SameLine();
            ImGui::TextDisabled("%zu of %zu bones mapped", g_ui.retargetMap.size(), ch->skeleton.bones.size());
            ImGui::BeginChild("##map", {0, ImGui::GetContentRegionAvail().y - 40 * s}, ImGuiChildFlags_Borders);
            for (auto& b : ch->skeleton.bones) {
                ImGui::PushID(b.name.c_str());
                std::string cur = g_ui.retargetMap.count(b.name) ? g_ui.retargetMap[b.name] : "(none)";
                ImGui::Text("%s", b.name.c_str());
                ImGui::SameLine(180 * s);
                ImGui::SetNextItemWidth(180 * s);
                if (ImGui::BeginCombo("##src", cur.c_str())) {
                    if (ImGui::Selectable("(none)")) g_ui.retargetMap.erase(b.name);
                    for (auto& sb : src->skeleton.bones)
                        if (ImGui::Selectable(sb.name.c_str(), sb.name == cur)) g_ui.retargetMap[b.name] = sb.name;
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (ImGui::Button(ICON_COPY " Retarget all clips into a new character") && !g_ui.retargetMap.empty()) {
                anim::Character out = *ch;
                for (auto& c : src->clips) {
                    anim::Clip rc = anim::retarget(c, src->skeleton, ch->skeleton, g_ui.retargetMap);
                    rc.name = (ch->clip(rc.name) ? "Retargeted " : "") + rc.name;
                    out.clips.push_back(std::move(rc));
                }
                std::string name = (ch->name.empty() ? std::string("Character") : ch->name) + "_retargeted";
                std::string rel = "Characters/" + name + ".fchar";
                std::string text = out.toJson().dump();
                if (m_session.vfs().writeText("content:/" + rel, text)) {
                    m_session.registry().add(rel, AssetType::Other);
                    m_session.registry().save(m_session.vfs());
                    (void)m_session.setProperty(e, "Animator", "character", "content:/" + rel);
                    toast(ToastLevel::Success, std::format("{} with {} clips -> {}", name, out.clips.size(), rel));
                } else {
                    toast(ToastLevel::Error, "Cannot write " + rel);
                }
            }
        }
    }
    // skeleton overlay in the viewport
    if (g_ui.showSkeleton && m_renderer) {
        auto bones = m_session.world().animation().boneMatrices(e);
        Mat4 w = scene.worldMatrix(e);
        for (size_t b = 0; b < bones.size() && b < ch->skeleton.bones.size(); ++b) {
            int p = ch->skeleton.bones[b].parent;
            if (p < 0) continue;
            m_renderer->addLine(w.transformPoint(bones[static_cast<size_t>(p)].c[3].xyz()), w.transformPoint(bones[b].c[3].xyz()), {1.0f, 0.85f, 0.2f});
        }
    }
    ImGui::End();
}

} // namespace fledge::editor
