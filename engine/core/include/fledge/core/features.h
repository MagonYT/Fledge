#pragma once
// Editions and features (docs/EDITIONS.md).
//
// One source tree builds two editions (CMake FLEDGE_EDITION=Standard|Advanced):
//   * Fledge           - the lean edition (Simple Mode by default)
//   * Fledge Advanced  - everything in Fledge plus the Advanced modules (engine/advanced/*, fledge_adv_* targets)
// The edition of an executable is decided at link time: it links fledge_edition_standard or fledge_edition_advanced
// (engine/edition), whose initEdition() sets currentEdition() and registers the linked Advanced modules here.
//
// A feature is an id plus an integer version ("advanced.weapons" v1). The registry knows:
//   * the catalog: every feature id this source tree has heard of (Standard + Advanced, available or planned), so a
//     Standard build can name the Advanced features a project needs without linking them;
//   * what this executable PROVIDES: Standard features always, Advanced features only from linked modules.
// Projects record the features they use in their manifest ("requiredFeatures", fledge/framework/edition_check.h).
// A project needing a feature this executable does not provide is declined, never edited (docs/EDITIONS.md).
//
// Extension point for later releases: an Advanced module's register function receives the hooks below and adds
// components (ComponentRegistry), editor MCP tools and the features it provides. Core never calls the hooks itself;
// the types are only forward-declared here.
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/json.h"

namespace fledge {

class ComponentRegistry;
class EditorSession;
namespace mcp {
class ToolRegistry;
}

enum class Edition { Standard, Advanced };
const char* editionName(Edition e);  // "Fledge" / "Fledge Advanced"
const char* editionId(Edition e);    // "standard" / "advanced" (manifest, MCP, --print-paths)
std::optional<Edition> parseEdition(std::string_view s);  // "standard" / "advanced" / "Fledge" / "Fledge Advanced"
// Folder name under %APPDATA% for the editor and tools of an edition ("Fledge" / "Fledge Advanced"). Packaged games
// always use "Fledge" (they are not edition-locked).
const char* editionUserDataFolder(Edition e);

// The edition of this executable. Standard until initEdition() (engine/edition) says otherwise.
Edition currentEdition();
void setCurrentEdition(Edition e);

enum class FeatureStatus { Available, Planned };

struct FeatureInfo {
    std::string id;       // "render.ssao", "advanced.weapons"
    int version = 1;      // bumped when the saved data of the feature changes incompatibly
    Edition edition = Edition::Standard;
    std::string title;    // "Weapon system"
    std::string summary;  // one line for the decline dialog / MCP
    std::string module;   // owning module or target ("fledge_render", "fledge_adv_weapons")
    FeatureStatus status = FeatureStatus::Available;
    std::string plannedFor;  // planned features: the release they are scheduled for ("1.3.0")
    Json toJson() const;
};

// One entry of a project's "requiredFeatures". The title and edition are stored too, so an older Fledge can name a
// feature that is not in its catalog.
struct FeatureRequirement {
    std::string id;
    int version = 1;
    std::string title;
    Edition edition = Edition::Standard;
    Json toJson() const;
    static std::optional<FeatureRequirement> fromJson(const Json& j);
    bool operator==(const FeatureRequirement& o) const { return id == o.id && version == o.version; }
};

// A module of this source tree (Advanced modules are fledge_adv_* targets). status: "available" (provides features),
// "scaffold" (registered, provides nothing yet: its features are planned).
struct ModuleInfo {
    std::string id;     // "fledge_adv_weapons"
    std::string title;  // "Weapons"
    Edition edition = Edition::Advanced;
    std::string status = "scaffold";
    std::vector<std::string> features;  // feature ids it provides or will provide
    Json toJson() const;
};

class FeatureRegistry {
public:
    // Process-wide registry, pre-filled with the catalog and the Standard features.
    static FeatureRegistry& get();
    FeatureRegistry();  // a fresh registry (catalog + Standard features); tests build their own

    // A linked module provides f (marks it available; adds it to the catalog when it is new).
    void provide(FeatureInfo f);
    void addModule(ModuleInfo m);  // a linked module (idempotent by id)
    // Declares that an MCP tool belongs to a feature (unavailable tools answer "requires Fledge Advanced").
    void declareTool(const std::string& tool, const std::string& featureId);

    const FeatureInfo* provided(std::string_view id) const;  // nullptr when this executable lacks it
    int providedVersion(std::string_view id) const;          // 0 = not provided
    const FeatureInfo* known(std::string_view id) const;     // catalog entry (provided or not)
    std::vector<const FeatureInfo*> providedFeatures() const;
    std::vector<const FeatureInfo*> unavailableFeatures() const;  // catalog entries this executable lacks
    const std::vector<FeatureInfo>& catalog() const { return m_catalog; }
    const std::vector<ModuleInfo>& modules() const { return m_modules; }
    // Feature an MCP tool belongs to ("" = core tool or unknown name).
    std::string toolFeature(const std::string& tool) const;

    // {"edition", "editionName", "features": [...provided], "unavailable": [...], "modules": [...]}
    Json describe() const;

    // Extension points for modules (see the header comment). Run by the layer that owns the registry type.
    std::vector<std::function<void(ComponentRegistry&)>> componentHooks;
    std::vector<std::function<void(mcp::ToolRegistry&, EditorSession&)>> editorToolHooks;

private:
    std::vector<FeatureInfo> m_catalog;
    std::vector<std::string> m_provided;  // ids
    std::vector<ModuleInfo> m_modules;
    std::vector<std::pair<std::string, std::string>> m_tools;  // tool -> feature
};

// Requirements this registry cannot satisfy, with a one-line reason each.
struct MissingFeature {
    FeatureRequirement required;
    int providedVersion = 0;  // 0 = absent; > 0 = present but older than required
    std::string title;        // best known name
    Edition edition = Edition::Standard;
    std::string reason;       // "needs Fledge Advanced" / "needs a newer version of Fledge (has v1, project needs v2)"
    Json toJson() const;
};
std::vector<MissingFeature> missingFeatures(const std::vector<FeatureRequirement>& required, const FeatureRegistry& reg);
// The lowest edition that can provide every requirement: Advanced when any requirement is an Advanced feature
// (by the requirement's own edition, the catalog, or the "advanced." id prefix).
Edition minimumEdition(const std::vector<FeatureRequirement>& required, const FeatureRegistry& reg);
// Union of two requirement lists (highest version per id), sorted by id.
std::vector<FeatureRequirement> mergeRequirements(const std::vector<FeatureRequirement>& a, const std::vector<FeatureRequirement>& b);
// A requirement for a feature this registry knows (title / edition filled from the catalog).
FeatureRequirement requirementFor(const FeatureRegistry& reg, const std::string& id, int version = 0);

} // namespace fledge
