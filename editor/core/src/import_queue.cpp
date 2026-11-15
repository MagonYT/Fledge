// Asset Store Import queue: links, files, user-initiated downloads, imports, undo and the downloads watcher.
#include "fledge/editor/import_queue.h"

#include <algorithm>
#include <cstdlib>
#include <format>

#include "fledge/ai/http.h"
#include "fledge/core/log.h"
#include "fledge/editor/editor_session.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace fledge::editor {

Json StoreImportSettings::toJson() const {
    return {{"watchDownloads", watchDownloads}, {"watchFolders", watchFolders}, {"watchUnityCache", watchUnityCache},
            {"autoImportSafeTypes", autoImportSafeTypes}, {"destination", destination}, {"extraHosts", extraHosts}, {"linkIpc", linkIpc}};
}
StoreImportSettings StoreImportSettings::fromJson(const Json& j) {
    StoreImportSettings s;
    if (!j.is_object()) return s;
    s.watchDownloads = j.value("watchDownloads", false);
    s.watchUnityCache = j.value("watchUnityCache", false);
    s.autoImportSafeTypes = j.value("autoImportSafeTypes", false);
    s.linkIpc = j.value("linkIpc", false);
    std::string d = j.value("destination", "StoreImports");
    s.destination = archive::sanitizeEntryPath(d).value_or("StoreImports");
    for (auto& f : j.value("watchFolders", Json::array()))
        if (f.is_string()) s.watchFolders.push_back(f.get<std::string>());
    for (auto& h : j.value("extraHosts", Json::array()))
        if (h.is_string() && !h.get<std::string>().empty()) s.extraHosts.push_back(toLower(h.get<std::string>()));
    return s;
}

const char* toString(ItemState s) {
    switch (s) {
        case ItemState::NeedsDownload: return "needs-download";
        case ItemState::Ready: return "ready";
        case ItemState::Downloading: return "downloading";
        case ItemState::Importing: return "importing";
        case ItemState::Imported: return "imported";
        case ItemState::Skipped: return "skipped";
        case ItemState::Failed: return "failed";
        case ItemState::Undone: return "undone";
        case ItemState::Forwarded: return "forwarded";
    }
    return "ready";
}

Json ImportItem::toJson(bool full) const {
    Json j = {{"id", id},
              {"title", title},
              {"store", store::toString(store)},
              {"source", sourceLabel},
              {"state", editor::toString(state)},
              {"message", message},
              {"detectedType", store::toString(kind)},
              {"size", size},
              {"licenceNote", licenceNote},
              {"fromAi", fromAi}};
    if (!listingUrl.empty()) j["listingUrl"] = listingUrl;
    if (!downloadUrl.empty()) j["downloadUrl"] = downloadUrl;
    if (!file.empty()) j["file"] = narrow(file.filename().wstring());  // the name only: user folders stay private
    if (inspection) {
        j["contents"] = inspection->counts;
        j["entries"] = inspection->entries;
    }
    if (report) {
        j["summary"] = report->summary();
        j["destination"] = "content:/" + report->destination;
        if (report->primary.guid.isValid()) j["primary"] = {{"guid", report->primary.guid.toString()}, {"path", "content:/" + report->primary.path}};
        if (full) j["report"] = report->toJson();
    }
    if (!reportPath.empty()) j["reportPath"] = "content:/" + reportPath;
    return j;
}

fs::path downloadsFolder() {
#ifdef _WIN32
    static const GUID kDownloads = {0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};  // FOLDERID_Downloads
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(kDownloads, 0, nullptr, &p)) && p) {
        fs::path out(p);
        CoTaskMemFree(p);
        return out;
    }
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home) return fs::path(home) / "Downloads";
#endif
    return {};
}
fs::path unityAssetStoreCache() {
#ifdef _WIN32
    if (const wchar_t* app = _wgetenv(L"APPDATA"); app && *app) return fs::path(app) / "Unity" / "Asset Store-5.x";
#endif
    return {};
}

std::vector<fs::path> defaultAllowedRoots(const StoreImportSettings& s, const fs::path& downloadDir) {
    std::vector<fs::path> roots;
    if (auto d = downloadsFolder(); !d.empty()) roots.push_back(d);  // where browsers save (the extension's file links)
    for (auto& f : s.watchFolders) roots.push_back(pathFromUtf8(f));
    if (auto u = unityAssetStoreCache(); !u.empty()) roots.push_back(u);
    if (!downloadDir.empty()) roots.push_back(downloadDir);
    return roots;
}

namespace {
std::string licenceNoteFor(store::Store s, store::FileKind k) {
    if (k == store::FileKind::UnrealAsset) return kUnrealAssetHelp;
    switch (s) {
        case store::Store::Fab:
            return "Fab: the Fab Standard License allows use in any engine, but listings marked 'Unreal Engine only', Epic-owned "
                   "content (e.g. Megascans) and legacy Marketplace licences may not. Check the listing before you ship.";
        case store::Store::UnityAssetStore:
            return "Unity Asset Store: Unity's EULA does not restrict assets to Unity, but you may not redistribute the files, "
                   "and publishers can add their own terms. Check the listing before you ship.";
        default: return "Unknown source: make sure you have the right to use this file in your game.";
    }
}
} // namespace

ImportQueue::ImportQueue(StoreImportSettings& settings, fs::path downloadDir) : m_settings(settings), m_downloadDir(std::move(downloadDir)) {}

ImportItem& ImportQueue::add(ImportItem item) {
    item.id = m_nextId++;
    m_items.push_back(std::move(item));
    if (m_items.size() > 200) m_items.erase(m_items.begin());  // keep the queue bounded
    if (onAdded) onAdded(m_items.back());
    return m_items.back();
}

ImportItem* ImportQueue::find(int id) {
    for (auto& i : m_items)
        if (i.id == id) return &i;
    return nullptr;
}

int ImportQueue::pendingCount() const {
    return static_cast<int>(std::count_if(m_items.begin(), m_items.end(),
                                          [](auto& i) { return i.state == ItemState::Ready || i.state == ItemState::NeedsDownload; }));
}

bool ImportQueue::fileAllowed(const fs::path& file) const {
    for (const auto& r : defaultAllowedRoots(m_settings, m_downloadDir))
        if (archive::pathWithin(r, file)) return true;
    return false;
}

void ImportQueue::inspect(ImportItem& item, Preview preview) {
    std::error_code ec;
    item.size = fs::file_size(osPath(item.file), ec);
    item.kind = store::detectFileKind(item.file);
    item.licenceNote = licenceNoteFor(item.store, item.kind);
    if (item.kind == store::FileKind::UnrealAsset) {
        item.state = ItemState::Failed;
        item.message = kUnrealAssetHelp;
        return;
    }
    if (item.kind == store::FileKind::Executable || item.kind == store::FileKind::Unknown || item.kind == store::FileKind::Directory) {
        item.state = ItemState::Failed;
        item.message = item.kind == store::FileKind::Executable ? "programs are never imported" : "not a file Fledge can import";
        return;
    }
    // A .unitypackage has to be decompressed to list it: only small ones are listed before the import.
    if (preview == Preview::Inspect && (item.kind != store::FileKind::UnityPackage || item.size <= (64ull << 20))) {
        auto in = store::inspectStoreFile(item.file);
        if (in) {
            item.inspection = *in;
            if (!in->note.empty()) {
                item.state = ItemState::Failed;
                item.message = in->note;
                return;
            }
        } else {
            item.state = ItemState::Failed;
            item.message = "cannot read the file: " + in.error();
            return;
        }
    }
    item.state = ItemState::Ready;
    item.message = std::format("{} ({:.1f} MB) ready to import", store::toString(item.kind), static_cast<double>(item.size) / (1024.0 * 1024.0));
}

int ImportQueue::addFile(const fs::path& file, const std::string& sourceLabel, const store::Provenance& hint, bool fromAi, Preview preview) {
    ImportItem it;
    it.file = file;
    it.sourceLabel = sourceLabel;
    it.fromAi = fromAi;
    it.provenance = hint;
    it.store = hint.store;
    if (it.store == store::Store::Unknown) {
        std::string ext = toLower(narrow(file.extension().wstring()));
        if (archive::pathWithin(unityAssetStoreCache(), file) || ext == ".unitypackage") it.store = store::Store::UnityAssetStore;
        else it.store = store::Store::Local;
    }
    it.provenance.store = it.store;
    it.listingUrl = hint.listingUrl;
    it.title = !hint.title.empty() ? hint.title : narrow(file.stem().wstring());
    std::error_code ec;
    if (!fs::is_regular_file(osPath(file), ec)) {
        it.state = ItemState::Failed;
        it.message = "file not found: " + narrow(file.filename().wstring());
    } else {
        inspect(it, preview);
    }
    return add(std::move(it)).id;
}

int ImportQueue::addLink(const std::string& link, const std::string& sourceLabel, bool fromAi) {
    store::StoreLink l = store::parseLink(link);
    if (l.kind == store::LinkKind::ImportFile) {
        store::Provenance p;
        p.store = l.store;
        p.listingUrl = l.url;
        p.title = l.title;
        p.licence = l.licence;
        int id = addFile(pathFromUtf8(l.filePath), sourceLabel, p, fromAi);
        if (ImportItem* i = find(id)) {
            i->link = link;
            i->linkKind = l.kind;
        }
        return id;
    }
    ImportItem it;
    it.link = link;
    it.linkKind = l.kind;
    it.sourceLabel = sourceLabel;
    it.fromAi = fromAi;
    it.store = l.store;
    it.productId = l.productId;
    it.title = !l.title.empty() ? l.title : !l.fileName.empty() ? l.fileName : !l.productId.empty() ? std::string(store::storeTitle(l.store)) + " " + l.productId : "Store link";
    it.provenance.store = l.store;
    it.provenance.title = l.title;
    it.provenance.licence = l.licence;
    it.provenance.productId = l.productId;
    it.licenceNote = licenceNoteFor(l.store, store::FileKind::Unknown);
    it.state = ItemState::NeedsDownload;
    switch (l.kind) {
        case store::LinkKind::Invalid:
            it.state = ItemState::Failed;
            it.message = "not a link Fledge can import: " + l.error;
            break;
        case store::LinkKind::ImportUrl:
        case store::LinkKind::Listing:
        case store::LinkKind::Download: {
            bool direct = store::fetchRefusal(l.url, m_settings.extraHosts).empty();
            if (direct) {
                it.downloadUrl = l.url;
                auto u = store::splitUrl(l.url);
                it.message = std::format("Press Download to fetch {} from {} (only the URL is requested; no sign-in, no cookies)",
                                         l.fileName.empty() ? "the file" : l.fileName, u ? u->host : "the store");
            } else {
                it.listingUrl = l.url;
                it.provenance.listingUrl = l.url;
                it.message = "Download this asset in your browser, where you are signed in (free or owned assets). Fledge picks the file up "
                             "from Send to Fledge or the downloads watcher. Fledge never signs in, scrapes listing pages or fetches paid content.";
            }
            break;
        }
        case store::LinkKind::UnityOpenInEditor:
            it.message = std::format("Unity's \"Open in Unity\" link only carries the product id ({}). The package itself is downloaded by "
                                     "Unity's Package Manager (My Assets) into %APPDATA%\\Unity\\Asset Store-5.x, which the watcher can watch. "
                                     "\"Open with the original app\" passes the link on to Unity.",
                                     l.productId.empty() ? "unknown" : l.productId);
            break;
        case store::LinkKind::EpicLauncher:
            it.message = "Fab \"Open in Launcher\" links only carry the listing. On fab.com choose a download format such as FBX, glTF or OBJ "
                         "(Unreal-format listings must be exported from Unreal). \"Open with the original app\" passes it on to the Epic Games Launcher.";
            break;
        case store::LinkKind::EpicPassThrough:
            it.state = ItemState::Forwarded;
            it.message = "not an asset link: handed to the Epic Games Launcher";
            break;
        default: break;
    }
    return add(std::move(it)).id;
}

Status ImportQueue::download(int id, ai::HttpTransport& http) {
    ImportItem* it = find(id);
    if (!it) return Status::failure("no such item");
    if (it->downloadUrl.empty()) return Status::failure("this item has no direct file link; download it in your browser");
    if (std::string why = store::fetchRefusal(it->downloadUrl, m_settings.extraHosts); !why.empty()) return Status::failure(why);
    it->state = ItemState::Downloading;
    ai::HttpRequest req;
    req.method = "GET";
    req.url = it->downloadUrl;
    req.timeoutMs = 3600000;
    req.headers["User-Agent"] = "Fledge-asset-import";  // nothing else is sent: no cookies, no tokens
    ai::HttpResponse r = http.send(req);
    if (r.status < 200 || r.status >= 300) {
        it->state = ItemState::Failed;
        it->message = r.status == 0 ? "download failed: " + r.error : std::format("download failed: HTTP {}", r.status);
        return Status::failure(it->message);
    }
    if (r.body.size() > (2ull << 30)) {
        it->state = ItemState::Failed;
        it->message = "download larger than 2 GiB: download it in your browser instead";
        return Status::failure(it->message);
    }
    auto u = store::splitUrl(it->downloadUrl);
    std::string leaf = u ? u->path.substr(u->path.rfind('/') + 1) : "download.zip";
    leaf = convert::sanitizeName(store::percentDecode(leaf), "download.zip");
    std::error_code ec;
    fs::create_directories(osPath(m_downloadDir), ec);
    fs::path out = m_downloadDir / pathFromUtf8(leaf);
    for (int n = 2; fs::exists(osPath(out), ec); ++n) out = m_downloadDir / pathFromUtf8(std::format("{}_{}{}", narrow(pathFromUtf8(leaf).stem().wstring()), n, narrow(pathFromUtf8(leaf).extension().wstring())));
    if (!writeFileBytes(out, std::span(reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size()))) {
        it->state = ItemState::Failed;
        it->message = "cannot save the download";
        return Status::failure(it->message);
    }
    it->file = out;
    it->provenance.listingUrl = it->provenance.listingUrl.empty() ? it->downloadUrl : it->provenance.listingUrl;
    inspect(*it);
    return it->state == ItemState::Failed ? Status::failure(it->message) : Status::success();
}

std::optional<store::ImportOptions> ImportQueue::beginImport(int id, std::string* error) {
    auto fail = [&](std::string e) {
        if (error) *error = std::move(e);
        return std::nullopt;
    };
    ImportItem* it = find(id);
    if (!it) return fail("no such import item");
    if (it->state == ItemState::Imported) return fail("already imported (undo it first to import again)");
    if (it->state == ItemState::Importing) return fail("already importing");
    if (it->file.empty()) return fail(it->message.empty() ? "nothing to import yet" : it->message);
    if (it->kind == store::FileKind::UnrealAsset) return fail(kUnrealAssetHelp);
    store::ImportOptions opt;
    opt.destRoot = m_settings.destination;
    opt.title = it->title;
    opt.provenance = it->provenance;
    opt.provenance.store = it->store;
    if (opt.provenance.listingUrl.empty()) opt.provenance.listingUrl = it->listingUrl;
    if (opt.provenance.title.empty()) opt.provenance.title = it->title;
    if (opt.provenance.productId.empty()) opt.provenance.productId = it->productId;
    opt.provenance.sourceFile = narrow(it->file.filename().wstring());
    it->state = ItemState::Importing;
    it->message = "importing ...";
    return opt;
}

void ImportQueue::finishImport(int id, const Result<store::ImportReport>& r) {
    ImportItem* it = find(id);
    if (!it) return;
    if (!r) {
        it->state = ItemState::Failed;
        it->message = r.error();
        return;
    }
    it->state = ItemState::Imported;
    it->report = *r;
    it->reportPath = r->destination + "/" + convert::sanitizeName(r->title, "Import") + ".fimport";
    it->message = r->summary();
}

Result<store::ImportReport> ImportQueue::import(int id, EditorSession& session) {
    std::string err;
    auto opt = beginImport(id, &err);
    if (!opt) return Result<store::ImportReport>::failure(err);
    auto r = session.importStoreFile(find(id)->file, *opt);
    finishImport(id, r);
    return r;
}

Status ImportQueue::skip(int id) {
    ImportItem* it = find(id);
    if (!it) return Status::failure("no such item");
    if (it->state == ItemState::Imported) return Status::failure("already imported; use Undo");
    it->state = ItemState::Skipped;
    it->message = "skipped";
    return {};
}

Status ImportQueue::undo(int id, EditorSession& session) {
    ImportItem* it = find(id);
    if (!it || !it->report) return Status::failure("nothing to undo");
    if (!session.hasProject()) return Status::failure(kNoProjectHelp);
    Status st = store::undoImport(*it->report, session.vfs(), session.registry());
    if (!st) return st;
    session.refreshAssets({});
    it->state = ItemState::Undone;
    it->message = std::format("removed {} file(s)", it->report->writtenFiles.size());
    it->report.reset();
    return {};
}

void ImportQueue::remove(int id) {
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [&](auto& i) { return i.id == id; }), m_items.end());
}
void ImportQueue::clearFinished() {
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                 [](auto& i) { return i.state == ItemState::Skipped || i.state == ItemState::Undone || i.state == ItemState::Forwarded; }),
                  m_items.end());
}

void ImportQueue::resetWatcher() {
    m_seen.clear();
    m_watchBaseline = false;
}

std::vector<int> ImportQueue::pollWatcher(std::chrono::steady_clock::time_point) {
    std::vector<int> added;
    if (!m_settings.watchDownloads && !m_settings.watchUnityCache) return added;
    std::vector<std::pair<fs::path, bool>> roots;  // folder, recursive
    if (m_settings.watchDownloads) {
        if (m_settings.watchFolders.empty()) roots.push_back({downloadsFolder(), false});
        for (auto& f : m_settings.watchFolders) roots.push_back({pathFromUtf8(f), false});
    }
    if (m_settings.watchUnityCache) roots.push_back({unityAssetStoreCache(), true});
    std::set<std::string> present;
    for (auto& [root, recursive] : roots) {
        std::error_code ec;
        if (root.empty() || !fs::is_directory(osPath(root), ec)) continue;
        auto visit = [&](const fs::directory_entry& e) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2)) return;
            std::string name = narrow(e.path().filename().wstring());
            if (!store::isWatchedExtension(name)) return;
            uint64_t size = e.file_size(ec2);
            std::string key = toLower(narrow(e.path().wstring()));
            present.insert(key);
            auto it = m_seen.find(key);
            if (!m_watchBaseline) {
                m_seen[key] = {size, -1};  // existed before the watcher started: never offered
            } else if (it == m_seen.end()) {
                m_seen[key] = {size, 0};
            } else if (it->second.second >= 0) {
                if (it->second.first == size && size > 0) {
                    it->second.second = -1;  // size stable over two polls: the download has finished
                    fs::path p = e.path();
                    if (p.native().rfind(L"\\\\?\\", 0) == 0) p = fs::path(p.native().substr(4));
                    int id = addFile(p, recursive ? "Unity Asset Store cache" : "Downloads folder");
                    if (ImportItem* item = find(id)) {
                        item->autoImport = m_settings.autoImportSafeTypes && item->state == ItemState::Ready;
                        if (!item->autoImport && item->state == ItemState::Ready) item->message = "New download found. Import it?";
                    }
                    added.push_back(id);
                } else {
                    it->second = {size, 0};
                }
            }
        };
        int n = 0;
        if (recursive) {
            for (auto it = fs::recursive_directory_iterator(osPath(root), fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator() && ++n < 20000; it.increment(ec)) {
                if (it.depth() > 4) it.disable_recursion_pending();
                visit(*it);
            }
        } else {
            for (auto it = fs::directory_iterator(osPath(root), fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::directory_iterator() && ++n < 20000; it.increment(ec))
                visit(*it);
        }
    }
    for (auto it = m_seen.begin(); it != m_seen.end();)  // forget deleted files
        it = present.contains(it->first) ? std::next(it) : m_seen.erase(it);
    m_watchBaseline = true;
    return added;
}

} // namespace fledge::editor
