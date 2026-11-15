// Asset store import: links, detection, provenance / attribution, and importing store downloads (see store_import.h).
#include "fledge/importers/store_import.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <functional>
#include <set>

#include "fledge/audio/audio.h"
#include "fledge/core/log.h"
#include "fledge/importers/unity_package.h"

namespace fledge::store {

const char* toString(Store s) {
    switch (s) {
        case Store::Fab: return "fab";
        case Store::UnityAssetStore: return "unity";
        case Store::Local: return "local";
        default: return "unknown";
    }
}
Store storeFromString(std::string_view s) {
    std::string l = toLower(s);
    if (l == "fab") return Store::Fab;
    if (l == "unity" || l == "unityassetstore" || l == "assetstore") return Store::UnityAssetStore;
    if (l == "local") return Store::Local;
    return Store::Unknown;
}
const char* storeTitle(Store s) {
    switch (s) {
        case Store::Fab: return "Fab";
        case Store::UnityAssetStore: return "Unity Asset Store";
        case Store::Local: return "Local file";
        default: return "Unknown";
    }
}
const char* toString(LinkKind k) {
    switch (k) {
        case LinkKind::ImportUrl: return "import-url";
        case LinkKind::ImportFile: return "import-file";
        case LinkKind::Listing: return "listing";
        case LinkKind::Download: return "download";
        case LinkKind::UnityOpenInEditor: return "unity-open-in-editor";
        case LinkKind::EpicLauncher: return "epic-launcher";
        case LinkKind::EpicPassThrough: return "epic-pass-through";
        default: return "invalid";
    }
}

// ------------------------------------------------------------------- URLs ----
std::string percentDecode(std::string_view s, bool* ok) {
    std::string out;
    if (ok) *ok = true;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
            int a = i + 2 < s.size() ? hex(s[i + 1]) : -1, b = i + 2 < s.size() ? hex(s[i + 2]) : -1;
            if (a < 0 || b < 0) {
                if (ok) *ok = false;
                return {};
            }
            out += static_cast<char>(a * 16 + b);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}
std::string percentEncode(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += std::format("%{:02X}", c);
    }
    return out;
}

std::optional<UrlParts> splitUrl(std::string_view url) {
    UrlParts u;
    size_t colon = url.find("://");
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    u.scheme = toLower(url.substr(0, colon));
    std::string_view rest = url.substr(colon + 3);
    size_t end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    if (auto at = authority.rfind('@'); at != std::string_view::npos) {
        u.hasUserInfo = true;
        authority = authority.substr(at + 1);
    }
    if (!authority.empty() && authority.front() == '[') {  // IPv6 literal
        u.host = std::string(authority);
    } else if (auto pc = authority.rfind(':'); pc != std::string_view::npos) {
        u.host = toLower(authority.substr(0, pc));
        std::string p(authority.substr(pc + 1));
        if (p.empty() || p.size() > 5 || !std::all_of(p.begin(), p.end(), ::isdigit)) return std::nullopt;
        u.port = std::stoi(p);
    } else {
        u.host = toLower(authority);
    }
    if (u.host.empty()) return std::nullopt;
    size_t frag = rest.find('#');
    if (frag != std::string_view::npos) rest = rest.substr(0, frag);
    size_t q = rest.find('?');
    u.path = std::string(rest.substr(0, q));
    if (q != std::string_view::npos) u.query = std::string(rest.substr(q + 1));
    if (u.path.empty()) u.path = "/";
    return u;
}

namespace {
bool ipLiteral(const std::string& host) {
    if (!host.empty() && host.front() == '[') return true;
    return !host.empty() && std::all_of(host.begin(), host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
}
bool hostIs(const std::string& host, std::string_view domain, bool subdomains) {
    if (host == domain) return true;
    return subdomains && host.size() > domain.size() + 1 && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
           host[host.size() - domain.size() - 1] == '.';
}
std::map<std::string, std::string> queryParams(std::string_view q, bool& ok) {
    std::map<std::string, std::string> out;
    ok = true;
    size_t p = 0;
    while (p <= q.size() && !q.empty()) {
        size_t amp = q.find('&', p);
        if (amp == std::string_view::npos) amp = q.size();
        std::string_view kv = q.substr(p, amp - p);
        size_t eq = kv.find('=');
        bool a = true, b = true;
        std::string k = percentDecode(kv.substr(0, eq), &a);
        std::string v = eq == std::string_view::npos ? "" : percentDecode(kv.substr(eq + 1), &b);
        if (!a || !b) ok = false;
        for (char c : v)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ok = false;  // decoded control characters (incl. NUL)
        if (!k.empty()) out[toLower(k)] = v;
        p = amp + 1;
    }
    return out;
}
std::string lastSegment(std::string_view path) {
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    size_t s = path.rfind('/');
    return std::string(s == std::string_view::npos ? path : path.substr(s + 1));
}
const std::set<std::string> kFetchExtensions = {".zip", ".unitypackage", ".fbx", ".glb", ".gltf", ".obj"};
} // namespace

bool hostAllowed(std::string_view hostIn, const std::vector<std::string>& extra) {
    std::string host = toLower(hostIn);
    if (host.empty() || ipLiteral(host)) return false;
    if (hostIs(host, "fab.com", true) || hostIs(host, "assetstore.unity.com", false)) return true;
    for (const auto& e : extra) {
        std::string d = toLower(e);
        if (!d.empty() && !ipLiteral(d) && hostIs(host, d, true)) return true;
    }
    return false;
}

std::string fetchRefusal(std::string_view url, const std::vector<std::string>& extraHosts) {
    auto u = splitUrl(url);
    if (!u) return "not a valid URL";
    if (u->scheme != "https") return "only https:// downloads are allowed";
    if (u->hasUserInfo) return "URLs with credentials are refused";
    if (u->port != -1 && u->port != 443) return "only the default HTTPS port is allowed";
    if (ipLiteral(u->host)) return "IP-address hosts are refused; only store domains are allowed";
    if (!hostAllowed(u->host, extraHosts))
        return "'" + u->host + "' is not on the store allow-list (fab.com, assetstore.unity.com and hosts added in Settings)";
    std::string leaf = toLower(lastSegment(u->path));
    size_t dot = leaf.rfind('.');
    if (dot == std::string::npos || !kFetchExtensions.contains(leaf.substr(dot)))
        return "this is not a direct file link (.zip, .unitypackage, .fbx, .glb, .gltf, .obj). Download it in your browser; "
               "Fledge picks the file up from the Send to Fledge extension or the downloads watcher";
    return {};
}

StoreLink parseLink(std::string_view text) {
    StoreLink l;
    l.original = std::string(text);
    auto invalid = [&](std::string why) {
        l.kind = LinkKind::Invalid;
        l.error = std::move(why);
        return l;
    };
    if (text.empty()) return invalid("empty link");
    if (text.size() > 4096) return invalid("link longer than 4096 characters");
    for (char c : text)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return invalid("link contains control characters");
    size_t colon = text.find(':');
    if (colon == std::string_view::npos) return invalid("not a link");
    std::string scheme = toLower(text.substr(0, colon));
    std::string_view rest = text.substr(colon + 1);

    if (scheme == "fledge") {
        if (rest.rfind("//", 0) == 0) rest.remove_prefix(2);
        size_t q = rest.find('?');
        std::string action = toLower(rest.substr(0, q));
        while (!action.empty() && action.back() == '/') action.pop_back();
        bool ok = true;
        auto params = queryParams(q == std::string_view::npos ? std::string_view{} : rest.substr(q + 1), ok);
        if (!ok) return invalid("malformed fledge:// parameters");
        l.title = params["title"].substr(0, 256);
        l.fileName = params["file"].substr(0, 256);
        l.licence = params["licence"].empty() ? params["license"].substr(0, 512) : params["licence"].substr(0, 512);
        if (action == "import") {
            if (params["url"].empty()) return invalid("fledge://import needs url=");
            StoreLink inner = parseLink(params["url"]);
            if (inner.kind != LinkKind::Listing && inner.kind != LinkKind::Download)
                return invalid("fledge://import: " + (inner.error.empty() ? std::string("url must be a store listing or a direct file link") : inner.error));
            l.kind = LinkKind::ImportUrl;
            l.store = inner.store;
            l.url = inner.url;
            l.productId = inner.productId;
            if (l.fileName.empty()) l.fileName = inner.fileName;
            return l;
        }
        if (action == "import-file") {
            std::string path = params["path"];
            if (path.empty()) return invalid("fledge://import-file needs path=");
            if (path.size() > 2048) return invalid("path too long");
            bool driveAbs = path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
            if (!driveAbs) return invalid("fledge://import-file needs an absolute local path (C:\\...); network paths are refused");
            if (("/" + path + "/").find("/../") != std::string::npos || ("\\" + path + "\\").find("\\..\\") != std::string::npos)
                return invalid("'..' is not allowed in the path");
            l.kind = LinkKind::ImportFile;
            l.filePath = path;
            l.fileName = narrow(pathFromUtf8(path).filename().wstring());
            if (!params["url"].empty()) {
                StoreLink src = parseLink(params["url"]);
                if (src.kind == LinkKind::Listing || src.kind == LinkKind::Download) {
                    l.url = src.url;
                    l.store = src.store;
                    l.productId = src.productId;
                }
            }
            if (l.store == Store::Unknown) l.store = storeFromString(params["store"]);
            return l;
        }
        return invalid("unknown fledge:// action '" + action + "' (supported: import, import-file)");
    }

    if (scheme == "http") return invalid("only https:// store links are accepted");
    if (scheme == "https") {
        auto u = splitUrl(text);
        if (!u || u->hasUserInfo || ipLiteral(u->host)) return invalid("not an acceptable web address");
        l.url = std::string(text);
        if (hostIs(u->host, "fab.com", true)) {
            l.store = Store::Fab;
            // /listings/<id> (optionally after a locale segment such as /en/)
            size_t at = u->path.find("/listings/");
            if (at != std::string::npos) {
                std::string id = u->path.substr(at + 10);
                id = id.substr(0, id.find('/'));
                if (!id.empty()) {
                    l.kind = LinkKind::Listing;
                    l.productId = id;
                    return l;
                }
            }
        } else if (hostIs(u->host, "assetstore.unity.com", false)) {
            l.store = Store::UnityAssetStore;
            if (u->path.rfind("/packages/", 0) == 0) {
                std::string slug = lastSegment(u->path);
                size_t dash = slug.rfind('-');
                std::string id = dash == std::string::npos ? "" : slug.substr(dash + 1);
                if (!id.empty() && std::all_of(id.begin(), id.end(), ::isdigit)) {
                    l.kind = LinkKind::Listing;
                    l.productId = id;
                    return l;
                }
            }
        }
        if (fetchRefusal(text).empty()) {
            l.kind = LinkKind::Download;
            l.fileName = lastSegment(u->path);
            if (l.store == Store::Unknown) l.store = Store::Unknown;
            return l;
        }
        return invalid(l.store == Store::Unknown ? "not a Fab or Unity Asset Store link" : "not a listing page or a direct file link");
    }

    if (scheme == "com.unity3d.kharma") {  // the Asset Store's "Open in Unity": com.unity3d.kharma:content/<productId>
        l.kind = LinkKind::UnityOpenInEditor;
        l.store = Store::UnityAssetStore;
        std::string r(rest);
        std::string digits;
        for (size_t i = r.size(); i-- > 0 && std::isdigit(static_cast<unsigned char>(r[i]));) digits.insert(digits.begin(), r[i]);
        l.productId = digits;
        return l;
    }
    if (scheme == "com.epicgames.launcher") {
        std::string r = toLower(rest);
        while (r.rfind('/', 0) == 0) r.erase(0, 1);
        bool asset = r.rfind("ue/marketplace", 0) == 0 || r.rfind("ue/fab", 0) == 0 || r.rfind("fab", 0) == 0 || r.rfind("ue/library", 0) == 0;
        l.kind = asset ? LinkKind::EpicLauncher : LinkKind::EpicPassThrough;
        l.store = asset ? Store::Fab : Store::Unknown;
        if (asset) l.productId = lastSegment(std::string(rest).substr(0, std::string(rest).find('?')));
        return l;
    }
    return invalid("unsupported link scheme '" + scheme + "'");
}

std::string makeImportLink(const std::string& url, const std::string& title, const std::string& fileName) {
    std::string s = "fledge://import?url=" + percentEncode(url);
    if (!title.empty()) s += "&title=" + percentEncode(title);
    if (!fileName.empty()) s += "&file=" + percentEncode(fileName);
    return s;
}
std::string makeImportFileLink(const std::string& filePath, const std::string& sourceUrl) {
    std::string s = "fledge://import-file?path=" + percentEncode(filePath);
    if (!sourceUrl.empty()) s += "&url=" + percentEncode(sourceUrl);
    return s;
}

// -------------------------------------------------------------- detection ----
const char* toString(FileKind k) {
    switch (k) {
        case FileKind::UnityPackage: return "Unity package";
        case FileKind::Zip: return "ZIP archive";
        case FileKind::Fbx: return "FBX model";
        case FileKind::Gltf: return "glTF model";
        case FileKind::Obj: return "OBJ mesh";
        case FileKind::Image: return "image";
        case FileKind::Audio: return "audio";
        case FileKind::UnrealAsset: return "Unreal asset";
        case FileKind::Executable: return "program";
        case FileKind::Directory: return "folder";
        default: return "unknown";
    }
}

FileKind detectFileKind(const fs::path& file) {
    std::error_code ec;
    if (fs::is_directory(osPath(file), ec)) return FileKind::Directory;
    archive::FileSource src(file);
    if (!src.ok()) return FileKind::Unknown;
    uint8_t h[64] = {};
    size_t n = src.read(h, sizeof(h));
    std::string ext = toLower(narrow(file.extension().wstring()));
    auto starts = [&](std::string_view m) { return n >= m.size() && std::memcmp(h, m.data(), m.size()) == 0; };
    if (starts("MZ") || starts("\x7f" "ELF")) return FileKind::Executable;
    if (n >= 4 && h[0] == 0xC1 && h[1] == 0x83 && h[2] == 0x2A && h[3] == 0x9E) return FileKind::UnrealAsset;
    if (ext == ".uasset" || ext == ".umap" || ext == ".uexp" || ext == ".ubulk") return FileKind::UnrealAsset;
    if (n >= 2 && h[0] == 0x1f && h[1] == 0x8b) return ext == ".unitypackage" || ext == ".gz" || ext == ".tgz" ? FileKind::UnityPackage : FileKind::Unknown;
    if (starts("PK\x03\x04") || starts(std::string_view("PK\x05\x06", 4))) return FileKind::Zip;
    if (starts("Kaydara FBX Binary")) return FileKind::Fbx;
    if (starts("glTF")) return FileKind::Gltf;
    if (starts("\x89PNG") || starts("\xFF\xD8\xFF") || starts("GIF8") || starts("8BPS") || starts("#?RADIANCE") || starts("#?RGBE")) return FileKind::Image;
    if (starts("BM") && (ext == ".bmp")) return FileKind::Image;
    if ((starts("RIFF") && n >= 12 && std::memcmp(h + 8, "WAVE", 4) == 0) || starts("OggS") || starts("fLaC") || starts("ID3")) return FileKind::Audio;
    if (ext == ".mp3" && n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return FileKind::Audio;
    if (ext == ".fbx") return FileKind::Fbx;  // ASCII FBX
    if (ext == ".gltf") return FileKind::Gltf;
    if (ext == ".obj") return FileKind::Obj;
    if (ext == ".tga") return FileKind::Image;
    return FileKind::Unknown;
}

bool isWatchedExtension(std::string_view fileName) {
    std::string l = toLower(fileName);
    for (const char* e : {".unitypackage", ".zip", ".fbx", ".glb", ".gltf", ".obj"})
        if (endsWith(l, e)) return true;
    return false;
}

// ------------------------------------------------------------- provenance ----
Json Provenance::toJson() const {
    Json j = Json::object();
    j["store"] = toString(store);
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) j[k] = v;
    };
    put("listingUrl", listingUrl);
    put("title", title);
    put("author", author);
    put("licence", licence);
    put("licenceUrl", licenceUrl);
    put("licenceText", licenceText);
    put("productId", productId);
    put("sourceFile", sourceFile);
    put("importedAt", importedAt);
    return j;
}
Provenance Provenance::fromJson(const Json& j) {
    Provenance p;
    if (!j.is_object()) return p;
    p.store = storeFromString(j.value("store", ""));
    p.listingUrl = j.value("listingUrl", "");
    p.title = j.value("title", "");
    p.author = j.value("author", "");
    p.licence = j.value("licence", "");
    p.licenceUrl = j.value("licenceUrl", "");
    p.licenceText = j.value("licenceText", "");
    p.productId = j.value("productId", "");
    p.sourceFile = j.value("sourceFile", "");
    p.importedAt = j.value("importedAt", "");
    return p;
}

std::vector<ThirdPartyAsset> collectThirdPartyAssets(const Vfs& vfs) {
    std::map<std::string, ThirdPartyAsset> groups;
    for (const auto& f : vfs.list("content")) {
        if (!endsWith(f, AssetRegistry::kMetaExtension)) continue;
        Json m = Json::parse(vfs.readText("content:/" + f).value_or(""), nullptr, false);
        if (m.is_discarded() || !m.contains("import") || !m["import"].is_object() || !m["import"].contains("provenance")) continue;
        Provenance p = Provenance::fromJson(m["import"]["provenance"]);
        std::string key = std::string(toString(p.store)) + "|" + p.listingUrl + "|" + p.title + "|" + p.sourceFile;
        auto& g = groups[key];
        if (g.files.empty()) g.provenance = p;
        g.files.push_back(f.substr(0, f.size() - std::char_traits<char>::length(AssetRegistry::kMetaExtension)));
    }
    std::vector<ThirdPartyAsset> out;
    for (auto& [k, g] : groups) {
        std::sort(g.files.begin(), g.files.end());
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return toLower(a.provenance.title) < toLower(b.provenance.title); });
    return out;
}

std::string attributionMarkdown(const std::vector<ThirdPartyAsset>& assets, const std::string& projectName) {
    std::string md = "# Third-party assets" + (projectName.empty() ? std::string() : " - " + projectName) + "\n\n";
    md += std::string("> ") + kStoreTermsCaveat + "\n\n";
    if (assets.empty()) return md + "No store assets have been imported into this project.\n";
    md += "| Asset | Store | Author | Licence | Listing | Files |\n|---|---|---|---|---|---|\n";
    auto cell = [](std::string s) {
        for (auto& c : s)
            if (c == '|' || c == '\n' || c == '\r') c = ' ';
        return s.empty() ? std::string("-") : s;
    };
    for (const auto& a : assets) {
        const Provenance& p = a.provenance;
        std::string licence = p.licence.empty() ? (p.licenceUrl.empty() ? "unknown - check the listing" : p.licenceUrl) : p.licence;
        if (!p.licence.empty() && !p.licenceUrl.empty()) licence += " (" + p.licenceUrl + ")";
        md += std::format("| {} | {} | {} | {} | {} | {} |\n", cell(p.title.empty() ? p.sourceFile : p.title), storeTitle(p.store), cell(p.author),
                          cell(licence), cell(p.listingUrl), a.files.size());
    }
    bool anyText = std::any_of(assets.begin(), assets.end(), [](auto& a) { return !a.provenance.licenceText.empty(); });
    if (anyText) {
        md += "\n## Licence texts included with the assets\n";
        for (const auto& a : assets)
            if (!a.provenance.licenceText.empty())
                md += "\n### " + (a.provenance.title.empty() ? a.provenance.sourceFile : a.provenance.title) + "\n\n```\n" + a.provenance.licenceText + "\n```\n";
    }
    return md;
}

Json attributionJson(const std::vector<ThirdPartyAsset>& assets) {
    Json arr = Json::array();
    for (const auto& a : assets) {
        Json j = a.provenance.toJson();
        j["files"] = a.files;
        arr.push_back(j);
    }
    return {{"format", "fledgethirdparty"}, {"version", 1}, {"caveat", kStoreTermsCaveat}, {"assets", arr}};
}

// ------------------------------------------------------------------ report ----
Json ImportReport::toJson() const {
    Json items_ = Json::array();
    for (const auto& i : items) items_.push_back({{"source", i.source}, {"kind", i.kind}, {"status", i.status}, {"output", i.output}, {"detail", i.detail}});
    Json created_ = Json::array();
    for (const auto& c : created) created_.push_back({{"guid", c.guid.toString()}, {"path", c.path}, {"type", fledge::toString(c.type)}});
    return {{"format", "fledgeimportreport"},
            {"version", 1},
            {"title", title},
            {"sourceFile", sourceFile},
            {"detectedType", detectedType},
            {"destination", destination},
            {"provenance", provenance.toJson()},
            {"primary", primary.guid.isValid() ? Json{{"guid", primary.guid.toString()}, {"path", primary.path}, {"type", fledge::toString(primary.type)}} : Json()},
            {"created", created_},
            {"writtenFiles", writtenFiles},
            {"items", items_},
            {"warnings", warnings},
            {"materialNotes", materialNotes},
            {"counts", {{"imported", imported}, {"converted", converted}, {"duplicates", duplicates}, {"skipped", skipped}, {"failed", failed}}}};
}
ImportReport ImportReport::fromJson(const Json& j) {
    ImportReport r;
    if (!j.is_object()) return r;
    r.title = j.value("title", "");
    r.sourceFile = j.value("sourceFile", "");
    r.detectedType = j.value("detectedType", "");
    r.destination = j.value("destination", "");
    r.provenance = Provenance::fromJson(j.value("provenance", Json::object()));
    auto rec = [](const Json& c) {
        AssetRecord a;
        if (!c.is_object()) return a;
        if (auto g = Guid::parse(c.value("guid", ""))) a.guid = *g;
        a.path = c.value("path", "");
        a.type = assetTypeFromString(c.value("type", ""));
        return a;
    };
    if (j.contains("primary")) r.primary = rec(j["primary"]);
    for (auto& c : j.value("created", Json::array())) r.created.push_back(rec(c));
    for (auto& f : j.value("writtenFiles", Json::array()))
        if (f.is_string()) r.writtenFiles.push_back(f.get<std::string>());
    for (auto& i : j.value("items", Json::array()))
        r.items.push_back({i.value("source", ""), i.value("kind", ""), i.value("status", ""), i.value("output", ""), i.value("detail", "")});
    for (auto& w : j.value("warnings", Json::array()))
        if (w.is_string()) r.warnings.push_back(w.get<std::string>());
    for (auto& w : j.value("materialNotes", Json::array()))
        if (w.is_string()) r.materialNotes.push_back(w.get<std::string>());
    Json c = j.value("counts", Json::object());
    r.imported = c.value("imported", 0);
    r.converted = c.value("converted", 0);
    r.duplicates = c.value("duplicates", 0);
    r.skipped = c.value("skipped", 0);
    r.failed = c.value("failed", 0);
    return r;
}
std::string ImportReport::summary() const {
    return std::format("{} imported, {} converted, {} duplicates, {} skipped, {} failed", imported, converted, duplicates, skipped, failed);
}

// ------------------------------------------------------------------ import ----
namespace {

std::string isoDate() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// Sanitises every component of a package / archive path, keeping extensions.
std::string sanitizeRel(const std::string& rel) {
    std::string out;
    size_t p = 0;
    while (p <= rel.size()) {
        size_t s = rel.find('/', p);
        if (s == std::string::npos) s = rel.size();
        std::string part = rel.substr(p, s - p);
        p = s + 1;
        if (part.empty()) continue;
        out += (out.empty() ? "" : "/") + convert::sanitizeName(part, "item", 96);
    }
    return out;
}

std::string replaceExt(const std::string& rel, const std::string& ext) {
    size_t slash = rel.rfind('/'), dot = rel.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return rel + ext;
    return rel.substr(0, dot) + ext;
}
std::string dirOf(const std::string& rel) {
    size_t s = rel.rfind('/');
    return s == std::string::npos ? "" : rel.substr(0, s);
}
std::string stemOf(const std::string& rel) {
    std::string leaf = rel.substr(rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1);
    return leaf.substr(0, leaf.rfind('.'));
}
std::string extOf(const std::string& rel) {
    std::string leaf = rel.substr(rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1);
    size_t d = leaf.rfind('.');
    return d == std::string::npos ? "" : toLower(leaf.substr(d));
}

class ContentWriter {
public:
    ContentWriter(Vfs& vfs, AssetRegistry& reg, ImportReport& rep, Json info) : m_vfs(vfs), m_reg(reg), m_rep(rep), m_info(std::move(info)) {}
    // A path not yet used by this import (case-insensitive); re-imports of the same package reuse the same paths.
    std::string unique(const std::string& rel) {
        std::string cand = rel;
        for (int i = 2; m_used.contains(toLower(cand)); ++i) cand = replaceExt(rel, "") + std::format("_{}", i) + (extOf(rel));
        m_used.insert(toLower(cand));
        return cand;
    }
    AssetRecord write(const std::string& rel, std::span<const uint8_t> bytes, AssetType type, const Json& extra = Json::object(),
                      std::optional<Guid> fixed = std::nullopt) {
        if (!m_vfs.write("content:/" + rel, bytes)) throw std::runtime_error("cannot write content:/" + rel);
        m_rep.writtenFiles.push_back(rel);
        AssetRecord rec;
        rec.path = rel;
        rec.type = type;
        if (type == AssetType::Other) return rec;
        const AssetRecord* existing = fixed ? m_reg.find(*fixed) : nullptr;
        Guid g = fixed && (!existing || existing->path == rel) ? m_reg.addWithGuid(rel, type, *fixed) : m_reg.add(rel, type);
        rec = *m_reg.find(g);
        Json info = m_info;
        for (auto& [k, v] : extra.items()) info[k] = v;
        m_reg.writeMeta(m_vfs, rec, info);
        m_rep.writtenFiles.push_back(rel + AssetRegistry::kMetaExtension);
        m_rep.created.push_back(rec);
        return rec;
    }
    AssetRecord writeText(const std::string& rel, const std::string& text, AssetType type, const Json& extra = Json::object(),
                          std::optional<Guid> fixed = std::nullopt) {
        return write(rel, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), type, extra, fixed);
    }
    // Records what a model importer wrote.
    void adopt(const ImportResult& r) {
        for (const auto& c : r.created) {
            m_rep.writtenFiles.push_back(c.path);
            m_used.insert(toLower(c.path));
            if (c.type != AssetType::Other) {
                m_rep.writtenFiles.push_back(c.path + AssetRegistry::kMetaExtension);
                m_rep.created.push_back(c);
            }
        }
        for (const auto& w : r.warnings) m_rep.warnings.push_back(w);
    }
    // Gives a freshly imported asset a fixed GUID (the Unity GUID) when that GUID is still free.
    bool rekey(AssetRecord& rec, const Guid& g) {
        if (m_reg.find(g)) return m_reg.find(g)->path == rec.path;
        std::string metaPath = "content:/" + rec.path + AssetRegistry::kMetaExtension;
        Json meta = Json::parse(m_vfs.readText(metaPath).value_or("{}"), nullptr, false);
        m_reg.remove(rec.guid);
        m_reg.addWithGuid(rec.path, rec.type, g);
        for (auto& c : m_rep.created)
            if (c.guid == rec.guid) c.guid = g;
        rec.guid = g;
        if (!meta.is_discarded() && meta.is_object()) {
            meta["guid"] = g.toString();
            m_vfs.writeText(metaPath, meta.dump(2));
        }
        return true;
    }
    const Json& info() const { return m_info; }
    AssetRegistry& registry() { return m_reg; }
    Vfs& vfs() { return m_vfs; }

private:
    Vfs& m_vfs;
    AssetRegistry& m_reg;
    ImportReport& m_rep;
    Json m_info;
    std::set<std::string> m_used;
};

void count(ImportReport& r, const ReportItem& item) {
    if (item.status == "imported" || item.status == "updated") ++r.imported;
    else if (item.status == "converted") ++r.converted;
    else if (item.status == "duplicate") ++r.duplicates;
    else if (item.status == "skipped") ++r.skipped;
    else if (item.status == "failed") ++r.failed;
    r.items.push_back(item);
}

bool looksLikeLicence(const std::string& name) {
    std::string l = toLower(name);
    return l.find("licen") != std::string::npos || l.find("eula") != std::string::npos || l.find("copying") != std::string::npos ||
           l.find("readme") != std::string::npos || l.find("credits") != std::string::npos;
}
void captureLicence(ImportReport& rep, const std::string& name, std::span<const uint8_t> bytes) {
    if (!rep.provenance.licenceText.empty() || !looksLikeLicence(name)) return;
    std::string ext = extOf(name);
    if (ext != ".txt" && ext != ".md" && !ext.empty()) return;
    std::string text(reinterpret_cast<const char*>(bytes.data()), std::min<size_t>(bytes.size(), 8192));
    for (auto& c : text)
        if (static_cast<unsigned char>(c) < 0x09) c = ' ';
    rep.provenance.licenceText = text + (bytes.size() > 8192 ? "\n[... truncated; the full file was imported]" : "");
}

bool keepImageBytes(const std::string& ext) { return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp"; }

// Imports one texture file: original bytes for PNG/JPG/TGA/BMP, PSD/GIF converted to PNG, .hdr kept. Returns the record
// or an error message.
Result<AssetRecord> importTexture(ContentWriter& w, const fs::path& file, std::string rel, bool linear, std::optional<Guid> fixed, ImageData* decoded,
                                  bool* convertedOut) {
    auto bytes = readFileBytes(file);
    if (!bytes) return Result<AssetRecord>::failure("cannot read the file");
    std::string ext = extOf(rel);
    if (ext == ".hdr") {
        if (!decodeHdr(*bytes)) return Result<AssetRecord>::failure("not a readable HDR image");
        return w.write(w.unique(rel), *bytes, AssetType::Texture, {{"colorSpace", "linear"}}, fixed);
    }
    std::string err;
    auto img = decodeImage(*bytes, &err);
    if (!img) return Result<AssetRecord>::failure("cannot decode image: " + err);
    bool keep = keepImageBytes(ext);
    if (convertedOut) *convertedOut = !keep;
    Bytes out = keep ? std::move(*bytes) : encodePng(*img);
    if (decoded) *decoded = std::move(*img);
    if (!keep) rel = replaceExt(rel, ".png");
    return w.write(w.unique(rel), out, AssetType::Texture, {{"colorSpace", linear ? "linear" : "srgb"}}, fixed);
}

// ---- Unity packages
Result<ImportReport> importUnityPackage(const fs::path& file, Vfs& vfs, AssetRegistry& reg, const ImportOptions& opt, ImportReport rep, int depth);

// ---- Zip archives (Fab downloads) and single files
Result<ImportReport> importZip(const fs::path& file, Vfs& vfs, AssetRegistry& reg, const ImportOptions& opt, ImportReport rep, int depth);

Result<ImportReport> importFile(const fs::path& file, Vfs& vfs, AssetRegistry& reg, const ImportOptions& opt, int depth) {
    using R = Result<ImportReport>;
    if (depth > 2) return R::failure("archives nested too deeply");
    std::error_code ec;
    if (!fs::is_regular_file(osPath(file), ec)) return R::failure("not a file: " + narrow(file.wstring()));
    uint64_t size = fs::file_size(osPath(file), ec);
    if (size > opt.limits.maxArchiveBytes) return R::failure(std::format("the file is larger than the {} MiB limit", opt.limits.maxArchiveBytes >> 20));
    FileKind kind = detectFileKind(file);
    if (kind == FileKind::UnrealAsset) return R::failure(kUnrealAssetHelp);
    if (kind == FileKind::Executable) return R::failure("programs are never imported");
    if (kind == FileKind::Unknown || kind == FileKind::Directory)
        return R::failure("unsupported file (supported: .unitypackage, .zip, .fbx, .glb, .gltf, .obj, images, audio)");
    ImportReport rep;
    rep.sourceFile = narrow(file.filename().wstring());
    rep.detectedType = toString(kind);
    rep.provenance = opt.provenance;
    if (rep.provenance.sourceFile.empty()) rep.provenance.sourceFile = rep.sourceFile;
    if (rep.provenance.importedAt.empty()) rep.provenance.importedAt = isoDate();
    std::string titleSrc = !opt.title.empty() ? opt.title : !rep.provenance.title.empty() ? rep.provenance.title : narrow(file.stem().wstring());
    rep.title = titleSrc;
    std::string folder = convert::sanitizeName(titleSrc, "Import", 64);
    rep.destination = (opt.destRoot.empty() ? std::string("StoreImports") : sanitizeRel(opt.destRoot)) + "/" + folder;

    if (kind == FileKind::UnityPackage) return importUnityPackage(file, vfs, reg, opt, std::move(rep), depth);
    if (kind == FileKind::Zip) return importZip(file, vfs, reg, opt, std::move(rep), depth);

    Json info = {{"importer", "store"}, {"provenance", rep.provenance.toJson()}};
    ContentWriter w(vfs, reg, rep, info);
    try {
        ModelImportOptions mo;
        mo.destDir = dirOf(rep.destination);
        mo.name = folder;
        mo.externalRoot = file.parent_path();
        mo.provenance = rep.provenance.toJson();
        mo.scale = opt.scale;
        if (kind == FileKind::Fbx || kind == FileKind::Gltf) {
            auto r = kind == FileKind::Fbx ? importFbx(file, vfs, reg, mo) : importGltf(file, vfs, reg, mo);
            if (!r) return R::failure(r.error());
            w.adopt(*r);
            rep.primary = r->primary;
            count(rep, {rep.sourceFile, kind == FileKind::Fbx ? "model" : "model", "converted", r->primary.path, std::format("{} assets", r->created.size())});
        } else if (kind == FileKind::Obj) {
            auto bytes = readFileBytes(file);
            if (!bytes) return R::failure("cannot read the file");
            std::string err;
            auto mesh = parseObj(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err);
            if (!mesh) return R::failure("OBJ import failed: " + err);
            std::string rel = rep.destination + "/" + convert::sanitizeName(rep.sourceFile, "mesh.obj");
            if (opt.objSpace != convert::SourceSpace::Fledge || opt.scale != 1.0f) {
                convert::convertMesh(*mesh, opt.objSpace, opt.scale);
                rep.primary = w.write(w.unique(replaceExt(rel, ".cmesh")), writeCookedMesh(*mesh), AssetType::Mesh, {{"space", convert::toString(opt.objSpace)}});
                count(rep, {rep.sourceFile, "model", "converted", rep.primary.path, std::string("converted from ") + convert::toString(opt.objSpace)});
            } else {
                rep.primary = w.write(w.unique(rel), *bytes, AssetType::Mesh);
                count(rep, {rep.sourceFile, "model", "imported", rep.primary.path, "materials from .mtl are not imported; assign a material"});
            }
        } else if (kind == FileKind::Image) {
            bool conv = false;
            auto r = importTexture(w, file, rep.destination + "/" + convert::sanitizeName(rep.sourceFile, "texture.png"), false, std::nullopt, nullptr, &conv);
            if (!r) return R::failure(r.error());
            rep.primary = *r;
            count(rep, {rep.sourceFile, "texture", conv ? "converted" : "imported", r->path, ""});
        } else if (kind == FileKind::Audio) {
            auto bytes = readFileBytes(file);
            std::string err;
            if (!bytes || !audio::probeClip(*bytes, &err)) return R::failure("audio import failed: " + err);
            rep.primary = w.write(w.unique(rep.destination + "/" + convert::sanitizeName(rep.sourceFile, "sound.wav")), *bytes, AssetType::Audio);
            count(rep, {rep.sourceFile, "audio", "imported", rep.primary.path, ""});
        }
    } catch (const std::exception& e) {
        return R::failure(e.what());
    }
    std::string reportPath = rep.destination + "/" + convert::sanitizeName(rep.title, "Import") + ".fimport";
    rep.writtenFiles.push_back(reportPath);
    vfs.writeText("content:/" + reportPath, rep.toJson().dump(2));
    reg.save(vfs);
    return rep;
}

Result<ImportReport> importUnityPackage(const fs::path& file, Vfs& vfs, AssetRegistry& reg, const ImportOptions& opt, ImportReport rep, int depth) {
    using R = Result<ImportReport>;
    (void)depth;
    if (opt.staging.empty()) return R::failure("internal: no staging folder");
    fs::path stage = opt.staging / std::format("unity{}", depth);
    std::error_code ec;
    fs::remove_all(osPath(stage), ec);
    auto pkg = unity::readPackage(file, stage / "x", opt.limits);
    if (!pkg) return R::failure("not a valid Unity package: " + pkg.error());
    for (auto& w : pkg->warnings) rep.warnings.push_back(w);
    if (rep.provenance.store == Store::Unknown) rep.provenance.store = Store::UnityAssetStore;
    Json info = {{"importer", "unitypackage"}, {"provenance", rep.provenance.toJson()}};
    ContentWriter w(vfs, reg, rep, info);
    const std::string base = rep.destination;

    // Target paths: package path without "Assets/", sanitised, below the import folder.
    std::map<std::string, const unity::PackageEntry*> byGuid;
    std::map<std::string, std::string> target;
    std::map<std::string, unity::EntryKind> kinds;
    std::map<std::string, std::string> reasons;
    for (const auto& e : pkg->entries) {
        byGuid[e.guid] = &e;
        std::string rel = e.pathname;
        if (rel.rfind("Assets/", 0) == 0) rel = rel.substr(7);
        target[e.guid] = base + "/" + sanitizeRel(rel);
        std::string why;
        kinds[e.guid] = e.folder ? unity::EntryKind::Folder : unity::classify(e.pathname, &why);
        reasons[e.guid] = why;
    }
    auto guidOf = [](const std::string& g) { return Guid::parse(g); };

    // 1. materials first (which textures are data maps)
    std::map<std::string, unity::UnityMaterial> mats;
    std::set<std::string> linear;
    for (const auto& e : pkg->entries) {
        if (kinds[e.guid] != unity::EntryKind::Material) continue;
        auto text = readFileText(e.assetFile);
        std::string err;
        auto m = text ? unity::parseMaterial(*text, &err) : std::nullopt;
        if (!m) {
            count(rep, {e.pathname, "material", "failed", "", text ? err : "cannot read"});
            continue;
        }
        if (m->name.empty()) m->name = stemOf(e.pathname);
        for (auto& g : unity::linearTextureGuids(*m)) linear.insert(g);
        mats[e.guid] = std::move(*m);
    }
    // 2. textures
    std::map<std::string, std::string> texFledge;  // unity guid -> fledge texture GUID
    std::map<std::string, ImageData> decoded;
    std::map<std::string, std::vector<std::string>> texByName;  // lower file name -> unity guids
    for (const auto& e : pkg->entries) {
        if (kinds[e.guid] != unity::EntryKind::Texture) continue;
        texByName[toLower(e.pathname.substr(e.pathname.rfind('/') + 1))].push_back(e.guid);
        auto fixed = guidOf(e.guid);
        if (const AssetRecord* ex = fixed ? reg.find(*fixed) : nullptr; ex && ex->path != target[e.guid] && ex->path != replaceExt(target[e.guid], ".png")) {
            texFledge[e.guid] = ex->guid.toString();
            count(rep, {e.pathname, "texture", "duplicate", ex->path, "already in the project (same Unity GUID)"});
            continue;
        }
        bool isLinear = linear.contains(e.guid) || unity::metaSaysLinear(e.meta);
        bool conv = false;
        bool update = fixed && reg.find(*fixed);
        auto r = importTexture(w, e.assetFile, target[e.guid], isLinear, fixed, nullptr, &conv);
        if (!r) {
            count(rep, {e.pathname, "texture", "failed", "", r.error()});
            continue;
        }
        texFledge[e.guid] = r->guid.toString();
        count(rep, {e.pathname, "texture", update ? "updated" : conv ? "converted" : "imported", r->path, isLinear ? "linear (data map)" : ""});
    }
    // 3. audio, fonts, documents, skipped kinds
    for (const auto& e : pkg->entries) {
        unity::EntryKind k = kinds[e.guid];
        if (k == unity::EntryKind::Audio) {
            auto fixed = guidOf(e.guid);
            if (const AssetRecord* ex = fixed ? reg.find(*fixed) : nullptr; ex && ex->path != target[e.guid]) {
                count(rep, {e.pathname, "audio", "duplicate", ex->path, "already in the project (same Unity GUID)"});
                continue;
            }
            auto bytes = readFileBytes(e.assetFile);
            std::string err;
            if (!bytes || !audio::probeClip(*bytes, &err)) {
                count(rep, {e.pathname, "audio", "failed", "", err.empty() ? "cannot read" : err});
                continue;
            }
            auto rec = w.write(w.unique(target[e.guid]), *bytes, AssetType::Audio, {{"unityGuid", e.guid}}, guidOf(e.guid));
            count(rep, {e.pathname, "audio", "imported", rec.path, ""});
        } else if (k == unity::EntryKind::Font || k == unity::EntryKind::Document) {
            auto bytes = readFileBytes(e.assetFile);
            if (!bytes) continue;
            if (k == unity::EntryKind::Document) captureLicence(rep, e.pathname, *bytes);
            auto rec = w.write(w.unique(target[e.guid]), *bytes, AssetType::Other);
            count(rep, {e.pathname, unity::toString(k), "imported", rec.path, k == unity::EntryKind::Document ? "copied as-is" : ""});
        } else if (k == unity::EntryKind::Script || k == unity::EntryKind::Shader || k == unity::EntryKind::UnityData || k == unity::EntryKind::Unsupported) {
            count(rep, {e.pathname, unity::toString(k), "skipped", "", reasons[e.guid]});
        }
    }
    // 4. materials -> .fmat
    std::map<std::string, std::string> matFledge;               // unity guid -> fledge material GUID
    std::map<std::string, std::vector<std::string>> matByName;  // lower material name / file stem -> unity guids
    auto imageOf = [&](const std::string& g) -> const ImageData* {
        if (auto it = decoded.find(g); it != decoded.end()) return &it->second;
        auto e = byGuid.find(g);
        if (e == byGuid.end() || e->second->assetFile.empty()) return nullptr;
        auto bytes = readFileBytes(e->second->assetFile);
        auto img = bytes ? decodeImage(*bytes) : std::nullopt;
        if (!img) return nullptr;
        return &(decoded[g] = std::move(*img));
    };
    for (auto& [g, m] : mats) {
        const unity::PackageEntry& e = *byGuid[g];
        matByName[toLower(m.name)].push_back(g);
        matByName[toLower(stemOf(e.pathname))].push_back(g);
        auto fixed = guidOf(g);
        if (const AssetRecord* ex = fixed ? reg.find(*fixed) : nullptr; ex && ex->path != replaceExt(target[g], ".fmat")) {
            matFledge[g] = ex->guid.toString();
            count(rep, {e.pathname, "material", "duplicate", ex->path, "already in the project (same Unity GUID)"});
            continue;
        }
        std::string matRel = replaceExt(target[g], ".fmat");
        unity::TextureProvider tp;
        tp.fledgeGuid = [&](const std::string& tg) { return texFledge.contains(tg) ? texFledge[tg] : std::string(); };
        tp.image = imageOf;
        tp.writePacked = [&](const std::string& name, const ImageData& img) {
            return w.write(w.unique(dirOf(matRel) + "/" + convert::sanitizeName(name, "packed") + ".png"), encodePng(img), AssetType::Texture,
                           {{"colorSpace", "linear"}, {"generated", "ORM from " + e.pathname}})
                .guid.toString();
        };
        unity::MaterialReport mr;
        Material fm = unity::mapMaterial(m, tp, mr);
        auto rec = w.writeText(w.unique(matRel), fm.toJson().dump(2), AssetType::Material, {{"unityGuid", g}, {"unityShader", mr.shader}}, fixed);
        matFledge[g] = rec.guid.toString();
        std::string detail = "shader " + mr.shader + (mr.notMapped.empty() ? "" : std::format("; {} item(s) not mapped", mr.notMapped.size()));
        for (auto& n : mr.notMapped) rep.materialNotes.push_back(m.name + ": " + n);
        count(rep, {e.pathname, "material", "converted", rec.path, detail});
    }
    // 5. models
    auto findTexture = [&](const std::string& modelDir, const std::string& fileName) -> std::string {
        auto it = texByName.find(toLower(narrow(pathFromUtf8(fileName).filename().wstring())));
        if (it == texByName.end()) return {};
        for (auto& g : it->second)  // prefer a texture in or below the model's folder
            if (byGuid[g]->pathname.rfind(modelDir, 0) == 0 && texFledge.contains(g)) return texFledge[g];
        for (auto& g : it->second)
            if (texFledge.contains(g)) return texFledge[g];
        return {};
    };
    for (const auto& e : pkg->entries) {
        if (kinds[e.guid] != unity::EntryKind::Model) continue;
        std::string ext = extOf(e.pathname);
        std::string modelDir = dirOf(e.pathname);
        auto fixed = guidOf(e.guid);
        // where this model's main asset lands: <dir>/<name>/<name>.fmodel (OBJ: the file itself)
        std::string expected = ext == ".obj" ? target[e.guid] : dirOf(target[e.guid]) + "/" + stemOf(target[e.guid]) + "/" + stemOf(target[e.guid]) + ".fmodel";
        if (const AssetRecord* ex = fixed ? reg.find(*fixed) : nullptr; ex && toLower(ex->path) != toLower(expected)) {
            count(rep, {e.pathname, "model", "duplicate", ex->path, "already in the project (same Unity GUID)"});
            continue;
        }
        unity::ModelMeta mm = unity::parseModelMeta(e.meta);
        ModelImportOptions mo;
        mo.destDir = dirOf(target[e.guid]);
        mo.name = stemOf(target[e.guid]);
        mo.externalRoot = stage / "x";
        mo.provenance = rep.provenance.toJson();
        mo.scale = mm.globalScale * opt.scale;
        mo.textureResolver = [&](const std::string& f) { return findTexture(modelDir, f); };
        mo.materialResolver = [&](const std::string& name) -> std::string {
            if (auto it = mm.materialRemap.find(name); it != mm.materialRemap.end() && matFledge.contains(it->second)) return matFledge[it->second];
            auto it = matByName.find(toLower(name));
            if (it == matByName.end()) return {};
            for (auto& g : it->second)  // Unity's default remap looks next to the model first
                if (dirOf(byGuid[g]->pathname).rfind(modelDir, 0) == 0 && matFledge.contains(g)) return matFledge[g];
            for (auto& g : it->second)
                if (matFledge.contains(g)) return matFledge[g];
            return {};
        };
        Result<ImportResult> r = Result<ImportResult>::failure("unsupported");
        if (ext == ".fbx" || ext == ".glb") {
            fs::path named = stage / "models" / pathFromUtf8(e.guid) / pathFromUtf8(convert::sanitizeName(e.pathname.substr(e.pathname.rfind('/') + 1), "model" + ext));
            fs::create_directories(osPath(named.parent_path()), ec);
            fs::copy_file(osPath(e.assetFile), osPath(named), fs::copy_options::overwrite_existing, ec);
            r = ext == ".fbx" ? importFbx(named, vfs, reg, mo) : importGltf(named, vfs, reg, mo);
        } else if (ext == ".gltf") {
            // .gltf refers to .bin / images by relative path: rebuild the package's folder tree around it
            fs::path tree = stage / "tree";
            for (const auto& o : pkg->entries)
                if (!o.folder && !o.assetFile.empty() && o.pathname.rfind(modelDir + "/", 0) == 0) {
                    fs::path dst = tree / pathFromUtf8(o.pathname);
                    fs::create_directories(osPath(dst.parent_path()), ec);
                    fs::copy_file(osPath(o.assetFile), osPath(dst), fs::copy_options::overwrite_existing, ec);
                }
            mo.externalRoot = tree;
            r = importGltf(tree / pathFromUtf8(e.pathname), vfs, reg, mo);
        } else if (ext == ".obj") {
            auto bytes = readFileBytes(e.assetFile);
            std::string err;
            auto mesh = bytes ? parseObj(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err) : std::nullopt;
            if (!mesh) {
                count(rep, {e.pathname, "model", "failed", "", "OBJ: " + err});
                continue;
            }
            // Unity imports OBJ with a mirrored X axis; the source data itself is right-handed, so keep it.
            auto rec = w.write(w.unique(target[e.guid]), *bytes, AssetType::Mesh, {{"unityGuid", e.guid}}, fixed);
            count(rep, {e.pathname, "model", "imported", rec.path, "OBJ material assignments are not imported"});
            if (!rep.primary.guid.isValid()) rep.primary = rec;
            continue;
        }
        if (!r) {
            count(rep, {e.pathname, "model", "failed", "", r.error()});
            continue;
        }
        w.adopt(*r);
        AssetRecord primary = r->primary;
        if (fixed) w.rekey(primary, *fixed);
        if (!rep.primary.guid.isValid() || rep.primary.type != AssetType::Model) rep.primary = primary;
        count(rep, {e.pathname, "model", "converted", primary.path,
                    std::format("{} assets{}", r->created.size(), mm.globalScale != 1.0f ? std::format(", scale {}", mm.globalScale) : "")});
    }
    if (!rep.primary.guid.isValid() && !rep.created.empty()) rep.primary = rep.created.front();
    std::string reportPath = base + "/" + convert::sanitizeName(rep.title, "Import") + ".fimport";
    rep.writtenFiles.push_back(reportPath);
    vfs.writeText("content:/" + reportPath, rep.toJson().dump(2));
    reg.save(vfs);
    fs::remove_all(osPath(stage), ec);
    FLEDGE_LOG_INFO("import", "Unity package '{}' -> {}: {}", rep.sourceFile, base, rep.summary());
    return rep;
}

struct TextureSet {
    std::string name;                                    // as spelled in the first file name ("Chair")
    std::map<convert::TextureRole, std::string> files;  // role -> archive-relative path
};

Result<ImportReport> importZip(const fs::path& file, Vfs& vfs, AssetRegistry& reg, const ImportOptions& opt, ImportReport rep, int depth) {
    using R = Result<ImportReport>;
    if (opt.staging.empty()) return R::failure("internal: no staging folder");
    fs::path stage = opt.staging / std::format("zip{}", depth);
    std::error_code ec;
    fs::remove_all(osPath(stage), ec);
    archive::ZipReader zip;
    if (Status st = zip.open(file, opt.limits); !st) return R::failure(st.error);
    std::vector<std::string> warnings;
    auto files = zip.extractAll(stage, warnings);
    if (!files) return R::failure(files.error());
    for (auto& w : warnings) rep.warnings.push_back(w);
    Json info = {{"importer", "zip"}, {"provenance", rep.provenance.toJson()}};
    ContentWriter w(vfs, reg, rep, info);
    const std::string base = rep.destination;
    // a single top-level folder ("Chair/...") is not repeated below the import folder
    std::string strip;
    if (!files->empty()) {
        std::string first = files->front().substr(0, files->front().find('/') + 1);
        if (first.size() > 1 && first.back() == '/' &&
            std::all_of(files->begin(), files->end(), [&](const std::string& f) { return f.rfind(first, 0) == 0; }))
            strip = first;
    }
    auto targetOf = [&](const std::string& rel) { return base + "/" + sanitizeRel(rel.substr(strip.size())); };

    std::map<std::string, std::string> texImported;            // archive path -> GUID
    std::map<std::string, std::vector<std::string>> texByName;  // lower leaf -> archive paths
    std::map<std::string, TextureSet> sets;                     // lower set name -> set
    int unrealFiles = 0, importable = 0;
    for (const auto& f : *files) {
        std::string why;
        unity::EntryKind k = unity::classify(f, &why);
        std::string ext = extOf(f);
        if (ext == ".uasset" || ext == ".umap" || ext == ".uexp" || ext == ".ubulk" || ext == ".uproject" || ext == ".uplugin") ++unrealFiles;
        else if (k != unity::EntryKind::Unsupported && k != unity::EntryKind::Script) ++importable;
        if (ext == ".unitypackage") ++importable;
        if (k == unity::EntryKind::Texture) {
            texByName[toLower(f.substr(f.rfind('/') + 1))].push_back(f);
            std::string set;
            convert::TextureRole role = convert::guessTextureRole(f, &set);
            if (role != convert::TextureRole::Unknown && !set.empty()) {
                TextureSet& ts = sets[toLower(set)];
                if (ts.name.empty()) ts.name = set;
                ts.files[role] = f;
            }
        }
    }
    if (importable == 0 && unrealFiles > 0) return R::failure(kUnrealAssetHelp);

    auto importTex = [&](const std::string& f, bool linear) -> std::string {
        if (auto it = texImported.find(f); it != texImported.end()) return it->second;
        bool conv = false;
        auto r = importTexture(w, stage / pathFromUtf8(f), targetOf(f), linear, std::nullopt, nullptr, &conv);
        if (!r) {
            count(rep, {f, "texture", "failed", "", r.error()});
            return texImported[f] = std::string();
        }
        count(rep, {f, "texture", conv ? "converted" : "imported", r->path, linear ? "linear (data map)" : ""});
        return texImported[f] = r->guid.toString();
    };
    auto roleLinear = [](convert::TextureRole r) { return r != convert::TextureRole::BaseColor && r != convert::TextureRole::Emissive; };
    std::map<std::string, std::string> setMaterial;  // set -> material GUID
    auto materialForSet = [&](const std::string& setKey) -> std::string {
        if (auto it = setMaterial.find(setKey); it != setMaterial.end()) return it->second;
        const TextureSet& s = sets[setKey];
        Material m;
        std::vector<std::string> notes;
        auto get = [&](convert::TextureRole r) { return s.files.contains(r) ? s.files.at(r) : std::string(); };
        if (auto f = get(convert::TextureRole::BaseColor); !f.empty()) m.baseColorTexture = importTex(f, false);
        if (auto f = get(convert::TextureRole::Normal); !f.empty()) {
            m.normalTexture = importTex(f, true);
            std::string l = toLower(f);
            if (l.find("dx") != std::string::npos || l.find("directx") != std::string::npos)
                notes.push_back("normal map looks DirectX-style (green down, as Unreal uses); Fledge expects OpenGL-style - flip green if lighting looks inverted");
        }
        auto decodeFile = [&](const std::string& f) -> std::optional<ImageData> {
            auto b = f.empty() ? std::nullopt : readFileBytes(stage / pathFromUtf8(f));
            return b ? decodeImage(*b) : std::nullopt;
        };
        if (auto f = get(convert::TextureRole::Orm); !f.empty()) {  // Unreal ORM / ARM = glTF layout
            m.metallicRoughnessTexture = m.occlusionTexture = importTex(f, true);
            m.metallic = m.roughness = 1.0f;
        } else if (auto ms = get(convert::TextureRole::MetallicSmoothness); !ms.empty()) {
            if (auto img = decodeFile(ms)) {
                auto occ = decodeFile(get(convert::TextureRole::Occlusion));
                ImageData orm = convert::unityMetallicSmoothnessToOrm(&*img, &*img, 1.0f, occ ? &*occ : nullptr);
                m.metallicRoughnessTexture = w.write(w.unique(dirOf(targetOf(ms)) + "/" + convert::sanitizeName(s.name, "set") + "_ORM.png"), encodePng(orm),
                                                     AssetType::Texture, {{"colorSpace", "linear"}}).guid.toString();
                if (occ) m.occlusionTexture = m.metallicRoughnessTexture;
                m.metallic = m.roughness = 1.0f;
            }
        } else {
            auto rough = decodeFile(get(convert::TextureRole::Roughness));
            auto smooth = rough ? std::nullopt : decodeFile(get(convert::TextureRole::Smoothness));
            auto metal = decodeFile(get(convert::TextureRole::Metallic));
            auto occ = decodeFile(get(convert::TextureRole::Occlusion));
            const ImageData* r = rough ? &*rough : smooth ? &*smooth : nullptr;
            if (r || metal || occ) {
                ImageData orm = convert::packChannels({occ ? &*occ : nullptr, 0}, {r, 0, !rough && smooth, 1.0f, 0.5f}, {metal ? &*metal : nullptr, 0, false, 1.0f, 0.0f});
                std::string anchor = !get(convert::TextureRole::Roughness).empty() ? get(convert::TextureRole::Roughness) : s.files.begin()->second;
                m.metallicRoughnessTexture = w.write(w.unique(dirOf(targetOf(anchor)) + "/" + convert::sanitizeName(s.name, "set") + "_ORM.png"), encodePng(orm),
                                                     AssetType::Texture, {{"colorSpace", "linear"}}).guid.toString();
                if (occ) m.occlusionTexture = m.metallicRoughnessTexture;
                m.roughness = r ? 1.0f : 0.5f;
                m.metallic = metal ? 1.0f : 0.0f;
            }
        }
        if (auto f = get(convert::TextureRole::Emissive); !f.empty()) {
            m.emissiveTexture = importTex(f, false);
            m.emissive = {1, 1, 1};
        }
        if (!get(convert::TextureRole::Opacity).empty()) notes.push_back("separate opacity map not mapped (Fledge uses base colour alpha)");
        if (!get(convert::TextureRole::Height).empty()) notes.push_back("height / displacement map not supported");
        std::string anchor = s.files.begin()->second;
        std::string rel = w.unique(dirOf(targetOf(anchor)) + "/" + convert::sanitizeName(s.name, "Material") + ".fmat");
        auto rec = w.writeText(rel, m.toJson().dump(2), AssetType::Material, {{"textureSet", setKey}});
        for (auto& n : notes) rep.materialNotes.push_back(s.name + ": " + n);
        count(rep, {s.name, "material", "converted", rec.path, std::format("built from {} texture(s) by file name", s.files.size())});
        return setMaterial[setKey] = rec.guid.toString();
    };
    auto normalizeMat = [](std::string n) {
        n = toLower(n);
        for (const char* p : {"mi_", "m_", "mat_", "mtl_"})
            if (n.rfind(p, 0) == 0) n = n.substr(std::char_traits<char>::length(p));
        for (const char* sfx : {"_mat", "_material", "_mtl", "_inst"})
            if (endsWith(n, sfx)) n = n.substr(0, n.size() - std::char_traits<char>::length(sfx));
        return n;
    };

    // models first (they pull in their textures), then everything else
    for (const auto& f : *files) {
        std::string ext = extOf(f);
        if (ext != ".fbx" && ext != ".glb" && ext != ".gltf" && ext != ".obj") continue;
        std::string modelDir = dirOf(f);
        ModelImportOptions mo;
        mo.destDir = dirOf(targetOf(f));
        mo.name = stemOf(targetOf(f));
        mo.externalRoot = stage;
        mo.provenance = rep.provenance.toJson();
        mo.scale = opt.scale;
        mo.textureResolver = [&](const std::string& tf) -> std::string {
            auto it = texByName.find(toLower(narrow(pathFromUtf8(tf).filename().wstring())));
            if (it == texByName.end()) return {};
            std::string best = it->second.front();
            for (auto& c : it->second)
                if (dirOf(c).rfind(modelDir, 0) == 0) best = c;
            return importTex(best, false);
        };
        mo.materialResolver = [&](const std::string& name) -> std::string {
            std::string n = normalizeMat(name);
            if (sets.contains(n)) return materialForSet(n);
            if (sets.size() == 1) return materialForSet(sets.begin()->first);  // one material set in the download
            return {};
        };
        if (ext == ".obj") {
            auto bytes = readFileBytes(stage / pathFromUtf8(f));
            std::string err;
            auto mesh = bytes ? parseObj(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err) : std::nullopt;
            if (!mesh) {
                count(rep, {f, "model", "failed", "", "OBJ: " + err});
                continue;
            }
            AssetRecord rec;
            if (opt.objSpace != convert::SourceSpace::Fledge || opt.scale != 1.0f) {
                convert::convertMesh(*mesh, opt.objSpace, opt.scale);
                rec = w.write(w.unique(replaceExt(targetOf(f), ".cmesh")), writeCookedMesh(*mesh), AssetType::Mesh);
            } else {
                rec = w.write(w.unique(targetOf(f)), *bytes, AssetType::Mesh);
            }
            count(rep, {f, "model", "imported", rec.path, "OBJ material assignments are not imported"});
            if (!rep.primary.guid.isValid()) rep.primary = rec;
            continue;
        }
        auto r = ext == ".fbx" ? importFbx(stage / pathFromUtf8(f), vfs, reg, mo) : importGltf(stage / pathFromUtf8(f), vfs, reg, mo);
        if (!r) {
            count(rep, {f, "model", "failed", "", r.error()});
            continue;
        }
        w.adopt(*r);
        if (!rep.primary.guid.isValid() || rep.primary.type != AssetType::Model) rep.primary = r->primary;
        count(rep, {f, "model", "converted", r->primary.path, std::format("{} assets", r->created.size())});
    }
    for (const auto& f : *files) {
        std::string why;
        unity::EntryKind k = unity::classify(f, &why);
        std::string ext = extOf(f);
        if (ext == ".unitypackage") {
            ImportOptions sub = opt;
            sub.destRoot = base;
            sub.title = stemOf(f);
            sub.staging = opt.staging / std::format("nested{}", depth);
            auto r = importFile(stage / pathFromUtf8(f), vfs, reg, sub, depth + 1);
            if (!r) {
                count(rep, {f, "unity package", "failed", "", r.error()});
                continue;
            }
            for (auto& x : r->writtenFiles) rep.writtenFiles.push_back(x);
            for (auto& c : r->created) rep.created.push_back(c);
            for (auto& i : r->items) count(rep, {f + ": " + i.source, i.kind, i.status, i.output, i.detail});
            for (auto& m : r->materialNotes) rep.materialNotes.push_back(m);
            if (!rep.primary.guid.isValid()) rep.primary = r->primary;
            continue;
        }
        if (ext == ".uasset" || ext == ".umap" || ext == ".uexp" || ext == ".ubulk") {
            count(rep, {f, "unreal", "skipped", "", "Unreal Engine asset: export it to FBX or glTF from Unreal (see the import report)"});
            continue;
        }
        if (k == unity::EntryKind::Model) continue;
        if (k == unity::EntryKind::Texture) {
            std::string set;
            convert::TextureRole role = convert::guessTextureRole(f, &set);
            importTex(f, role != convert::TextureRole::Unknown && roleLinear(role));
        } else if (k == unity::EntryKind::Audio) {
            auto bytes = readFileBytes(stage / pathFromUtf8(f));
            std::string err;
            if (!bytes || !audio::probeClip(*bytes, &err)) {
                count(rep, {f, "audio", "failed", "", err});
                continue;
            }
            auto rec = w.write(w.unique(targetOf(f)), *bytes, AssetType::Audio);
            count(rep, {f, "audio", "imported", rec.path, ""});
        } else if (k == unity::EntryKind::Document || k == unity::EntryKind::Font) {
            auto bytes = readFileBytes(stage / pathFromUtf8(f));
            if (!bytes) continue;
            if (k == unity::EntryKind::Document) captureLicence(rep, f, *bytes);
            auto rec = w.write(w.unique(targetOf(f)), *bytes, AssetType::Other);
            count(rep, {f, unity::toString(k), "imported", rec.path, "copied as-is"});
        } else if (k != unity::EntryKind::Material && k != unity::EntryKind::Folder) {
            count(rep, {f, unity::toString(k), "skipped", "", why});
        } else if (k == unity::EntryKind::Material) {
            count(rep, {f, "material", "skipped", "", "Unity .mat outside a .unitypackage has no texture references to resolve"});
        }
    }
    // texture sets that no model claimed still become materials (a texture-only download)
    for (auto& [key, s] : sets)
        if (s.files.size() >= 2 && !setMaterial.contains(key)) materialForSet(key);
    if (!rep.primary.guid.isValid() && !rep.created.empty()) rep.primary = rep.created.front();
    std::string reportPath = base + "/" + convert::sanitizeName(rep.title, "Import") + ".fimport";
    rep.writtenFiles.push_back(reportPath);
    vfs.writeText("content:/" + reportPath, rep.toJson().dump(2));
    reg.save(vfs);
    fs::remove_all(osPath(stage), ec);
    FLEDGE_LOG_INFO("import", "Archive '{}' -> {}: {}", rep.sourceFile, base, rep.summary());
    return rep;
}

} // namespace

Result<ImportReport> importStoreFile(const fs::path& file, Vfs& vfs, AssetRegistry& registry, const ImportOptions& options) {
    if (!dynamic_cast<DirectoryProvider*>(vfs.provider("content"))) return Result<ImportReport>::failure("no project is open (content:/ is not on disk)");
    return importFile(file, vfs, registry, options, 0);
}

Result<Inspection> inspectStoreFile(const fs::path& file, const archive::Limits& limits) {
    using R = Result<Inspection>;
    Inspection in;
    std::error_code ec;
    in.kind = detectFileKind(file);
    if (in.kind != FileKind::Directory) in.fileSize = fs::file_size(osPath(file), ec);
    if (in.kind == FileKind::UnrealAsset) in.note = kUnrealAssetHelp;
    else if (in.kind == FileKind::Executable) in.note = "programs are never imported";
    if (in.kind == FileKind::UnityPackage) {
        archive::FileSource src(file);
        std::map<std::string, std::string> pathnames;
        std::set<std::string> withAsset;
        std::string* cur = nullptr;
        archive::TarParser tar(
            limits,
            [&](const archive::TarEntry& e) -> archive::Sink {
                cur = nullptr;
                size_t slash = e.path.find('/');
                std::string guid = e.path.substr(0, slash);
                if (!unity::isUnityGuid(guid) || slash == std::string::npos) return {};
                std::string name = e.path.substr(slash + 1);
                if (name == "asset") {
                    withAsset.insert(guid);
                    in.contentBytes += e.size;
                    return [](const uint8_t*, size_t) { return true; };
                }
                if (name == "pathname") {
                    cur = &pathnames[guid];
                    return [&](const uint8_t* d, size_t n) {
                        if (cur->size() + n > 4096) return false;
                        cur->append(reinterpret_cast<const char*>(d), n);
                        return true;
                    };
                }
                return [](const uint8_t*, size_t) { return true; };
            },
            [&](const archive::TarEntry&) { cur = nullptr; });
        Status st = archive::gunzip(src, [&](const uint8_t* d, size_t n) { return tar.feed(d, n); }, limits);
        if (!st) return R::failure(tar.error().empty() ? st.error : tar.error());
        for (auto& [g, p] : pathnames) {
            std::string first = p.substr(0, p.find('\n'));
            if (!withAsset.contains(g)) continue;
            in.counts[unity::toString(unity::classify(first))]++;
            ++in.entries;
        }
        in.warnings = tar.warnings();
    } else if (in.kind == FileKind::Zip) {
        archive::ZipReader zr;
        if (Status st = zr.open(file, limits); !st) return R::failure(st.error);
        for (const auto& e : zr.entries()) {
            if (e.directory) continue;
            if (!e.skipReason.empty()) {
                in.warnings.push_back(e.rawPath + ": " + e.skipReason);
                continue;
            }
            std::string ext = extOf(e.path);
            std::string kind = (ext == ".uasset" || ext == ".umap" || ext == ".uexp" || ext == ".ubulk") ? "unreal"
                               : ext == ".unitypackage"                                                  ? "unity package"
                                                                                                          : unity::toString(unity::classify(e.path));
            in.counts[kind]++;
            in.contentBytes += e.size;
            ++in.entries;
        }
        if (in.counts.contains("unreal") && in.counts.size() == 1) in.note = kUnrealAssetHelp;
    } else if (in.kind != FileKind::Unknown && in.kind != FileKind::Directory) {
        in.entries = 1;
        in.contentBytes = in.fileSize;
        in.counts[toString(in.kind)] = 1;
    }
    return in;
}

Status undoImport(const ImportReport& report, Vfs& vfs, AssetRegistry& registry) {
    auto* dir = dynamic_cast<DirectoryProvider*>(vfs.provider("content"));
    if (!dir) return Status::failure("no project is open");
    std::error_code ec;
    std::set<std::string> folders;
    int removed = 0;
    for (const auto& rel : report.writtenFiles) {
        fs::path p = dir->resolve(rel);
        if (p.empty()) continue;  // never outside Content/
        if (fs::remove(osPath(p), ec)) ++removed;
        for (std::string d = dirOf(rel); !d.empty(); d = dirOf(d)) folders.insert(d);
    }
    for (const auto& c : report.created)
        if (const AssetRecord* r = registry.find(c.guid); r && r->path == c.path) registry.remove(c.guid);
    // deepest first: remove folders the import left empty
    std::vector<std::string> sorted(folders.begin(), folders.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
    for (const auto& d : sorted) {
        fs::path p = dir->resolve(d);
        if (!p.empty() && fs::is_directory(osPath(p), ec) && fs::is_empty(osPath(p), ec)) fs::remove(osPath(p), ec);
    }
    registry.save(vfs);
    FLEDGE_LOG_INFO("import", "Undid import '{}': {} files removed", report.title, removed);
    return {};
}

} // namespace fledge::store
