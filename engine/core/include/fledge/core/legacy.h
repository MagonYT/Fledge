#pragma once
// Migration from the engine's pre-0.8 name. Fledge was called "CAGE" before v0.8; projects made with
// it use other file extensions and format tags. Loaders accept the old format tags, and the editor
// converts an old project into a NEW folder (copy, then convert - the original is never touched).
// This file and legacy.cpp are the only places that spell the old names.
#include <string>
#include <string_view>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge::legacy {

// Old extension -> new extension (".cageproject" -> ".fledge", ".cagescene" -> ".fscene", ...); "" if not legacy.
std::string_view newExtension(std::string_view oldExtension);
bool isLegacyProjectFile(const fs::path& file);
// Old format tag for a current one ("fscene" -> the pre-0.8 tag), "" if none.
std::string_view legacyFormat(std::string_view currentTag);
// j["format"] is `tag` or its pre-0.8 equivalent.
bool formatIs(const Json& j, std::string_view tag);
// Rewrites file references, format tags and script includes/namespaces in a text file.
std::string migrateText(std::string text);

struct MigrationResult {
    fs::path projectFile;   // the converted .fledge file in the new folder
    int filesRenamed = 0;
    int filesRewritten = 0;
};
// Copies the old project's folder to "<folder>-Fledge" (or "-Fledge-2", ...) next to it, without
// Intermediate/ and Build/, then renames and rewrites the copy.
Result<MigrationResult> migrateProject(const fs::path& legacyProjectFile);

} // namespace fledge::legacy
