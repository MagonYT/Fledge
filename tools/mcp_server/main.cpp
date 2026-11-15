// fledge_mcp_server - headless MCP server for a Fledge project.
//
//   fledge_mcp_server --project <file.fledge> [--http [port]] [--allow-destructive]
//                   [--allow <tool>]... [--deny <tool>]... [--read-only]
//                   [--allow-script-builds] [--compiler <g++.exe>]
//
// AI master switch: if the user turned AI off in the editor (editor_settings.json, per user), the server
// refuses to start (exit code 5). --ai-control sets the control level (default "full": a headless server
// cannot show approval prompts; "ask" refuses every change). Risky categories need explicit flags:
// --allow-destructive, --allow-script-builds, --allow-downloads; packaging is allowed unless --deny-packaging.
//
// --allow-script-builds lets MCP clients compile and load Fledge Script files (native code) with
// build_scripts. Off by default: only enable it for clients and projects you trust.
//
// Default transport is stdio (for Claude Desktop / Claude Code / any MCP client
// that spawns servers). --http serves Streamable HTTP on 127.0.0.1 only.
// Rendering tools (take_screenshot) need the GPU editor: Fledge --mcp-http.
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

#include "fledge/core/features.h"
#include "fledge/core/log.h"
#include "fledge/core/log_file.h"
#include "fledge/core/paths.h"
#include "fledge/core/version.h"
#include "fledge/edition/edition.h"
#include "fledge/editor/editor_control.h"
#include "fledge/editor/editor_session.h"
#include "fledge/mcp/mcp_server.h"
#include "fledge/mcp/transports.h"

using namespace fledge;

int main(int argc, char** argv) {
    initEdition();
    setUserDataFolderName(editionUserDataFolder(currentEdition()));
    fs::path projectFile;
    bool http = false;
    int port = 8765;
    mcp::ToolPolicy policy;
    bool allowScriptBuilds = false, allowDownloads = false, allowDestructive = false, denyPackaging = false;
    std::string aiControl = "full";  // headless: nobody can click "Allow"; risky categories still need their flags
    fs::path compiler, settingsDir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--project" && i + 1 < argc) projectFile = pathFromUtf8(argv[++i]);
        else if (a == "--http") {
            http = true;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) port = std::atoi(argv[++i]);
        } else if (a == "--allow-destructive") allowDestructive = true;
        else if (a == "--allow" && i + 1 < argc) policy.allowedDestructive.insert(argv[++i]);
        else if (a == "--deny" && i + 1 < argc) policy.denied.insert(argv[++i]);
        else if (a == "--read-only") policy.readOnlyMode = true;
        else if (a == "--allow-script-builds") allowScriptBuilds = true;
        else if (a == "--allow-downloads") allowDownloads = true;
        else if (a == "--deny-packaging") denyPackaging = true;
        else if (a == "--ai-control" && i + 1 < argc) aiControl = argv[++i];
        else if (a == "--settings-dir" && i + 1 < argc) settingsDir = pathFromUtf8(argv[++i]);
        else if (a == "--compiler" && i + 1 < argc) compiler = pathFromUtf8(argv[++i]);
        else if (a == "--log-file" && i + 1 < argc) ++i;  // startPersistentLog reads it from argv
        else if (a == "--help" || a == "-h") {
            std::fprintf(stderr, "usage: fledge_mcp_server --project <file.fledge> [--http [port]] [--allow-destructive] "
                                 "[--allow tool] [--deny tool] [--read-only] [--allow-script-builds] [--allow-downloads] [--deny-packaging]\n"
                                 "       [--ai-control off|ask|auto|full] [--settings-dir dir] [--compiler g++.exe] [--log-file path]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    // AI master switch: the user's editor setting (per user, never from a project file).
    initEnginePaths(false);
    // Persistent log (1.2.1): <user data>/Logs/fledge-<date>.log (shared with the editor of the same edition) or --log-file.
    // stdout stays the MCP stdio channel: the log goes to stderr + the file.
    startPersistentLog(std::string("fledge_mcp_server ") + FLEDGE_VERSION_STRING, enginePaths().userData / "Logs", argc, argv);
    fs::path settingsFile = (settingsDir.empty() ? enginePaths().userData : settingsDir) / "editor_settings.json";
    AiController userAi;
    editor::StoreImportSettings storeImport;  // import destination etc. (the watcher and link IPC are editor-only)
    editor::AutosaveSettings autosave;        // recovery snapshots only: this server never auto-saves the real scene file
    if (auto text = readFileText(settingsFile)) {
        Json j = Json::parse(*text, nullptr, false);
        if (j.is_object() && j.contains("aiControl")) userAi.fromJson(j["aiControl"]);
        if (j.is_object() && j.contains("autosave")) autosave = editor::AutosaveSettings::fromJson(j["autosave"]);
        if (j.is_object() && j.contains("storeImport")) storeImport = editor::StoreImportSettings::fromJson(j["storeImport"]);
    }
    if (!userAi.enabled()) {
        std::fprintf(stderr,
                     "fledge_mcp_server: AI features are turned OFF for this user (%s: aiControl.enabled = false).\n"
                     "The MCP server does not run while AI is off. Turn AI back on in the Fledge editor "
                     "(Settings > AI > Enable AI features, the status bar, or the command palette).\n",
                     narrow(settingsFile.wstring()).c_str());
        return 5;
    }

    EditorSession session;
    session.storeImport = storeImport;
    session.autosave = autosave;  // recovery snapshots of unsaved AI edits; allowRealSave is never passed here (docs/MCP.md)
    session.storeImport.watchDownloads = session.storeImport.watchUnityCache = session.storeImport.autoImportSafeTypes = false;
    session.ai.setLevel(aiLevelFromString(aiControl));
    // per-tool --allow <tool> opts that destructive tool in; the policy below keeps the others hidden
    session.ai.setCategoryAllowed(AiCategory::Destructive, allowDestructive || !policy.allowedDestructive.empty());
    session.ai.setCategoryAllowed(AiCategory::ScriptBuild, allowScriptBuilds);
    session.ai.setCategoryAllowed(AiCategory::Download, allowDownloads);
    session.ai.setCategoryAllowed(AiCategory::Packaging, !denyPackaging);
    bool cliReadOnly = policy.readOnlyMode;
    session.ai.applyTo(policy);
    policy.readOnlyMode = policy.readOnlyMode || cliReadOnly;
    policy.allowDestructive = allowDestructive;
    session.scriptSettings.compiler = compiler;
    // Audit log of every AI tool call (1.2.4). Full MCP Control (editorControl) has no switch here: it needs the editor window.
    session.auditLog = std::make_shared<editor::McpAuditLog>((settingsDir.empty() ? enginePaths().userData : settingsDir) / "Logs");
    session.aiCaller = http ? "mcp-http" : "mcp-stdio";
    if (!projectFile.empty()) {
        if (auto st = session.openProject(projectFile); !st) {
            FLEDGE_LOG_ERROR("mcp", "{}", st.error);
            // A project that needs features this edition lacks is declined untouched (docs/EDITIONS.md): exit code 6.
            return session.lastDecline() ? 6 : 2;
        }
    } else {
        FLEDGE_LOG_WARN("mcp", "No --project given; project tools will report 'no project is open'");
    }

    mcp::ToolRegistry tools;
    registerEngineTools(tools, session);
    mcp::McpServer server(tools, policy);
    registerEngineResources(server, session);

    // Play mode needs ticking even without a UI.
    std::mutex worldMutex;
    auto handler = [&](const std::string& msg) {
        std::lock_guard lock(worldMutex);
        if (http) session.aiCaller = "mcp-http:" + mcp::currentHttpSessionTag();
        std::string reply = server.handleText(msg);
        session.pollAutosave();  // headless: recovery snapshots only, never the real scene file
        return reply;
    };

    if (http) {
        mcp::HttpServerOptions opt;
        opt.port = port;
        if (const char* tok = std::getenv("FLEDGE_MCP_TOKEN"); tok && *tok) opt.bearerToken = tok;
        mcp::HttpServer srv;
        if (!srv.start(opt, handler)) return 3;
        std::fprintf(stderr, "Fledge MCP server on http://127.0.0.1:%d/mcp (Ctrl+C to quit)\n", port);
        session.startWatching();  // live asset reload while serving
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            std::lock_guard lock(worldMutex);
            session.pollAssetChanges();
            session.tick(1.0f / 60.0f);
            session.pollAutosave();
        }
    }
    FLEDGE_LOG_INFO("mcp", "Fledge MCP server ready on stdio ({} tools)", tools.all().size());
    mcp::runStdio(handler, std::cin, std::cout);
    session.closeProjectRecovery();  // unsaved edits stay recoverable; a clean scene leaves no snapshots behind
    return 0;
}
