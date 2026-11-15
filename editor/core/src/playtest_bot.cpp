#include "fledge/editor/playtest_bot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <random>
#include <set>

#include "fledge/core/log.h"
#include "fledge/editor/editor_session.h"

namespace fledge {

namespace {
constexpr int kW = 87, kA = 65, kS = 83, kD = 68, kSpace = 32, kShift = 340;

EntityId findSubject(Scene& scene) {
    for (EntityId e : scene.entities())
        if (toLower(scene.displayName(e)) == "player") return e;
    for (EntityId e : scene.entities())
        if (auto* t = scene.get<TagsComponent>(e); t && toLower(t->tags).find("player") != std::string::npos) return e;
    for (EntityId e : scene.entities())
        if (auto* c = scene.get<CameraComponent>(e); c && c->primary) return e;
    for (EntityId e : scene.entities())
        if (scene.get<BehavioursComponent>(e)) return e;
    return scene.entities().empty() ? kNullEntity : scene.entities().front();
}
} // namespace

BotReport runPlaytestBot(EditorSession& s, const BotOptions& o) {
    BotReport r;
    if (!s.playing())
        if (auto st = s.play(); !st) {
            r.error = st.error;
            return r;
        }
    s.setPaused(false);
    Scene& scene = s.scene();
    EntityId subject = findSubject(scene);
    if (subject == kNullEntity) {
        r.error = "the scene is empty";
        return r;
    }
    r.subject = scene.displayName(subject);
    float lowest = 0;
    for (EntityId e : scene.entities())
        if (scene.get<MeshRendererComponent>(e)) lowest = std::min(lowest, scene.worldTransform(e).position.y);
    float fallY = lowest - 20.0f;

    // action weights: forward, back, left, right, fwd-left, fwd-right, jump, sprint-forward, idle
    std::vector<double> weights = {6, 1.5, 2, 2, 2, 2, 1.5, 2, 0.5};
    std::string g = toLower(o.goal);
    auto bias = [&](std::initializer_list<const char*> words, int idx, double w) {
        for (const char* k : words)
            if (g.find(k) != std::string::npos) {
                weights[static_cast<size_t>(idx)] += w;
                r.notes.push_back(std::format("goal keyword '{}' favours action {}", k, idx));
                break;
            }
    };
    if (!g.empty()) {
        bias({"jump", "climb", "platform"}, 6, 6);
        bias({"explore", "everywhere", "run", "reach", "find"}, 7, 6);
        bias({"left"}, 2, 4);
        bias({"right"}, 3, 4);
        bias({"back", "retreat"}, 1, 4);
    }
    std::mt19937 rng(o.seed);
    std::discrete_distribution<int> pick(weights.begin(), weights.end());
    std::uniform_real_distribution<float> dur(0.4f, 1.6f), look(-6.0f, 6.0f);

    // errors logged by scripts during the run
    std::vector<std::pair<int64_t, std::string>> errors;
    TimeMachine& tm = s.timeMachine;
    int sink = Logger::get().addSink([&](const LogEntry& e) {
        if (e.level == LogLevel::Error && (e.category == "script" || e.category == "script")) errors.push_back({tm.currentStep(), e.message});
    });

    r.firstStep = tm.currentStep();
    int64_t steps = std::llround(o.seconds * 60.0);
    std::set<std::pair<int, int>> cells;
    Vec3 prev = scene.worldTransform(subject).position, stuckRef = prev;
    int64_t stuckSince = 0, lastStuckReport = -100000;
    int action = 0;
    float actionLeft = 0;
    std::vector<double> stepMs;
    bool fell = false;
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < steps && s.playing(); ++i) {
        if (actionLeft <= 0) {
            action = pick(rng);
            actionLeft = dur(rng);
        }
        actionLeft -= TimeMachine::kStep;
        script::InputFrame in;
        auto down = [&](int k) { in.down[static_cast<size_t>(k)] = true; };
        switch (action) {
            case 0: down(kW); break;
            case 1: down(kS); break;
            case 2: down(kA); break;
            case 3: down(kD); break;
            case 4: down(kW), down(kA); break;
            case 5: down(kW), down(kD); break;
            case 6: down(kW), down(kSpace); break;
            case 7: down(kW), down(kShift); break;
            default: break;
        }
        in.mouseDx = (i % 20 == 0) ? look(rng) : 0.0f;
        s.liveInput = in;
        auto a = std::chrono::steady_clock::now();
        s.tick(TimeMachine::kStep);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        stepMs.push_back(ms);
        if (!scene.isValid(subject)) {
            r.issues.push_back({"softlock", tm.currentStep(), r.subject + " was destroyed"});
            break;
        }
        Vec3 p = scene.worldTransform(subject).position;
        r.distance += length(p - prev);
        prev = p;
        cells.insert({static_cast<int>(std::floor(p.x / 2)), static_cast<int>(std::floor(p.z / 2))});
        if (p.y < fallY && !fell) {
            fell = true;
            r.issues.push_back({"fall", tm.currentStep(), std::format("{} fell to y = {:.1f} (lowest geometry {:.1f})", r.subject, p.y, lowest)});
            break;
        }
        bool moving = action != 8;
        if (length(p - stuckRef) > 0.1f || !moving) {
            stuckRef = p;
            stuckSince = i;
        } else if (i - stuckSince > 300 && i - lastStuckReport > 600) {
            r.issues.push_back({"softlock", tm.currentStep(), std::format("{} did not move for 5 s while the bot was moving (at {})", r.subject,
                                                                          std::format("{:.1f}, {:.1f}, {:.1f}", p.x, p.y, p.z))});
            lastStuckReport = i;
            actionLeft = 0;  // try something else
        }
        if (stepMs.size() > 30) {
            std::vector<double> sorted(stepMs.end() - 30, stepMs.end());
            std::nth_element(sorted.begin(), sorted.begin() + 15, sorted.end());
            double median = sorted[15];
            if (ms > 8.0 && ms > 5 * median)
                r.issues.push_back({"perf spike", tm.currentStep(), std::format("a gameplay step took {:.1f} ms (median {:.2f} ms)", ms, median)});
        }
    }
    r.wallSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    Logger::get().removeSink(sink);
    for (auto& [step, msg] : errors) r.issues.push_back({"script error", step, msg});
    std::sort(r.issues.begin(), r.issues.end(), [](const BotIssue& a, const BotIssue& b) { return a.step < b.step; });
    if (r.issues.size() > 50) r.issues.resize(50);
    r.cellsVisited = static_cast<int>(cells.size());
    r.lastStep = tm.currentStep();
    s.liveInput = {};
    s.setPaused(true);
    r.ran = true;
    return r;
}

} // namespace fledge
