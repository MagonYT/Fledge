#include "fledge/gameplay/mission.h"

#include <algorithm>
#include <format>
#include <set>

namespace fledge::gameplay {

const char* toString(MissionState s) {
    switch (s) {
        case MissionState::Running: return "Running";
        case MissionState::Succeeded: return "Succeeded";
        case MissionState::Failed: return "Failed";
        default: return "Idle";
    }
}

const std::vector<std::string>& MissionGraph::nodeTypes() {
    static const std::vector<std::string> t = {"Start", "GoTo", "EnterVehicle", "DriveTo", "Collect", "Eliminate", "Wait",
                                               "Dialogue", "SetWanted", "LoseWanted", "Timer", "Reward", "End"};
    return t;
}

MissionGraph MissionGraph::fromJson(const Json& j) {
    MissionGraph g;
    if (!j.is_object()) return g;
    g.name = j.value("name", std::string("Mission"));
    if (j.contains("nodes") && j["nodes"].is_array())
        for (auto& n : j["nodes"]) {
            if (!n.is_object()) continue;
            MissionNode mn;
            mn.id = n.value("id", 0);
            mn.type = n.value("type", std::string());
            mn.params = n;
            g.nodes.push_back(mn);
        }
    if (j.contains("links") && j["links"].is_array())
        for (auto& l : j["links"])
            if (l.is_object()) g.links.push_back({l.value("from", 0), l.value("to", 0), l.value("on", std::string()) == "fail"});
    return g;
}

Json MissionGraph::toJson() const {
    Json jn = Json::array(), jl = Json::array();
    for (auto& n : nodes) {
        Json o = n.params.is_object() ? n.params : Json::object();
        o["id"] = n.id;
        o["type"] = n.type;
        jn.push_back(o);
    }
    for (auto& l : links) {
        Json o = {{"from", l.from}, {"to", l.to}};
        if (l.onFail) o["on"] = "fail";
        jl.push_back(o);
    }
    return {{"name", name}, {"nodes", jn}, {"links", jl}};
}

const MissionNode* MissionGraph::node(int id) const {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
int MissionGraph::startNode() const {
    for (auto& n : nodes)
        if (n.type == "Start") return n.id;
    return nodes.empty() ? 0 : nodes.front().id;
}
int MissionGraph::next(int from, bool fail) const {
    for (auto& l : links)
        if (l.from == from && l.onFail == fail) return l.to;
    return 0;
}

std::vector<std::string> MissionGraph::validate() const {
    std::vector<std::string> out;
    const auto& types = nodeTypes();
    int starts = 0, ends = 0;
    std::set<int> ids;
    for (auto& n : nodes) {
        if (!ids.insert(n.id).second) out.push_back(std::format("duplicate node id {}", n.id));
        if (std::find(types.begin(), types.end(), n.type) == types.end()) out.push_back(std::format("node {}: unknown type '{}'", n.id, n.type));
        starts += n.type == "Start";
        ends += n.type == "End";
        if ((n.type == "GoTo" || n.type == "DriveTo") && n.params.value("target", std::string()).empty() && !n.params.contains("position"))
            out.push_back(std::format("node {} ({}): needs a target entity or a position", n.id, n.type));
        if (n.type != "End" && next(n.id, false) == 0) out.push_back(std::format("node {} ({}) has no outgoing link", n.id, n.type));
    }
    if (starts != 1) out.push_back(std::format("needs exactly one Start node (has {})", starts));
    if (ends == 0) out.push_back("needs at least one End node");
    for (auto& l : links)
        if (!ids.count(l.from) || !ids.count(l.to)) out.push_back(std::format("link {} -> {} points to a missing node", l.from, l.to));
    // reachability from Start
    std::set<int> seen;
    std::vector<int> stack = {startNode()};
    while (!stack.empty()) {
        int n = stack.back();
        stack.pop_back();
        if (!seen.insert(n).second) continue;
        for (auto& l : links)
            if (l.from == n) stack.push_back(l.to);
    }
    for (auto& n : nodes)
        if (!seen.count(n.id)) out.push_back(std::format("node {} ({}) cannot be reached from Start", n.id, n.type));
    return out;
}

void MissionRunner::start(const MissionGraph& g) {
    m_graph = g;
    m_state = MissionState::Running;
    m_node = 0;
    m_timer = -1;
    m_timerNode = 0;
    m_visited.clear();
    m_objective.clear();
    m_subtitle.clear();
    m_waypoint.reset();
    m_node = -1;  // enter Start on the first update (needs the context)
}

void MissionRunner::finish(bool success, const MissionContext& ctx) {
    (void)ctx;
    m_state = success ? MissionState::Succeeded : MissionState::Failed;
    m_waypoint.reset();
    m_timer = -1;
}

void MissionRunner::enter(int id, const MissionContext& ctx) {
    m_node = id;
    m_nodeTime = 0;
    m_visited.push_back(id);
    const MissionNode* n = m_graph.node(id);
    if (!n) {
        finish(false, ctx);
        return;
    }
    const Json& p = n->params;
    std::string text = p.value("text", std::string());
    if (!text.empty() && n->type != "Dialogue") m_objective = text;
    m_waypoint.reset();
    if (p.contains("position") && p["position"].is_array() && p["position"].size() == 3)
        m_waypoint = Vec3{p["position"][0].get<float>(), p["position"][1].get<float>(), p["position"][2].get<float>()};
    else if (!p.value("target", std::string()).empty() && ctx.positionOf)
        m_waypoint = ctx.positionOf(p.value("target", std::string()));
    if (n->type == "Dialogue") {
        m_subtitle = text;
        m_subtitleLeft = p.value("seconds", 3.0f);
    }
    if (n->type == "Timer") {
        m_timer = p.value("seconds", 60.0f);
        m_timerNode = id;
    }
    if (n->type == "SetWanted" && ctx.setWanted) ctx.setWanted(p.value("level", 1));
    if (n->type == "Reward" && ctx.reward) ctx.reward(p.value("money", 0), p.value("item", std::string()), p.value("count", 1));
    if (n->type == "End") {
        bool ok = p.value("success", true);
        if (ok && ctx.reward && p.value("reward", 0) != 0) ctx.reward(p.value("reward", 0), "", 0);
        if (!text.empty()) m_objective = text;
        finish(ok, ctx);
    }
}

bool MissionRunner::update(float dt, const MissionContext& ctx) {
    if (m_state != MissionState::Running) return false;
    int before = m_node;
    if (m_node == -1) {
        enter(m_graph.startNode(), ctx);
        if (m_state != MissionState::Running) return true;
    }
    if (m_subtitleLeft > 0) {
        m_subtitleLeft -= dt;
        if (m_subtitleLeft <= 0) m_subtitle.clear();
    }
    // failure sources: timer, player death
    bool failed = false;
    if (m_timer >= 0) {
        m_timer -= dt;
        if (m_timer <= 0) failed = true;
    }
    if (ctx.playerDead && ctx.playerDead()) failed = true;
    if (failed) {
        int f = m_graph.next(m_node, true);
        if (!f && m_timerNode) f = m_graph.next(m_timerNode, true);
        m_timer = -1;
        if (f) enter(f, ctx);
        else {
            m_objective = "Mission failed";
            finish(false, ctx);
        }
        return true;
    }
    for (int guard = 0; guard < 16 && m_state == MissionState::Running; ++guard) {  // instant nodes chain in one update
        const MissionNode* n = m_graph.node(m_node);
        if (!n) {
            finish(false, ctx);
            break;
        }
        m_nodeTime += guard == 0 ? dt : 0;
        const Json& p = n->params;
        auto near = [&](bool vehicle) {
            std::optional<Vec3> target = m_waypoint;
            if (!p.value("target", std::string()).empty() && ctx.positionOf) target = ctx.positionOf(p.value("target", std::string()));
            if (target) m_waypoint = target;
            if (!target || !ctx.playerPosition) return false;
            if (vehicle && (!ctx.playerVehicle || ctx.playerVehicle().empty())) return false;
            Vec3 d = *target - ctx.playerPosition();
            d.y *= 0.25f;  // generous vertically
            return length(d) <= p.value("radius", 5.0f);
        };
        bool done = false;
        const std::string& t = n->type;
        if (t == "Start" || t == "SetWanted" || t == "Reward" || t == "Timer") done = true;
        else if (t == "GoTo") done = near(false);
        else if (t == "DriveTo") done = near(true);
        else if (t == "EnterVehicle") {
            std::string want = p.value("target", std::string()), have = ctx.playerVehicle ? ctx.playerVehicle() : "";
            done = !have.empty() && (want.empty() || want == have);
        } else if (t == "Collect") done = ctx.itemCount && ctx.itemCount(p.value("item", std::string())) >= p.value("count", 1);
        else if (t == "Eliminate") done = ctx.isDead && ctx.isDead(p.value("target", std::string()));
        else if (t == "Wait") done = m_nodeTime >= p.value("seconds", 1.0f);
        else if (t == "Dialogue") done = m_nodeTime >= p.value("seconds", 3.0f);
        else if (t == "LoseWanted") done = ctx.wantedLevel && ctx.wantedLevel() == 0;
        else if (t == "End") break;
        if (!done) break;
        int nx = m_graph.next(m_node, false);
        if (!nx) {
            finish(true, ctx);
            break;
        }
        enter(nx, ctx);
    }
    return m_node != before;
}

Json MissionRunner::saveState() const {
    return {{"state", toString(m_state)}, {"node", m_node}, {"timer", m_timer}, {"timerNode", m_timerNode}, {"objective", m_objective},
            {"visited", m_visited}};
}

void MissionRunner::loadState(const MissionGraph& g, const Json& j) {
    m_graph = g;
    std::string st = j.value("state", std::string("Idle"));
    m_state = st == "Running" ? MissionState::Running : st == "Succeeded" ? MissionState::Succeeded : st == "Failed" ? MissionState::Failed : MissionState::Idle;
    m_node = j.value("node", 0);
    m_timer = j.value("timer", -1.0f);
    m_timerNode = j.value("timerNode", 0);
    m_objective = j.value("objective", std::string());
    m_visited = j.value("visited", std::vector<int>{});
    m_nodeTime = 0;
    m_waypoint.reset();
}

} // namespace fledge::gameplay
