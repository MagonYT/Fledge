// Fledge
//   Fledge [project.fledge] [--project <file>] [--portable] [--settings-dir <dir>] [--mcp-http [port]] [--log-file <path>]
//               [--smoke-test --frames N [--screenshot out.png [--screenshot-scale 0.5]]]//               [--theme dark|light] [--workspace Level|Scripting|Code|Animation|AI] [--simple | --pro]
//               [--show welcome|wizard|palette|settings|shortcuts|about|graphics|project-settings] [--palette-query text] [--ui-demo-chat]
//   Fledge --print-paths     (resolved install / user-data paths, then exit)
//   Fledge --migrate-settings-from-standard [--apply] [--overwrite] [--no-ai-keys] [--include-mode] [--no-ai-permissions]
//                            (Fledge Advanced: copy Fledge's settings into this edition; dry run unless --apply; then exit)
//   Fledge --open-url <link> (fledge:// handler: hands the link to the running editor, or starts one with it queued)
//   Fledge --link-handlers status|register-fledge|unregister-fledge|restore <scheme>|restore-all|uninstall
//   Fledge --restore-link-handlers   (= --link-handlers restore-all; the uninstaller runs --link-handlers uninstall)
//
// GUI subsystem on Windows: no console window. Output still reaches a pipe or file
// (ctest, scripts) and the parent console when started from one.
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <format>

#include "fledge/core/features.h"
#include "fledge/core/log_file.h"
#include "fledge/core/paths.h"
#include "fledge/core/version.h"
#include "fledge/edition/edition.h"
#include "fledge/editor/import_ipc.h"
#include "fledge/editor/link_handlers.h"
#include "fledge/editor/project_templates.h"
#include "fledge/editor/settings_migration.h"
#include "fledge/importers/store_import.h"
#include "editor_app.h"

int main(int argc, char** argv) {
    using namespace fledge;
    attachParentConsoleIfNeeded();
    initEdition();  // Fledge or Fledge Advanced (link-time), registers the Advanced modules
    setUserDataFolderName(editionUserDataFolder(currentEdition()));  // %APPDATA%\Fledge or %APPDATA%\Fledge Advanced
    editor::EditorOptions opt;
    bool portable = false, printPaths = false, linkCli = false, migrate = false, migrateApply = false;
    editor::MigrationOptions migrateOpt;
    std::vector<std::string> linkArgs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--link-handlers") {  // the rest of the command line belongs to it
            linkCli = true;
            while (i + 1 < argc) linkArgs.push_back(argv[++i]);
        } else if (a == "--restore-link-handlers") {
            linkCli = true;
            linkArgs = {"restore-all"};
        } else if (a == "--open-url") opt.openUrls.push_back(next());  // what the fledge:// handler runs
        else if (a == "--project") opt.projectFile = pathFromUtf8(next());
        else if (a == "--settings-dir") opt.settingsDir = pathFromUtf8(next());
        else if (a == "--portable") portable = true;
        else if (a == "--print-paths") printPaths = true;
        else if (a == "--migrate-settings-from-standard") migrate = true;
        else if (a == "--apply") migrateApply = true;
        else if (a == "--overwrite") migrateOpt.mode = editor::MigrationMode::Overwrite;
        else if (a == "--no-ai-keys") migrateOpt.includeAiKeys = false;
        else if (a == "--include-mode") migrateOpt.includeEditionMode = true;
        else if (a == "--no-ai-permissions") migrateOpt.includeAiPermissions = false;
        else if (a == "--mcp-http") {
            opt.mcpHttpPort = 8765;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) opt.mcpHttpPort = std::atoi(argv[++i]);
        } else if (a == "--smoke-test") {
            if (opt.smokeFrames <= 0) opt.smokeFrames = 60;
        } else if (a == "--frames") opt.smokeFrames = std::atoi(next().c_str());
        else if (a == "--screenshot") opt.screenshotOut = pathFromUtf8(next());
        else if (a == "--screenshot-scale") opt.screenshotScale = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--theme") opt.theme = next();
        else if (a == "--workspace") opt.workspace = next();
        else if (a == "--simple") opt.mode = "simple";
        else if (a == "--pro") opt.mode = "pro";
        else if (a == "--show") opt.show = next();
        else if (a == "--palette-query") opt.paletteQuery = next();
        else if (a == "--ui-demo-chat") opt.demoChat = true;
        else if (a == "--open-script") opt.openScript = next();
        else if (a == "--select") opt.select = next();
        else if (a == "--no-ai") opt.aiOff = true;
        else if (a == "--log-file") next();  // startPersistentLog reads it from argv
        else if (a == "--mock-ai-transport") opt.mockAiTransport = true;
        else if (a == "--version") {
            std::printf("%s %s\n", editionName(currentEdition()), FLEDGE_VERSION_STRING);
            return 0;
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: Fledge [project.fledge] [--project file] [--portable] [--settings-dir dir] [--mcp-http [port]] [--log-file path]\n"
                        "                   [--smoke-test --frames N [--screenshot out.png [--screenshot-scale s]]]\n"
                        "                   [--theme dark|light] [--workspace Level|Scripting|Code|Animation|AI] [--simple | --pro]\n"
                        "                   [--show welcome|wizard|palette|settings|shortcuts|about|graphics|project-settings] [--palette-query text] [--ui-demo-chat]\n"
                        "       Fledge --print-paths | --version\n"
                        "       Fledge --migrate-settings-from-standard [--apply] [--overwrite] [--no-ai-keys] [--include-mode] [--no-ai-permissions]\n"
                        "       Fledge --open-url <fledge://... link>\n"
                        "       Fledge --link-handlers status|register-fledge|unregister-fledge|restore <scheme>|restore-all|uninstall\n");
            return 0;
        } else if (!a.empty() && a[0] != '-' && opt.projectFile.empty()) {
            opt.projectFile = pathFromUtf8(a);  // "Open with" / .fledge file association
        }
    }
    initEnginePaths(portable);
    if (linkCli) return editor::runLinkHandlerCli(linkArgs, executablePath());
    if (!opt.openUrls.empty()) {
        // Single instance: hand links to the running editor; start the editor only for links nobody took.
        fs::path userData = opt.settingsDir.empty() ? enginePaths().userData : opt.settingsDir;
        std::vector<std::string> left;
        for (const auto& url : opt.openUrls) {
            store::StoreLink l = store::parseLink(url);
            if (l.kind == store::LinkKind::Invalid) {
                std::printf("ignored link: %s\n", l.error.c_str());
                continue;
            }
            std::string reply;
            if (editor::forwardToRunningEditor(userData, url, &reply)) std::printf("link handed to the running editor: %s\n", reply.c_str());
            else left.push_back(url);
        }
        std::fflush(stdout);
        if (left.empty()) return 0;
        opt.openUrls = left;
    }
    if (printPaths) {
        const EnginePaths& p = enginePaths();
        std::printf("%s", p.describe().c_str());
        std::printf("templates.count: %zu\nsamples.count: %zu\nversion: %s\nedition: %s\neditionName: %s\n", listTemplates(p.templates).size(),
                    listSamples(p.samples).size(), FLEDGE_VERSION_STRING, editionId(currentEdition()), editionName(currentEdition()));
        std::fflush(stdout);
        return 0;
    }
    if (migrate) {  // headless settings migration Fledge -> Fledge Advanced (docs/MIGRATING_SETTINGS.md)
        if (currentEdition() != Edition::Advanced) {
            std::printf("MIGRATE-SETTINGS refused: --migrate-settings-from-standard copies Fledge's settings into Fledge Advanced; run it with Fledge Advanced.\n");
            std::fflush(stdout);
            return 2;
        }
        editor::MigrationEndpoints e = editor::migrationEndpointsForThisProcess(opt.settingsDir);
        if (e.source.empty()) {
            std::printf("MIGRATE-SETTINGS refused: %s\n", e.note.c_str());
            std::fflush(stdout);
            return 1;
        }
        editor::MigrationPlan plan = editor::planSettingsMigration(e.source, e.destination, migrateOpt);
        editor::MigrationResult r = plan;
        if (migrateApply && !plan.refused) {
            if (auto st = editor::checkMigrationEndpoints(plan, e); !st) {
                std::printf("MIGRATE-SETTINGS refused: %s\n", st.error.c_str());
                std::fflush(stdout);
                return 1;
            }
            r = editor::runSettingsMigration(plan, migrateOpt);
        }
        std::printf("%s%sMIGRATE-SETTINGS %s %s\n", r.text().c_str(), migrateApply ? "" : "(dry run: add --apply to write)\n",
                    r.ok() ? "OK" : "FAILED", r.summary().c_str());
        std::fflush(stdout);
        return r.ok() ? 0 : 1;
    }
    // Persistent log (1.2.1): <user data>/Logs/fledge-<date>.log (per edition: %APPDATA%\Fledge or %APPDATA%\Fledge Advanced),
    // or --log-file <path>; plus the crash handler (exception, module + RVA, last log lines, minidump next to the log).
    startPersistentLog(std::format("{} {}", editionName(currentEdition()), FLEDGE_VERSION_STRING), enginePaths().userData / "Logs", argc, argv);
    editor::EditorApp app(opt);
    return app.run();
}
