// GoalTrigger + MissionService - event bus and coroutine example.
// GoalTrigger checks the horizontal distance to the tagged player (no physics triggers yet)
// and emits GoalReached once; MissionService runs a coroutine that waits for that event.
#include "GameEvents.h"

using namespace fledge::script;

class GoalTrigger : public Behaviour {
public:
    FLEDGE_PROPERTY(float, radius, 2.5f, Range(0.1f, 20), Tooltip("Horizontal distance that counts as reached"));

    void OnUpdate(float) override {
        if (m_done) return;
        Entity player = Scene::FindWithTag("player");
        if (!player) return;
        Vec3 d = player.Position() - GetEntity().Position();
        d.y = 0;
        if (d.Length() < radius) {
            m_done = true;
            Events::Emit(GoalReached{player, GetEntity()});
        }
    }

private:
    bool m_done = false;
};
FLEDGE_BEHAVIOUR(GoalTrigger)

class MissionService : public Service {
public:
    FLEDGE_PROPERTY(int, completed, 0, ReadOnly(), Tooltip("Goals reached this session"));

    void OnStart() override { StartCoroutine(Run()); }

private:
    Task Run() {
        Log::Info("Mission: reach the glowing marker on the platform");
        GoalReached e = co_await WaitForEvent<GoalReached>();
        ++completed;
        Log::Info("Mission complete: {} reached '{}' after {:.1f}s", e.player.Name(), e.goal.Name(), Time::Elapsed());
        co_await WaitSeconds(1.0f);
        e.goal.SetColor(Color{0.2f, 1.0f, 0.3f});
    }
};
FLEDGE_SERVICE(MissionService)
