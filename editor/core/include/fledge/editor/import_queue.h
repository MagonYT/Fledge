#pragma once
// Asset Store Import queue (UI-free; the editor panel, the MCP tools and the tests share it).
//
// Items arrive from links (fledge://, store links Fledge took over), the downloads watcher, drag and drop, the file
// picker or MCP. Nothing is imported or downloaded without the user's click, except:
//   * import_asset_file over MCP (the AI control level decides, like import_asset), and
//   * watcher items of safe types when the user turned on "auto-import safe types" (off by default).
// URL fetches are only for direct HTTPS file links on allow-listed store hosts and only when the user presses Download;
// listing pages are never fetched or scraped - the card says to download in the browser, where the user is logged in.
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/importers/store_import.h"

namespace fledge::ai {
class HttpTransport;
}

namespace fledge {
class EditorSession;
}

namespace fledge::editor {

// Per-user settings (editor_settings.json "storeImport"). Every switch is off by default.
struct StoreImportSettings {
    bool watchDownloads = false;          // ask before importing new store files in the watch folders
    std::vector<std::string> watchFolders;  // empty = the Downloads folder
    bool watchUnityCache = false;         // also %APPDATA%\Unity\Asset Store-5.x (where Unity puts .unitypackage downloads)
    bool autoImportSafeTypes = false;     // watcher: import .glb/.gltf/.fbx/.obj/.zip/.unitypackage without asking
    std::string destination = "StoreImports";  // content-relative folder
    std::vector<std::string> extraHosts;  // extra HTTPS hosts for direct file downloads
    bool linkIpc = false;                 // listen for links from `Fledge --open-url` (on when a handler is registered)
    Json toJson() const;
    static StoreImportSettings fromJson(const Json& j);
};

enum class ItemState { NeedsDownload, Ready, Downloading, Importing, Imported, Skipped, Failed, Undone, Forwarded };
const char* toString(ItemState s);

struct ImportItem {
    int id = 0;
    std::string title;
    store::Store store = store::Store::Unknown;
    std::string sourceLabel;  // "Send to Fledge (browser)", "Downloads folder", "Unity 'Open in Unity' link", "MCP (AI)" ...
    std::string link;         // original link, if any
    store::LinkKind linkKind = store::LinkKind::Invalid;
    std::string listingUrl, downloadUrl, productId;
    fs::path file;            // local file (once downloaded / found)
    uint64_t size = 0;
    store::FileKind kind = store::FileKind::Unknown;
    std::optional<store::Inspection> inspection;
    std::string licenceNote;
    ItemState state = ItemState::Ready;
    std::string message;
    std::optional<store::ImportReport> report;
    std::string reportPath;   // content-relative .fimport
    bool fromAi = false, autoImport = false;
    store::Provenance provenance;
    Json toJson(bool full = false) const;
};

// The folders a file may come from over IPC / the watcher (Downloads, the Unity cache, watch folders, the queue's own
// download folder). Paths outside them can still be imported by the user (picker, drag and drop) and by import_asset_file.
std::vector<fs::path> defaultAllowedRoots(const StoreImportSettings& settings, const fs::path& downloadDir);
fs::path downloadsFolder();      // the user's Downloads (FOLDERID_Downloads), "" if unknown
fs::path unityAssetStoreCache(); // %APPDATA%\Unity\Asset Store-5.x

class ImportQueue {
public:
    ImportQueue(StoreImportSettings& settings, fs::path downloadDir);

    // A link from IPC, the command line, the browser extension or MCP. Never fetches anything.
    int addLink(const std::string& link, const std::string& sourceLabel, bool fromAi = false);
    // A local file (picker, drag and drop, watcher, MCP). Inspects it (kind, contents, licence note).
    enum class Preview { Inspect, Skip };
    int addFile(const fs::path& file, const std::string& sourceLabel, const store::Provenance& hint = {}, bool fromAi = false,
                Preview preview = Preview::Inspect);
    ImportItem* find(int id);
    const std::vector<ImportItem>& items() const { return m_items; }
    int pendingCount() const;  // items waiting for the user

    // User actions
    Status download(int id, ai::HttpTransport& http);  // direct file links only; fetchRefusal() rules
    Result<store::ImportReport> import(int id, EditorSession& session);
    // The two halves of import() for hosts that run the import on a worker thread (the editor): options for the item
    // (marks it Importing), then the result (on the main thread).
    std::optional<store::ImportOptions> beginImport(int id, std::string* error = nullptr);
    void finishImport(int id, const Result<store::ImportReport>& result);
    Status skip(int id);
    Status undo(int id, EditorSession& session);
    void remove(int id);
    void clearFinished();
    bool fileAllowed(const fs::path& file) const;  // IPC / watcher location rule

    // Downloads watcher: call periodically (the editor: every ~2 s). New, size-stable files with watched extensions
    // become items; partial downloads (.crdownload, .part, .tmp, .download) are ignored. Files present when the
    // watcher started are not offered. Returns ids added.
    std::vector<int> pollWatcher(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void resetWatcher();  // forget the baseline (settings changed)

    std::function<void(const ImportItem&)> onAdded;  // the editor shows a toast / opens the panel
    const fs::path& downloadDir() const { return m_downloadDir; }

private:
    StoreImportSettings& m_settings;
    fs::path m_downloadDir;
    std::vector<ImportItem> m_items;
    int m_nextId = 1;
    bool m_watchBaseline = false;
    std::map<std::string, std::pair<uint64_t, int>> m_seen;  // path -> (size, stable polls); -1 polls = offered / baseline
    ImportItem& add(ImportItem item);
    void inspect(ImportItem& item, Preview preview = Preview::Inspect);
};

} // namespace fledge::editor
