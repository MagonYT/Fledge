#pragma once
// Editor glue for the Unreal Blueprint -> Fledge Script converter (engine/blueprint, docs/BLUEPRINT_CONVERTER.md): shared by
// the Convert Blueprint panel and the MCP tool convert_blueprint. Generated scripts go through EditorSession::writeScriptFile
// (sandboxed to Scripts/, undoable, tagged AI when an AI asked); reports go to <project>/Intermediate/BlueprintConversions/.
#include <memory>
#include <string>

#include "fledge/blueprint/blueprint.h"

namespace fledge {

class EditorSession;

// Options + the mapping table they point at (built-in, merged with <project>/blueprint_mappings.json when present).
struct BlueprintJob {
    blueprint::Options options;
    std::shared_ptr<blueprint::MappingTable> table;
    std::vector<std::string> notes;  // e.g. a project mapping file that failed validation
};
BlueprintJob makeBlueprintJob(const EditorSession& s, const std::string& className, const std::string& sourceName, bool unrealSpace = true);

struct BlueprintWrite {
    blueprint::Conversion conversion;  // as written (class possibly renamed to stay unique)
    std::vector<std::string> files;    // Scripts/-relative
    std::string reportMarkdown, reportJson;  // project-relative
    std::vector<std::string> notes;
};
// Converts and writes into Scripts/<folder>/ without ever overwriting (unique class name), plus the report files.
Result<BlueprintWrite> writeBlueprintToProject(EditorSession& s, std::string_view text, const BlueprintJob& job, const std::string& folder, bool fromAi);

// File extensions the converter accepts from disk (.t3d .txt .copy .uasset .umap).
bool isBlueprintSourceFile(const fs::path& p);

} // namespace fledge
