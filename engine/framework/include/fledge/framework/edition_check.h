#pragma once
// Open / decline rule for projects (docs/EDITIONS.md).
//
// Before anything else touches a project (no migration, no .meta rescan, no lock or temp files, no save on exit), the
// editor, the MCP server and fledgepack compare the manifest's "requiredFeatures" (and "minEdition") with the features
// this executable provides. When anything is missing the project is DECLINED: it is not opened for editing, the user is
// told exactly which features it needs and why, and the files stay byte-for-byte unchanged.
//
// checkProjectEdition() only reads the project file. It never writes, creates or locks anything.
#include <string>
#include <vector>

#include "fledge/core/features.h"
#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge {

struct EditionCheck {
    bool ok = true;                       // false = decline (do not open for editing)
    std::vector<MissingFeature> missing;  // what this executable lacks
    Edition required = Edition::Standard; // minimum edition the manifest asks for
    fs::path projectFile;

    // "This project needs Fledge Advanced" / "This project needs a newer Fledge"
    std::string title() const;
    // Paragraphs for the dialog / log: what is missing, why, and that nothing was changed.
    std::string explanation() const;
    // One line ("needs Fledge Advanced: Weapon system, Ballistics; nothing was changed").
    std::string summary() const;
    Json toJson() const;  // {"ok", "requiredEdition", "missing": [...], "title", "explanation"}
};

EditionCheck checkProjectEdition(const Json& manifest, const FeatureRegistry& reg = FeatureRegistry::get());
// Reads the file; a file that cannot be read or parsed passes (the normal loader reports that error).
EditionCheck checkProjectEdition(const fs::path& projectFile, const FeatureRegistry& reg = FeatureRegistry::get());

} // namespace fledge
