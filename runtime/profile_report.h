#pragma once
// fledge_runtime --profile: one PROFILE line with the average CPU ms per frame of every GameWorld system
// (frames after the 10-frame warm-up), the scene load time and the process memory (docs/PERFORMANCE.md).
#include "fledge/framework/game_world.h"

namespace fledge {
void printProfile(GameWorld& world, double sceneLoadMs, double avgFrameMs);
}
