#pragma once
// Ask the Scene ("Why?"): explains, from engine data only, what produces an entity's look and behaviour.
// It covers the mesh, material, lights and render pass; rotators, Fledge Script behaviours (with their source
// files) and modules; the last edits from the undo history (user or AI); and what would break if the
// entity were deleted (children, references from other entities, scripts that look it up by name).
// The AI only rephrases this report, and only when AI features are on.
#include <string>
#include <utility>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/scene/scene.h"

namespace fledge {

class EditorSession;

struct WhyReport {
    std::string title;
    std::vector<std::pair<std::string, std::vector<std::string>>> sections;  // heading -> lines
    const std::vector<std::string>* section(const std::string& heading) const;
    std::string toText() const;
};

WhyReport explainEntity(EditorSession& session, EntityId e);

} // namespace fledge
