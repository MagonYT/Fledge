#pragma once
// Asset store import (editor/tools only): links from fab.com / the Unity Asset Store, the fledge:// scheme, file type
// detection, provenance and the "Third-party assets" attribution list, and the import of store downloads
// (.unitypackage, Fab .zip, FBX / glTF / OBJ, textures, audio) into a project. docs/ASSET_STORES.md has the full picture.
//
// Ownership: Fledge only imports files the user already has (downloaded in their own browser or launcher). It never
// logs in, never reads browser cookies or launcher tokens, never scrapes listing pages and never fetches paid content.
// A URL fetch is limited to a direct HTTPS file link on an allow-listed store host that the user clicked.
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/assets/asset_registry.h"
#include "fledge/core/json.h"
#include "fledge/importers/archive.h"
#include "fledge/importers/convert.h"
#include "fledge/importers/importers.h"

namespace fledge::store {

// Shown in the UI, the attribution export, the MCP tools and the docs.
inline constexpr const char* kStoreTermsCaveat =
    "Check the licence before you ship. Store assets are licensed by their publishers. Many allow use in any engine, but not "
    "all: Fab content marked 'Unreal Engine only' and Epic-owned content (for example Megascans, Paragon or MetaHumans) may "
    "only be used with Unreal Engine, legacy Unreal Marketplace licences differ from the Fab Standard License, and Unity "
    "Asset Store publishers can add their own terms (Unity's EULA also forbids redistributing the asset files themselves). "
    "Fledge records where each asset came from; it cannot decide whether a licence allows your use.";

enum class Store { Unknown, Fab, UnityAssetStore, Local };
const char* toString(Store s);  // "fab", "unity", "local", "unknown"
Store storeFromString(std::string_view s);
const char* storeTitle(Store s);  // "Fab", "Unity Asset Store", "Local file", "Unknown"

// ---------------------------------------------------------------- links ----
enum class LinkKind {
    Invalid,
    ImportUrl,        // fledge://import?url=...   (a listing page or a direct file link)
    ImportFile,       // fledge://import-file?path=... (a file the browser / user already downloaded)
    Listing,          // https://www.fab.com/listings/<id>, https://assetstore.unity.com/packages/.../<slug>-<id>
    Download,         // https://<allow-listed store host>/...<file>.zip|.unitypackage|.fbx|.glb|.gltf|.obj
    UnityOpenInEditor,  // com.unity3d.kharma:content/<productId>  (the Asset Store's "Open in Unity")
    EpicLauncher,     // com.epicgames.launcher://ue/marketplace/... or .../fab/...  (asset related)
    EpicPassThrough,  // com.epicgames.launcher://apps/... (game launches): always handed back to the launcher
};
const char* toString(LinkKind k);

struct StoreLink {
    LinkKind kind = LinkKind::Invalid;
    Store store = Store::Unknown;
    std::string original;   // the link as received
    std::string url;        // listing / download URL (https), when known
    std::string filePath;   // ImportFile: the local file (UTF-8)
    std::string productId;  // Unity product id, Fab listing id, Epic slug
    std::string title, fileName, licence;  // optional hints from the extension ("Send to Fledge")
    std::string error;      // Invalid: why
};
// Parses any link Fledge accepts. Never fetches anything. Rejects control characters, over-long input (> 4096),
// non-HTTPS web URLs, credentials in URLs, IP-literal hosts and unknown schemes.
StoreLink parseLink(std::string_view text);
// fledge://import?url=<https listing or file>&title=...  (what the browser extension and the docs use)
std::string makeImportLink(const std::string& url, const std::string& title = {}, const std::string& fileName = {});
std::string makeImportFileLink(const std::string& filePath, const std::string& sourceUrl = {});
std::string percentDecode(std::string_view s, bool* ok = nullptr);
std::string percentEncode(std::string_view s);

// Host allow-list for URL fetches. Built-in: fab.com and its subdomains, assetstore.unity.com. `extra` = hosts the user
// added in Settings ("example.com" also allows "*.example.com"). Only https:// on the default port is ever allowed.
bool hostAllowed(std::string_view host, const std::vector<std::string>& extra = {});
struct UrlParts {
    std::string scheme, host, path, query;
    int port = -1;
    bool hasUserInfo = false;
};
std::optional<UrlParts> splitUrl(std::string_view url);
// Why a URL may not be fetched ("" = it may): HTTPS only, allow-listed host, no credentials, no raw IPs, a file name
// with an importable extension.
std::string fetchRefusal(std::string_view url, const std::vector<std::string>& extraHosts = {});

// ------------------------------------------------------------- detection ----
enum class FileKind { Unknown, UnityPackage, Zip, Fbx, Gltf, Obj, Image, Audio, UnrealAsset, Executable, Directory };
const char* toString(FileKind k);
// By content (magic bytes) first, extension second. Unreal .uasset / .umap and executables are recognised so they
// can be refused with a clear message.
FileKind detectFileKind(const fs::path& file);
// The kinds the downloads watcher and auto-import consider ("safe types": nothing executable).
bool isWatchedExtension(std::string_view fileName);

// -------------------------------------------------------------- provenance ----
struct Provenance {
    Store store = Store::Unknown;
    std::string listingUrl, title, author, licence, licenceUrl, licenceText, productId;
    std::string sourceFile;  // file name only (never the user's full path)
    std::string importedAt;  // ISO 8601 date
    Json toJson() const;
    static Provenance fromJson(const Json& j);
};
// One row of the "Third-party assets" list (assets grouped by their store listing).
struct ThirdPartyAsset {
    Provenance provenance;
    std::vector<std::string> files;  // content-relative asset paths
};
// Scans every .meta under content:/ for import.provenance.
std::vector<ThirdPartyAsset> collectThirdPartyAssets(const Vfs& vfs);
std::string attributionMarkdown(const std::vector<ThirdPartyAsset>& assets, const std::string& projectName);
Json attributionJson(const std::vector<ThirdPartyAsset>& assets);

// ------------------------------------------------------------------ import ----
struct ReportItem {
    std::string source;  // path inside the package / archive, or the file name
    std::string kind;    // model, texture, material, script, ...
    std::string status;  // imported | converted | duplicate | updated | skipped | failed
    std::string output;  // content-relative path
    std::string detail;
};
struct ImportReport {
    std::string title, sourceFile, detectedType, destination;  // destination: content-relative folder
    Provenance provenance;
    AssetRecord primary;                      // the main asset (a model when there is one)
    std::vector<AssetRecord> created;         // registered assets
    std::vector<std::string> writtenFiles;    // every content-relative file written (assets, .meta, sources): undo list
    std::vector<ReportItem> items;
    std::vector<std::string> warnings;
    std::vector<std::string> materialNotes;   // what did not map, per material
    int imported = 0, converted = 0, duplicates = 0, skipped = 0, failed = 0;
    Json toJson() const;
    static ImportReport fromJson(const Json& j);
    std::string summary() const;  // one line
};

struct ImportOptions {
    std::string destRoot = "StoreImports";  // content-relative parent folder; the import goes to <destRoot>/<title>/
    fs::path staging;                       // empty temp folder for extraction (required; removed by the caller)
    archive::Limits limits;
    Provenance provenance;
    std::string title;                      // folder name ("" = from the file name)
    convert::SourceSpace objSpace = convert::SourceSpace::Fledge;  // loose OBJ files (no axis metadata)
    float scale = 1.0f;
};
// Imports one store download / file into the content mounted at content:/. Refuses .uasset / .umap (kUnrealAssetHelp)
// and executables. Unity packages: assets keep their Unity GUID as Fledge GUID, so importing the same package again
// updates in place and assets shared between packages are imported once ("duplicate").
Result<ImportReport> importStoreFile(const fs::path& file, Vfs& vfs, AssetRegistry& registry, const ImportOptions& options);

// What a file contains, without importing it (queue cards): kind, counts per entry kind, total size, warnings.
struct Inspection {
    FileKind kind = FileKind::Unknown;
    uint64_t fileSize = 0, contentBytes = 0;
    std::map<std::string, int> counts;  // entry kind -> number
    int entries = 0;
    std::string note;                   // e.g. the Unreal refusal
    std::vector<std::string> warnings;
};
Result<Inspection> inspectStoreFile(const fs::path& file, const archive::Limits& limits = {});

// Removes everything an import wrote (files listed in report.writtenFiles, their registry records and empty folders).
Status undoImport(const ImportReport& report, Vfs& vfs, AssetRegistry& registry);

} // namespace fledge::store
