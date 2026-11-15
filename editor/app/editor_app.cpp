#include "editor_app.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#ifdef _WIN32
#include <imgui_impl_dx11.h>
struct ID3D11Device;
struct ID3D11DeviceContext;
#endif

#include <algorithm>
#include <cstdlib>
#include <format>

#include "fledge/core/log.h"
#include "fledge/core/redact.h"
#include "fledge/core/version.h"
#include "editor_code.h"
#include "editor_icons.h"

namespace fledge::editor {

EditorApp::EditorApp(EditorOptions options) : m_opt(std::move(options)) {}
EditorApp::~EditorApp() { shutdown(); }

namespace {
// Box-filter downscale for docs screenshots (--screenshot-scale).
ImageData resizeImage(const ImageData& src, int w, int h) {
    ImageData out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int x0 = x * src.width / w, x1 = std::max(x0 + 1, (x + 1) * src.width / w);
            int y0 = y * src.height / h, y1 = std::max(y0 + 1, (y + 1) * src.height / h);
            unsigned acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx, ++n)
                    for (int c = 0; c < 4; ++c) acc[c] += src.rgba[(static_cast<size_t>(yy) * src.width + xx) * 4 + c];
            for (int c = 0; c < 4; ++c) out.rgba[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(acc[c] / n);
        }
    return out;
}

fs::path defaultProjectsDir(const EnginePaths& paths, const fs::path& settingsDir, bool customSettingsDir) {
    if (paths.portable) return paths.root / "Projects";
    if (customSettingsDir) return settingsDir / "Projects";
#ifdef _WIN32
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home) return fs::path(home) / "Documents" / "Fledge Projects";
#endif
    return settingsDir / "Projects";
}
} // namespace

// ------------------------------------------------------------------ setup --
bool EditorApp::init() {
    m_paths = enginePaths();
    bool customSettings = !m_opt.settingsDir.empty();
    if (!customSettings) m_opt.settingsDir = m_paths.userData;
    std::error_code ec;
    fs::create_directories(m_opt.settingsDir, ec);
    m_prefs.projectsDir = defaultProjectsDir(m_paths, m_opt.settingsDir, customSettings);
    // Settings migration (editor_migration.cpp): --settings-dir is the destination; applied migrations reload before the next frame.
    if (customSettings) m_session.customSettingsDir = m_opt.settingsDir;
    m_session.onUserSettingsMigrated = [this] { m_migrationReloadPending = true; };

    WindowDesc wd;
    wd.title = std::string(editionName(currentEdition())) + " Editor";
    wd.width = 1600;
    wd.height = 900;
    wd.maximized = m_opt.smokeFrames <= 0;
#ifdef _WIN32
    // Direct3D 11 submits the same scene in a fraction of the OpenGL CPU time (docs/PERFORMANCE.md).
    wd.openGL = false;
    m_window = Window::createGlfw(wd);
    if (m_window) {
        rhi::DeviceCreateInfo info;
        info.nativeWindow = m_window->nativeHandle();
        info.width = std::max(1, m_window->width());
        info.height = std::max(1, m_window->height());
        info.vsync = wd.vsync;
        m_device = rhi::createRenderDevice(rhi::Backend::D3D11, info);
        if (!m_device) {
            FLEDGE_LOG_WARN("editor", "Direct3D 11 did not start; falling back to OpenGL");
            m_window.reset();
        }
    }
#endif
    if (!m_device) {
        wd.openGL = true;
        m_window = Window::createGlfw(wd);
        if (!m_window) {
            FLEDGE_LOG_ERROR("editor", "The editor needs a Direct3D 11 or OpenGL 3.3 display. Use fledge_mcp_server for headless work.");
            return false;
        }
        m_device = rhi::createRenderDevice(rhi::Backend::OpenGL, m_window->glLoader());
        if (!m_device) return false;
    }
    FLEDGE_LOG_INFO("editor", "Editor renderer: {}", m_device->description());
    m_renderer = std::make_unique<ForwardRenderer>(*m_device);
    m_viewportRt = m_device->createRenderTarget(64, 64);
    m_session.world().setRenderer(m_renderer.get());
    {  // audio: the real device, except when FLEDGE_AUDIO=null (CI, silent runs)
        const char* env = std::getenv("FLEDGE_AUDIO");
        m_session.world().openAudio((env && std::string_view(env) == "null"));
    }
    m_session.screenshotProvider = [this](int w, int h) { return captureViewport(w, h); };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath;
    iniPath = narrow((m_opt.settingsDir / "editor_layout.ini").wstring());
    io.IniFilename = iniPath.c_str();
    if (m_device->backend() == rhi::Backend::OpenGL) {
        ImGui_ImplGlfw_InitForOpenGL(m_window->glfwHandle(), true);
        ImGui_ImplOpenGL3_Init("#version 330");
    } else {
#ifdef _WIN32
        ImGui_ImplGlfw_InitForOther(m_window->glfwHandle(), true);
        if (!ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(m_device->nativeDevice()),
                                 static_cast<ID3D11DeviceContext*>(m_device->nativeContext()))) {
            FLEDGE_LOG_ERROR("editor", "Dear ImGui Direct3D 11 backend failed to start");
            return false;
        }
#else
        return false;
#endif
    }

    registerEngineTools(m_tools, m_session);
    m_session.startBackgroundImport = [this](int id) { startStoreImport(id); };
    m_session.startBackgroundAssetImport = [this](const fs::path& source, const ModelImportOptions& options, bool instantiate) {
        return startAssetImport(source, options, instantiate);
    };
    m_session.backgroundAssetImportStatus = [this](int id) {
        auto job = m_assetImportJobs.find(id);
        return job == m_assetImportJobs.end() ? Json() : job->second;
    };
    initEditorControl();  // Full MCP Control host + the audit log (<settings>/Logs/mcp-audit-<date>.jsonl)
    m_session.hasLicenseUi = true;  // AI upscaler install requests become licence dialogs for the user
    m_mcp = std::make_unique<mcp::McpServer>(m_tools);
    registerEngineResources(*m_mcp, m_session);

    m_creds = ai::createDefaultCredentialStore();
    if (m_opt.mockAiTransport) {  // explicitly requested offline transport
        auto mock = std::make_unique<ai::MockHttpTransport>();
        m_mockHttp = mock.get();
        m_http = std::move(mock);
    } else {
        // Failed requests are retried with a growing delay; the chat says so each time (see editor_ai.cpp).
        auto retry = std::make_unique<ai::RetryingHttpTransport>(ai::createDefaultHttpTransport());
        m_httpRetry = retry.get();
        m_httpRetry->shouldStop = [this] { return !aiOn(); };
        m_httpRetry->onRetry = [this](const ai::RetryNotice& n) {
            std::lock_guard l(m_chatMutex);
            m_chat.push_back({ChatLine::Kind::Info, std::format("{} Trying again in {:.0f} s (try {} of {}).", redact(n.reason),
                                                                 std::max(1.0, n.delayMs / 1000.0), n.attempt + 1, n.maxAttempts)});
        };
        m_http = std::move(retry);
    }
    m_code = std::make_unique<CodeWorkspace>(*this);
    registerCommands();
    registerPanels();
    loadSettings();
    m_editorSplash = m_prefs.showSplash && m_opt.smokeFrames <= 0;
    // Fresh installs of Fledge start in Simple Mode with the tour, fresh installs of Fledge Advanced in Pro Mode (the
    // toggle works the same in both); tests, screenshots (--workspace) and --pro start in Pro Mode.
    bool forcedPro = m_opt.mode == "pro" || !m_opt.workspace.empty() || (m_opt.smokeFrames > 0 && m_opt.mode != "simple");
    if (m_opt.mode == "simple" || (m_firstRun && !forcedPro && currentEdition() == Edition::Standard)) m_prefs.simpleMode = true;
    if (forcedPro && m_opt.mode != "simple") m_prefs.simpleMode = false;
    if (m_prefs.simpleMode) {
        if (m_prefs.workspace != "Simple") m_prefs.proWorkspace = m_prefs.workspace;
        m_prefs.workspace = "Simple";
        m_pendingReset = true;
        if (!m_prefs.tourDone && m_opt.smokeFrames <= 0) startTour();
    } else if (m_prefs.workspace == "Simple") {
        m_prefs.workspace = m_prefs.proWorkspace;
    }
    applyStartupOptions();
    m_fontsDirty = true;
    rebuildFonts();
    rebuildProvider();

    fs::path project = m_opt.projectFile;
    if (project.empty() && m_prefs.restoreLastProject && !m_recent.items().empty() && fs::exists(m_recent.items().front(), ec))
        project = m_recent.items().front();
    if (!project.empty()) openProject(project);
    // Scripted runs (smoke frames, screenshots) open samples in place: they neither write recovery files there nor
    // show the recovery dialog (their snapshots, if any, are left alone).
    if (scriptedRun() && m_session.recoveryOffer()) m_session.keepSavedVersion();
    if (!m_opt.select.empty() && m_session.hasProject()) m_session.select(m_session.scene().findByName(m_opt.select));
    if (!m_session.hasProject() && m_prefs.showWelcomeOnStartup && m_opt.smokeFrames <= 0 && !m_prefs.simpleMode) m_showWelcome = true;
    initMigrationOffer();  // Fledge Advanced: offer once to bring Fledge's settings over (non-blocking banner)
    if (m_opt.aiOff) m_session.ai.setEnabled(false);
    m_session.ai.applyTo(m_mcp->policy());
    if (m_opt.mcpHttpPort > 0) startMcpHttp(m_opt.mcpHttpPort);
    else if (m_mcpAutoStart) startMcpHttp(m_mcpPortSetting);
    initStoreImport();  // link IPC (when enabled), drag and drop, --open-url links
    return true;
}

void EditorApp::applyStartupOptions() {
    if (m_opt.theme == "light") m_prefs.theme = Theme::Light;
    else if (m_opt.theme == "dark") m_prefs.theme = Theme::Dark;
    if (!m_opt.workspace.empty()) {
        m_prefs.workspace = m_opt.workspace;
        m_pendingReset = true;  // default layout of that workspace
    }
    if (m_opt.show == "palette") openPalette(!m_opt.paletteQuery.empty() && m_opt.paletteQuery[0] == '>', m_opt.paletteQuery);
    if (m_opt.show == "welcome") m_showWelcome = true;
    if (m_opt.show == "wizard") {
        m_showWelcome = true;
        m_welcomeTab = 1;
    }
    if (m_opt.show == "settings") m_showSettings = true;
    if (m_opt.show == "tour") startTour();
    if (m_opt.show == "shortcuts") m_showShortcuts = true;
    if (m_opt.show == "about") m_showAbout = true;
    if (m_opt.show == "project-settings") m_showProjectSettings = true;
    if (m_opt.show == "graphics") {
        m_frontGraphics = true;  // front tab after the layout is built
        if (PanelDef* d = panel("Graphics & Platform")) d->open = true;
        m_focusQueue.push_back("Graphics & Platform");
    }
    if (m_opt.demoChat) {
        std::lock_guard l(m_chatMutex);
        m_chat.clear();
        auto add = [&](ChatLine c) {
            c.demo = true;
            m_chat.push_back(std::move(c));
        };
        add({ChatLine::Kind::User, "Add three red cubes in a row next to the sphere and light them warmly."});
        add({ChatLine::Kind::Assistant, "Sure - I'll create a red material, place three cubes 1.5 m apart and add a warm point light."});
        ChatLine t1{ChatLine::Kind::Tool, ""};
        t1.toolName = "create_material";
        t1.toolArgs = R"({"name":"Red","properties":{"baseColor":[0.9,0.1,0.08],"roughness":0.4}})";
        t1.toolResult = "Created Materials/Red.fmat";
        t1.toolDone = true;
        add(t1);
        ChatLine t2{ChatLine::Kind::Tool, ""};
        t2.toolName = "create_entity";
        t2.toolArgs = R"({"name":"Red Cube 1","components":{"Transform":{"position":[2,0.5,0]},"MeshRenderer":{"mesh":"builtin:cube"}}})";
        t2.toolResult = "entity 12";
        t2.toolDone = true;
        add(t2);
        ChatLine t3{ChatLine::Kind::Tool, ""};
        t3.toolName = "delete_entity";
        t3.toolArgs = R"({"entity":3})";
        t3.toolResult = "error: tool 'delete_entity' is destructive and not enabled (Settings > MCP Server)";
        t3.toolDone = true;
        t3.toolError = true;
        add(t3);
        add({ChatLine::Kind::Assistant, "Done: three red cubes are in a row at x = 2, 3.5 and 5 with a warm light above them. "
                                        "Each step is in the History panel and can be undone."});
        // Markdown and the thinking view (the demo doubles as the visual check of the chat renderer).
        ChatLine th{ChatLine::Kind::Thinking, "The user wants a summary. List what was created, then the settings in a table."};
        th.open = true;
        add(th);
        add({ChatLine::Kind::Assistant,
             "## Summary\n"
             "The scene now has **three cubes** and *one* light. The material is `Materials/Red.fmat`, see the "
             "[material docs](https://example.com/docs/materials).\n\n"
             "1. Select a cube\n2. Open the **Inspector**\n   - change `baseColor`\n   - ~~old roughness~~ new roughness\n\n"
             "| Setting | Value |\n|---|---|\n| Light colour | `1.0, 0.85, 0.6` |\n| Spacing | **1.5 m** |\n\n"
             "```cpp\nvoid OnStart() {\n    Log::Info(\"three cubes\");\n}\n```\n"
             "> Every step can be undone from the History panel."});
    }
}

void EditorApp::shutdown() {
    if (!m_window) return;
    m_shuttingDown = true;  // no new background work (e.g. queued script rebuilds) from here on
    m_mcpHttp.stop();
    m_linkIpc.stop();  // removes the endpoint file: later links start a new editor
    while (m_aiBusy) {  // let the worker finish; it may be waiting on main-thread tool calls
        m_mainQueue.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (m_aiThread.joinable()) m_aiThread.join();
    if (m_connThread.joinable()) m_connThread.join();
    for (auto& t : m_taskThreads) {  // background cook / package
        while (t.joinable()) {
            bool allDone = true;
            {
                std::lock_guard l(m_taskMutex);
                for (auto& task : m_tasks) allDone &= task->done.load();
            }
            m_mainQueue.drain();
            if (allDone) t.join();
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    m_mainQueue.drain();
    if (m_session.playing()) (void)m_session.stop();
    if (!scriptedRun()) m_session.closeProjectRecovery();  // clean scene: snapshots deleted; unsaved: a final snapshot is kept for the next start
    if (m_code) m_code->saveSettings();
    m_code.reset();  // persists tabs (incl. unsaved edits), stops terminals and clangd
    saveSettings();
    for (auto& [k, v] : m_uiTextures)
        if (v.second) m_device->destroyTexture(v.second);
    m_uiTextures.clear();
    if (m_device && m_device->backend() == rhi::Backend::D3D11) {
#ifdef _WIN32
        ImGui_ImplDX11_Shutdown();
#endif
    } else ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    releaseThumbnails();
    m_session.world().setRenderer(nullptr);
    m_device->destroyRenderTarget(m_viewportRt);
    m_renderer.reset();
    m_device.reset();
    m_window.reset();
}

void EditorApp::loadSettings() {
    auto text = readFileText(m_opt.settingsDir / "editor_settings.json");
    if (!text) {
        m_firstRun = true;
        return;
    }
    Json j = Json::parse(*text, nullptr, false);
    if (j.is_discarded()) return;
    if (j.contains("ai")) m_aiConfig = ai::ProviderConfig::fromJson(j["ai"]);
    if (j.contains("mcp")) {
        const Json& m = j["mcp"];
        m_mcpPortSetting = m.value("port", 8765);
        m_mcpAllowDestructive = m.value("allowDestructive", false);
        m_mcpReadOnly = m.value("readOnly", false);
        m_mcpAutoStart = m.value("autoStart", false);
        if (m.contains("allowedDestructive") && m["allowedDestructive"].is_array())
            for (auto& t : m["allowedDestructive"])
                if (t.is_string()) m_mcp->policy().allowedDestructive.insert(t.get<std::string>());
    }
    if (j.contains("ui") && j["ui"].is_object()) {
        const Json& u = j["ui"];
        m_prefs.theme = u.value("theme", "dark") == "light" ? Theme::Light : Theme::Dark;
        m_prefs.uiScale = u.value("uiScale", 0.0f);
        m_prefs.fontSize = u.value("fontSize", 15.0f);
        m_prefs.cameraSpeed = u.value("cameraSpeed", 4.0f);
        m_prefs.mouseSensitivity = u.value("mouseSensitivity", 1.0f);
        m_prefs.invertY = u.value("invertY", false);
        m_prefs.lockMouseInPlay = u.value("lockMouseInPlay", true);
        m_prefs.showGrid = u.value("showGrid", true);
        m_prefs.showStats = u.value("showStats", true);
        m_prefs.showOrientation = u.value("showOrientation", true);
        m_prefs.workspace = u.value("workspace", "Level");
        m_prefs.showWelcomeOnStartup = u.value("showWelcome", true);
        m_prefs.showSplash = u.value("showSplash", true);
        m_prefs.singleExePackage = u.value("singleExePackage", true);
        m_prefs.packageRunInBackground = u.value("packageRunInBackground", true);
        m_prefs.notifyWhenPackageDone = u.value("notifyWhenPackageDone", true);
        m_prefs.openFolderWhenDone = u.value("openFolderWhenDone", true);
        m_prefs.shutdownAfterPackage = u.value("shutdownAfterPackage", false);
        m_prefs.restoreLastProject = u.value("restoreLastProject", false);
        m_prefs.simpleMode = u.value("simpleMode", false);
        m_prefs.tourDone = u.value("tourDone", false);
        m_prefs.blueprintNoticeAccepted = u.value("blueprintNoticeAccepted", false);
        m_prefs.migrationOffer = u.contains("migrationOffer") && u["migrationOffer"].is_string() ? u["migrationOffer"].get<std::string>() : "";
        m_prefs.proWorkspace = u.value("proWorkspace", "Level");
        if (m_prefs.workspace == "Simple" && !m_prefs.simpleMode) m_prefs.workspace = m_prefs.proWorkspace;
        if (u.contains("projectsDir") && u["projectsDir"].is_string() && !u["projectsDir"].get<std::string>().empty())
            m_prefs.projectsDir = pathFromUtf8(u["projectsDir"].get<std::string>());
    }
    if (j.contains("recentProjects")) m_recent.fromJson(j["recentProjects"]);
    if (j.contains("autosave")) m_session.autosave = editor::AutosaveSettings::fromJson(j["autosave"]);  // per user, never from a project
    if (j.contains("aiControl")) m_session.ai.fromJson(j["aiControl"]);  // per user only: never read from a project
    if (j.contains("storeImport")) m_session.storeImport = StoreImportSettings::fromJson(j["storeImport"]);  // per user, all off by default
    if (j.contains("scripts") && j["scripts"].is_object()) {
        const Json& sj = j["scripts"];
        m_session.scriptSettings.compiler = pathFromUtf8(sj.value("compiler", ""));
        m_session.scriptSettings.autoBuild = sj.value("autoBuild", true);
        if (sj.value("allowAiBuilds", false)) m_session.ai.setCategoryAllowed(AiCategory::ScriptBuild, true);  // pre-0.7 setting
        m_externalEditor = sj.value("externalEditor", "");
    }
    if (j.contains("openPanels") && j["openPanels"].is_array()) {
        for (auto& p : m_panels) p.open = false;
        for (auto& n : j["openPanels"])
            if (n.is_string())
                if (PanelDef* p = panel(n.get<std::string>())) p->open = true;
    }
}

void EditorApp::saveSettings() {
    // Provider/model/URL, UI prefs and recent projects only - API keys live in the credential store.
    Json allowed = Json::array();
    for (auto& t : m_mcp->policy().allowedDestructive) allowed.push_back(t);
    Json j = {{"ai", m_aiConfig.toJson()},
              {"mcp",
               {{"port", m_mcpPortSetting},
                {"allowDestructive", m_mcpAllowDestructive},
                {"readOnly", m_mcpReadOnly},
                {"autoStart", m_mcpAutoStart},
                {"allowedDestructive", allowed}}},
              {"ui",
               {{"theme", m_prefs.theme == Theme::Light ? "light" : "dark"},
                {"uiScale", m_prefs.uiScale},
                {"fontSize", m_prefs.fontSize},
                {"cameraSpeed", m_prefs.cameraSpeed},
                {"mouseSensitivity", m_prefs.mouseSensitivity},
                {"invertY", m_prefs.invertY},
                {"lockMouseInPlay", m_prefs.lockMouseInPlay},
                {"showGrid", m_prefs.showGrid},
                {"showStats", m_prefs.showStats},
                {"showOrientation", m_prefs.showOrientation},
                {"workspace", m_prefs.workspace},
                {"showWelcome", m_prefs.showWelcomeOnStartup},
                {"showSplash", m_prefs.showSplash},
                {"singleExePackage", m_prefs.singleExePackage},
                {"packageRunInBackground", m_prefs.packageRunInBackground},
                {"notifyWhenPackageDone", m_prefs.notifyWhenPackageDone},
                {"openFolderWhenDone", m_prefs.openFolderWhenDone},
                {"shutdownAfterPackage", m_prefs.shutdownAfterPackage},
                {"restoreLastProject", m_prefs.restoreLastProject},
                {"simpleMode", m_prefs.simpleMode},
                {"tourDone", m_prefs.tourDone},
                {"blueprintNoticeAccepted", m_prefs.blueprintNoticeAccepted},
                {"migrationOffer", m_prefs.migrationOffer},
                {"proWorkspace", m_prefs.proWorkspace},
                {"projectsDir", narrow(m_prefs.projectsDir.wstring())}}},
              {"recentProjects", m_recent.toJson()}};
    Json open = Json::array();
    for (auto& p : m_panels)
        if (p.open) open.push_back(p.name);
    j["openPanels"] = open;
    j["aiControl"] = m_session.ai.toJson();
    j["autosave"] = m_session.autosave.toJson();
    j["storeImport"] = m_session.storeImport.toJson();
    j["scripts"] = {{"compiler", narrow(m_session.scriptSettings.compiler.wstring())},
                    {"autoBuild", m_session.scriptSettings.autoBuild},
                    {"externalEditor", m_externalEditor}};
    writeFileText(m_opt.settingsDir / "editor_settings.json", j.dump(2));
}

void EditorApp::startMcpHttp(int port) {
    if (!aiOn()) {  // master switch: the port stays closed
        if (ImGui::GetCurrentContext()) toast(ToastLevel::Warning, "AI features are off - the MCP server cannot start");
        return;
    }
    mcp::HttpServerOptions opt;
    opt.port = port;
    if (const char* tok = std::getenv("FLEDGE_MCP_TOKEN"); tok && *tok) opt.bearerToken = tok;
    // Requests arrive on the HTTP thread; tools run on the main thread.
    bool ok = m_mcpHttp.start(opt, [this](const std::string& body) {
        ++m_mcpRequests;
        m_mcpLastRequest = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!aiOn()) return mcp::rpc::makeError(Json(), -32000, "AI features are turned off in the editor").dump();
        if (std::string denied = mcpPreApprove(body); !denied.empty()) return denied;
        std::string caller = "mcp-http:" + mcp::currentHttpSessionTag();  // audit log: transport + session prefix
        return m_mainQueue.call<std::string>([this, body, caller] {
            m_session.aiCaller = caller;
            std::string r = m_mcp->handleText(body);
            m_session.aiCaller = "assistant";
            return r;
        });
    });
    if (ImGui::GetCurrentContext())
        toast(ok ? ToastLevel::Success : ToastLevel::Error,
              ok ? std::format("MCP HTTP server listening on http://127.0.0.1:{}/mcp", port) : std::format("Cannot bind MCP port {}", port));
}

void EditorApp::togglePlay() {
    if (m_session.playing()) (void)m_session.stop();
    else {
        m_userPlayPending = true;  // user-initiated: the mouse lock may engage
        (void)m_session.play();
    }
}

// Idle throttling: an editor nobody is using should not redraw at 60 Hz. Unfocused -> at most 10 fps, focused but
// untouched for 2 s (and not playing / building) -> at most 20 fps. Any input event wakes it at once
// (glfwWaitEventsTimeout returns on events). Smoke runs count frames, so they never throttle.
void EditorApp::idleThrottle(double now) {
    GLFWwindow* gw = m_window ? m_window->glfwHandle() : nullptr;
    if (!gw || m_opt.smokeFrames > 0) return;
    ImGuiIO& io = ImGui::GetIO();
    bool input = io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || io.InputQueueCharacters.Size > 0;
    for (int b = 0; b < 5 && !input; ++b) input = io.MouseDown[b];
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !input; ++k) input = ImGui::IsKeyDown(static_cast<ImGuiKey>(k));
    if (input) m_lastInputTime = now;
    bool busy = m_session.playing() || m_simulating || m_scriptBuildRunning || m_packageRunning || m_cookRunning || m_capture.mode() != CaptureMode::None;
    double wait = 0;
    if (glfwGetWindowAttrib(gw, GLFW_FOCUSED) != GLFW_TRUE) wait = busy ? 0 : 0.1;
    else if (!busy && now - m_lastInputTime > 2.0) wait = 0.05;
    if (wait > 0) glfwWaitEventsTimeout(wait);
}
void EditorApp::refreshFontTexture() {
    if (m_frame <= 0 || !m_device) return;
#ifdef _WIN32
    if (m_device->backend() == rhi::Backend::D3D11) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
        return;
    }
#endif
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

// ------------------------------------------------------------------- loop --
int EditorApp::run() {
    if (!init()) return 1;
    if (m_window && m_window->glfwHandle()) m_mainQueue.wake = [] { glfwPostEmptyEvent(); };  // thread-safe
    Clock clock;
    while (!m_window->shouldClose() && !m_quit) {
        m_window->pollEvents();
        float dt = clock.tick();
        m_frameMs = m_frameMs * 0.9f + dt * 1000.0f * 0.1f;
        m_mainQueue.drain();
        if (auto changed = m_session.pollAssetChanges(); !changed.empty())
            toast(ToastLevel::Info, std::format("Reloaded {} asset{} from disk", changed.size(), changed.size() == 1 ? "" : "s"));
        updateScripts();
        if (m_code) m_code->update();
        m_session.tick(dt);
        updateAutosave();
        if (m_simulating && !m_session.playing()) m_session.world().stepPhysics(dt);  // Simulate (physics only)
        if (!m_session.playing()) m_session.world().animate(dt);  // Animator previews / bind poses in edit mode
        if (!m_session.playing()) m_session.world().updateAudio(dt);  // previews, listener (play mode: tick does it)
        beginFrameWork();
        const bool gl = m_device->backend() == rhi::Backend::OpenGL;
        if (gl) ImGui_ImplOpenGL3_NewFrame();
#ifdef _WIN32
        else ImGui_ImplDX11_NewFrame();
#endif
        ImGui_ImplGlfw_NewFrame();
        pinCapturedMouse();  // mouse lock / camera drag: relative motion, ImGui pointer pinned in the view
        ImGui::NewFrame();
        frame(dt);
        ImGui::Render();
        const int ww = m_window->width(), hh = m_window->height();
        if (!gl) m_device->resizeSwapchain(ww, hh);
        m_device->beginPass({}, ww, hh, {0.05f, 0.05f, 0.06f, 1});
        if (gl) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#ifdef _WIN32
        else ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#endif
        ++m_frame;
        if (m_opt.smokeFrames > 0 && m_frame >= m_opt.smokeFrames) {
            if (!m_opt.screenshotOut.empty()) {
                ImageData img;
                img.width = m_window->width();
                img.height = m_window->height();
                if (m_device->readPixels({}, img.width, img.height, img.rgba)) {
                    if (m_opt.screenshotScale < 0.99f) img = resizeImage(img, std::max(1, static_cast<int>(img.width * m_opt.screenshotScale)),
                                                                         std::max(1, static_cast<int>(img.height * m_opt.screenshotScale)));
                    writeFileBytes(m_opt.screenshotOut, encodePng(img));
                }
            }
            m_quit = true;
        }
        if (gl) m_window->swapBuffers();
        else m_device->present();
        idleThrottle(clock.now());
    }
    std::printf("EDITOR OK frames=%d project=%s entities=%zu\n", m_frame,
                m_session.hasProject() ? m_session.project().name.c_str() : "(none)", m_session.scene().entities().size());
    std::fflush(stdout);
    shutdown();
    return 0;
}

} // namespace fledge::editor
