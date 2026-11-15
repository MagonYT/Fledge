#include "profile_report.h"

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace fledge {

void printProfile(GameWorld& world, double sceneLoadMs, double avgFrameMs) {
    const auto& t = world.systemTimes();
    double n = t.ticks > 0 ? t.ticks : 1, r = t.renders > 0 ? t.renders : 1;
    double peakMb = 0, workingMb = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        peakMb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
        workingMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    std::printf("PROFILE frames=%d avgFrameMs=%.3f sceneLoadMs=%.1f modules=%.3f scripts=%.3f gameplay=%.3f physics=%.3f animation=%.3f "
                "lateGameplay=%.3f streaming=%.3f audio=%.3f worldMatrices=%.3f drawItems=%.3f owItems=%.3f meshItems=%.3f animItems=%.3f hud=%.3f renderer=%.3f workingSetMB=%.1f peakWorkingSetMB=%.1f\n",
                t.ticks, avgFrameMs, sceneLoadMs, t.modules / n, t.scripts / n, t.gameplay / n, t.physics / n, t.animation / n, t.lateGameplay / n,
                t.streaming / n, t.audio / n, t.worldMatrices / r, t.drawItems / r, t.openWorldItems / r, t.meshItems / r, t.animatedItems / r, t.hud / r, t.renderer / r, workingMb, peakMb);
    std::fflush(stdout);
}

} // namespace fledge
