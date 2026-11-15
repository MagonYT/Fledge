#include "fledge/editor/time_machine.h"

#include "fledge/core/log.h"
#include "fledge/editor/editor_session.h"

namespace fledge {

void TimeMachine::reset() {
    m_snaps.clear();
    m_inputs.clear();
    m_base = m_current = 0;
    m_bytes = 0;
    m_branches = 0;
}

TimeMachine::Snap TimeMachine::capture(EditorSession& s, int64_t step) const {
    Snap sn;
    sn.step = step;
    sn.scene = s.scene().toJson();
    sn.scripts = Json::array();
    auto& scripts = s.world().scripts();
    for (EntityId e : s.scene().entities())
        for (auto& cls : scripts.liveClasses(e))
            if (auto props = scripts.liveProperties(e, cls)) sn.scripts.push_back({{"entity", e}, {"class", cls}, {"props", *props}});
    sn.physics = s.world().savePhysicsState();
    sn.bytes = sn.scene.dump().size() + sn.scripts.dump().size() + sn.physics.size();
    return sn;
}

void TimeMachine::restore(EditorSession& s, const Snap& snap) {
    auto& world = s.world();
    // Script objects are rebuilt from the restored scene, then get their live property values back.
    world.endPlay();
    (void)s.scene().fromJson(snap.scene);
    world.beginPlay();
    if (!snap.physics.empty() && !world.restorePhysicsState(snap.physics))
        FLEDGE_LOG_WARN("timemachine", "physics state could not be restored exactly (bodies changed); continuing from the scene state");
    for (auto& p : snap.scripts) {
        EntityId e = p.value("entity", kNullEntity);
        std::string cls = p.value("class", "");
        if (!p.contains("props") || !p["props"].is_object()) continue;
        for (auto& [k, v] : p["props"].items()) (void)world.scripts().setLiveProperty(e, cls, k, v);
    }
}

void TimeMachine::beginPlay(EditorSession& s) {
    reset();
    if (!enabled) return;
    Snap sn = capture(s, 0);
    m_bytes += sn.bytes;
    m_snaps.push_back(std::move(sn));
}

void TimeMachine::afterStep(EditorSession& s, const script::InputFrame& input) {
    if (!enabled) return;
    if (m_current < lastStep()) branchHere();  // stepping from the past = a new branch
    m_inputs.push_back(input);
    m_bytes += sizeof(script::InputFrame);
    ++m_current;
    if (snapshotEvery > 0 && m_current % snapshotEvery == 0) {
        Snap sn = capture(s, m_current);
        m_bytes += sn.bytes;
        m_snaps.push_back(std::move(sn));
    }
    enforceCap();
}

void TimeMachine::enforceCap() {
    // Drop the oldest snapshot (and the inputs before the next one) until under the cap; keep at least one.
    while (m_bytes > memoryCap && m_snaps.size() > 1) {
        m_bytes -= m_snaps.front().bytes;
        m_snaps.pop_front();
        int64_t newBase = m_snaps.front().step;
        while (m_base < newBase && !m_inputs.empty()) {
            m_inputs.pop_front();
            m_bytes -= sizeof(script::InputFrame);
            ++m_base;
        }
    }
}

bool TimeMachine::seek(EditorSession& s, int64_t step) {
    if (!enabled || m_snaps.empty() || step < firstStep() || step > lastStep() || !s.playing()) return false;
    s.setPaused(true);
    const Snap* best = &m_snaps.front();
    for (auto& sn : m_snaps)
        if (sn.step <= step) best = &sn;
    restore(s, *best);
    // Re-simulate the recorded inputs (deterministic fixed steps) up to the target.
    for (int64_t t = best->step; t < step; ++t) {
        const script::InputFrame& in = m_inputs[static_cast<size_t>(t - m_base)];
        s.world().scripts().setInput(in);
        s.world().tick(kStep);
    }
    m_current = step;
    return true;
}

void TimeMachine::branchHere() {
    if (m_current >= lastStep()) return;
    size_t keep = static_cast<size_t>(m_current - m_base);
    while (m_inputs.size() > keep) {
        m_inputs.pop_back();
        m_bytes -= sizeof(script::InputFrame);
    }
    while (m_snaps.size() > 1 && m_snaps.back().step > m_current) {
        m_bytes -= m_snaps.back().bytes;
        m_snaps.pop_back();
    }
    ++m_branches;
    FLEDGE_LOG_INFO("editor", "Time Machine: branched at step {} ({:.2f} s)", m_current, static_cast<double>(m_current) * kStep);
}

} // namespace fledge
