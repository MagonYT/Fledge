// Asset Store Import: the queue panel, Settings page, take-over consent prompt, Simple Mode flow, link IPC, downloads
// watcher, OS drag and drop and the file picker. The logic lives in editor/core (import_queue.h, link_handlers.h,
// import_ipc.h) and engine/assets/importers (store_import.h); this file is UI. docs/ASSET_STORES.md.
#include <algorithm>
#include <format>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#endif

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/core/log.h"

namespace fledge::editor {

namespace {
EditorApp* g_dropTarget = nullptr;
std::mutex* g_dropMutex = nullptr;
std::vector<std::string>* g_dropFiles = nullptr;
void onDrop(GLFWwindow*, int count, const char** paths) {
    if (!g_dropMutex) return;
    std::lock_guard l(*g_dropMutex);
    for (int i = 0; i < count && i < 64; ++i) g_dropFiles->push_back(paths[i]);
    glfwPostEmptyEvent();
}

#ifdef _WIN32
void shellOpen(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void showInFolder(const fs::path& file) {
    std::wstring args = L"/select,\"" + file.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}
// Only store listing pages (https, allow-listed hosts) are ever opened in the browser from here.
void openListing(const std::string& url) {
    store::StoreLink l = store::parseLink(url);
    if (l.kind != store::LinkKind::Listing && l.kind != store::LinkKind::Download) return;
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
void shellOpen(const fs::path&) {}
void showInFolder(const fs::path&) {}
void openListing(const std::string&) {}
#endif

ImVec4 stateColor(ItemState s) {
    switch (s) {
        case ItemState::Imported: return {0.35f, 0.8f, 0.45f, 1};
        case ItemState::Failed: return {1.0f, 0.45f, 0.4f, 1};
        case ItemState::Ready: return {0.4f, 0.7f, 1.0f, 1};
        case ItemState::NeedsDownload: return {1.0f, 0.75f, 0.3f, 1};
        default: return {0.65f, 0.65f, 0.65f, 1};
    }
}
std::string sizeText(uint64_t b) {
    if (b == 0) return "-";
    if (b < 1024 * 1024) return std::format("{:.0f} KB", static_cast<double>(b) / 1024.0);
    return std::format("{:.1f} MB", static_cast<double>(b) / (1024.0 * 1024.0));
}
} // namespace

LinkHandlers& EditorApp::linkHandlers() {
    if (!m_linkSys.handlers) m_linkSys = openSystemLinkHandlers();
    return *m_linkSys.handlers;
}

void EditorApp::initStoreImport() {
    g_dropTarget = this;
    g_dropMutex = &m_dropMutex;
    g_dropFiles = &m_droppedFiles;
    if (m_window) glfwSetDropCallback(static_cast<GLFWwindow*>(m_window->glfwHandle()), &onDrop);
    m_session.imports().onAdded = [this](const ImportItem& it) {
        if (!ImGui::GetCurrentContext()) return;
        if (PanelDef* p = panel("Asset Store Import"); p && !m_prefs.simpleMode) {
            p->open = true;
            m_focusQueue.push_back("Asset Store Import");
        }
        if (m_prefs.simpleMode) m_showSimpleStore = true;
        toast(it.state == ItemState::Failed ? ToastLevel::Warning : ToastLevel::Info, "Asset Store Import: " + it.title + " - " + it.message.substr(0, 120));
    };
    // The fledge:// handler registered by the installer turns the link IPC on (reading the registry only).
    bool tests = m_opt.smokeFrames > 0;
    if (!m_session.storeImport.linkIpc && !tests) {
        fs::path exe = executablePath();
        if (linkHandlers().isOurs(LinkHandlers::kFledge, exe)) m_session.storeImport.linkIpc = true;
    }
    applyLinkIpc();
    for (const auto& url : m_opt.openUrls) handleIncomingLink(url, "fledge:// link");
}

void EditorApp::applyLinkIpc() {
    bool want = m_session.storeImport.linkIpc;
    if (want && !m_linkIpc.running()) {
        auto st = m_linkIpc.start(
            m_opt.settingsDir,
            [this](const std::string& link) {
                m_mainQueue.post([this, link] { handleIncomingLink(link, "fledge:// link"); });
                return std::string("queued in the Fledge editor");
            },
            [this](const fs::path& f) { return m_session.imports().fileAllowed(f); });
        if (!st) toast(ToastLevel::Warning, "Link IPC: " + st.error);
    } else if (!want && m_linkIpc.running()) {
        m_linkIpc.stop();
    }
}

void EditorApp::handleIncomingLink(const std::string& link, const std::string& source) {
    store::StoreLink l = store::parseLink(link);
    if (l.kind == store::LinkKind::EpicPassThrough) {  // a game launch etc.: straight back to the Epic Games Launcher
        auto st = launchWithCommand(linkHandlers().originalCommand(LinkHandlers::kEpic), link, executablePath());
        if (!st) toast(ToastLevel::Warning, "Could not hand the link to the Epic Games Launcher: " + st.error);
        return;
    }
    // Links come from web pages (through the browser's protocol prompt): a file link may only name a file in an expected
    // place, the same rule the IPC server applies (pasted links and the file picker are the user's own choice).
    if (l.kind == store::LinkKind::ImportFile && source != "Pasted link" && !m_session.imports().fileAllowed(pathFromUtf8(l.filePath))) {
        toast(ToastLevel::Warning, "Refused a fledge:// file link outside the Downloads folder, the Unity cache and the watch folders");
        FLEDGE_LOG_WARN("links", "refused import-file link for a file outside the allowed folders");
        return;
    }
    std::string label = source;
    if (l.kind == store::LinkKind::UnityOpenInEditor) label = "Unity \"Open in Unity\" link";
    else if (l.kind == store::LinkKind::EpicLauncher) label = "Epic Games Launcher link";
    else if (l.kind == store::LinkKind::ImportFile || l.kind == store::LinkKind::ImportUrl) label = "Send to Fledge (browser)";
    (void)m_session.imports().addLink(link, label);
    if (m_window) glfwFocusWindow(static_cast<GLFWwindow*>(m_window->glfwHandle()));
}

void EditorApp::updateStoreImport() {
    std::vector<std::string> dropped;
    {
        std::lock_guard l(m_dropMutex);
        dropped.swap(m_droppedFiles);
    }
    for (auto& f : dropped) {
        std::string ext = toLower(narrow(pathFromUtf8(f).extension().wstring()));
        if (ext == ".fledge") {  // a project file dropped on the window opens it, as before
            openProject(pathFromUtf8(f));
            continue;
        }
        m_session.imports().addFile(pathFromUtf8(f), "Drag and drop");
    }
    double now = ImGui::GetTime();
    if (now - m_lastWatchPoll > 2.0) {
        m_lastWatchPoll = now;
        for (int id : m_session.imports().pollWatcher())
            if (ImportItem* it = m_session.imports().find(id); it && it->autoImport && m_session.hasProject()) startStoreImport(id);
    }
}

void EditorApp::startStoreImport(int id) {
    if (!m_session.hasProject()) {
        toast(ToastLevel::Warning, "Open or create a project first: imports go into its Content folder");
        return;
    }
    const auto& items = m_session.imports().items();
    if (std::count_if(items.begin(), items.end(), [](const auto& item) { return item.state == ItemState::Importing; }) >= 4) {
        toast(ToastLevel::Warning, "Four store imports are already queued.");
        return;
    }
    std::string err;
    auto opt = m_session.imports().beginImport(id, &err);
    if (!opt) {
        toast(ToastLevel::Error, err);
        return;
    }
    ImportItem* it = m_session.imports().find(id);
    fs::path file = it->file;
    std::string title = it->title;
    if (opt->destRoot.empty()) opt->destRoot = m_session.storeImport.destination;
    Project project = m_session.project();
    opt->staging = project.rootDir() / "Intermediate" / "StoreImport" / pathFromUtf8(Guid::generate().toString());
    // The import runs on a worker with its own VFS + registry over the project's Content/ (like the cook); the main
    // thread then rescans, so the editor's registry picks everything up from the .meta files.
    runInBackground("Importing " + title, [this, id, file, project, o = *opt](BackgroundTask& t) {
        t.progress = -1;
        std::lock_guard lock(m_storeImportMutex);
        Vfs vfs;
        vfs.mount("content", std::make_shared<DirectoryProvider>(project.contentDir()));
        AssetRegistry reg;
        (void)reg.load(vfs);
        reg.deferIndexWrites();  // the main thread publishes the merged registry from the written sidecars
        std::error_code ec;
        auto r = [&]() -> Result<store::ImportReport> {
            try {
                if (!archive::pathWithin(project.rootDir(), o.staging))
                    return Result<store::ImportReport>::failure("The import staging folder must be inside the project.");
                fs::create_directories(osPath(o.staging), ec);
                if (ec) return Result<store::ImportReport>::failure("Could not create the import staging folder: " + ec.message());
                return store::importStoreFile(file, vfs, reg, o);
            } catch (const std::exception& error) {
                return Result<store::ImportReport>::failure(error.what());
            }
        }();
        if (archive::pathWithin(project.rootDir(), o.staging)) fs::remove_all(osPath(o.staging), ec);
        std::string msg = r ? r->summary() : r.error();
        m_mainQueue.post([this, id, r, projectFile = project.file] {
            m_session.imports().finishImport(id, r);
            if (m_session.hasProject() && m_session.project().file == projectFile) m_session.refreshAssets({});
            if (r) toast(ToastLevel::Success, "Imported " + r->title + ": " + r->summary());
            else toast(ToastLevel::Error, "Import failed: " + r.error().substr(0, 300));
        });
        return msg;
    });
}

void EditorApp::startStoreDownload(int id) {
    ImportItem* it = m_session.imports().find(id);
    if (!it || !m_http) return;
    if (std::string why = store::fetchRefusal(it->downloadUrl, m_session.storeImport.extraHosts); !why.empty()) {
        toast(ToastLevel::Warning, why);
        return;
    }
    it->state = ItemState::Downloading;
    // a private transport per download (the AI transport may be a test mock); same rules: the URL only, no cookies
    runInBackground("Downloading " + it->title, [this, id](BackgroundTask& t) {
        t.progress = -1;
        auto http = ai::createDefaultHttpTransport();
        Status st = m_mainQueue.call<Status>([this, id, &http] { return m_session.imports().download(id, *http); });
        m_mainQueue.post([this, st] { toast(st ? ToastLevel::Success : ToastLevel::Error, st ? "Download finished" : st.error); });
        return st ? std::string("ok") : st.error;
    });
}

void EditorApp::forwardToOriginalApp(int id) {
    ImportItem* it = m_session.imports().find(id);
    if (!it || it->link.empty()) return;
    std::string scheme = it->linkKind == store::LinkKind::UnityOpenInEditor ? LinkHandlers::kUnity : LinkHandlers::kEpic;
    auto st = launchWithCommand(linkHandlers().originalCommand(scheme), it->link, executablePath());
    if (!st) toast(ToastLevel::Warning, st.error);
    else {
        it->state = ItemState::Forwarded;
        it->message = "passed on to the original app";
    }
}

void EditorApp::exportThirdPartyList() {
    if (!m_session.hasProject()) {
        toast(ToastLevel::Warning, "Open a project first");
        return;
    }
    auto assets = store::collectThirdPartyAssets(m_session.vfs());
    fs::path md = m_session.project().rootDir() / "ThirdPartyAssets.md", js = m_session.project().rootDir() / "ThirdPartyAssets.json";
    bool ok = writeFileText(md, store::attributionMarkdown(assets, m_session.project().name)) && writeFileText(js, store::attributionJson(assets).dump(2));
    m_storeExportResult = ok ? std::format("Wrote {} and .json ({} listing(s))", narrow(md.wstring()), assets.size()) : "Could not write the list";
    toast(ok ? ToastLevel::Success : ToastLevel::Error, m_storeExportResult);
#ifdef _WIN32
    if (ok) showInFolder(md);
#endif
}

void EditorApp::chooseStoreFile(int forItem) {
#ifdef _WIN32
    wchar_t buf[4096] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"Store downloads (*.unitypackage;*.zip;*.fbx;*.glb;*.gltf;*.obj)\0*.unitypackage;*.zip;*.fbx;*.glb;*.gltf;*.obj\0"
                      L"Images and audio\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.wav;*.mp3;*.ogg;*.flac\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 4096;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (auto d = downloadsFolder(); !d.empty()) ofn.lpstrInitialDir = d.c_str();
    if (!GetOpenFileNameW(&ofn)) return;
    fs::path file(buf);
    store::Provenance hint;
    if (ImportItem* it = forItem ? m_session.imports().find(forItem) : nullptr) {  // "Choose downloaded file" on a link card
        hint = it->provenance;
        hint.listingUrl = it->listingUrl;
        hint.title = it->title;
        int id = m_session.imports().addFile(file, it->sourceLabel, hint);
        (void)id;
        it->state = ItemState::Skipped;
        it->message = "replaced by the downloaded file";
        return;
    }
    m_session.imports().addFile(file, "File picker", hint);
#else
    (void)forItem;
#endif
}

// ------------------------------------------------------------------ panel ----
void EditorApp::drawStoreItem(ImportItem& it, bool simple) {
    float s = effectiveScale();
    ImGui::PushID(it.id);
    ImGui::BeginChild("card", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushFont(m_fontLarge ? m_fontLarge : nullptr);
    ImGui::TextUnformatted(it.title.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(stateColor(it.state), "[%s]", toString(it.state));
    ImGui::TextDisabled("%s  |  %s  |  %s  |  %s", store::storeTitle(it.store), it.sourceLabel.c_str(), store::toString(it.kind), sizeText(it.size).c_str());
    if (it.inspection && !it.inspection->counts.empty()) {
        std::string c;
        for (auto& [k, n] : it.inspection->counts) c += std::format("{}{} {}", c.empty() ? "" : ", ", n, k);
        ImGui::TextDisabled("Contains: %s", c.c_str());
    }
    ImGui::PushTextWrapPos(0);
    if (!it.message.empty()) ImGui::TextUnformatted(it.message.c_str());
    if (!it.licenceNote.empty() && !simple) ImGui::TextColored({1.0f, 0.8f, 0.4f, 1}, "%s %s", ICON_WARNING, it.licenceNote.c_str());
    ImGui::PopTextWrapPos();
    if (it.report && it.report->primary.guid.isValid() && !simple) {
        if (ImTextureID tex = thumbnail(it.report->primary)) {
            ImGui::Image(tex, {64 * s, 64 * s});
            ImGui::SameLine();
        }
        ImGui::TextDisabled("Main asset: content:/%s", it.report->primary.path.c_str());
    }
    bool busy = it.state == ItemState::Importing || it.state == ItemState::Downloading;
    ImGui::BeginDisabled(busy);
    switch (it.state) {
        case ItemState::NeedsDownload:
            if (!it.downloadUrl.empty() && ImGui::Button(ICON_DOWNLOAD " Download")) startStoreDownload(it.id);
            if (!it.listingUrl.empty()) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_EXTERNAL " Open listing")) openListing(it.listingUrl);
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FOLDER_OPEN " Choose downloaded file...")) chooseStoreFile(it.id);
            if (it.linkKind == store::LinkKind::UnityOpenInEditor || it.linkKind == store::LinkKind::EpicLauncher) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_ARROW_RIGHT " Open with the original app")) forwardToOriginalApp(it.id);
            }
            ImGui::SameLine();
            if (ImGui::Button("Skip")) (void)m_session.imports().skip(it.id);
            break;
        case ItemState::Ready:
            if (simple ? ImGui::Button(ICON_DOWNLOAD "  Import into my game", {-1, 36 * s}) : ImGui::Button(ICON_DOWNLOAD " Import")) startStoreImport(it.id);
            if (!simple) {
                ImGui::SameLine();
                if (ImGui::Button("Skip")) (void)m_session.imports().skip(it.id);
                ImGui::SameLine();
                if (ImGui::Button(ICON_FOLDER_OPEN " Show in folder")) showInFolder(it.file);
            }
            break;
        case ItemState::Imported:
            if (it.report && it.report->primary.type == AssetType::Model && ImGui::Button(ICON_PLUS " Add to scene")) {
                auto e = m_session.instantiateAsset(it.report->primary.guid.toString());
                if (e) m_session.select(*e);
                else toast(ToastLevel::Error, e.error());
            }
            if (!simple) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FOLDER_OPEN " Show in folder") && it.report && m_session.hasProject())
                    shellOpen(m_session.project().contentDir() / pathFromUtf8(it.report->destination));
                ImGui::SameLine();
                if (ImGui::Button(ICON_LIST " Report")) m_storeReportOpen = m_storeReportOpen == it.id ? 0 : it.id;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_UNDO " Undo import")) {
                if (auto st = m_session.imports().undo(it.id, m_session); !st) toast(ToastLevel::Error, st.error);
            }
            break;
        case ItemState::Failed:
            if (!it.file.empty() && it.kind != store::FileKind::UnrealAsset && ImGui::Button("Retry")) {
                it.state = ItemState::Ready;
                startStoreImport(it.id);
            }
            break;
        default: break;
    }
    if (it.state != ItemState::Importing && it.state != ItemState::Downloading && it.state != ItemState::Imported) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_XMARK)) {
            m_session.imports().remove(it.id);
            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::PopID();
            return;
        }
    }
    ImGui::EndDisabled();
    if (busy) ImGui::TextDisabled(ICON_SPINNER " working...");
    if (m_storeReportOpen == it.id && it.report) {
        const store::ImportReport& r = *it.report;
        ImGui::SeparatorText("Conversion report");
        ImGui::Text("%s -> content:/%s", r.sourceFile.c_str(), r.destination.c_str());
        ImGui::TextUnformatted(r.summary().c_str());
        if (ImGui::BeginTable("rep", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, {0, 220 * s})) {
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Output");
            ImGui::TableSetupColumn("Detail");
            ImGui::TableHeadersRow();
            for (const auto& i : r.items) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(i.status.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(i.source.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(i.output.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(i.detail.c_str());
            }
            ImGui::EndTable();
        }
        if (!r.materialNotes.empty()) {
            ImGui::TextDisabled("Did not map:");
            for (const auto& n : r.materialNotes) ImGui::BulletText("%s", n.c_str());
        }
        for (const auto& w : r.warnings) ImGui::TextDisabled("%s %s", ICON_WARNING, w.c_str());
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void EditorApp::drawStoreImportPanel() {
    PanelDef* p = panel("Asset Store Import");
    if (!ImGui::Begin("Asset Store Import", p ? &p->open : nullptr)) {
        ImGui::End();
        return;
    }
    float s = effectiveScale();
    if (ImGui::Button(ICON_FOLDER_OPEN " Choose file...")) chooseStoreFile();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320 * s);
    bool add = ImGui::InputTextWithHint("##link", "paste a fab.com / assetstore.unity.com link", &m_storeLinkInput, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(ICON_PLUS " Add link") || add) && !m_storeLinkInput.empty()) {
        handleIncomingLink(m_storeLinkInput, "Pasted link");
        m_storeLinkInput.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FILE " Third-party assets list")) exportThirdPartyList();
    ImGui::SameLine();
    if (ImGui::Button(ICON_GEAR)) {
        m_showSettings = true;
        m_settingsPage = "Asset Store Import";
    }
    const StoreImportSettings& st = m_session.storeImport;
    ImGui::TextDisabled("Watcher: %s   |   links from the browser: %s   |   destination: content:/%s/", st.watchDownloads || st.watchUnityCache ? "on" : "off",
                        m_linkIpc.running() ? "on" : "off", st.destination.c_str());
    if (ImGui::CollapsingHeader(ICON_WARNING " Licences and store terms")) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(store::kStoreTermsCaveat);
        ImGui::TextDisabled("Fledge only imports files you already have: it never signs in, reads browser cookies or launcher tokens, scrapes "
                            "listing pages or downloads paid content.");
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    auto& items = const_cast<std::vector<ImportItem>&>(m_session.imports().items());
    if (items.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Nothing queued. Ways in:");
        ImGui::BulletText("Install the Send to Fledge browser extension (Settings > Asset Store Import) and click its button on a "
                          "fab.com or Unity Asset Store page, or download as usual.");
        ImGui::BulletText("Drop a .unitypackage, .zip, .fbx, .glb / .gltf or .obj onto this window, or use Choose file.");
        ImGui::BulletText("Turn on the downloads watcher: Fledge asks before importing new store downloads.");
        ImGui::BulletText("Unreal .uasset / .umap files cannot be imported: export them to FBX or glTF from Unreal.");
        ImGui::PopTextWrapPos();
    }
    for (size_t i = items.size(); i-- > 0;) {
        if (i >= m_session.imports().items().size()) continue;  // removed while drawing
        drawStoreItem(items[i], false);
    }
    ImGui::End();
}

void EditorApp::drawSimpleStoreImport() {
    if (!m_showSimpleStore) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({520 * s, 460 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_DOWNLOAD "  Add from an asset store###SimpleStore", &m_showSimpleStore, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("1. Find a free or owned asset on fab.com or the Unity Asset Store.");
    ImGui::TextUnformatted("2. Download it in your browser (for Fab, pick FBX, glTF or OBJ), or press Send to Fledge.");
    ImGui::TextUnformatted("3. It shows up below. Press Import into my game.");
    ImGui::TextDisabled("Check the asset's licence: some only allow use in Unity or Unreal.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button(ICON_FOLDER_OPEN "  I already downloaded it...", {-1, 34 * s})) chooseStoreFile();
    ImGui::Separator();
    auto& items = const_cast<std::vector<ImportItem>&>(m_session.imports().items());
    int shown = 0;
    for (size_t i = items.size(); i-- > 0 && shown < 6;) {
        if (i >= m_session.imports().items().size()) continue;
        ImportItem& it = items[i];
        if (it.state == ItemState::Skipped || it.state == ItemState::Undone || it.state == ItemState::Forwarded) continue;
        drawStoreItem(it, true);
        ++shown;
    }
    if (shown == 0) ImGui::TextDisabled("Nothing here yet.");
    ImGui::End();
}

// --------------------------------------------------------------- settings ----
void EditorApp::drawStoreImportSettings() {
    float s = effectiveScale();
    StoreImportSettings& st = m_session.storeImport;
    bool changed = false;
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored({1.0f, 0.8f, 0.4f, 1}, "%s %s", ICON_WARNING, store::kStoreTermsCaveat);
    ImGui::PopTextWrapPos();
    fs::path exe = executablePath();
    LinkHandlers& h = linkHandlers();

    ImGui::SeparatorText("fledge:// links");
    HandlerStatus fs_ = h.status(LinkHandlers::kFledge, exe);
    ImGui::Text("fledge:// %s", fs_.ours ? "opens this Fledge" : fs_.currentCommand.empty() ? "is not registered" : "opens another program");
    if (!fs_.ours) {
        if (ImGui::Button(ICON_LINK " Register fledge:// for my account")) {
            auto r = h.registerFledge(exe);
            toast(r ? ToastLevel::Success : ToastLevel::Error, r ? "fledge:// links now open Fledge" : r.error);
            if (r) st.linkIpc = changed = true;
        }
    } else if (ImGui::Button("Remove the fledge:// handler")) {
        auto r = h.unregisterFledge(exe);
        toast(r ? ToastLevel::Success : ToastLevel::Error, r ? "fledge:// handler removed" : r.error);
    }
    changed |= ImGui::Checkbox("Single instance: links go to this running editor (localhost, per-user token)", &st.linkIpc);

    ImGui::SeparatorText("Store links (optional take-over, off by default)");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("The browser extension is the recommended route. Taking over changes which program opens these links for your "
                        "Windows account; Fledge backs up the current handler first and passes on links it cannot import.");
    ImGui::PopTextWrapPos();
    for (const auto& scheme : LinkHandlers::storeSchemes()) {
        HandlerStatus hs = h.status(scheme, exe);
        bool on = hs.ours;
        if (ImGui::Checkbox(("Open " + hs.title + " in Fledge").c_str(), &on)) {
            if (on) {  // consent first
                m_consentScheme = scheme;
                m_consentText = h.consentText(scheme, exe);
                m_consentOpen = true;
            } else {
                auto r = h.restore(scheme, exe);
                toast(r ? ToastLevel::Success : ToastLevel::Error, r ? "Restored the original handler for " + scheme : r.error);
            }
        }
        ImGui::TextDisabled("    now: %s%s", hs.currentCommand.empty() ? "(no program)" : hs.currentCommand.c_str(), hs.backupExists ? "   (backup stored)" : "");
    }
    if (ImGui::Button(ICON_UNDO " Restore original handlers")) {
        auto r = h.restoreAll(exe);
        toast(r ? ToastLevel::Success : ToastLevel::Error, r ? "Original link handlers restored" : r.error);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(also: the uninstaller, or Fledge --link-handlers restore-all)");

    ImGui::SeparatorText("Browser extension: Send to Fledge (recommended)");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Adds a Send to Fledge button on fab.com and assetstore.unity.com pages and offers the files you download there. It "
                        "sends nothing anywhere: it only opens a fledge:// link with the page URL or the downloaded file's name and path. "
                        "Needs the fledge:// handler above. Chrome / Edge: chrome://extensions > Developer mode > Load unpacked > the "
                        "chromium folder. Firefox: about:debugging > Load Temporary Add-on > firefox/manifest.json.");
    ImGui::PopTextWrapPos();
    fs::path extDir = m_paths.root / "tools" / "browser_extension";
    if (ImGui::Button(ICON_FOLDER_OPEN " Open the extension folder")) shellOpen(extDir);

    ImGui::SeparatorText("Downloads watcher (off by default)");
    changed |= ImGui::Checkbox("Watch my Downloads folder and ask before importing new .unitypackage / .zip / .fbx / .glb / .gltf / .obj files", &st.watchDownloads);
    changed |= ImGui::Checkbox("Also watch Unity's Asset Store cache (%APPDATA%\\Unity\\Asset Store-5.x)", &st.watchUnityCache);
    changed |= ImGui::Checkbox("Auto-import safe types without asking (model and package files only; never programs)", &st.autoImportSafeTypes);
    ImGui::TextDisabled("Watched folders: %s", st.watchFolders.empty() ? narrow(downloadsFolder().wstring()).c_str() : "");
    for (size_t i = 0; i < st.watchFolders.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::BulletText("%s", st.watchFolders[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_XMARK)) {
            st.watchFolders.erase(st.watchFolders.begin() + static_cast<long>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    static std::string newFolder;
    ImGui::SetNextItemWidth(360 * s);
    ImGui::InputTextWithHint("##wf", "add a folder to watch (replaces Downloads)", &newFolder);
    ImGui::SameLine();
    if (ImGui::Button("Add##wf") && !newFolder.empty()) {
        std::error_code ec;
        if (fs::is_directory(pathFromUtf8(newFolder), ec)) {
            st.watchFolders.push_back(newFolder);
            newFolder.clear();
            changed = true;
        } else {
            toast(ToastLevel::Warning, "Not a folder");
        }
    }

    ImGui::SeparatorText("Destination and downloads");
    static std::string dest;
    if (dest.empty()) dest = st.destination;
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::InputText("Import folder (inside Content/)", &dest, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        if (auto p = archive::sanitizeEntryPath(dest)) {
            st.destination = *p;
            changed = true;
        }
        dest = st.destination;
    }
    ImGui::TextDisabled("Direct HTTPS downloads are allowed from fab.com and assetstore.unity.com%s", st.extraHosts.empty() ? "." : " and:");
    for (size_t i = 0; i < st.extraHosts.size(); ++i) {
        ImGui::PushID(1000 + static_cast<int>(i));
        ImGui::BulletText("%s", st.extraHosts[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_XMARK)) {
            st.extraHosts.erase(st.extraHosts.begin() + static_cast<long>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    static std::string newHost;
    ImGui::SetNextItemWidth(260 * s);
    ImGui::InputTextWithHint("##host", "add a host, e.g. cdn.example.com", &newHost);
    ImGui::SameLine();
    if (ImGui::Button("Add##host") && !newHost.empty()) {
        std::string hst = toLower(newHost);
        if (hst.find_first_of("/:@ ") == std::string::npos && hst.find('.') != std::string::npos) {
            st.extraHosts.push_back(hst);
            changed = true;
        } else {
            toast(ToastLevel::Warning, "Enter a host name only (no scheme, port or path)");
        }
        newHost.clear();
    }
    if (changed) {
        m_session.imports().resetWatcher();
        applyLinkIpc();
        saveSettings();
    }
}

void EditorApp::drawStoreConsentDialog() {
    if (m_consentOpen) {
        ImGui::OpenPopup("Take over store links?###consent");
        m_consentOpen = false;
    }
    float s = effectiveScale();
    ImGui::SetNextWindowSize({640 * s, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Take over store links?###consent", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(m_consentText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ImGui::Button("Take over (back up the current handler first)", {0, 0})) {
            auto r = linkHandlers().takeOver(m_consentScheme, executablePath(), true);
            toast(r ? ToastLevel::Success : ToastLevel::Error, r ? m_consentScheme + " links now open Fledge (backup stored)" : r.error);
            if (r && !m_session.storeImport.linkIpc) {
                m_session.storeImport.linkIpc = true;
                applyLinkIpc();
                saveSettings();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120 * s, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace fledge::editor
