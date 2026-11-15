#pragma once
// Project templates, sample projects and the recent-projects list, used by the
// editor's Welcome screen and New Project wizard. No UI code: unit-tested.
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge {

struct ProjectTemplate {
    std::string id;           // folder name, e.g. "ThirdPerson"
    std::string name;         // display name
    std::string description;
    std::string status;       // "complete", "starter (stub gameplay)", ...
    std::vector<std::string> tags;
    int order = 100;
    fs::path dir;
    fs::path preview;         // optional preview.png (empty if missing)
};

// Scans <templatesDir>/*/template.json (sorted by order, then name).
std::vector<ProjectTemplate> listTemplates(const fs::path& templatesDir);

// Project names become folder and file names: letters, digits, '_', '-', ' ' (1..64 chars, no leading/trailing space).
bool isValidProjectName(const std::string& name);

// Copies the template into <parentDir>/<name>/. Template.fledge becomes
// <name>.fledge with name/gameName/window.title rewritten. Fails if the
// destination already exists. Returns the new .fledge path.
Result<fs::path> createProjectFromTemplate(const ProjectTemplate& tpl, const fs::path& parentDir, const std::string& name);

struct SampleProject {
    std::string name;
    fs::path projectFile;
};
// Every <samplesDir>/*/<x>.fledge.
std::vector<SampleProject> listSamples(const fs::path& samplesDir);

// Copies a sample (without Build/ and Intermediate/) to <parentDir>/<folder name>/,
// so samples in a read-only or shared install are never modified. If the copy
// already exists it is reused. Returns the copied .fledge path.
Result<fs::path> copySampleProject(const SampleProject& sample, const fs::path& parentDir);

// Most-recent-first list of .fledge paths (deduplicated, max 12).
class RecentProjects {
public:
    static constexpr size_t kMax = 12;
    void add(const fs::path& projectFile);
    void remove(const fs::path& projectFile);
    const std::vector<fs::path>& items() const { return m_items; }
    Json toJson() const;
    void fromJson(const Json& j);

private:
    std::vector<fs::path> m_items;
};

} // namespace fledge
