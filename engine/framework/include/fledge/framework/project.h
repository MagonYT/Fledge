#pragma once
// .fledge - project manifest (JSON). Example:
// {
//   "format": "fledgeproject", "version": 1,
//   "name": "HelloFledge", "gameName": "HelloFledge",
//   "startupScene": "Scenes/Main.fscene",
//   "modules": ["HelloFledgeGame"],
//   "icon": "Icon.ico", "subsystem": "gui",
//   "window": { "title": "Hello Fledge", "width": 1280, "height": 720 },
//   "requiredFeatures": [ {"id": "advanced.weapons", "version": 1, "title": "Weapon system", "edition": "advanced"} ],
//   "minEdition": "advanced"
// }
// requiredFeatures / minEdition: docs/EDITIONS.md. requiredFeatures lists the edition features the project uses; the
// editor adds an entry the first time a feature from an Advanced module is used, never before. minEdition is derived
// from it on save (omitted for projects that need nothing beyond Fledge) and is not meant to be edited by hand.
// Old projects without the fields require nothing extra. Top-level keys a newer Fledge wrote are kept ("extra").
// SECURITY: project files are shared/committed, so they must never hold
// credentials. Project::validate() rejects secret-looking keys.
#include <string>
#include <vector>

#include "fledge/core/features.h"
#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/render/graphics_settings.h"
#include "fledge/render/renderer.h"

namespace fledge {

struct Project {
    static constexpr const char* kExtension = ".fledge";
    static constexpr const char* kPakManifestName = "Game.fledge";  // name inside a .fpak

    fs::path file;  // absolute path of the .fledge (empty in packaged games)
    std::string name = "Untitled";
    std::string gameName = "Untitled";
    std::string startupScene = "Scenes/Main.fscene";
    std::vector<std::string> modules;
    std::string icon;  // relative to project root, optional .ico
    // "subsystem": "gui" (default: no console window when the packaged game is double-clicked;
    // output still works when redirected) or "console" (keeps a console window, handy for debugging).
    bool guiSubsystem = true;
    // Boot splash before the first game frame. Both editions. false writes "splash": false.
    bool splash = true;
    std::string windowTitle;
    int windowWidth = 1280, windowHeight = 720;
    // "cook": { "textureCompression": "auto|bc1|bc3|none", "mips": true, "tangents": true }
    std::string textureCompression = "auto";
    bool textureMips = true;
    bool meshTangents = true;
    // "render": RenderSettings (tonemapper, exposure, bloom, shadows, environment, ...)
    RenderSettings render;
    // "graphics": API (auto/d3d9/10/11/12/opengl/vulkan), preset, upscaler, ray tracing, HDR, vsync,
    // window mode, render scale, shader cache - fledge/render/graphics_settings.h
    GraphicsSettings graphics;
    // "input": Fledge Script input map overrides (actions / axes), see fledge/script/input_map.h
    Json input;
    // "ui": game UI font (a .ttf / .otf under Content/, "" = bundled Roboto) and the language of the
    // Content/Localization/<lang>.json string tables (docs/GAMEPLAY.md "UI")
    struct UiSettings {
        std::string font;
        float fontSize = 32;  // atlas pixel height (text scales from it)
        std::string language = "en", fallbackLanguage = "en";
    } ui;
    // Edition features this project uses (docs/EDITIONS.md). Sorted by id, one entry per id.
    std::vector<FeatureRequirement> requiredFeatures;
    // "minEdition" as found in the file ("" when absent). Saving rewrites it from requiredFeatures.
    std::string minEditionInFile;
    // Top-level keys this version does not know: kept verbatim and written back by toJson().
    Json extra = Json::object();
    // Derived minimum edition: Advanced when any required feature is an Advanced feature.
    Edition minEdition() const;
    // Adds (or raises the version of) a requirement. Returns true when the list changed.
    bool requireFeature(const FeatureRequirement& r);

    fs::path rootDir() const { return file.parent_path(); }
    fs::path contentDir() const { return rootDir() / "Content"; }
    fs::path buildDir() const { return rootDir() / "Build"; }
    fs::path cookDir() const { return rootDir() / "Intermediate" / "Cooked"; }  // incremental cook cache
    // Fledge Script files: sources in Scripts/, build output in Intermediate/Scripts/, module <gameName>Scripts.dll.
    fs::path scriptsDir() const { return rootDir() / "Scripts"; }
    fs::path scriptBuildDir() const { return rootDir() / "Intermediate" / "Scripts"; }
    std::string scriptModuleName() const { return gameName + "Scripts"; }
    bool hasScripts() const;  // Scripts/ contains at least one .cpp
    // Latest development build of the script module (Intermediate/Scripts/current.txt), empty if none.
    fs::path currentScriptModule() const;

    Json toJson() const;
    static Result<Project> fromJson(const Json& j);
    static Result<Project> load(const fs::path& file);
    Status save() const;
    static Result<Project> createNew(const fs::path& directory, const std::string& name);  // scaffolds folders + empty scene
    static Status validate(const Json& j);  // rejects secret-looking keys anywhere in the document
};

} // namespace fledge
