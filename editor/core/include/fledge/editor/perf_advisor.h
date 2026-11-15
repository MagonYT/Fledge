#pragma once
// "Make it run well": scene analysis with concrete suggestions. Some suggestions come with one-click
// fixes, applied as one undoable step:
//   - never-visible objects (beyond every camera's far plane): hide them;
//   - too many shadow-casting point/spot lights: keep the two strongest;
//   - repeated mesh + material pairs: informational (they are GPU-instanced automatically on GL / D3D11);
//
//   - heavy meshes far from the camera: raise their LOD bias (one-click fix). LOD chains are
//     generated automatically (meshoptimizer).
// The editor measures frame time before and after applying fixes (the "re-test").
#include <string>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/scene/scene.h"

namespace fledge {

class EditorSession;

struct PerfSuggestion {
    std::string kind;  // "culling" | "shadows" | "instancing" | "lod"
    std::string text;
    std::vector<EntityId> entities;
    bool fixable = false;  // has a one-click fix
};

struct PerfReport {
    int meshes = 0, lights = 0, shadowLights = 0;
    uint64_t triangles = 0;
    std::vector<PerfSuggestion> suggestions;
};

PerfReport analyzePerformance(EditorSession& session, Vec3 cameraPos);
// Applies every fixable suggestion as one undoable step; returns how many entities changed.
int applyPerformanceFixes(EditorSession& session, const PerfReport& report);

} // namespace fledge
