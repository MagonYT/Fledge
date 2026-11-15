#pragma once
// Playtest Bots: one-click bot sessions. The bot plays the game headlessly at full speed: it injects
// keyboard and mouse input through the same Fledge Script input path a player uses, as fixed steps that the
// Time Machine records. Every issue therefore carries a step that the Time Machine can open.
//
// Baseline (no AI): a seeded wanderer with movement / jump / look actions and coverage heuristics. It
// visits new 2 m cells, turns away when stuck, and reports:
//   - script errors: Fledge Script / script errors logged during the run;
//   - softlocks: no movement for 5 s while moving;
//   - falls: below the lowest geometry by 20 m;
//   - perf spikes: a step slower than 5x the median and over 8 ms.
// With AI features on, a text goal is accepted. It biases the action mix through keywords
// ("jump", "explore", "left", ...). That is a heuristic, not an AI planner (see docs/INNOVATIONS.md).
#include <cstdint>
#include <string>
#include <vector>

#include "fledge/scene/scene.h"

namespace fledge {

class EditorSession;

struct BotIssue {
    std::string kind;  // "script error" | "softlock" | "fall" | "perf spike"
    int64_t step = 0;  // Time Machine step
    std::string detail;
};

struct BotOptions {
    double seconds = 30.0;  // simulated time
    uint32_t seed = 1;
    std::string goal;       // optional text goal (keyword bias)
};

struct BotReport {
    bool ran = false;
    std::string error;
    std::string subject;  // what the bot watched (player / camera)
    int64_t firstStep = 0, lastStep = 0;
    int cellsVisited = 0;
    float distance = 0, wallSeconds = 0;
    std::vector<BotIssue> issues;
    std::vector<std::string> notes;
};

// Starts play mode if needed; leaves the session playing and paused at the end, so the Time Machine
// can jump to any issue.
BotReport runPlaytestBot(EditorSession& session, const BotOptions& options);

} // namespace fledge
