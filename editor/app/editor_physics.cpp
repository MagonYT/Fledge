// Physics in the editor (docs/PHYSICS.md): one-click "Add physics to selection", vehicle presets, the physics debug
// view, and Simulate (physics only, in the viewport, without scripts; Stop restores, Stop & keep makes the settled
// transforms one undo step).
#include <format>

#include "editor_app.h"
#include "editor_icons.h"

namespace fledge {
Result<int> addPhysicsToSelection(EditorSession& s, const std::vector<EntityId>& entities, const std::string& motion);
EntityId createVehicleFromPreset(EditorSession& s, const std::string& preset, Vec3 position, const std::string& name);
}

namespace fledge::editor {

void EditorApp::startSimulation() {
    if (m_simulating || m_session.playing()) return;
    if (!GameWorld::sceneNeedsPhysics(m_session.scene())) {
        toast(ToastLevel::Info, "Nothing to simulate: add physics to some objects first (Edit > Add Physics to Selection)");
        return;
    }
    m_simSnapshot = m_session.scene().toJson();
    m_session.world().startPhysics();
    m_simulating = m_session.world().physics() != nullptr;
}

void EditorApp::stopSimulation(bool keep) {
    if (!m_simulating) return;
    m_simulating = false;
    std::vector<std::pair<EntityId, Transform>> settled;
    if (keep)
        for (EntityId e : m_session.scene().entities())
            if (auto* rb = m_session.scene().get<RigidBodyComponent>(e); rb && rb->motion == MotionType::Dynamic)
                settled.push_back({e, decompose(m_session.scene().worldMatrix(e))});
    m_session.world().stopPhysics();
    (void)m_session.scene().fromJson(m_simSnapshot);  // back to the edit state (simulation is not part of the history)
    m_simSnapshot = Json();
    if (keep && !settled.empty()) {
        m_session.beginEdit("Keep simulated transforms");
        for (auto& [e, t] : settled)
            if (m_session.scene().isValid(e)) (void)m_session.setWorldTransform(e, t);
        m_session.endEdit();
        toast(ToastLevel::Success, std::format("Kept the simulated transforms of {} objects (one undo step)", settled.size()));
    }
}

void EditorApp::registerPhysicsCommands() {
    CommandRegistry& R = m_commands;
    auto editable = [this] { return m_session.hasProject() && !m_session.playing() && !m_simulating; };
    R.add({"physics.add", "Add Physics to Selection", "Edit", 0, ICON_MAGNET,
           [this] {
               auto n = addPhysicsToSelection(m_session, m_session.selection, "");
               if (!n) toast(ToastLevel::Error, n.error());
               else if (*n == 0) toast(ToastLevel::Info, "The selection already has colliders (or has no meshes)");
               else toast(ToastLevel::Success, std::format("Added a fitted collider + rigid body to {} objects", *n));
           },
           [this, editable] { return editable() && !m_session.selection.empty(); }});
    auto vehicle = [this](const char* preset) {
        Vec3 at{0, 0, 0};
        if (auto v = m_session.world().viewerOverride) at = {v->x, 0, v->z - 8};
        if (auto h = m_session.world().terrainHeight(at.x, at.z)) at.y = *h;
        EntityId e = createVehicleFromPreset(m_session, preset, at, "");
        if (e) m_session.select(e);
    };
    R.add({"physics.create_car", "Create Car (drivable)", "Edit", 0, ICON_CUBES, [vehicle] { vehicle("car"); }, editable});
    R.add({"physics.create_truck", "Create Truck (drivable)", "Edit", 0, ICON_CUBES, [vehicle] { vehicle("truck"); }, editable});
    R.add({"physics.create_bike", "Create Motorbike (drivable)", "Edit", 0, ICON_CUBES, [vehicle] { vehicle("bike"); }, editable});
    R.add({"physics.simulate", "Simulate Physics (no scripts)", "Play", ImGuiMod_Alt | ImGuiKey_F5, ICON_PLAY,
           [this] {
               if (m_simulating) stopSimulation(false);
               else startSimulation();
           },
           [this] { return m_session.hasProject() && !m_session.playing(); }, [this] { return m_simulating; }});
    R.add({"physics.simulate_keep", "Stop Simulating && Keep Transforms", "Play", 0, ICON_MAGNET, [this] { stopSimulation(true); },
           [this] { return m_simulating; }});
    R.add({"physics.debug", "Physics Debug View (colliders)", "View", 0, ICON_BUG,
           [this] { m_session.world().physicsDebug = !m_session.world().physicsDebug; }, nullptr,
           [this] { return m_session.world().physicsDebug; }});
}

} // namespace fledge::editor
