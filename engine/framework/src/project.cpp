#include "fledge/framework/project.h"
#include "fledge/core/legacy.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "fledge/scene/components.h"
#include "fledge/scene/scene.h"

namespace fledge {

Json Project::toJson() const {
    Json j = {{"format", "fledgeproject"}, {"version", 1},          {"name", name},
              {"gameName", gameName},    {"startupScene", startupScene}, {"modules", modules}};
    if (!icon.empty()) j["icon"] = icon;
    j["subsystem"] = guiSubsystem ? "gui" : "console";
    if (!splash) j["splash"] = false;
    j["render"] = render.toJson();
    j["graphics"] = graphics.toJson();
    if (!input.is_null()) j["input"] = input;
    if (!ui.font.empty() || ui.language != "en" || ui.fallbackLanguage != "en" || ui.fontSize != 32)
        j["ui"] = {{"font", ui.font}, {"fontSize", ui.fontSize}, {"language", ui.language}, {"fallbackLanguage", ui.fallbackLanguage}};
    j["cook"] = {{"textureCompression", textureCompression}, {"mips", textureMips}, {"tangents", meshTangents}};
    j["window"] = {{"title", windowTitle.empty() ? gameName : windowTitle}, {"width", windowWidth}, {"height", windowHeight}};
    if (!requiredFeatures.empty()) {
        Json req = Json::array();
        for (const auto& r : requiredFeatures) req.push_back(r.toJson());
        j["requiredFeatures"] = req;
        if (minEdition() != Edition::Standard) j["minEdition"] = editionId(minEdition());
    }
    for (auto& [k, v] : extra.items())
        if (!j.contains(k)) j[k] = v;  // written by a newer Fledge: kept
    return j;
}

Edition Project::minEdition() const { return minimumEdition(requiredFeatures, FeatureRegistry::get()); }

bool Project::requireFeature(const FeatureRequirement& r) {
    auto merged = mergeRequirements(requiredFeatures, {r});
    if (merged.size() == requiredFeatures.size() &&
        std::equal(merged.begin(), merged.end(), requiredFeatures.begin(), [](auto& a, auto& b) { return a == b; }))
        return false;
    requiredFeatures = std::move(merged);
    return true;
}

static bool looksSecret(std::string key) {
    key = toLower(key);
    for (const char* bad : {"apikey", "api_key", "token", "secret", "password", "credential", "bearer"})
        if (key.find(bad) != std::string::npos) return true;
    return false;
}

static Status scanSecrets(const Json& j, const std::string& path) {
    if (j.is_object()) {
        for (auto& [k, v] : j.items()) {
            if (looksSecret(k))
                return Status::failure(std::format("refusing secret-looking key '{}{}' in project file; "
                                                   "store credentials with the AI settings (Credential Manager)", path, k));
            if (auto st = scanSecrets(v, path + k + "."); !st) return st;
        }
    } else if (j.is_array()) {
        for (auto& v : j)
            if (auto st = scanSecrets(v, path); !st) return st;
    }
    return {};
}

Status Project::validate(const Json& j) { return scanSecrets(j, ""); }

Result<Project> Project::fromJson(const Json& j) {
    if (!j.is_object() || !legacy::formatIs(j, "fledgeproject")) return Result<Project>::failure("not a .fledge file");
    if (auto st = validate(j); !st) return Result<Project>::failure(st.error);
    Project p;
    p.name = j.value("name", "Untitled");
    p.gameName = j.value("gameName", p.name);
    p.startupScene = j.value("startupScene", p.startupScene);
    p.modules = j.value("modules", std::vector<std::string>{});
    p.icon = j.value("icon", "");
    std::string subsystem = j.value("subsystem", "gui");
    if (subsystem != "gui" && subsystem != "console") return Result<Project>::failure("subsystem must be \"gui\" or \"console\"");
    p.guiSubsystem = subsystem == "gui";
    if (j.contains("splash") && j["splash"].is_boolean()) p.splash = j["splash"].get<bool>();
    if (j.contains("render")) {
        if (auto st = p.render.fromJson(j["render"]); !st) return Result<Project>::failure("render settings: " + st.error);
        p.render.sanitize();
    }
    if (j.contains("graphics")) {
        auto g = GraphicsSettings::fromJson(j["graphics"]);
        if (!g) return Result<Project>::failure(g.error());
        p.graphics = g.value();
    }
    if (j.contains("input")) {
        if (!j["input"].is_object() && !j["input"].is_null()) return Result<Project>::failure("\"input\" must be an object");
        p.input = j["input"];
    }
    if (j.contains("ui")) {
        const auto& u = j["ui"];
        if (!u.is_object()) return Result<Project>::failure("\"ui\" must be an object");
        p.ui.font = u.value("font", std::string());
        p.ui.fontSize = std::clamp(u.value("fontSize", 32.0f), 8.0f, 96.0f);
        p.ui.language = u.value("language", std::string("en"));
        p.ui.fallbackLanguage = u.value("fallbackLanguage", std::string("en"));
    }
    if (j.contains("cook")) {
        const auto& c = j["cook"];
        p.textureCompression = c.value("textureCompression", "auto");
        p.textureMips = c.value("mips", true);
        p.meshTangents = c.value("tangents", true);
        if (p.textureCompression != "auto" && p.textureCompression != "bc1" && p.textureCompression != "bc3" && p.textureCompression != "none")
            return Result<Project>::failure("cook.textureCompression must be auto, bc1, bc3 or none");
    }
    if (j.contains("window")) {
        const auto& w = j["window"];
        p.windowTitle = w.value("title", p.gameName);
        p.windowWidth = w.value("width", 1280);
        p.windowHeight = w.value("height", 720);
    }
    if (j.contains("requiredFeatures")) {
        const auto& rf = j["requiredFeatures"];
        if (!rf.is_array()) return Result<Project>::failure("\"requiredFeatures\" must be an array");
        std::vector<FeatureRequirement> req;
        for (size_t i = 0; i < rf.size(); ++i) {
            auto r = FeatureRequirement::fromJson(rf[i]);
            // never ignore a requirement we cannot read: a project could lose features that way
            if (!r) return Result<Project>::failure(std::format("requiredFeatures[{}] is not a valid feature requirement", i));
            req.push_back(*r);
        }
        p.requiredFeatures = mergeRequirements(req, {});
    }
    if (j.contains("minEdition")) {
        if (!j["minEdition"].is_string()) return Result<Project>::failure("\"minEdition\" must be a string");
        p.minEditionInFile = j["minEdition"].get<std::string>();
    }
    static const char* kKnownKeys[] = {"format", "version", "name", "gameName", "startupScene", "modules", "icon", "subsystem", "splash", "render",
                                       "graphics", "input", "ui", "cook", "window", "requiredFeatures", "minEdition"};
    for (auto& [k, v] : j.items())
        if (std::find(std::begin(kKnownKeys), std::end(kKnownKeys), k) == std::end(kKnownKeys)) p.extra[k] = v;
    // gameName becomes a file name: keep it safe.
    for (char c : p.gameName)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            return Result<Project>::failure("gameName may only contain letters, digits, '_' and '-'");
    if (p.gameName.empty()) return Result<Project>::failure("gameName is empty");
    return p;
}

bool Project::hasScripts() const {
    std::error_code ec;
    if (file.empty() || !fs::is_directory(scriptsDir(), ec)) return false;
    for (auto it = fs::recursive_directory_iterator(scriptsDir(), ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file() && it->path().extension() == ".cpp") return true;
    return false;
}

fs::path Project::currentScriptModule() const {
    if (file.empty()) return {};
    auto name = readFileText(scriptBuildDir() / "current.txt");
    if (!name) return {};
    std::string n = *name;
    while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
    if (n.empty() || n.find('/') != std::string::npos || n.find('\\') != std::string::npos) return {};
    fs::path p = scriptBuildDir() / "bin" / pathFromUtf8(n);
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? p : fs::path();
}

Result<Project> Project::load(const fs::path& file) {
    auto text = readFileText(file);
    if (!text) return Result<Project>::failure("cannot read " + narrow(file.wstring()));
    Json j = Json::parse(*text, nullptr, false);
    if (j.is_discarded()) return Result<Project>::failure("project file is not valid JSON");
    auto r = fromJson(j);
    if (r) r->file = fs::absolute(file);
    return r;
}

Status Project::save() const {
    Json j = toJson();
    if (auto st = validate(j); !st) return st;
    return writeFileText(file, j.dump(2)) ? Status::success() : Status::failure("cannot write project file");
}

Result<Project> Project::createNew(const fs::path& directory, const std::string& projectName) {
    Project p;
    p.name = projectName;
    p.gameName = projectName;
    p.windowTitle = projectName;
    p.file = fs::absolute(directory / (projectName + kExtension));
    std::error_code ec;
    fs::create_directories(p.contentDir() / "Scenes", ec);
    fs::create_directories(p.contentDir() / "Meshes", ec);
    fs::create_directories(p.contentDir() / "Textures", ec);
    if (ec) return Result<Project>::failure("cannot create project folders");
    Scene s;
    s.name = "Main";
    EntityId cam = s.createEntity("Camera");
    s.get<TransformComponent>(cam)->position = {0, 2, 6};
    s.get<TransformComponent>(cam)->rotation = Quat::fromEuler({radians(-15), 0, 0});
    s.add<CameraComponent>(cam);
    EntityId sun = s.createEntity("Sun");
    s.get<TransformComponent>(sun)->rotation = Quat::fromEuler({radians(-50), radians(30), 0});
    s.add<LightComponent>(sun);
    if (!writeFileText(p.contentDir() / "Scenes" / "Main.fscene", s.toJson().dump(2)))
        return Result<Project>::failure("cannot write default scene");
    if (auto st = p.save(); !st) return Result<Project>::failure(st.error);
    return p;
}

} // namespace fledge
