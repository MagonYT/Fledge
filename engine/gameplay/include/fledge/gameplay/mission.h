#pragma once
// Missions (docs/GAMEPLAY.md "Missions"): a graph of objective nodes. The mission graph editor in the
// editor writes this JSON into a Mission component; MissionRunner evaluates it every frame.
//
// {"name": "First Job", "nodes": [{"id": 1, "type": "Start", "x": 0, "y": 0},
//                                  {"id": 2, "type": "GoTo", "target": "Garage", "radius": 6, "text": "Go to the garage"},
//                                  {"id": 3, "type": "End", "success": true, "text": "Job done", "reward": 500}],
//  "links": [{"from": 1, "to": 2}, {"from": 2, "to": 3}]}
//
// Node types:
//   Start                       entry point
//   GoTo        target|position, radius, text
//   EnterVehicle  target ("" = any vehicle), text
//   DriveTo     target|position, radius, text   (must be in a vehicle)
//   Collect     item, count, text               (inventory)
//   Eliminate   target, text                    (entity dead or destroyed)
//   Wait        seconds, text
//   Dialogue    text, seconds                   (subtitle line)
//   SetWanted   level                           (instant)
//   LoseWanted  text                            (until the wanted level is 0)
//   Timer       seconds  - fails the mission when it runs out (runs alongside the next nodes)
//   Reward      money, item, count              (instant)
//   End         success, text, reward
// Links may carry "on": "fail" to branch when a node fails (Timer / player death); default is success.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/math.h"

namespace fledge::gameplay {

struct MissionNode {
    int id = 0;
    std::string type;
    Json params = Json::object();
};

struct MissionGraph {
    std::string name;
    std::vector<MissionNode> nodes;
    struct Link {
        int from = 0, to = 0;
        bool onFail = false;
    };
    std::vector<Link> links;
    static MissionGraph fromJson(const Json& j);
    Json toJson() const;  // keeps every param (editor positions included)
    const MissionNode* node(int id) const;
    int startNode() const;
    int next(int from, bool fail) const;  // 0 = none
    // Problems an author should fix (missing Start / End, dangling links, unknown types, unreachable nodes).
    std::vector<std::string> validate() const;
    static const std::vector<std::string>& nodeTypes();
};

// What the runner asks the game about.
struct MissionContext {
    std::function<std::optional<Vec3>(const std::string& entityName)> positionOf;
    std::function<Vec3()> playerPosition;
    std::function<std::string()> playerVehicle;  // "" when on foot, else the vehicle entity name
    std::function<int(const std::string& item)> itemCount;
    std::function<bool(const std::string& entityName)> isDead;  // dead or no longer exists
    std::function<int()> wantedLevel;
    std::function<bool()> playerDead;
    // Effects
    std::function<void(int level)> setWanted;
    std::function<void(int money, const std::string& item, int count)> reward;
};

enum class MissionState { Idle = 0, Running = 1, Succeeded = 2, Failed = 3 };
const char* toString(MissionState s);

class MissionRunner {
public:
    void start(const MissionGraph& g);
    void stop() { m_state = MissionState::Idle; }
    // Advances; returns true when the current node changed this update.
    bool update(float dt, const MissionContext& ctx);
    MissionState state() const { return m_state; }
    int currentNode() const { return m_node; }
    const std::string& objective() const { return m_objective; }  // HUD text
    const std::string& subtitle() const { return m_subtitle; }
    std::optional<Vec3> waypoint() const { return m_waypoint; }  // minimap / marker
    float timerLeft() const { return m_timer; }                   // < 0 = no timer
    const std::vector<int>& visited() const { return m_visited; }
    Json saveState() const;
    void loadState(const MissionGraph& g, const Json& j);

private:
    void enter(int id, const MissionContext& ctx);
    void finish(bool success, const MissionContext& ctx);
    MissionGraph m_graph;
    MissionState m_state = MissionState::Idle;
    int m_node = 0;
    float m_nodeTime = 0, m_timer = -1;
    int m_timerNode = 0;
    std::string m_objective, m_subtitle;
    float m_subtitleLeft = 0;
    std::optional<Vec3> m_waypoint;
    std::vector<int> m_visited;
};

} // namespace fledge::gameplay
