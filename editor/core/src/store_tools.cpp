// MCP tools for the Asset Store Import queue (docs/MCP.md, docs/ASSET_STORES.md).
//   import_asset_url      queue a store link (downloads permission). NEVER downloads: listings are downloaded by the user
//                         in their browser; direct file links wait for the user's Download click in the editor.
//   import_asset_file     import a local store download (.unitypackage, .zip, FBX / glTF / OBJ ...) with provenance.
//   list_pending_imports  the queue.
//   get_import_report     one import's full report (what was converted, skipped, what did not map).
#include <format>

#include "fledge/editor/editor_session.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge {

using mcp::Tool;
using mcp::ToolResult;

namespace {
Json schema(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
} // namespace

void registerStoreTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"import_asset_url", "Queue an asset store link",
             "Adds a fab.com / Unity Asset Store listing, a direct HTTPS file link on a store host, or a fledge:// link to the "
             "editor's Asset Store Import queue. It NEVER downloads: listing pages are downloaded by the user in their own browser "
             "(Fledge never signs in or scrapes), and direct file links wait for the user to press Download. Returns the queue item. "
             "Check the licence: many store licences do not allow use outside Unity / Unreal.",
             schema({{"url", {{"type", "string"}, {"description", "https://www.fab.com/listings/..., https://assetstore.unity.com/packages/..., or fledge://import?..."}}},
                     {"title", {{"type", "string"}}}},
                    {"url"}),
             false, false, [&s](const Json& a) {
                 std::string url = a["url"].get<std::string>();
                 store::StoreLink l = store::parseLink(url);
                 if (l.kind == store::LinkKind::Invalid) return ToolResult::error("not an importable store link: " + l.error);
                 if (l.kind == store::LinkKind::ImportFile) return ToolResult::error("use import_asset_file for local files");
                 int id = s.imports().addLink(url, "MCP (AI)", true);
                 editor::ImportItem* it = s.imports().find(id);
                 if (it && !a.value("title", "").empty()) it->title = a["title"].get<std::string>();
                 Json out = it ? it->toJson() : Json{{"id", id}};
                 out["note"] = "Queued only. The user downloads or confirms it in the editor (Asset Store Import panel); nothing was fetched.";
                 out["licenceCaveat"] = store::kStoreTermsCaveat;
                 return ToolResult::json(out);
             }});

    reg.add({"import_asset_file", "Import an asset store download",
             "Imports a local file the user already has: a Unity .unitypackage (rebuilds its folders; converts FBX / OBJ / glTF "
             "meshes, textures, audio and Standard / URP / HDRP materials; skips C# scripts, shaders and editor code, never runs "
             "anything), a Fab .zip (FBX / glTF / OBJ + textures), or a single .fbx / .glb / .gltf / .obj / image / audio file. "
             "Unreal .uasset / .umap is refused (export to FBX / glTF from Unreal). Provenance (store, listing, title, author, "
             "licence) is recorded in every asset's .meta. Writes files (not in the undo history; the queue's Undo removes them).",
             schema({{"path", {{"type", "string"}, {"description", "absolute path of the downloaded file"}}},
                     {"title", {{"type", "string"}, {"description", "folder name / asset title (default: file name)"}}},
                     {"store", {{"type", "string"}, {"enum", {"fab", "unity", "local", "unknown"}}}},
                     {"listingUrl", {{"type", "string"}}},
                     {"author", {{"type", "string"}}},
                     {"licence", {{"type", "string"}, {"description", "licence name or URL as shown on the listing"}}},
                     {"instantiate", {{"type", "boolean"}, {"description", "also add the main model to the scene (undoable)"}}},
                     {"background", {{"type", "boolean"}, {"description", "queue in the editor and return immediately; poll get_import_report with the returned id"}}}},
                    {"path"}),
             false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return ToolResult::error(kNoProjectHelp);
                 if (a.value("background", false) && a.value("instantiate", false))
                     return ToolResult::error("background import cannot instantiate automatically; use instantiate_asset after it completes");
                 if (a.value("background", false) && !s.startBackgroundImport)
                     return ToolResult::error("background import requires the graphical editor");
                 store::Provenance p;
                 p.store = store::storeFromString(a.value("store", ""));
                 p.listingUrl = a.value("listingUrl", "");
                 if (!p.listingUrl.empty()) {
                     store::StoreLink l = store::parseLink(p.listingUrl);
                     if (l.kind != store::LinkKind::Listing && l.kind != store::LinkKind::Download) return ToolResult::error("listingUrl: " + (l.error.empty() ? "not a store listing" : l.error));
                     if (p.store == store::Store::Unknown) p.store = l.store;
                     p.productId = l.productId;
                 }
                 p.title = a.value("title", "");
                 p.author = a.value("author", "");
                 p.licence = a.value("licence", "");
                 const bool background = a.value("background", false);
                 const auto preview = background ? editor::ImportQueue::Preview::Skip : editor::ImportQueue::Preview::Inspect;
                 int id = s.imports().addFile(pathFromUtf8(a["path"].get<std::string>()), "MCP (AI)", p, true, preview);
                 editor::ImportItem* it = s.imports().find(id);
                 if (!it) return ToolResult::error("internal: queue item missing");
                 if (it->state == editor::ItemState::Failed) return ToolResult::error(it->message);
                 if (background) {
                     s.startBackgroundImport(id);
                     it = s.imports().find(id);
                     if (!it || it->state != editor::ItemState::Importing) return ToolResult::error("could not start background import");
                     Json out = it->toJson();
                     out["next"] = "Poll get_import_report with this id until the state is imported or failed.";
                     return ToolResult::json(out);
                 }
                 auto r = s.imports().import(id, s);
                 if (!r) return ToolResult::error(r.error());
                 Json out = it->toJson();
                 out["materialNotes"] = r->materialNotes;
                 Json skipped = Json::array();
                 for (auto& i : r->items)
                     if (i.status == "skipped" || i.status == "failed") skipped.push_back({{"source", i.source}, {"status", i.status}, {"why", i.detail}});
                 out["skippedOrFailed"] = skipped;
                 out["licenceCaveat"] = store::kStoreTermsCaveat;
                 if (a.value("instantiate", false) && r->primary.guid.isValid()) {
                     auto e = s.instantiateAsset(r->primary.guid.toString());
                     if (e) out["entity"] = *e;
                     else out["instantiateError"] = e.error();
                 }
                 return ToolResult::json(out);
             }});

    reg.add({"list_pending_imports", "List asset store imports",
             "Lists the Asset Store Import queue: id, title, store, source (browser extension, downloads watcher, link, MCP), "
             "state (needs-download / ready / imported / failed ...), detected type, size, contents and licence note.",
             schema({{"all", {{"type", "boolean"}, {"description", "include finished items (default: pending only)"}}}}), true, false,
             [&s](const Json& a) {
                 bool all = a.value("all", false);
                 Json items = Json::array();
                 for (const auto& it : s.imports().items())
                     if (all || it.state == editor::ItemState::Ready || it.state == editor::ItemState::NeedsDownload || it.state == editor::ItemState::Downloading || it.state == editor::ItemState::Importing)
                         items.push_back(it.toJson());
                 return ToolResult::json({{"items", items}, {"pending", s.imports().pendingCount()}});
             }});

    reg.add({"get_import_report", "Get an import report",
             "Returns the full report of an import: every package entry with its status (imported / converted / duplicate / "
             "skipped / failed) and reason, the material mapping notes (what did not map), warnings and provenance. Pass the "
             "queue id, or reportPath (content:/....fimport) for earlier imports.",
             schema({{"id", {{"type", "integer"}}}, {"reportPath", {{"type", "string"}}}}), true, false, [&s](const Json& a) {
                 if (a.contains("id")) {
                     editor::ImportItem* it = s.imports().find(a["id"].get<int>());
                     if (!it) return ToolResult::error("no import with that id");
                     if (!it->report) return ToolResult::json(it->toJson());
                     return ToolResult::json(it->report->toJson());
                 }
                 std::string p = a.value("reportPath", "");
                 if (p.empty()) return ToolResult::error("pass id or reportPath");
                 if (!startsWith(p, "content:/")) p = "content:/" + p;
                 if (!endsWith(toLower(p), ".fimport")) return ToolResult::error("reportPath must be a .fimport file");
                 auto text = s.vfs().readText(p);
                 if (!text) return ToolResult::error("report not found: " + p);
                 Json j = Json::parse(*text, nullptr, false);
                 if (j.is_discarded()) return ToolResult::error("report is not valid JSON");
                 return ToolResult::json(j);
             }});
}

} // namespace fledge
