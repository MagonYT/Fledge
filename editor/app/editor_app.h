#pragma once
#include "fledge/gameplay/widgets.h"
// Fledge Editor application (Dear ImGui docking UI).
//
// Source layout:
//   editor_app.cpp       setup, settings, main loop, frame orchestration
//   editor_theme.cpp     dark/light themes, fonts (Roboto + Font Awesome + Cousine), DPI scaling
//   editor_commands.cpp  command registry, shortcuts, command palette, Keyboard Shortcuts dialog
//   editor_shell.cpp     menus, toolbar, status bar, toasts, workspaces/layouts, panel registry,
//                        background tasks, Output log, Settings, Welcome screen + New Project wizard
//   editor_ai.cpp        AI Assistant panel
//   editor_viewport.cpp  viewport, camera, picking, gizmo, overlays (stats, orientation cube, view modes)
//   editor_panels.cpp    hierarchy, inspector, history
//   editor_assets.cpp    content browser, thumbnails, material editor, render settings
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <thread>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "fledge/ai/assistant.h"
#include "fledge/ai/credentials.h"
#include "fledge/ai/http.h"
#include "fledge/core/paths.h"
#include "fledge/editor/blueprint_convert.h"
#include "fledge/editor/editor_control.h"
#include "fledge/editor/editor_session.h"
#include "fledge/editor/upscaler_manager.h"
#include "fledge/render/graphics_api.h"
#include "fledge/editor/file_watcher.h"
#include "fledge/editor/import_ipc.h"
#include "fledge/editor/import_queue.h"
#include "fledge/editor/link_handlers.h"
#include "fledge/editor/mouse_capture.h"
#include "fledge/editor/project_templates.h"
#include "fledge/editor/settings_migration.h"
#include "fledge/editor/viewport_math.h"
#include "fledge/mcp/mcp_server.h"
#include "fledge/mcp/transports.h"
#include "fledge/platform/window.h"
#include "fledge/render/renderer.h"

namespace fledge {
struct BotReport;
struct PerfReport;
}
namespace fledge::editor {

struct EditorOptions {
    fs::path projectFile;
    fs::path settingsDir;     // default: enginePaths().userData (%APPDATA%/Fledge, or <root>/UserData when portable)
    int mcpHttpPort = 0;      // >0 = start MCP HTTP server on 127.0.0.1:port
    int smokeFrames = -1;     // >0 = exit after N frames (CI)
    fs::path screenshotOut;   // with smokeFrames: write a window PNG at the end
    float screenshotScale = 1.0f;
    // Startup UI state (docs screenshots, tests)
    std::string theme;        // "dark" | "light"
    std::string workspace;    // "Level" | "Scripting" | "Animation" | "AI"
    std::string show;         // "welcome" | "palette" | "settings" | "shortcuts" | "wizard"
    std::string paletteQuery;
    bool demoChat = false;
    std::string openScript;   // Scripts/-relative file to open in the Script Editor at startup
    std::string select;       // entity name to select at startup
    bool aiOff = false;       // --no-ai: start with AI features switched off (saved per user)
    std::string mode;         // "simple" | "pro": force the UI mode at startup (--simple / --pro)
    bool mockAiTransport = false;  // tests: counting mock HTTP transport instead of WinHTTP    // fill the AI panel with a canned, clearly-labelled demo transcript (no provider call)
    std::vector<std::string> openUrls;  // --open-url links no running editor took (fledge:// handler): queued at startup
};

// Runs closures on the main thread (used by MCP HTTP, assistant worker, background tasks).
class MainThreadQueue {
public:
    void post(std::function<void()> fn) {
        {
            std::lock_guard l(m_mutex);
            m_q.push_back(std::move(fn));
        }
        if (auto w = wake.load()) w();  // an idle-throttled main loop sleeps in glfwWaitEventsTimeout: wake it
    }
    std::atomic<void (*)()> wake{nullptr};  // glfwPostEmptyEvent once the window exists
    void drain() {
        std::deque<std::function<void()>> q;
        {
            std::lock_guard l(m_mutex);
            q.swap(m_q);
        }
        for (auto& f : q) f();
    }
    template <typename R>
    R call(std::function<R()> fn) {  // blocks the calling (non-main) thread
        auto p = std::make_shared<std::promise<R>>();
        auto fut = p->get_future();
        post([p, fn = std::move(fn)] { p->set_value(fn()); });
        return fut.get();
    }

private:
    std::mutex m_mutex;
    std::deque<std::function<void()>> m_q;
};

inline constexpr const char* kAssetPayload = "FLEDGE_ASSET";  // payload: NUL-terminated content-relative path

// ---------------------------------------------------------------- commands --
struct Command {
    std::string id;        // "file.save", "view.theme.light", ...
    std::string title;     // "Save Scene"
    std::string category;  // "File", "Edit", "View", ...
    ImGuiKeyChord shortcut = 0;
    const char* icon = "";
    std::function<void()> run;
    std::function<bool()> enabled;  // null = always
    std::function<bool()> checked;  // null = not a toggle
    bool global = false;            // shortcut also works while a text field is focused
    bool aiFeature = false;         // hidden / refused while AI features are switched off
};

class CommandRegistry {
public:
    void add(Command c);
    const Command* find(const std::string& id) const;
    const std::vector<Command>& all() const { return m_commands; }
    bool isEnabled(const Command& c) const { return !c.enabled || c.enabled(); }
    bool execute(const std::string& id);
    void dispatchShortcuts(bool textInputActive, bool viewportFlying, bool gameOwnsInput);
    bool isHidden(const Command& c) const { return hidden && hidden(c); }
    std::function<bool(const Command&)> hidden;  // e.g. AI commands while AI is off
    static std::string chordName(ImGuiKeyChord chord);

private:
    std::vector<Command> m_commands;
};

// --------------------------------------------------------------- UI state --
enum class Theme { Dark, Light };

struct EditorPrefs {
    Theme theme = Theme::Dark;
    float uiScale = 0.0f;   // 0 = follow the monitor DPI
    float fontSize = 15.0f; // at 100% scale
    float cameraSpeed = 4.0f;
    float mouseSensitivity = 1.0f;
    bool invertY = false;
    bool lockMouseInPlay = true;  // hide + lock the cursor to the game view while playing (Esc releases)
    bool showGrid = true, showStats = true, showOrientation = true;
    std::string workspace = "Level";
    fs::path projectsDir;  // default location for new projects / sample copies
    bool showWelcomeOnStartup = true;
    bool showSplash = true;          // editor boot splash; both editions
    bool singleExePackage = true;    // false = exe plus a content file beside it
    bool packageRunInBackground = true;
    bool notifyWhenPackageDone = true;
    bool openFolderWhenDone = true;
    bool shutdownAfterPackage = false;  // never implied; only if the user checks it
    bool restoreLastProject = false;
    bool simpleMode = false;        // Simple Mode (Describe -> Play -> Share); fresh installs start in it
    bool tourDone = false;          // the 60-second first-run tour was finished or skipped
    bool blueprintNoticeAccepted = false;  // Convert Blueprint: the one-time licence notice was acknowledged
    std::string migrationOffer;     // Fledge Advanced first-launch offer to migrate Fledge's settings: "" | "later" | "never" | "done"
    std::string proWorkspace = "Level";  // workspace to return to from Simple Mode
};

enum class ToastLevel { Info, Success, Warning, Error };
struct Toast {
    ToastLevel level;
    std::string text;
    double time;  // ImGui::GetTime() when posted
};

struct BackgroundTask {
    int id = 0;
    std::string name;
    std::atomic<float> progress{0};  // 0..1, < 0 = indeterminate
    std::string detail;              // guarded by EditorApp::m_taskMutex
    std::atomic<bool> done{false};
};

struct PanelDef {
    std::string name;  // window title (also the dock id)
    const char* icon;
    std::string category;  // "Scene", "Assets", "AI", "Scripting", ...
    std::function<void()> draw;
    bool open = true;
    bool placeholder = false;  // STUB panel for an upcoming milestone
    bool aiFeature = false;    // hidden while AI features are switched off
};

struct ChatLine {
    enum class Kind { User, Assistant, Tool, Error, Info, Thinking } kind;
    std::string text;
    // Tool cards
    std::string toolName;
    std::string toolArgs;     // compact JSON (redacted)
    std::string toolResult;   // summary
    bool toolDone = false, toolError = false;
    int historyIndex = -1;    // command index created by this tool call (Undo link), -1 = none
    bool undone = false;
    // "streaming-looking" reveal of assistant text (the providers return whole responses)
    double revealStart = 0;
    bool demo = false;
    bool open = false;        // Thinking: expanded
};

// Status line: assigning a string posts a toast (keeps the panels' `m_status = ...` call sites).
class EditorApp;
class CodeWorkspace;
struct StatusSink {
    EditorApp* app = nullptr;
    StatusSink& operator=(const std::string& text);
};

class EditorApp {
public:
    explicit EditorApp(EditorOptions options);
    ~EditorApp();
    int run();

    void toast(ToastLevel level, std::string text);
    bool aiOn() const { return m_session.ai.enabled(); }
    void setAiEnabled(bool on);
    // AI approval prompt (AI control level). Called from NON-main threads (MCP HTTP, assistant worker):
    // blocks until the user answers on the main thread (or 120 s pass). true = allowed.
    bool requestAiApproval(const mcp::Tool& tool, const Json& args);

private:
    friend struct StatusSink;
    friend class CodeWorkspace;
    // Code workspace (editor_code.cpp)
    std::unique_ptr<CodeWorkspace> m_code;
    ImFont* m_fontCode = nullptr;
    void drawCodePanel();
    // Open world (editor_world.cpp)
    void drawComponentTools(EntityId e, const std::string& component);
    bool updateTerrainBrush(const ViewItem& view, ImVec2 origin, ImVec2 size);
    void worldFrameHooks();
    struct TerrainBrush {
        bool active = false;
        int mode = 0;  // world::BrushMode
        float radius = 8, strength = 0.5f, flattenHeight = 0;
        int layer = 0;
        EntityId terrain = kNullEntity;
        bool stroking = false;
        std::string beforeHeights, beforeSplat;
        Vec3 lastHit{0, 0, 0};
        bool hasHit = false;
    } m_brush;
    bool m_showChunkGrid = false, m_streamAtCamera = true;
    // Simple Mode (editor_simple.cpp)
    void setSimpleMode(bool on);
    void drawSimpleTopBar();
    void drawSimplePanel();
    void drawTour();
    void startTour();
    void simpleNewGame(int templateIndex, const std::string& prompt);
    void drawTimeMachineBar(ImVec2 origin, ImVec2 size);  // editor_innovations.cpp
    void drawWhyPopup();
    EntityId m_whyEntity = kNullEntity;
    void drawPlaytestBot();
    void drawShipWindow();
    void drawKeepDialog();
    void drawPerfWindow();
    bool m_showPerf = false;
    std::shared_ptr<fledge::PerfReport> m_perfReport;
    float m_perfBefore = 0, m_perfAfter = 0;
    int m_perfRetestAt = -1;
    std::vector<char> m_keepChecks;
    void startShip();
    bool m_showShip = false, m_shipRunning = false;
    std::string m_capsuleImport, m_shipResult;
    bool m_showBot = false;
    std::string m_botGoal;
    float m_botSeconds = 30;
    int m_botSeed = 1;
    std::shared_ptr<fledge::BotReport> m_botReport;
    bool m_whyOpen = false;
    std::string m_simplePrompt, m_lastPackageDir;
    int m_tourStep = -1;
    float m_simpleRight = 0;
    double m_tourStepTime = 0;
    bool m_firstRun = false;
    // Asset Store Import (editor_store.cpp): queue panel, settings, consent prompt, link IPC, downloads watcher
    void initStoreImport();
    void updateStoreImport();  // per frame (main thread)
    void drawStoreImportPanel();
    void drawStoreImportSettings();
    void drawStoreConsentDialog();
    void drawSimpleStoreImport();
    void drawStoreItem(ImportItem& item, bool simple);
    void handleIncomingLink(const std::string& link, const std::string& source);  // main thread
    void startStoreImport(int id);
    void startStoreDownload(int id);
    void forwardToOriginalApp(int id);
    void exportThirdPartyList();
    void chooseStoreFile(int forItem = 0);
    // Convert Blueprint (editor_blueprint.cpp): Unreal Blueprint T3D -> Fledge Script (docs/BLUEPRINT_CONVERTER.md)
    void drawBlueprintPanel();
    void drawBlueprintWindow();  // Simple Mode
    void drawBlueprintContents(bool simple);
    void loadBlueprintFile();
    void refreshBlueprintPreview(bool force);
    std::string m_bpText, m_bpClass, m_bpFolder, m_bpSource = "pasted text", m_bpLastConverted, m_bpStatus, m_bpReportMd;
    std::unique_ptr<blueprint::Conversion> m_bpResult;
    bool m_bpUnrealSpace = true, m_showBlueprintSimple = false;
    // Settings migration Fledge -> Fledge Advanced (editor_migration.cpp, docs/MIGRATING_SETTINGS.md)
    void openMigrationWindow(bool fromOffer);
    void refreshMigrationPreview();
    void drawMigrationWindow();
    void drawMigrationBanner();
    void drawMigrationSettingsPage();
    void applyMigratedSettings();  // before ImGui::NewFrame: reload editor_settings.json, code editor settings and the layout
    void initMigrationOffer();
    // Full MCP Control (editor_mcp_control.cpp, fledge/editor/editor_control.h): the host behind the editorControl MCP tools,
    // the Settings > MCP Server switch (per session, warning first) and the status bar indicator with one-click revoke.
    friend class EditorControlHostImpl;
    std::unique_ptr<EditorControlHost> m_controlHost;
    void initEditorControl();
    void drawFullMcpControlSettings();
    void setFullMcpControl(bool on);
    bool m_fullControlConfirm = false;
    MigrationEndpoints m_migEndpoints;
    MigrationOptions m_migOptions;
    std::unique_ptr<MigrationResult> m_migPlan, m_migResult;
    bool m_showMigration = false, m_migrationBanner = false;
    std::atomic<bool> m_migrationReloadPending{false};
    void applyLinkIpc();  // starts / stops the link IPC server to match the settings
    LinkHandlers& linkHandlers();
    SystemLinkHandlers m_linkSys;
    ImportIpcServer m_linkIpc;
    std::string m_consentScheme, m_consentText, m_storeLinkInput, m_storeExportResult;
    bool m_consentOpen = false, m_showSimpleStore = false;
    int m_storeReportOpen = 0;
    double m_lastWatchPoll = -1e9;
    std::vector<std::string> m_droppedFiles;  // OS drag and drop (GLFW callback), handled on the next frame
    std::mutex m_dropMutex;
    // setup
    bool init();
    void shutdown();
    void loadSettings();
    void saveSettings();
    void rebuildProvider();
    void drawAiProviderSettings();   // Settings > AI Providers (presets, custom endpoint form, test connection)
    void startTestConnection();      // worker thread; result lands in m_connResult
    void startMcpHttp(int port);
    void registerCommands();
    // Physics (editor_physics.cpp): simulate-in-viewport (physics only, no scripts) and one-click helpers.
    void registerPhysicsCommands();
    void startSimulation();
    void stopSimulation(bool keep);
    bool simulating() const { return m_simulating; }
    bool m_simulating = false;
    Json m_simSnapshot;
    void registerPanels();
    void applyStartupOptions();

    // theme / fonts (editor_theme.cpp)
    void applyTheme();
    void rebuildFonts();  // before NewFrame
    void refreshFontTexture();  // upload the rebuilt atlas to the active editor renderer
    float effectiveScale() const;
    ImFont* m_fontUi = nullptr;
    ImFont* m_fontMono = nullptr;
    ImFont* m_fontLarge = nullptr;
    bool m_iconsLoaded = false;
    bool m_fontsDirty = true;
    float m_appliedScale = 0;

    // frame
    void frame(float dt);
    void beginFrameWork();  // before ImGui::NewFrame (fonts, layout loads)
    void drawMenuBar();
    void drawToolbar();
    void drawStatusBar();
    void drawToasts();
    void drawPanels(float dt);
    void drawViewport(float dt);
    void drawViewportOverlays(const ViewItem& view, ImVec2 origin, ImVec2 size);
    void drawOrientationCube(const ViewItem& view, ImVec2 corner);
    void drawHierarchy();
    void drawEntityNode(EntityId e, int depth = 0, bool recurse = true);
    void drawHistory();
    void drawGizmo(const ViewItem& view, ImVec2 origin, ImVec2 size);
    void focusSelection();
    void setCameraView(Vec3 forward);
    Ray mouseRay(const ViewItem& view, ImVec2 origin, ImVec2 size) const;
    void drawInspector();
    void drawContentBrowser();
    void drawMaterialEditor();
    void drawUiEditor();
    Result<int> startAssetImport(const fs::path& source, const ModelImportOptions& options, bool instantiate);
    void drawRenderSettings();
    void openMaterialEditor(const std::string& ref);
    void dropAsset(const std::string& relPath, EntityId target, std::optional<Vec3> worldPos);
    ImTextureID thumbnail(const AssetRecord& rec);
    void releaseThumbnails();
    static AssetType assetKindFor(const std::string& propertyName);
    bool assetPicker(AssetType kind, std::string& picked);
    void drawOutput();
    void drawAssistant();
    void drawToolCard(ChatLine& line, int index);
    bool drawPlaceholderPanel(const char* title, const char* milestone, const std::vector<std::string>& planned);
    void drawAnimationPanel();
    void drawAudioPanel();
    void drawMissionsPanel();
    void drawGameplayPanel();
    void drawStreamingPanel();

    // shell (editor_shell.cpp)
    void drawCommandPalette();
    void openPalette(bool commandsOnly, const std::string& query = {});
    void drawShortcutsDialog();
    void drawSettingsWindow();
    void drawWelcome();
    void drawAbout();
    // Editions (editor_edition.cpp): decline dialog and the single "planned for Fledge Advanced" hint
    void showDecline(const EditionCheck& check);
    void drawDecline();
    void drawAdvancedHint();
    // Auto-save and crash recovery (editor_autosave.cpp): per-frame poll, recovery dialog, Settings page, status bar line
    void updateAutosave();
    bool scriptedRun() const { return m_opt.smokeFrames > 0; }
    bool autosaveBusy();
    void drawRecoveryDialog();
    bool drawAutosaveSettings();  // true when a setting changed
    std::string m_autoSavedLabel;     // "Auto-saved 14:32" (status bar), empty until the first auto-save of this session
    std::string m_lastAutosaveError;  // shown once per distinct error
    std::optional<EditionCheck> m_decline;
    bool m_showDecline = false;
    void switchWorkspace(const std::string& name);
    void resetLayout();
    void saveLayout();
    void buildWorkspaceLayout(const std::string& name, unsigned dockId);
    PanelDef* panel(const std::string& name);
    BackgroundTask& startTask(const std::string& name);
    void runInBackground(const std::string& name, std::function<std::string(BackgroundTask&)> work);
    void startCook(bool force);
    void startPackage();
    void beginPackage(bool singleExe);
    void drawPackageDialog();
    void drawEditorSplash();
    bool openProject(const fs::path& file);
    void createFromTemplate();
    ImTextureID loadUiTexture(const fs::path& png, int* w = nullptr, int* h = nullptr);
    void trackRect(const std::string& id);  // onboarding tour: remember the last item's center

    // Fledge Script files (editor_scripts.cpp)
    void startScriptBuild(bool force = false, bool userInitiated = true);
    void updateScripts();  // per frame: watcher -> debounced auto build -> hot reload
    void openScriptFile(const std::string& rel, int line = 0);
    bool saveScriptFile();
    void openInExternalIde(const std::string& rel, int line);
    void drawScriptEditor();
    void drawCodeEditor();
    void drawBuildOutput();
    void drawNewScriptDialog();
    void drawBehavioursInspector(EntityId e);
    void drawAddScriptMenu(EntityId e);
    void feedPlayInput();
    std::unique_ptr<FileWatcher> m_scriptWatcher;
    fs::path m_scriptProject;
    bool m_scriptBuildRunning = false, m_scriptRebuildQueued = false;
    double m_scriptChangeTime = -1, m_scriptBuildStart = 0, m_scriptJumpTime = 0;
    std::string m_scriptFile, m_scriptText, m_scriptSaved;
    int m_scriptJumpLine = 0, m_scriptCursorLine = 1, m_scriptCursorCol = 1;
    bool m_scriptEditorFocused = false, m_vpFocused = false, m_buildShowLog = false;
    bool m_showNewScript = false;
    std::string m_newScriptName = "NewBehaviour", m_newScriptError;
    int m_newScriptKind = 0;
    std::string m_externalEditor;  // e.g. code -g "{file}:{line}" "{scripts}"; empty = system default app
    bool m_cbShowScripts = false;
    bool m_cbUsedOnly = false;
    std::unordered_set<std::string> m_sceneUsed;
    size_t m_sceneUsedStamp = 0;
    uint64_t m_vpIdleKey = 0;
    bool m_vpIdleValid = false;
    bool m_shuttingDown = false;
    struct AiApproval {
        std::string tool, title, category, args;
        std::shared_ptr<std::promise<int>> answer;  // 0 deny, 1 once, 2 session
    };
    std::mutex m_approvalMutex;
    std::deque<AiApproval> m_approvals;
    void drawAiApproval();
    void drawAiControlSettings();
    std::string mcpPreApprove(const std::string& body);  // "" = continue, else a JSON-RPC response (denied)

    // helpers
    ViewItem editorView(float aspect) const;
    void updateEditorCamera(float dt);
    // Mouse capture (play lock / camera drags): editor_viewport.cpp
    void updateMouseCapture();
    void pinCapturedMouse();  // after the ImGui backend's NewFrame: relative delta + ImGui pointer pinned
    ImVec2 viewportMouseDelta() const;
    MouseCapture m_capture;
    CaptureUpdate m_captureState;
    ImVec2 m_captureAnchor{}, m_captureDelta{};
    double m_captureRestoreX = 0, m_captureRestoreY = 0, m_captureLastX = 0, m_captureLastY = 0;
    bool m_suppressGameLeft = false;  // the click that toggled the lock, until released
    bool m_userPlayPending = false;  // set by user Play paths (button, F5, menu, Simple Mode) just before session.play()
    bool m_userStartedPlay = false;  // the running session was started by the user (else MCP / AI / bot: no auto-lock)
    bool m_capturePlayingSeen = false;
    int m_userPlayPendingAge = 0;
    Result<ImageData> captureViewport(int w, int h);
    void sendAssistant(const std::string& text);
    void doPackage() { startPackage(); }
    void togglePlay();

    EditorOptions m_opt;
    EnginePaths m_paths;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<rhi::RenderDevice> m_device;
    std::unique_ptr<ForwardRenderer> m_renderer;
    rhi::RenderTargetHandle m_viewportRt;
    int m_vpW = 0, m_vpH = 0;
    bool m_vpHovered = false;

    EditorSession m_session;
    mcp::ToolRegistry m_tools;
    std::unique_ptr<mcp::McpServer> m_mcp;
    mcp::HttpServer m_mcpHttp;
    MainThreadQueue m_mainQueue;
    std::atomic<uint64_t> m_mcpRequests{0};
    std::atomic<double> m_mcpLastRequest{-1e9};  // seconds (steady clock)

    CommandRegistry m_commands;
    std::vector<PanelDef> m_panels;
    EditorPrefs m_prefs;
    RecentProjects m_recent;

    // editor camera (fly with RMB+WASD, orbit with Alt+LMB around m_camPivot, pan with MMB)
    Vec3 m_camPos{0, 3, 8};
    float m_camYaw = 0, m_camPitch = radians(-15);
    float m_camDist = 8.0f;
    DebugView m_viewMode = DebugView::None;

    // gizmo / picking
    GizmoSettings m_gizmo;
    GizmoDrag m_drag;
    std::vector<std::pair<EntityId, Transform>> m_dragStart;
    int m_hoverAxis = -1;
    ViewItem m_lastView;
    ImVec2 m_vpOrigin{0, 0}, m_vpSize{1, 1};
    bool m_overlayHovered = false;  // mouse over a viewport overlay widget (no picking)

    // hierarchy rename
    EntityId m_renaming = kNullEntity;
    std::string m_renameBuf;
    bool m_renameFocus = false;

    // AI
    ai::ProviderConfig m_aiConfig;
    std::unique_ptr<ai::CredentialStore> m_creds;
    std::unique_ptr<ai::HttpTransport> m_http;
    ai::RetryingHttpTransport* m_httpRetry = nullptr;  // m_http when it retries (not in tests): chat shows each retry
    ai::MockHttpTransport* m_mockHttp = nullptr;
    std::unique_ptr<ai::LlmProvider> m_provider;
    std::unique_ptr<ai::AssistantSession> m_assistant;
    ai::KeySource m_keySource = ai::KeySource::None;
    std::mutex m_chatMutex;
    std::vector<ChatLine> m_chat;
    std::thread m_aiThread;
    std::atomic<bool> m_aiBusy{false};
    std::string m_chatInput;
    // Chat scrolling: follows new output only while the view is at the bottom; scrolled up, a "new messages" pill appears.
    bool m_chatStick = true;
    float m_chatLastScrollY = 0;
    size_t m_chatSeen = 0;
    bool m_ctxScene = true, m_ctxSelection = true, m_ctxProject = false;
    std::string m_keyInput;
    // provider settings form state (never holds a secret)
    std::string m_extraBodyText;       // JSON text of custom.extraBody while editing
    bool m_extraBodyTextInit = false;  // text mirrors m_aiConfig until the user edits it
    std::string m_extraBodyError;
    std::mutex m_connMutex;
    std::thread m_connThread;
    std::atomic<bool> m_connBusy{false};
    ai::ConnectionTestResult m_connResult;
    bool m_connHasResult = false;
    bool m_mcpAllowDestructive = false;
    bool m_mcpReadOnly = false;
    bool m_mcpAutoStart = false;
    int m_mcpPortSetting = 8765;

    // shell state
    bool m_firstLayout = true;
    std::string m_pendingWorkspace;  // applied before the next NewFrame
    bool m_pendingReset = false;
    std::vector<std::string> m_focusQueue;  // windows to bring to front after the next panel pass (active tabs)
    bool m_showPalette = false, m_paletteCommandsOnly = false, m_paletteFocus = false;
    std::string m_paletteQuery;
    int m_paletteSel = 0;
    bool m_paletteVisible = false, m_welcomeVisible = false;  // drawn this frame
    bool m_showShortcuts = false, m_showSettings = false, m_showWelcome = false, m_showAbout = false;
    bool m_showJoin = false;
    std::string m_joinAddress = "127.0.0.1:7777";
    int m_hostPort = 7777;
    void drawJoinDialog();
    bool m_toolchainPrompt = false;     // "Install C++ toolchain" licence prompt
    std::atomic<bool> m_toolchainBusy{false};
    void drawToolchainPrompt();
    std::string m_settingsPage = "Appearance";
    std::string m_shortcutFilter;
    // welcome / wizard
    int m_welcomeTab = 0;  // 0 = start, 1 = new project wizard
    std::vector<ProjectTemplate> m_templates;
    std::vector<SampleProject> m_samples;
    int m_wizardTemplate = 0;
    std::string m_wizardName = "MyGame", m_wizardLocation, m_wizardError;
    std::unordered_map<std::string, std::pair<ImTextureID, rhi::TextureHandle>> m_uiTextures;
    std::string m_openPathInput;
    // output log
    bool m_logLevels[4] = {true, true, true, true};  // debug/trace, info, warn, error
    std::string m_logCategory, m_consoleFilter;
    bool m_consoleAutoScroll = true;
    // toasts + tasks
    std::deque<Toast> m_toasts;
    std::mutex m_taskMutex;
    std::mutex m_storeImportMutex;
    std::vector<std::unique_ptr<BackgroundTask>> m_tasks;
    std::vector<std::thread> m_taskThreads;
    int m_nextTaskId = 1;
    bool m_cookRunning = false, m_packageRunning = false;
    bool m_showPackageDialog = false;
    bool m_editorSplash = false;
    double m_splashStart = 0;

    // graphics & platform (editor_graphics.cpp)
    void drawGraphicsPanel();
    void drawGraphicsContent();
    void drawUpscalerVersions();
    void openLicenseDialog(const std::string& id, bool requestedByAi);
    void drawLicenseDialog();
    void drawProjectSettings();
    void launchPreview(const std::string& api, const std::vector<std::string>& extraArgs);
    void startAutoTune();
    const rhi::ApiProbe& apiProbe(rhi::Backend b);
    rhi::Backend gameBackend(const GraphicsSettings& g);  // first available API of the project's fallback chain
    void applyGraphics(const GraphicsSettings& g, const RenderSettings* r, const std::string& label);
    bool gfxSlider(const char* label, float& buffer, float projectValue, float lo, float hi, const char* fmt, int flags = 0);
    std::map<rhi::Backend, rhi::ApiProbe> m_apiProbes;
    bool m_apiProbed = false;
    struct LicenseState {
        bool open = false, loading = false, hashOk = true, requestedByAi = false;
        std::string id, text, error;
    } m_license;
    std::set<std::string> m_upscalerBusy;
    bool m_autoTuneRunning = false;
    std::string m_autoTuneResult;
    bool m_showProjectSettings = false, m_frontGraphics = false;
    std::string m_projectSettingsPage = "Graphics & Platform";
    std::string m_gfxActive;
    float m_gfxSharpness = 0.3f, m_gfxRenderScale = 1.0f, m_gfxPaperWhite = 200.0f, m_gfxPeak = 1000.0f;

    // content browser / materials
    struct Thumb {
        rhi::RenderTargetHandle rt;
        rhi::TextureHandle tex;
        uint64_t native = 0;
        bool flipY = false;  // render targets are bottom-up
    };
    std::unordered_map<std::string, Thumb> m_thumbs;
    uint64_t m_thumbGeneration = 0;
    int m_thumbBudget = 0;
    std::string m_assetFilter;
    int m_nextAssetImportId = 1;
    std::map<int, Json> m_assetImportJobs;
    std::string m_uiEditorPath = "UI/Menu.fui";
    std::string m_uiEditorLoadedPath;
    std::optional<std::string> m_uiEditorBaseline;
    std::optional<gameplay::UiDocument> m_uiEditorDocument;
    fs::path m_uiEditorProject;
    std::string m_uiEditorStatus;
    int m_uiEditorAspect = 0;
    std::string m_newMaterialName = "NewMaterial";
    std::string m_materialEditRef;
    bool m_focusMaterialEditor = false;
    std::string m_importPath;
    StatusSink m_status{this};
    int m_frame = 0;
    double m_lastInputTime = 0;
    void idleThrottle(double now);  // editor_app.cpp
    float m_frameMs = 0;

    // Onboarding tour target positions.
    std::unordered_map<std::string, ImVec2> m_uiRects;
    bool m_quit = false;
};

} // namespace fledge::editor
