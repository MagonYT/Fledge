// fledge_runtime - the shipping game player.
//
// Links ONLY engine runtime modules (core, platform, render, scene, assets,
// script, framework). No editor, AI, MCP or credential code - enforced by
// cmake/CheckRuntimeDeps.cmake and the runtime_no_ai_strings test.
//
// Content discovery order:
//   1. --pak <file>
//   2. a .fpak appended to this executable (packaged game)
//   3. <exe-stem>.fpak next to the executable (sidecar)
//   4. --project <file.fledge> or --content <dir> (loose files, development)
//
// Usage: fledge_runtime [--pak f | --project p | --content dir] [--rhi auto|d3d12|d3d11|d3d10|d3d9|opengl|vulkan|null]
//                     [--no-fallback] [--render key=value]... [--graphics key=value]... [--path-trace] [--freeze-time]
//                     [--scripts <module.dll>] [--scene <file.fscene>] [--lang <code>] [--headless] [--smoke-test] [--frames N]
//                     [--screenshot out.png] [--width W --height H] [--log-file path]
//                     [--server [host:]port [--net-clients N] | --connect host:port [--net-bot]]  (docs/NETWORKING.md)
//
// Graphics API: --rhi overrides the project's "graphics.api". The fallback chain
// (fledge/render/graphics_api.h) tries the next API when one is unavailable, e.g. D3D12 -> D3D11 ->
// OpenGL; --no-fallback disables that (tests).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <vector>
#include <string>

#include "fledge/assets/image.h"
#include "fledge/assets/pak.h"
#include "fledge/core/log.h"
#include "fledge/core/log_file.h"
#include "fledge/core/paths.h"
#include "fledge/core/version.h"
#include "fledge/edition/edition.h"
#include "fledge/framework/game_world.h"
#include "fledge/gameplay/ui.h"
#include "fledge/framework/project.h"
#include "fledge/platform/window.h"
#include "fledge/render/graphics_api.h"
#include "fhtml_overlay.h"
#include "net_session.h"
#include "profile_report.h"
#include "settings_menu.h"

#include <thread>

using namespace fledge;

namespace {
struct Args {
    fs::path pak, content, project, screenshot, scripts, scene, logFile;
    bool crashTest = false;
    bool profile = false;  // --profile: per-system CPU times + memory after the run
    bool developer = false;
    bool hiddenWindow = false;
    std::string fhtml;
    bool noSplash = false;
    bool headless = false, smoke = false, noFallback = false, pathTrace = false, freeze = false, menu = false, noInstancing = false, cpuSkinning = false, countNonFinite = false, injectNonFinite = false,
         noAudio = false;
    std::string preset;  // --preset low|medium|high|ultra|everything (auto-tune benchmark)
    std::optional<Project> projectManifest;  // set by --project
    int frames = -1, width = 0, height = 0;
    std::string rhi;                            // empty = project graphics.api
    std::string language;                       // --lang overrides the project's ui.language
    std::vector<std::string> renderOverrides;  // RenderSettings "key=value" (value parsed as JSON when possible)
    std::vector<std::string> graphicsOverrides;  // GraphicsSettings "key=value" (rt.<key> for ray tracing)
    NetOptions net;                               // --server / --connect / --net-clients / --net-bot
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        auto passOn = [&](const std::string& v) {  // content arguments are handed to spawned net clients
            a.net.contentArgs.push_back(s);
            a.net.contentArgs.push_back(v);
            return v;
        };
        if (s == "--pak") a.pak = pathFromUtf8(passOn(next()));
        else if (s == "--content") a.content = pathFromUtf8(passOn(next()));
        else if (s == "--project") a.project = pathFromUtf8(passOn(next()));
        else if (s == "--server") a.net.server = next();
        else if (s == "--connect") a.net.connect = next();
        else if (s == "--net-clients") a.net.spawnClients = std::atoi(next().c_str());
        else if (s == "--net-bot") a.net.bot = true;
        else if (s == "--headless") a.headless = true;
        else if (s == "--hidden-window") a.hiddenWindow = true;
        else if (s == "--smoke-test") a.smoke = true;
        else if (s == "--frames") a.frames = std::atoi(next().c_str());
        else if (s == "--screenshot") a.screenshot = pathFromUtf8(next());
        else if (s == "--width") a.width = std::atoi(next().c_str());
        else if (s == "--height") a.height = std::atoi(next().c_str());
        else if (s == "--rhi") a.rhi = next();
        else if (s == "--render") a.renderOverrides.push_back(next());
        else if (s == "--graphics") a.graphicsOverrides.push_back(next());
        else if (s == "--no-fallback") a.noFallback = true;
        else if (s == "--path-trace") a.pathTrace = true;  // progressive path-traced reference (D3D12 + DXR)
        else if (s == "--no-instancing") a.noInstancing = true;
        else if (s == "--no-audio") a.noAudio = true;  // null audio device (smoke tests never make sound)
        else if (s == "--count-nonfinite") a.countNonFinite = true;  // 1.3.3 debug: NaN / Inf pixel counter (SMOKE-TEST nonFinite*)
        else if (s == "--inject-nonfinite") a.injectNonFinite = true;  // 1.3.3 tests: NaN / Inf pixels written into the HDR image
        else if (s == "--cpu-skinning") a.cpuSkinning = true;  // tests: CPU vs GPU skinning comparison  // tests: per-item draws (instancing comparison)
        else if (s == "--profile") a.profile = true;  // PROFILE line: per-system CPU ms per frame, scene load, memory
        else if (s == "--developer") a.developer = true;  // F8 console, F9 HUD
        else if (s == "--fhtml") a.fhtml = next();  // Content-relative .fhtml page
        else if (s == "--no-splash") a.noSplash = true;
        else if (s == "--freeze-time") a.freeze = true;    // dt = 0 (static screenshots, path-trace convergence)
        else if (s == "--preset") a.preset = next();
        else if (s == "--settings-menu") a.menu = true;    // open the player settings menu at start (screenshots)
        else if (s == "--scripts") a.scripts = pathFromUtf8(passOn(next()));
        else if (s == "--scene") a.scene = pathFromUtf8(passOn(next()));
        else if (s == "--log-file") a.logFile = pathFromUtf8(next());  // instead of <user data>/Games/<game>/Logs/fledge-<date>.log
        else if (s == "--crash-test") a.crashTest = true;  // tests: crash on purpose after the log is open (crash handler test)
        else if (s == "--lang") a.language = next();  // UI language (Content/Localization/<lang>.json)  // editor out-of-process play: the (unsaved) scene the editor wrote
        else if (s == "--help" || s == "-h") {
            std::printf("usage: %s [--pak file | --project file | --content dir] [--rhi auto|d3d12|d3d11|d3d10|d3d9|opengl|vulkan|null] "
                        "[--no-fallback] [--render key=value]... [--graphics key=value]... "
                        "[--headless] [--smoke-test] [--frames N] [--screenshot out.png] [--log-file path] [--developer] [--fhtml page.fhtml] "
                        "[--server [host:]port [--net-clients N] | --connect host:port [--net-bot]]\n", argv[0]);
            std::exit(0);
        }
    }
    if (a.smoke && a.frames < 0) a.frames = 60;
    if (a.frames > 0) a.net.clientFrames = a.frames;
    return a;
}
} // namespace

int main(int argc, char** argv) {
    attachParentConsoleIfNeeded();  // packaged games use the GUI subsystem
    initEdition();  // Standard edition features
    Args args = parseArgs(argc, argv);
    fs::path exe = executablePath();
    Vfs vfs;
    std::string source;

    if (!args.pak.empty()) {
        auto pak = PakReader::openFile(args.pak);
        if (!pak) { FLEDGE_LOG_ERROR("runtime", "Cannot open pak {}", narrow(args.pak.wstring())); return 2; }
        vfs.mount("content", std::make_shared<PakProvider>(std::move(pak)));
        source = "pak file";
    } else if (auto appended = PakReader::openAppended(exe)) {
        vfs.mount("content", std::make_shared<PakProvider>(std::move(appended)));
        source = "embedded pak";
    } else if (auto side = PakReader::openFile(exe.parent_path() / (exe.stem().wstring() + L".fpak"))) {
        vfs.mount("content", std::make_shared<PakProvider>(std::move(side)));
        source = "sidecar pak";
    } else if (!args.project.empty()) {
        auto p = Project::load(args.project);
        if (!p) { FLEDGE_LOG_ERROR("runtime", "{}", p.error()); return 2; }
        // Cooked data from the cook cache (fledgepack --cook) overlays the loose content.
        vfs.mount("content", std::make_shared<OverlayProvider>(std::vector<std::shared_ptr<FileProvider>>{
                                 std::make_shared<DirectoryProvider>(p->cookDir()), std::make_shared<DirectoryProvider>(p->contentDir())}));
        source = "project " + narrow(args.project.filename().wstring());
        args.projectManifest = p.value();
    } else if (!args.content.empty()) {
        vfs.mount("content", std::make_shared<DirectoryProvider>(args.content));
        source = "loose content";
    } else {
        FLEDGE_LOG_ERROR("runtime", "No game content found (no embedded pak, no sidecar, no --pak/--content)");
        return 2;
    }

    // Project manifest: packaged games carry Game.fledge inside the pak.
    Project project = args.projectManifest.value_or(Project{});
    if (!args.projectManifest) {
        if (auto text = vfs.readText(std::string("content:/") + Project::kPakManifestName)) {
            auto p = Project::fromJson(Json::parse(*text, nullptr, false));
            if (p) project = p.value();
            else FLEDGE_LOG_WARN("runtime", "Invalid embedded project manifest: {}", p.error());
        }
    }
    FLEDGE_LOG_INFO("runtime", "Starting '{}' from {}", project.gameName, source);
    // Editions (docs/EDITIONS.md): fledgepack ships the runtime the game needs; a game whose manifest asks for features this
    // runtime lacks says so and stops instead of running without them.
    if (auto missing = missingFeatures(project.requiredFeatures, FeatureRegistry::get()); !missing.empty()) {
        std::string list;
        for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m.title + " (" + m.reason + ")";
        FLEDGE_LOG_ERROR("runtime", "'{}' needs features this runtime does not have: {}", project.gameName, list);
        std::printf("RUNTIME-MISSING-FEATURES %s\n", list.c_str());
        std::fflush(stdout);
        return 7;
    }

    AssetRegistry registry;
    if (auto st = registry.load(vfs); !st) FLEDGE_LOG_WARN("runtime", "{}", st.error);

    // Render setting overrides (testing / tuning): --render debugView=Normal --render msaa=4 ...
    for (const auto& kv : args.renderOverrides) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string key = kv.substr(0, eq), value = kv.substr(eq + 1);
        Json v = Json::parse(value, nullptr, false);
        if (v.is_discarded()) v = value;
        if (auto st = project.render.fromJson(Json{{key, v}}); !st) FLEDGE_LOG_WARN("runtime", "--render {}: {}", kv, st.error);
    }
    project.render.sanitize();
    // Per-game user data: shader cache + the player's graphics overrides (in-game settings menu, F10).
    // Smoke tests ignore saved overrides so they are deterministic.
    fs::path gameData = enginePaths().userData / "Games" / pathFromUtf8(project.gameName.empty() ? "Untitled" : project.gameName);
    // Persistent log (1.2.1): <user data>/Games/<game>/Logs/fledge-<date>.log or --log-file; startup lines are backfilled.
    {
        LogFileOptions lo;
        lo.appName = std::format("fledge_runtime {} '{}'", FLEDGE_VERSION_STRING, project.gameName);
        lo.directory = gameData / "Logs";
        lo.overridePath = args.logFile;
        if (auto st = LogFile::get().open(lo); !st) FLEDGE_LOG_WARN("runtime", "No log file: {}", st.error);
        else FLEDGE_LOG_INFO("runtime", "Log file: {}", narrow(LogFile::get().path().wstring()));
        installCrashHandler();
        if (args.crashTest) crashForTest();
    }
    fs::path playerSettings = gameData / "settings.json";
    if (!args.smoke) project.graphics = PlayerSettingsMenu::loadOverrides(playerSettings, project.graphics);
    // --preset first, so --graphics key=value can still override single values of the preset
    if (!args.preset.empty()) {
        GraphicsPreset p;
        if (fromString(args.preset, p)) applyGraphicsPreset(p, project.render, project.graphics);
        else FLEDGE_LOG_WARN("runtime", "--preset {}: unknown preset", args.preset);
    }
    if (!args.graphicsOverrides.empty()) {
        Json gj = project.graphics.toJson();
        for (const auto& kv : args.graphicsOverrides) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            Json v = Json::parse(kv.substr(eq + 1), nullptr, false);
            if (v.is_discarded()) v = kv.substr(eq + 1);
            std::string key = kv.substr(0, eq);
            if (key.rfind("rt.", 0) == 0) gj["rayTracing"][key.substr(3)] = v;
            else gj[key] = v;
        }
        if (auto g = GraphicsSettings::fromJson(gj)) project.graphics = g.value();
        else FLEDGE_LOG_WARN("runtime", "--graphics: {}", g.error());
    }

    if (project.graphics.shaderCache) rhi::setShaderCacheDirectory(gameData / "ShaderCache");

    // ---- graphics API + fallback chain -----------------------------------------------------
    bool headless = args.headless || args.rhi == "null";
    rhi::GraphicsApi api = project.graphics.api;
    if (!headless && !args.rhi.empty() && !rhi::graphicsApiFromString(args.rhi, api))
        FLEDGE_LOG_WARN("runtime", "--rhi {}: unknown graphics API, using the project setting ({})", args.rhi, rhi::toString(api));
    std::vector<rhi::Backend> chain = headless ? std::vector<rhi::Backend>{} : rhi::fallbackChain(api);
    if (args.noFallback && !chain.empty()) chain.resize(1);
    WindowDesc wd;
    wd.visible = !args.hiddenWindow;
    wd.title = project.windowTitle.empty() ? project.gameName : project.windowTitle;
    wd.width = args.width ? args.width : project.windowWidth;
    wd.height = args.height ? args.height : project.windowHeight;
    bool vsync = !args.smoke && project.graphics.vsync == VsyncMode::On;
    wd.vsync = vsync;
    std::unique_ptr<Window> window;
    std::unique_ptr<rhi::RenderDevice> device;
    bool windowIsGL = false;
    // Auto prefers full-pipeline devices: a reduced one (e.g. D3D9) is kept only as a last resort.
    std::unique_ptr<rhi::RenderDevice> reduced;
    std::unique_ptr<Window> reducedWindow;
    for (rhi::Backend b : chain) {
        bool wantGL = b == rhi::Backend::OpenGL;
        if (!window || windowIsGL != wantGL) {
            window.reset();
            wd.openGL = wantGL;
            window = Window::createGlfw(wd);
            windowIsGL = wantGL;
        }
        if (!window) {
            FLEDGE_LOG_WARN("runtime", "{}: could not create a window", rhi::toString(b));
            continue;
        }
        rhi::DeviceCreateInfo info;
        info.glLoader = window->glLoader();
        info.nativeWindow = window->nativeHandle();
        info.width = window->width();
        info.height = window->height();
        info.vsync = vsync;
        info.allowTearing = project.graphics.vsync == VsyncMode::Vrr;
        info.hdr = project.graphics.hdr;
        device = rhi::createRenderDevice(b, info);
        if (device && api == rhi::GraphicsApi::Auto && !device->caps().fullPipeline && !reduced) {
            FLEDGE_LOG_INFO("runtime", "{} runs a reduced pipeline; trying the next API first", rhi::toString(b));
            reduced = std::move(device);
            reducedWindow = std::move(window);
            continue;
        }
        if (device) break;
        FLEDGE_LOG_WARN("runtime", "{} unavailable, trying the next graphics API", rhi::toString(b));
    }
    if (!device && reduced) {
        device = std::move(reduced);
        window = std::move(reducedWindow);
    }
    reduced.reset();
    reducedWindow.reset();
    if (device && api != rhi::GraphicsApi::Auto && rhi::toGraphicsApi(device->backend()) != api)
        FLEDGE_LOG_WARN("runtime", "Requested {} but running on {} (fallback)", rhi::displayName(api), rhi::toString(device->backend()));
    if (!window || !device) {
        if (!headless) FLEDGE_LOG_WARN("runtime", "No GPU window available, falling back to headless Null RHI");
        window = Window::createNull(wd);
        device = rhi::createRenderDevice(rhi::Backend::Null);
    }
    if (!args.smoke && !args.hiddenWindow && project.graphics.windowMode != WindowMode::Windowed)
        window->setDisplayMode(static_cast<int>(project.graphics.windowMode), project.graphics.width, project.graphics.height);
    if (!args.smoke && !args.hiddenWindow) window->requestFocus();
    ForwardRenderer renderer(*device);
    std::unique_ptr<FhtmlOverlay> browser;
    if (args.fhtml.empty() && project.extra.contains("fhtmlPage") && project.extra["fhtmlPage"].is_string())
        args.fhtml = project.extra["fhtmlPage"].get<std::string>();
    if (!args.fhtml.empty()) {
        const auto relative = pathFromUtf8(args.fhtml);
        const bool safePath = !relative.has_root_path() && endsWith(toLower(args.fhtml), ".fhtml") &&
                              std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }) &&
                              args.fhtml.find(':') == std::string::npos;
        auto page = safePath ? vfs.readText("content:/" + args.fhtml) : std::nullopt;
        if (page) {
            browser = std::make_unique<FhtmlOverlay>(*device);
            if (!browser->start(*page, gameData / "Fhtml")) browser.reset();
        }
        if (!browser) FLEDGE_LOG_WARN("fhtml", "Could not start '{}'. Check the Content path, page size, and Fhtml helper.", args.fhtml);
    }
    renderer.instancingEnabled = !args.noInstancing;
    renderer.gpuSkinningEnabled = !args.cpuSkinning;
    renderer.setNonFiniteCheck(args.countNonFinite);
    renderer.setNonFiniteInjection(args.injectNonFinite);

    GameWorld world(vfs, registry);
    world.renderSettings = project.render;
    world.uiSettings = project.ui;
    if (!args.language.empty()) world.uiSettings.language = args.language;
    world.graphicsSettings = project.graphics;
    world.pathTraceReference = args.pathTrace;
    PlayerSettingsMenu menu(world.graphicsSettings, world.renderSettings, playerSettings, *device, *window);
    menu.setOpen(args.menu);
    world.setRenderer(&renderer);
    world.modules().addSearchPath(exe.parent_path());
    for (auto& m : project.modules)
        if (auto st = world.modules().load(m); !st) FLEDGE_LOG_WARN("runtime", "{}", st.error);

    // Fledge Script files: --scripts, else <gameName>Scripts.dll next to the exe (packaged game),
    // else the project's latest development build (Intermediate/Scripts).
    fs::path scriptModule = args.scripts;
    if (scriptModule.empty()) {
        fs::path packaged = exe.parent_path() / (widen(project.scriptModuleName()) + L".dll");
        std::error_code ec;
        if (fs::is_regular_file(packaged, ec)) scriptModule = packaged;
        else if (args.projectManifest) scriptModule = args.projectManifest->currentScriptModule();
    }
    if (auto st = world.scripts().input().load(project.input); !st) FLEDGE_LOG_WARN("runtime", "{}", st.error);
    if (!args.smoke)  // the player's own bindings (F10 > Controls) override the project's
        if (Json mine = PlayerSettingsMenu::loadInputOverrides(playerSettings); !mine.is_null())
            if (auto st = world.scripts().input().load(mine); !st) FLEDGE_LOG_WARN("runtime", "{}", st.error);
    menu.setInputMap(&world.scripts().input());
    if (!scriptModule.empty()) {
        if (auto st = world.scripts().loadModule(scriptModule); !st) FLEDGE_LOG_ERROR("runtime", "{}", st.error);
    } else if (args.projectManifest && args.projectManifest->hasScripts()) {
        FLEDGE_LOG_WARN("runtime", "Project has Scripts/ but no built script module: run `fledgepack --build-scripts` (or Build in the editor)");
    }

    std::string scenePath = "content:/" + project.startupScene;
    if (!args.scene.empty()) {  // a scene file outside the content (editor play session); assets still resolve from content
        vfs.mount("session", std::make_shared<DirectoryProvider>(args.scene.parent_path()));
        scenePath = "session:/" + narrow(args.scene.filename().wstring());
    }
    bool showSplash = project.splash && !args.noSplash && !args.smoke && !headless && !args.hiddenWindow && window && device && window->width() > 0;
    if (showSplash) {
        window->pollEvents();
        int w = std::max(1, window->width()), h = std::max(1, window->height());
        gameplay::UiCanvas splash(static_cast<float>(w), static_cast<float>(h));
        splash.rect(0, 0, static_cast<float>(w), static_cast<float>(h), {0.06f, 0.07f, 0.09f, 1});
        float bar = std::min(420.0f, static_cast<float>(w) * 0.34f);
        splash.rect((w - bar) * 0.5f, h * 0.52f, bar, 6, {0.93f, 0.45f, 0.22f, 1});
        std::string mark = "FLEDGE";
        Vec2 markSize = splash.measure(mark, 6);
        splash.text((w - markSize.x) * 0.5f, h * 0.52f - 78, mark, {0.95f, 0.95f, 0.96f, 1}, 6, false);
        std::string game = project.gameName.empty() ? project.name : project.gameName;
        Vec2 gameSize = splash.measure(game, 2.4f);
        splash.text((w - gameSize.x) * 0.5f, h * 0.52f + 22, game, {0.75f, 0.78f, 0.82f, 1}, 2.4f, false);
        std::vector<ForwardRenderer::OverlayVertex> overlay;
        overlay.reserve(splash.vertices().size());
        for (const auto& vertex : splash.vertices()) overlay.push_back({vertex.pos, vertex.color, vertex.uv});
        renderer.setOverlay(std::move(overlay));
        ViewItem view;
        view.position = {0, 2, 6};
        view.view = Mat4::lookAt(view.position, {0, 0, 0}, {0, 1, 0});
        view.projection = Mat4::perspective(radians(60), static_cast<float>(w) / static_cast<float>(h), 0.1f, 100);
        renderer.render({}, w, h, view, {}, {}, RenderOptions{});
        device->present();
        window->swapBuffers();
    }
    auto loadStart = std::chrono::steady_clock::now();
    auto loaded = world.loadScene(scenePath);
    double sceneLoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
    if (auto st = loaded; !st) {
        FLEDGE_LOG_ERROR("runtime", "{}", st.error);
        return 3;
    }
    world.openAudio(args.smoke || args.headless || args.noAudio);
    world.saveDirectory = playerSettings.parent_path() / "saves";  // <user data>/Games/<game>/saves (never the game folder)
    world.beginPlay();

    Clock clock;
    std::unique_ptr<NetSession> net;
    if (args.net.active()) {
        net = NetSession::start(args.net, world.scene(), clock.now());
        if (!net) return 5;
    }
    // A server that launched its own bot clients runs until they are done, not for --frames.
    bool frameLimited = !(net && net->isServer() && args.net.spawnClients > 0);
    double nextNetTick = clock.now(), netStart = clock.now();
    int frame = 0;
    uint32_t lastDraws = 0, lastTris = 0;
    double timedStart = 0;
    int timedFrames = 0;
    bool fhtmlToggleHeld = false, fhtmlVisible = true;
    const std::string virtualPage = "content:/" + args.fhtml;
    DeveloperOverlay developer;
    int lastW = window->width(), lastH = window->height(), skipped = 0;
    while (!window->shouldClose()) {
        window->pollEvents();
        if (!developer.consoleOpen()) menu.update(window->input());
        const bool developerInput = args.developer && developer.update(window->input(), !menu.isOpen());
        const bool gameInput = !menu.isOpen() && !developerInput;
        const bool fhtmlToggle = window->input().raw[296]; // F7
        if (browser && gameInput && fhtmlToggle && !fhtmlToggleHeld) fhtmlVisible = !fhtmlVisible;
        fhtmlToggleHeld = fhtmlToggle;
        auto matchesPage = [&](const UiScreenComponent& screen) {
            return screen.document == args.fhtml || screen.document == virtualPage;
        };
        bool hasScreen = false, screenVisible = false, screenInteractive = false;
        if (browser) {
            for (const auto& screen : world.scene().pool<UiScreenComponent>().components()) {
                if (!matchesPage(screen)) continue;
                hasScreen = true;
                screenVisible |= screen.visible;
                screenInteractive |= screen.visible && screen.interactive;
            }
        }
        const bool showBrowser = gameInput && fhtmlVisible && (!hasScreen || screenVisible);
        const bool browserInput = showBrowser && (!hasScreen || screenInteractive) && browser && browser->texture();
        if (browser) {
            browser->setState(world.uiStateJson());
            browser->update(*window, browserInput);
            if (auto action = browser->takeAction(); !action.empty()) {
                for (auto& screen : world.scene().pool<UiScreenComponent>().components()) {
                    if (!screen.visible || !screen.interactive || !matchesPage(screen)) continue;
                    screen.clicked = action;
                    ++screen.clicks;
                }
            }
        }
        renderer.setBrowserOverlay(showBrowser && browser ? browser->texture() : rhi::TextureHandle{});
        // Escape is for the game (pause, inventory) and the F10 settings menu. Closing the window is Alt+F4 / the title-bar X.
        float dt = args.smoke ? 1.0f / 60.0f : clock.tick();  // fixed step in smoke tests => deterministic
        if (args.freeze) dt = 0.0f;
        if (net) {  // multiplayer: server and clients step at a fixed 60 Hz in real time
            constexpr double kStep = 1.0 / 60.0;
            nextNetTick += kStep;
            double wait = nextNetTick - clock.now();
            if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
            else if (wait < -0.25) nextNetTick = clock.now();  // fell far behind (debugger, window drag): no catch-up burst
            dt = args.freeze ? 0.0f : static_cast<float>(kStep);
        }
        {
            script::InputFrame in;
            if (gameInput && !browserInput) {
                in.down = window->input().raw;
                in.mouseDx = window->input().mouseDelta.x;
                in.mouseDy = window->input().mouseDelta.y;
                in.pad = window->input().padAxes;
            }
            world.scripts().setInput(in);
            world.uiPointer.pos = gameInput && !browserInput ? window->input().mousePos : Vec2{-1, -1};
            world.uiPointer.down = gameInput && !browserInput && window->input().mouseDown(MouseButton::Left);
        }
        world.tick(dt);
        if (net) net->update(clock.now(), gameInput && !browserInput ? &window->input() : nullptr);
        // Standalone games lock the mouse whenever the game owns input. Scripted FPS titles have no
        // PlayerController; requiring one left the cursor free in packaged builds. An interactive .fui
        // screen, the settings menu, FHTML, or a lost window focus releases it. Clicking the window
        // recaptures if Windows refused the first lock (common until the window is focused).
        bool wantLock = !args.smoke && !headless && gameInput && !browserInput && !world.wantsCursor();
        if (wantLock && window->focused()) window->setCursorCaptured(true);
        else window->setCursorCaptured(false);
        int w = window->width(), h = window->height();
        if (args.smoke && frame > 0 && (w != lastW || h != lastH))  // something outside resized / minimised the window
            FLEDGE_LOG_WARN("runtime", "smoke test: window size changed from {}x{} to {}x{} at frame {} (accumulation restarts)", lastW, lastH, w, h, frame);
        lastW = w;
        lastH = h;
        device->resizeSwapchain(w, h);
        if (frame == 10) {
            timedStart = clock.now();  // skip warm-up (shader compile, IBL precompute)
            world.resetSystemTimes();
        }
        if (w > 0 && h > 0) {
            float aspect = static_cast<float>(w) / static_cast<float>(h);
            std::optional<ViewItem> view = net ? net->view(aspect) : std::nullopt;  // net client: follow its player
            if (!view) view = world.primaryView(aspect);
            if (!view) {
                ViewItem v;
                v.position = {0, 2, 6};
                v.view = Mat4::lookAt(v.position, {0, 0, 0}, {0, 1, 0});
                v.projection = Mat4::perspective(radians(60), aspect, 0.1f, 500);
                view = v;
            }
            menu.draw(renderer, w, h);
            if (args.developer && !menu.isOpen()) developer.draw(renderer, w, h, dt, world.scene().entities().size(), lastDraws, lastTris);
            world.render({}, w, h, *view);
            lastDraws = device->stats().drawCalls;
            lastTris = device->stats().triangles;
            if (!args.screenshot.empty() && args.frames > 0 && frame == args.frames - 1) {
                ImageData img;
                img.width = w;
                img.height = h;
                if (device->readPixels({}, w, h, img.rgba) && writeFileBytes(args.screenshot, encodePng(img)))
                    FLEDGE_LOG_INFO("runtime", "Wrote screenshot {}", narrow(args.screenshot.wstring()));
            }
        }
        device->present();
        window->swapBuffers();
        if (frame >= 10) ++timedFrames;
        // smoke tests count rendered frames only: a minimised window (0x0) must not eat the frame budget
        bool rendered = w > 0 && h > 0;
        if (!frameLimited) {
            ++frame;
            if (net->spawnedClientsDone() || clock.now() - netStart > 180.0) break;  // bot clients finished (or hung)
            continue;
        }
        if (args.frames > 0 && (rendered || !args.smoke || ++skipped > args.frames * 10) && ++frame >= args.frames) break;
    }
    int netExit = net ? net->finish() : 0;
    net.reset();
    double avgMs = timedFrames > 0 ? (clock.now() - timedStart) * 1000.0 / timedFrames : 0.0;
    size_t behaviours = world.scripts().behaviourCount(), scriptObjects = world.scripts().objectCount();
    size_t physicsBodies = world.physics() ? world.physics()->bodyCount() : 0;  // smoke stats (before endPlay tears down)
    int audioVoices = world.audioStats().voices;
    if (args.profile) printProfile(world, sceneLoadMs, avgMs);
    world.endPlay();
    world.setRenderer(nullptr);

    if (args.smoke) {
        std::printf("SMOKE-TEST OK game=%s frames=%d entities=%zu drawCalls=%u triangles=%u backend=%s modules=%zu scripts=%s behaviours=%zu scriptObjects=%zu passes=%zu avgFrameMs=%.2f renderRes=%dx%d upscaler=%s rt=%d ptSamples=%u instanced=%d gpuSkinned=%d culled=%d physicsBodies=%zu audioVoices=%d audioDevice=%s viewmodelItems=%d decals=%d nonFiniteHdr=%d nonFiniteHdrInf=%d nonFinitePost=%d\n",
                    project.gameName.c_str(), frame, world.scene().entities().size(), lastDraws, lastTris,
                    rhi::toString(device->backend()), world.modules().loadedModules().size(),
                    world.scripts().hasModule() ? narrow(world.scripts().module().path.filename().wstring()).c_str() : "none", behaviours,
                    scriptObjects, renderer.lastFrame().passes.size(), avgMs, renderer.lastFrame().renderWidth, renderer.lastFrame().renderHeight,
                    toString(renderer.lastFrame().upscaler), renderer.lastFrame().rayTracing ? 1 : 0, renderer.lastFrame().pathTraceSamples,
                    renderer.lastFrame().instancedItems, renderer.lastFrame().gpuSkinned,
                    world.renderStats().frustumCulled + world.renderStats().occlusionCulled + world.renderStats().distanceCulled, physicsBodies,
                    audioVoices, world.audioDevice() ? world.audioDevice()->name().c_str() : "none", renderer.lastFrame().viewmodelItems,
                    renderer.lastFrame().decals, renderer.lastFrame().nonFiniteHdr, renderer.lastFrame().nonFiniteHdrInf,
                    renderer.lastFrame().nonFinitePost);
        std::fflush(stdout);
    }
    return netExit;
}
