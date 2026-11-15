// Events shared by the third-person starter scripts.
#pragma once
#include <fledge/script/script.h>

// GoalTrigger emits this when the player reaches the goal marker; MissionService waits for it.
struct GoalReached {
    fledge::script::Entity player;
    fledge::script::Entity goal;
};
