#include "fledge/editor/project_templates.h"
#include "fledge/core/legacy.h"

#include <algorithm>
#include <cctype>

#include "fledge/framework/project.h"
#include "fledge/gameplay/city.h"

namespace fledge {

std::vector<ProjectTemplate> listTemplates(const fs::path& templatesDir) {
    std::vector<ProjectTemplate> out;
    std::error_code ec;
    if (!fs::is_directory(templatesDir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(templatesDir, ec)) {
        if (!entry.is_directory()) continue;
        auto text = readFileText(entry.path() / "template.json");
        if (!text) continue;
        Json j = Json::parse(*text, nullptr, false);
        if (!j.is_object() || !legacy::formatIs(j, "fledgetemplate")) continue;
        if (!fs::is_regular_file(entry.path() / "Template.fledge", ec)) continue;
        ProjectTemplate t;
        t.id = narrow(entry.path().filename().wstring());
        t.name = j.value("name", t.id);
        t.description = j.value("description", "");
        t.status = j.value("status", "");
        t.order = j.value("order", 100);
        if (j.contains("tags") && j["tags"].is_array())
            for (auto& tag : j["tags"])
                if (tag.is_string()) t.tags.push_back(tag.get<std::string>());
        t.dir = entry.path();
        if (fs::is_regular_file(entry.path() / "preview.png", ec)) t.preview = entry.path() / "preview.png";
        out.push_back(std::move(t));
    }
    std::sort(out.begin(), out.end(), [](const ProjectTemplate& a, const ProjectTemplate& b) {
        return a.order != b.order ? a.order < b.order : a.name < b.name;
    });
    return out;
}

bool isValidProjectName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    for (char c : name) {
        auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-' || c == ' ')) return false;
    }
    return true;
}

namespace {
Status copyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return Status::failure("cannot create " + narrow(to.wstring()) + ": " + ec.message());
    for (auto it = fs::recursive_directory_iterator(from, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return Status::failure("cannot read " + narrow(from.wstring()) + ": " + ec.message());
        fs::path rel = fs::relative(it->path(), from, ec);
        std::string top = narrow(rel.begin()->wstring());
        if (top == "Build" || top == "Intermediate") {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        // Fledge Script IDE hints contain absolute paths of the source copy; the first build regenerates them.
        std::string name = narrow(it->path().filename().wstring());
        if (top == "Scripts" && (name == "compile_commands.json" || name == ".clangd" || name == "CMakeLists.txt")) continue;
        if (it->is_directory()) {
            fs::create_directories(to / rel, ec);
        } else if (it->is_regular_file()) {
            fs::create_directories((to / rel).parent_path(), ec);
            fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) return Status::failure("cannot copy " + narrow(rel.wstring()) + ": " + ec.message());
    }
    return {};
}
} // namespace

Result<fs::path> createProjectFromTemplate(const ProjectTemplate& tpl, const fs::path& parentDir, const std::string& name) {
    if (!isValidProjectName(name))
        return Result<fs::path>::failure("invalid project name '" + name + "' (letters, digits, space, '_' and '-' only)");
    if (parentDir.empty()) return Result<fs::path>::failure("no location given");
    fs::path dest = parentDir / pathFromUtf8(name);
    std::error_code ec;
    if (fs::exists(dest, ec)) return Result<fs::path>::failure("folder already exists: " + narrow(dest.wstring()));
    if (auto st = copyTree(tpl.dir, dest); !st) return Result<fs::path>::failure(st.error);
    // Generated content (City Sandbox): the template ships the hand-made entities; the city is built here.
    if (auto meta = readFileText(dest / "template.json")) {
        Json tj = Json::parse(*meta, nullptr, false);
        if (tj.is_object() && tj.contains("generate")) {
            auto projText = readFileText(dest / "Template.fledge");
            Json pj = projText ? Json::parse(*projText, nullptr, false) : Json();
            fs::path scenePath = dest / "Content" / pathFromUtf8(pj.is_object() ? pj.value("startupScene", std::string("Scenes/Main.fscene")) : "Scenes/Main.fscene");
            auto sceneText = readFileText(scenePath);
            Json sj = sceneText ? Json::parse(*sceneText, nullptr, false) : Json();
            Scene scene;
            if (!sj.is_object() || !scene.fromJson(sj)) return Result<fs::path>::failure("template scene does not load");
            gameplay::applyGenerateSpec(scene, tj["generate"]);
            if (!writeFileText(scenePath, scene.toJson().dump(1) + "\n")) return Result<fs::path>::failure("cannot write the generated scene");
        }
    }
    fs::remove(dest / "template.json", ec);
    fs::remove(dest / "preview.png", ec);
    fs::path tplProject = dest / "Template.fledge";
    auto text = readFileText(tplProject);
    if (!text) return Result<fs::path>::failure("template has no Template.fledge");
    Json j = Json::parse(*text, nullptr, false);
    if (j.is_discarded()) return Result<fs::path>::failure("template project file is not valid JSON");
    j["name"] = name;
    std::string game;
    for (char c : name)
        if (c != ' ') game += c;
    j["gameName"] = game;
    if (!j.contains("window") || !j["window"].is_object()) j["window"] = Json::object();
    j["window"]["title"] = name;
    fs::path projectFile = dest / pathFromUtf8(name + Project::kExtension);
    if (!writeFileText(projectFile, j.dump(2) + "\n")) return Result<fs::path>::failure("cannot write project file");
    fs::remove(tplProject, ec);
    auto check = Project::load(projectFile);
    if (!check) return Result<fs::path>::failure("created project does not load: " + check.error());
    return projectFile;
}

std::vector<SampleProject> listSamples(const fs::path& samplesDir) {
    std::vector<SampleProject> out;
    std::error_code ec;
    if (!fs::is_directory(samplesDir, ec)) return out;
    for (const auto& dir : fs::directory_iterator(samplesDir, ec)) {
        if (!dir.is_directory()) continue;
        for (const auto& f : fs::directory_iterator(dir.path(), ec)) {
            if (f.is_regular_file() && f.path().extension() == Project::kExtension) {
                out.push_back({narrow(dir.path().filename().wstring()), f.path()});
                break;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const SampleProject& a, const SampleProject& b) { return a.name < b.name; });
    return out;
}

Result<fs::path> copySampleProject(const SampleProject& sample, const fs::path& parentDir) {
    fs::path srcDir = sample.projectFile.parent_path();
    fs::path dest = parentDir / srcDir.filename();
    fs::path destProject = dest / sample.projectFile.filename();
    std::error_code ec;
    if (fs::is_regular_file(destProject, ec)) return destProject;  // reuse an earlier copy
    if (fs::exists(dest, ec)) return Result<fs::path>::failure("folder exists but holds no project: " + narrow(dest.wstring()));
    if (auto st = copyTree(srcDir, dest); !st) return Result<fs::path>::failure(st.error);
    return destProject;
}

void RecentProjects::add(const fs::path& projectFile) {
    fs::path p = projectFile.lexically_normal();
    remove(p);
    m_items.insert(m_items.begin(), p);
    if (m_items.size() > kMax) m_items.resize(kMax);
}

void RecentProjects::remove(const fs::path& projectFile) {
    fs::path p = projectFile.lexically_normal();
    std::erase_if(m_items, [&](const fs::path& x) { return toLower(narrow(x.wstring())) == toLower(narrow(p.wstring())); });
}

Json RecentProjects::toJson() const {
    Json a = Json::array();
    for (auto& p : m_items) a.push_back(narrow(p.wstring()));
    return a;
}

void RecentProjects::fromJson(const Json& j) {
    m_items.clear();
    if (!j.is_array()) return;
    for (auto it = j.rbegin(); it != j.rend(); ++it)
        if (it->is_string()) add(pathFromUtf8(it->get<std::string>()));
}

} // namespace fledge
