// Scene Hierarchy (tree, drag-and-drop reparenting, rename), Inspector
// (reflection-driven, every edit is an undoable command) and History panels.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <format>

#include "editor_app.h"

namespace fledge::editor {

namespace {
constexpr const char* kEntityPayload = "FLEDGE_ENTITY";
}

AssetType EditorApp::assetKindFor(const std::string& propertyName) {
    if (propertyName == "mesh") return AssetType::Mesh;
    if (propertyName == "material") return AssetType::Material;
    return AssetType::Texture;
}

bool EditorApp::assetPicker(AssetType kind, std::string& picked) {
    bool chosen = false;
    if (kind != AssetType::Mesh && ImGui::MenuItem("(none)")) {
        picked.clear();
        chosen = true;
    }
    if (kind == AssetType::Mesh)
        for (const char* b : {"builtin:cube", "builtin:sphere", "builtin:plane"})
            if (ImGui::MenuItem(b)) {
                picked = b;
                chosen = true;
            }
    for (auto& r : m_session.registry().all()) {
        if (r.type != kind) continue;
        if (ImGui::MenuItem(r.path.c_str())) {
            picked = r.guid.toString();
            chosen = true;
        }
    }
    return chosen;
}

void EditorApp::drawHierarchy() {
    if (!ImGui::Begin("Scene Hierarchy", &panel("Scene Hierarchy")->open)) {
        ImGui::End();
        return;
    }
    Scene& scene = m_session.scene();
    ImGui::TextDisabled("%s (%zu)", scene.name.c_str(), scene.entities().size());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("AddEntity");
    if (ImGui::BeginPopup("AddEntity")) {
        EntityId parent = m_session.primarySelection();
        bool asChild = ImGui::GetIO().KeyShift && parent != kNullEntity;
        auto make = [&](const char* name, Json comps) {
            comps["Transform"]["position"] = asChild ? Json::array({0, 1, 0}) : Json::array({0, 0.5, 0});
            EntityId e = m_session.createEntity(name, asChild ? parent : kNullEntity, comps);
            m_session.select(e);
        };
        ImGui::TextDisabled("hold Shift to add as child of the selection");
        if (ImGui::MenuItem("Empty")) make("Entity", Json::object());
        if (ImGui::MenuItem("Cube")) make("Cube", {{"MeshRenderer", {{"mesh", "builtin:cube"}}}});
        if (ImGui::MenuItem("Sphere")) make("Sphere", {{"MeshRenderer", {{"mesh", "builtin:sphere"}}}});
        if (ImGui::MenuItem("Plane")) make("Plane", {{"MeshRenderer", {{"mesh", "builtin:plane"}}}});
        if (ImGui::MenuItem("Point Light")) make("Point Light", {{"Light", {{"type", "Point"}, {"intensity", 2.0}, {"range", 8.0}}}});
        if (ImGui::MenuItem("Camera")) make("Camera", {{"Camera", {{"primary", false}}}});
        ImGui::EndPopup();
    }
    ImGui::Separator();
    const bool large = scene.entities().size() > 200;
    if (!large) {
        for (EntityId e : scene.roots()) drawEntityNode(e);
    } else {
        // A big scene starts collapsed and only the rows on screen are built. Default-open used to create one widget
        // per entity every frame, which is the editor stall on a project the size of a full game.
        std::vector<std::pair<EntityId, int>> rows;
        std::function<void(EntityId, int)> walk = [&](EntityId e, int depth) {
            if (!scene.isValid(e)) return;
            rows.emplace_back(e, depth);
            ImGui::PushID(static_cast<int>(e));
            bool open = ImGui::GetStateStorage()->GetInt(ImGui::GetID("##row"), 0) != 0;
            ImGui::PopID();
            if (!open) return;
            for (EntityId c : scene.childrenOf(e)) walk(c, depth + 1);
        };
        for (EntityId e : scene.roots()) walk(e, 0);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()), ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                drawEntityNode(rows[static_cast<size_t>(i)].first, rows[static_cast<size_t>(i)].second, false);
    }

    // Empty space below the tree: drop here to unparent, click to clear the selection.
    ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##hierarchy_empty", {std::max(rest.x, 1.0f), std::max(rest.y, 24.0f)});
    if (ImGui::IsItemClicked()) m_session.selection.clear();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* ap = ImGui::AcceptDragDropPayload(kAssetPayload)) dropAsset(static_cast<const char*>(ap->Data), kNullEntity, std::nullopt);
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            EntityId child = *static_cast<const EntityId*>(p->Data);
            if (scene.parentOf(child) != kNullEntity) (void)m_session.setParent(child, kNullEntity);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
}

void EditorApp::drawEntityNode(EntityId e, int depth, bool recurse) {
    Scene& scene = m_session.scene();
    ImGui::PushID(static_cast<int>(e));
    const auto& children = scene.childrenOf(e);
    bool large = scene.entities().size() > 200;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!large) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (!recurse) flags |= ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (!recurse && depth > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + depth * ImGui::GetTreeNodeToLabelSpacing());
    const char* nodeId = large ? "##row" : "##node";
    if (m_session.isSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;
    bool open;
    if (m_renaming == e) {
        open = ImGui::TreeNodeEx(nodeId, flags | ImGuiTreeNodeFlags_AllowOverlap, "%s", "");
        ImGui::SameLine();
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rename", &m_renameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (!m_renameBuf.empty()) (void)m_session.rename(e, m_renameBuf);
            m_renaming = kNullEntity;
        } else if (ImGui::IsItemDeactivated()) {
            m_renaming = kNullEntity;  // focus lost / Escape: cancel
        }
    } else {
        open = ImGui::TreeNodeEx(nodeId, flags, "%s", scene.displayName(e).c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_session.select(e, ImGui::GetIO().KeyCtrl);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_renaming = e;
            m_renameBuf = scene.displayName(e);
            m_renameFocus = true;
        }
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            m_renaming = e;
            m_renameBuf = scene.displayName(e);
            m_renameFocus = true;
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kEntityPayload, &e, sizeof(EntityId));
            ImGui::Text("Move %s", scene.displayName(e).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* ap = ImGui::AcceptDragDropPayload(kAssetPayload)) dropAsset(static_cast<const char*>(ap->Data), e, std::nullopt);
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                EntityId child = *static_cast<const EntityId*>(p->Data);
                if (child != e && !scene.isAncestor(child, e)) {
                    auto st = m_session.setParent(child, e);
                    if (!st) m_status = st.error;
                } else {
                    m_status = "cannot parent an entity to itself or its descendant";
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (!m_session.isSelected(e)) m_session.select(e);
            if (ImGui::MenuItem("Add child")) {
                EntityId c = m_session.createEntity("Child", e);
                m_session.select(c);
            }
            if (ImGui::MenuItem("Rename", "F2")) {
                m_renaming = e;
                m_renameBuf = scene.displayName(e);
                m_renameFocus = true;
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) m_session.selection = m_session.duplicate(m_session.selection);
            if (ImGui::MenuItem("Unparent", nullptr, false, scene.parentOf(e) != kNullEntity)) (void)m_session.setParent(e, kNullEntity);
            if (ImGui::MenuItem("Why? (Ask the Scene)", "Ctrl+Shift+Y")) {
                m_whyEntity = e;
                m_whyOpen = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) (void)m_session.destroyEntities(m_session.selection);
            ImGui::EndPopup();
        }
    }
    if (recurse && open) {
        // copy: the tree may change while iterating (drag-drop)
        std::vector<EntityId> kids = scene.isValid(e) ? scene.childrenOf(e) : std::vector<EntityId>{};
        for (EntityId c : kids)
            if (scene.isValid(c)) drawEntityNode(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorApp::drawInspector() {
    if (!ImGui::Begin("Inspector", &panel("Inspector")->open)) {
        ImGui::End();
        return;
    }
    Scene& scene = m_session.scene();
    EntityId e = m_session.primarySelection();
    if (!scene.isValid(e)) {
        ImGui::TextDisabled("Select an entity (click in the viewport or the hierarchy).");
        ImGui::End();
        return;
    }
    if (m_session.selection.size() > 1)
        ImGui::TextColored({1, 0.75f, 0.3f, 1}, "%zu selected - editing the primary (last clicked) only", m_session.selection.size());
    EntityId parent = scene.parentOf(e);
    ImGui::TextDisabled("Entity %u | parent: %s", e, parent ? scene.displayName(parent).c_str() : "(root)");
    Transform wt = scene.worldTransform(e);
    ImGui::TextDisabled("world position: %.2f %.2f %.2f", wt.position.x, wt.position.y, wt.position.z);

    // Each widget change becomes a command; continuous drags/typing merge until the widget is released.
    auto commit = [&](const std::string& comp, const PropertyInfo& p, const Json& value) {
        auto st = m_session.setProperty(e, comp, p.name, value, std::format("insp:{}:{}:{}", e, comp, p.name));
        if (!st) m_status = st.error;
    };
    std::string toRemove;
    for (const auto& ct : ComponentRegistry::get().types()) {
        void* comp = scene.getComponent(e, ct.name);
        if (!comp) continue;
        ImGui::PushID(ct.name.c_str());
        bool open = ImGui::CollapsingHeader(ct.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (ct.removable) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 10);
            if (ImGui::SmallButton("x")) toRemove = ct.name;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove component");
        }
        if (open && ct.name == "Behaviours") {
            drawBehavioursInspector(e);
        } else if (open) {
            for (const auto& p : ct.type.properties) {
                ImGui::PushID(p.name.c_str());
                PropertyValue v = p.get(comp);
                const char* label = p.name.c_str();
                switch (p.type) {
                    case PropertyType::Bool: {
                        bool b = std::get<bool>(v);
                        if (ImGui::Checkbox(label, &b)) commit(ct.name, p, b);
                        break;
                    }
                    case PropertyType::Int: {
                        int i = std::get<int>(v);
                        if (ImGui::DragInt(label, &i)) commit(ct.name, p, i);
                        break;
                    }
                    case PropertyType::Float: {
                        float f = std::get<float>(v);
                        if (ImGui::DragFloat(label, &f, 0.05f, p.minValue, p.maxValue)) commit(ct.name, p, f);
                        break;
                    }
                    case PropertyType::Vec3: {
                        Vec3 x = std::get<Vec3>(v);
                        if (ImGui::DragFloat3(label, &x.x, 0.05f)) commit(ct.name, p, Json(x));
                        break;
                    }
                    case PropertyType::Color: {
                        Vec3 x = std::get<Vec3>(v);
                        if (ImGui::ColorEdit3(label, &x.x)) commit(ct.name, p, Json(x));
                        break;
                    }
                    case PropertyType::Quat: {
                        Vec3 eul = std::get<Quat>(v).toEuler();
                        Vec3 deg{degrees(eul.x), degrees(eul.y), degrees(eul.z)};
                        if (ImGui::DragFloat3(label, &deg.x, 0.5f)) commit(ct.name, p, Json::array({deg.x, deg.y, deg.z}));
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("pitch, yaw, roll (degrees)");
                        break;
                    }
                    case PropertyType::String:
                    case PropertyType::AssetRef: {
                        std::string s = std::get<std::string>(v);
                        if (p.type == PropertyType::String && startsWith(p.description, "[data]")) {  // bulk data: tools edit it
                            ImGui::TextDisabled("%s: %s", label, s.empty() ? "(empty)" : std::format("{:.1f} KB of data", s.size() / 1024.0).c_str());
                            break;
                        }
                        if (ct.name == "Transform" && p.name == "attachBone") {  // bone socket: the parent's bones
                            std::vector<std::string> bones = parent ? m_session.world().animation().boneNames(scene, parent) : std::vector<std::string>{};
                            if (bones.empty() && s.empty()) {
                                ImGui::BeginDisabled();
                                ImGui::InputText(label, &s);
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip("Bone socket: parent this entity under an entity with an Animator, then pick one of its bones");
                                break;
                            }
                            bool known = s.empty() || std::find(bones.begin(), bones.end(), s) != bones.end();
                            std::string preview = s.empty() ? "(none: follow the parent)" : known ? s : s + "  (not found)";
                            if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                                static std::string filter;
                                ImGui::InputTextWithHint("##bonefilter", "filter bones", &filter);
                                if (ImGui::Selectable("(none: follow the parent)", s.empty())) commit(ct.name, p, std::string());
                                for (auto& b : bones)
                                    if (filter.empty() || toLower(b).find(toLower(filter)) != std::string::npos)
                                        if (ImGui::Selectable(b.c_str(), b == s)) commit(ct.name, p, b);
                                ImGui::EndCombo();
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Bone socket: follow this bone of the parent's Animator; position / rotation / scale are an offset in the bone's space");
                            break;
                        }
                        if (ImGui::InputText(label, &s)) commit(ct.name, p, s);
                        if (p.type == PropertyType::AssetRef) {
                            if (ImGui::IsItemHovered() && !s.empty()) ImGui::SetTooltip("%s", m_session.registry().resolve(s).c_str());
                            ImGui::SameLine();
                            if (ImGui::SmallButton("...")) ImGui::OpenPopup("pick");
                            if (ImGui::BeginPopup("pick")) {
                                std::string picked;
                                if (assetPicker(assetKindFor(p.name), picked)) commit(ct.name, p, picked);
                                ImGui::EndPopup();
                            }
                            if (p.name == "material" && !s.empty()) {
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Edit")) openMaterialEditor(s);
                            }
                            if (ImGui::BeginDragDropTarget()) {  // drop an asset from the Content Browser onto the field
                                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                                    std::string rel(static_cast<const char*>(pl->Data));
                                    if (auto* rec = m_session.registry().findByPath(rel)) commit(ct.name, p, rec->guid.toString());
                                }
                                ImGui::EndDragDropTarget();
                            }
                        }
                        break;
                    }
                    case PropertyType::Json: {
                        ImGui::TextDisabled("%s: %s", label, std::get<Json>(v).dump().c_str());
                        break;
                    }
                    case PropertyType::Enum: {
                        int i = std::get<int>(v);
                        const char* preview =
                            i >= 0 && i < static_cast<int>(p.enumNames.size()) ? p.enumNames[static_cast<size_t>(i)].c_str() : "?";
                        if (ImGui::BeginCombo(label, preview)) {
                            for (size_t k = 0; k < p.enumNames.size(); ++k)
                                if (ImGui::Selectable(p.enumNames[k].c_str(), static_cast<int>(k) == i)) commit(ct.name, p, static_cast<int>(k));
                            ImGui::EndCombo();
                        }
                        break;
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) m_session.history().closeMerge();
                if (!p.description.empty() && p.type != PropertyType::Quat && p.type != PropertyType::AssetRef && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", p.description.c_str());
                ImGui::PopID();
            }
            drawComponentTools(e, ct.name);  // Terrain brushes, spline points, ... (editor_world.cpp)
        }
        ImGui::PopID();
    }
    if (!toRemove.empty()) (void)m_session.removeComponent(e, toRemove);
    ImGui::Separator();
    if (ImGui::Button("Add Component")) ImGui::OpenPopup("AddComp");
    if (ImGui::BeginPopup("AddComp")) {
        if (ImGui::BeginMenu("Script (Fledge Script behaviour)")) {
            drawAddScriptMenu(e);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        for (const auto& ct : ComponentRegistry::get().types())
            if (ct.name != "Behaviours" && !scene.getComponent(e, ct.name) && ImGui::MenuItem(ct.name.c_str())) (void)m_session.addComponent(e, ct.name);
        ImGui::EndPopup();
    }
    ImGui::End();
}

void EditorApp::drawHistory() {
    if (!ImGui::Begin("History", &panel("History")->open)) {
        ImGui::End();
        return;
    }
    auto& h = m_session.history();
    bool playing = m_session.playing();
    ImGui::BeginDisabled(playing);
    ImGui::BeginDisabled(!h.canUndo());
    if (ImGui::Button("Undo")) m_session.undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!h.canRedo());
    if (ImGui::Button("Redo")) m_session.redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) h.clear();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(playing ? "(play mode: edits are not recorded)" : "%zu entries", h.commands().size());
    ImGui::Separator();
    ImGui::BeginChild("entries");
    if (ImGui::Selectable("<scene loaded>", h.cursor() == 0) && !playing)
        while (h.canUndo()) m_session.undo();
    for (size_t i = 0; i < h.commands().size(); ++i) {
        const fledge::Command& c = h.commands()[i];
        bool applied = i < h.cursor();
        ImGui::PushID(static_cast<int>(i));
        if (!applied) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1));
        std::string text = std::format("{:3}  {}{}", i + 1, c.source == EditSource::AI ? "[AI] " : "", c.label);
        // Clicking an entry moves the history cursor to just after it.
        if (ImGui::Selectable(text.c_str(), i + 1 == h.cursor()) && !playing) {
            while (h.cursor() > i + 1 && m_session.undo()) {}
            while (h.cursor() < i + 1 && m_session.redo()) {}
        }
        if (!applied) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s | %zu entit%s | t=%.1fs", toString(c.source), c.after.size(),
                                                      c.after.size() == 1 ? "y" : "ies", c.timeSeconds);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace fledge::editor
