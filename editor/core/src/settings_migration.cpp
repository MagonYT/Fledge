// Settings migration Fledge -> Fledge Advanced (settings_migration.h, docs/MIGRATING_SETTINGS.md).
// One function does the work for both the dry run and the real run, so the preview shows exactly what applying does.
#include "fledge/editor/settings_migration.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <format>
#include <set>

#include "fledge/ai/llm.h"
#include "fledge/core/features.h"
#include "fledge/core/log.h"
#include "fledge/editor/autosave.h"
#include "fledge/editor/project_templates.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fledge::editor {

const char* const kMigrationKeysNotice =
    "API keys saved in Settings > AI Providers are kept in Windows Credential Manager (\"Fledge/llm/...\"), which Fledge and Fledge "
    "Advanced already share for your Windows user, so they are not copied (keys in environment variables are not touched either). "
    "\"AI keys\" covers key-like values typed into the plain settings file: custom endpoint extra headers, extra body fields and URL "
    "parameters whose names look secret (key, token, auth, ...). They are copied only because this is the same Windows user on the "
    "same machine; untick it to leave them out.";

const char* toString(MigrationMode m) { return m == MigrationMode::Overwrite ? "overwrite" : "merge"; }

const char* toString(MigrationAction a) {
    switch (a) {
        case MigrationAction::Copy: return "copy";
        case MigrationAction::Merge: return "merge";
        case MigrationAction::Overwrite: return "overwrite";
        case MigrationAction::Unchanged: return "unchanged";
        case MigrationAction::Skip: return "skip";
        case MigrationAction::Exclude: return "exclude";
        case MigrationAction::Error: return "error";
    }
    return "?";
}

Json MigrationItem::toJson() const {
    Json j = {{"path", path}, {"what", what}, {"action", toString(action)}, {"detail", detail}, {"bytes", bytes}};
    if (!added.empty()) j["added"] = added;
    if (!replaced.empty()) j["replaced"] = replaced;
    if (!kept.empty()) j["kept"] = kept;
    if (!withheld.empty()) j["withheld"] = withheld;
    return j;
}

std::string MigrationResult::summary() const {
    if (refused) return "Settings migration refused: " + (errors.empty() ? std::string("unknown reason") : errors.front());
    return std::format("{}{} copied, {} merged, {} overwritten, {} unchanged, {} skipped, {} excluded, {} backed up{}", applied ? "" : "Dry run: ",
                       copied, merged, overwritten, unchanged, skipped, excluded, backedUp, failed ? std::format(", {} failed", failed) : "");
}

std::string MigrationResult::text() const {
    std::string t = std::format("Settings migration ({}, {})\n  from: {}\n  to:   {}\n{}\n", toString(options.mode), applied ? "applied" : "dry run",
                                narrow(source.wstring()), narrow(destination.wstring()), summary());
    for (auto& i : items) {
        t += std::format("  [{}] {} - {}{}{}\n", toString(i.action), i.path, i.what, i.detail.empty() ? "" : ": ", i.detail);
        auto list = [&](const char* label, const std::vector<std::string>& v) {
            if (v.empty()) return;
            std::string s;
            for (size_t k = 0; k < v.size(); ++k) s += (k ? ", " : "") + v[k];
            t += std::format("      {}: {}\n", label, s);
        };
        list("added", i.added);
        list("replaced", i.replaced);
        list("kept", i.kept);
        list("not copied", i.withheld);
    }
    if (!backupDir.empty()) t += "Backups: " + narrow(backupDir.wstring()) + "\n";
    for (auto& n : notes) t += "Note: " + n + "\n";
    for (auto& e : errors) t += "Error: " + e + "\n";
    return t;
}

Json MigrationResult::toJson() const {
    Json it = Json::array();
    for (auto& i : items) it.push_back(i.toJson());
    return {{"source", narrow(source.wstring())},
            {"destination", narrow(destination.wstring())},
            {"mode", toString(options.mode)},
            {"includeAiKeys", options.includeAiKeys},
            {"includeEditionMode", options.includeEditionMode},
            {"includeAiPermissions", options.includeAiPermissions},
            {"applied", applied},
            {"refused", refused},
            {"ok", ok()},
            {"summary", summary()},
            {"counts",
             {{"copied", copied}, {"merged", merged}, {"overwritten", overwritten}, {"unchanged", unchanged}, {"skipped", skipped},
              {"excluded", excluded}, {"backedUp", backedUp}, {"failed", failed}}},
            {"backupDir", backupDir.empty() ? Json(nullptr) : Json(narrow(backupDir.wstring()))},
            {"items", it},
            {"notes", notes},
            {"errors", errors}};
}

namespace {

constexpr const char* kBackupFolder = "Migration Backups";

// ---- path helpers ------------------------------------------------------------------------------------------------------
bool isLink(const fs::path& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(osPath(p).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;  // symlinks and junctions
#endif
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(osPath(p), ec));
}

std::wstring comparable(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    std::wstring s = (ec ? fs::absolute(p, ec) : c).lexically_normal().wstring();
    std::replace(s.begin(), s.end(), L'\\', L'/');
    while (s.size() > 1 && s.back() == L'/') s.pop_back();
#ifdef _WIN32
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
#endif
    return s;
}

bool samePath(const fs::path& a, const fs::path& b) { return !a.empty() && !b.empty() && comparable(a) == comparable(b); }

bool nested(const fs::path& a, const fs::path& b) {  // one inside the other
    std::wstring x = comparable(a) + L"/", y = comparable(b) + L"/";
    return x.starts_with(y) || y.starts_with(x);
}

std::string localTimestamp() {
    std::time_t t = std::time(nullptr);
    const std::tm* tm = std::localtime(&t);
    char buf[32] = "backup";
    if (tm) std::strftime(buf, sizeof buf, "%Y-%m-%d_%H-%M-%S", tm);
    return buf;
}

// ---- item rules --------------------------------------------------------------------------------------------------------
enum class Rule { Settings, JsonMerge, File, LayoutsDir, Exclude, Unknown };

struct Known {
    Rule rule;
    const char* what;
    const char* why = "";
};

Known classify(const std::string& name, bool dir) {
    std::string l = toLower(name);
    if (!dir) {
        if (l == "editor_settings.json") return {Rule::Settings, "Editor settings"};
        if (l == "code_editor.json") return {Rule::JsonMerge, "Code Editor settings"};
        if (l == "editor_layout.ini") return {Rule::File, "Window layout"};
        if (endsWith(l, ".dmp") || endsWith(l, ".mdmp")) return {Rule::Exclude, "Crash dump", "machine-specific diagnostics"};
        if (endsWith(l, ".log")) return {Rule::Exclude, "Log file", "logs stay with the edition that wrote them"};
        if (endsWith(l, ".tmp")) return {Rule::Exclude, "Temporary file", "unfinished write"};
        if (l == "portable.txt") return {Rule::Exclude, "Portable marker", "install-specific"};
        return {Rule::Unknown, "Unknown file", "not a setting this version knows; not copied"};
    }
    if (l == "layouts") return {Rule::LayoutsDir, "Saved workspace layouts"};
    if (l == "logs") return {Rule::Exclude, "Log files", "logs and crash reports stay with the edition that wrote them"};
    if (l == "crashes" || l == "crashdumps") return {Rule::Exclude, "Crash dumps", "machine-specific diagnostics"};
    if (l == "toolchains") return {Rule::Exclude, "Downloaded C++ toolchain", "large and install-specific; Settings > Scripts installs it (a copied compiler path keeps working)"};
    if (l == "storedownloads") return {Rule::Exclude, "Asset store downloads", "download cache"};
    if (l == "upscalers") return {Rule::Exclude, "Upscaler runtimes", "download cache with vendor licences accepted per install"};
    if (l == "games") return {Rule::Exclude, "Packaged games' player data", "belongs to the games, not to the editor"};
    if (l == "ipc") return {Rule::Exclude, "Link IPC endpoint", "per running editor"};
    if (l == "intermediate" || l == "cache" || l == "caches" || l == "shadercache") return {Rule::Exclude, "Cache", "rebuilt automatically"};
    if (l == "selftestprojects" || l == "projects") return {Rule::Exclude, "Projects", "projects are never moved; the projects folder setting is copied"};
    if (l == toLower(kBackupFolder)) return {Rule::Exclude, "Migration backups", "backups of an earlier migration"};
    return {Rule::Unknown, "Unknown folder", "not a setting this version knows; not copied"};
}

std::string joinKey(const std::string& base, const std::string& k) { return base.empty() ? k : base + "." + k; }

// Objects merge key by key; anything else is a value. Merge: only missing keys are added. Overwrite: the source's value wins.
void deepMerge(Json& dst, const Json& src, MigrationMode mode, const std::string& base, MigrationItem& item) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        std::string key = joinKey(base, it.key());
        if (!dst.contains(it.key())) {
            dst[it.key()] = it.value();
            item.added.push_back(key);
        } else if (dst[it.key()].is_object() && it.value().is_object()) {
            deepMerge(dst[it.key()], it.value(), mode, key, item);
        } else if (dst[it.key()] != it.value()) {
            if (mode == MigrationMode::Overwrite) {
                dst[it.key()] = it.value();
                item.replaced.push_back(key);
            } else {
                item.kept.push_back(key);
            }
        }
    }
}

std::string recentKey(const Json& v) { return v.is_string() ? toLower(v.get<std::string>()) : std::string(); }

// Recent projects: a union, the winning side first, capped like RecentProjects.
Json mergeRecent(const Json& dst, const Json& src, MigrationMode mode) {
    const Json& first = mode == MigrationMode::Overwrite ? src : dst;
    const Json& second = mode == MigrationMode::Overwrite ? dst : src;
    Json out = Json::array();
    std::set<std::string> seen;
    for (const Json* list : {&first, &second}) {
        if (!list->is_array()) continue;
        for (auto& v : *list)
            if (v.is_string() && seen.insert(recentKey(v)).second && out.size() < RecentProjects::kMax) out.push_back(v);
    }
    return out;
}

// URL query parameters with secret-looking names are key material.
std::string stripSecretQuery(const std::string& url, bool& removed) {
    size_t q = url.find('?');
    if (q == std::string::npos) return url;
    size_t hash = url.find('#', q);
    std::string query = url.substr(q + 1, hash == std::string::npos ? std::string::npos : hash - q - 1);
    std::string kept;
    size_t a = 0;
    while (a <= query.size()) {
        size_t b = query.find('&', a);
        std::string part = query.substr(a, b == std::string::npos ? std::string::npos : b - a);
        std::string name = part.substr(0, part.find('='));
        if (!part.empty()) {
            if (ai::looksLikeSecretName(name)) removed = true;
            else kept += (kept.empty() ? "" : "&") + part;
        }
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return url.substr(0, q) + (kept.empty() ? "" : "?" + kept) + (hash == std::string::npos ? "" : url.substr(hash));
}

// Removes key material from the "ai" section (names recorded in item.withheld, values never).
void withholdAiKeys(Json& ai, MigrationItem& item) {
    if (!ai.is_object()) return;
    for (const char* k : {"baseUrl"}) {
        if (ai.contains(k) && ai[k].is_string()) {
            bool removed = false;
            ai[k] = stripSecretQuery(ai[k].get<std::string>(), removed);
            if (removed) item.withheld.push_back(std::string("ai.") + k + " (secret-looking URL parameters)");
        }
    }
    if (!ai.contains("custom") || !ai["custom"].is_object()) return;
    Json& c = ai["custom"];
    if (c.contains("url") && c["url"].is_string()) {
        bool removed = false;
        c["url"] = stripSecretQuery(c["url"].get<std::string>(), removed);
        if (removed) item.withheld.push_back("ai.custom.url (secret-looking URL parameters)");
    }
    if (c.contains("extraHeaders") && c["extraHeaders"].is_array()) {
        Json keep = Json::array();
        for (auto& h : c["extraHeaders"]) {
            std::string name = h.is_object() && h.contains("name") && h["name"].is_string() ? h["name"].get<std::string>() : "";
            if (ai::looksLikeSecretName(name)) item.withheld.push_back("ai.custom.extraHeaders[" + name + "]");
            else keep.push_back(h);
        }
        c["extraHeaders"] = keep;
    }
    if (c.contains("extraBody") && c["extraBody"].is_object()) {
        Json keep = Json::object();
        for (auto it = c["extraBody"].begin(); it != c["extraBody"].end(); ++it) {
            if (ai::looksLikeSecretName(it.key())) item.withheld.push_back("ai.custom.extraBody." + it.key());
            else keep[it.key()] = it.value();
        }
        c["extraBody"] = keep;
    }
}

bool eraseKey(Json& j, const char* section, const char* key) {
    if (!j.contains(section) || !j[section].is_object() || !j[section].contains(key)) return false;
    j[section].erase(key);
    return true;
}

// ---- the run -----------------------------------------------------------------------------------------------------------
struct Runner {
    fs::path src, dst;
    MigrationOptions opt;
    bool apply = false;
    MigrationResult r;
    uint64_t totalBytes = 0;
    size_t files = 0;

    void add(MigrationItem i) {
        switch (i.action) {
            case MigrationAction::Copy: ++r.copied; break;
            case MigrationAction::Merge: ++r.merged; break;
            case MigrationAction::Overwrite: ++r.overwritten; break;
            case MigrationAction::Unchanged: ++r.unchanged; break;
            case MigrationAction::Skip: ++r.skipped; break;
            case MigrationAction::Exclude: ++r.excluded; break;
            case MigrationAction::Error: ++r.failed; break;
        }
        r.items.push_back(std::move(i));
    }

    // Reads a source file within the caps. nullopt: item already filled with the reason.
    std::optional<std::string> readSource(const fs::path& file, MigrationItem& item) {
        std::error_code ec;
        uint64_t size = fs::file_size(osPath(file), ec);
        if (ec) {
            item.action = MigrationAction::Error;
            item.detail = "cannot read: " + fileErrorReason(file);
            return std::nullopt;
        }
        item.bytes = size;
        if (size > opt.maxFileBytes) {
            item.action = MigrationAction::Skip;
            item.detail = std::format("too large ({} bytes, limit {}): not copied", size, opt.maxFileBytes);
            return std::nullopt;
        }
        if (++files > opt.maxFiles || totalBytes + size > opt.maxTotalBytes) {
            item.action = MigrationAction::Skip;
            item.detail = "over the migration's total size / file count limit: not copied";
            return std::nullopt;
        }
        totalBytes += size;
        auto bytes = readFileBytes(file);
        if (!bytes) {
            item.action = MigrationAction::Error;
            item.detail = "cannot read: " + fileErrorReason(file);
            return std::nullopt;
        }
        return std::string(bytes->begin(), bytes->end());
    }

    std::optional<std::string> readDest(const fs::path& file) {
        std::error_code ec;
        if (!fs::exists(osPath(file), ec)) return std::nullopt;
        auto bytes = readFileBytes(file);
        if (!bytes) return std::nullopt;
        return std::string(bytes->begin(), bytes->end());
    }

    bool backup(const std::string& rel, const fs::path& file, MigrationItem& item) {
        std::error_code ec;
        if (!fs::exists(osPath(file), ec)) return true;
        if (r.backupDir.empty()) {
            std::string stamp = opt.timestamp ? opt.timestamp() : localTimestamp();
            fs::path base = dst / kBackupFolder;
            fs::path dir = base / pathFromUtf8(stamp);
            for (int i = 2; fs::exists(osPath(dir), ec) && i < 1000; ++i) dir = base / pathFromUtf8(stamp + "_" + std::to_string(i));
            r.backupDir = dir;
        }
        fs::path to = r.backupDir / pathFromUtf8(rel);
        fs::create_directories(osPath(to.parent_path()), ec);
        auto bytes = readFileBytes(file);
        if (!bytes || !RecoveryStore::atomicWrite(to, std::string(bytes->begin(), bytes->end()))) {
            item.action = MigrationAction::Error;
            item.detail = "could not back up the existing file; it was left unchanged";
            return false;
        }
        ++r.backedUp;
        return true;
    }

    // Writes (apply) a changed destination file, backing up the old one first.
    void commit(const std::string& rel, const fs::path& file, const std::string& data, bool existed, MigrationItem& item) {
        if (!apply) return;
        if (existed && !backup(rel, file, item)) return;
        if (!RecoveryStore::atomicWrite(file, data)) {
            item.action = MigrationAction::Error;
            item.detail = "could not write " + rel + " (the destination file is unchanged" + (existed ? "; a backup was made)" : ")");
        }
    }

    void copyFile(const std::string& rel, const char* what) {
        MigrationItem item{rel, what};
        fs::path from = src / pathFromUtf8(rel), to = dst / pathFromUtf8(rel);
        auto data = readSource(from, item);
        if (!data) return add(std::move(item));
        auto cur = readDest(to);
        if (cur && *cur == *data) {
            item.action = MigrationAction::Unchanged;
            item.detail = "already the same";
        } else if (cur && opt.mode == MigrationMode::Merge) {
            item.action = MigrationAction::Skip;
            item.detail = "Fledge Advanced already has its own; kept (Overwrite replaces it, with a backup)";
        } else {
            item.action = cur ? MigrationAction::Overwrite : MigrationAction::Copy;
            item.detail = cur ? "replaced (backup kept)" : "copied";
            commit(rel, to, *data, cur.has_value(), item);
        }
        add(std::move(item));
    }

    void jsonFile(const std::string& rel, const char* what, bool settings) {
        MigrationItem item{rel, what};
        fs::path from = src / pathFromUtf8(rel), to = dst / pathFromUtf8(rel);
        auto data = readSource(from, item);
        if (!data) return add(std::move(item));
        Json source = Json::parse(*data, nullptr, false);
        if (source.is_discarded() || !source.is_object()) {
            item.action = MigrationAction::Skip;
            item.detail = "the Fledge file is not a valid settings JSON object; nothing copied from it";
            r.notes.push_back(rel + " in the Fledge folder could not be read as JSON and was skipped");
            return add(std::move(item));
        }
        std::vector<std::string> notes;
        if (settings) filterSettings(source, item, notes);
        auto cur = readDest(to);
        Json dest = Json::object();
        bool destValid = true;
        if (cur) {
            dest = Json::parse(*cur, nullptr, false);
            if (dest.is_discarded() || !dest.is_object()) {
                destValid = false;
                dest = Json::object();
            }
        }
        if (cur && !destValid && opt.mode == MigrationMode::Merge) {
            item.action = MigrationAction::Skip;
            item.detail = "the Fledge Advanced file is not valid JSON, so nothing can be merged into it; Overwrite replaces it (with a backup)";
            return add(std::move(item));
        }
        Json merged = dest;
        if (settings) mergeSettings(merged, source, item);
        else deepMerge(merged, source, opt.mode, "", item);
        std::string detail;
        for (auto& n : notes) detail += (detail.empty() ? "" : "; ") + n;
        if (cur && destValid && merged == dest) {
            item.action = MigrationAction::Unchanged;
            item.detail = item.kept.empty() ? "nothing missing" : "nothing missing; settings that differ were kept (Overwrite lets Fledge's win)";
        } else {
            item.action = !cur ? MigrationAction::Copy : (!destValid || !item.replaced.empty()) ? MigrationAction::Overwrite : MigrationAction::Merge;
            item.detail = !cur ? "copied" : !destValid ? "the invalid Fledge Advanced file was replaced (backup kept)"
                                : item.replaced.empty() ? "missing settings added" : "settings replaced by Fledge's (backup kept)";
            commit(rel, to, merged.dump(2), cur.has_value(), item);
        }
        if (!detail.empty() && item.action != MigrationAction::Error) item.detail += "; " + detail;
        add(std::move(item));
    }

    // Edition-specific keys and (options) AI permissions / key material leave the source before merging.
    void filterSettings(Json& s, MigrationItem& item, std::vector<std::string>& notes) {
        if (!opt.includeEditionMode) {
            for (const char* k : {"simpleMode", "workspace"})
                if (eraseKey(s, "ui", k)) item.kept.push_back(std::string("ui.") + k + " (edition default)");
        }
        if (eraseKey(s, "ui", "migrationOffer")) item.kept.push_back("ui.migrationOffer (edition-specific)");
        if (eraseKey(s, "storeImport", "linkIpc")) item.kept.push_back("storeImport.linkIpc (follows each edition's link handler)");
        if (!opt.includeAiPermissions) {
            for (const char* k : {"aiControl", "mcp"})
                if (s.contains(k)) {
                    s.erase(k);
                    item.withheld.push_back(std::string(k) + " (AI permissions are not migrated by an AI)");
                }
        }
        if (!opt.includeAiKeys && s.contains("ai")) withholdAiKeys(s["ai"], item);
        if (s.contains("scripts") && s["scripts"].is_object() && s["scripts"].contains("compiler") && s["scripts"]["compiler"].is_string()) {
            fs::path c = pathFromUtf8(s["scripts"]["compiler"].get<std::string>());
            if (!c.empty() && nested(c, src)) notes.push_back("the C++ compiler path points into the Fledge folder (its installed toolchain); Fledge Advanced uses it from there");
        }
    }

    void mergeSettings(Json& d, const Json& s, MigrationItem& item) {
        for (auto it = s.begin(); it != s.end(); ++it) {
            const std::string& k = it.key();
            if (k == "recentProjects") {
                Json m = mergeRecent(d.contains(k) ? d[k] : Json::array(), it.value(), opt.mode);
                if (!d.contains(k)) item.added.push_back(k);
                else if (m != d[k] && opt.mode == MigrationMode::Merge) item.added.push_back(k + " (entries added to the list)");
                else if (m != d[k]) item.replaced.push_back(k + " (combined list, Fledge's first)");
                d[k] = m;
            } else if (k == "ai" || k == "openPanels") {  // one unit: a provider configuration / a panel set is not mixed
                if (!d.contains(k)) {
                    d[k] = it.value();
                    item.added.push_back(k);
                } else if (d[k] != it.value()) {
                    if (opt.mode == MigrationMode::Overwrite) {
                        d[k] = it.value();
                        item.replaced.push_back(k);
                    } else {
                        item.kept.push_back(k);
                    }
                }
            } else if (!d.contains(k)) {
                d[k] = it.value();
                item.added.push_back(k);
            } else if (d[k].is_object() && it.value().is_object()) {
                deepMerge(d[k], it.value(), opt.mode, k, item);
            } else if (d[k] != it.value()) {
                if (opt.mode == MigrationMode::Overwrite) {
                    d[k] = it.value();
                    item.replaced.push_back(k);
                } else {
                    item.kept.push_back(k);
                }
            }
        }
    }

    void layouts(const std::string& name) {
        fs::path dir = src / pathFromUtf8(name);
        std::error_code ec;
        std::vector<std::string> names;
        for (fs::directory_iterator it(osPath(dir), ec), end; !ec && it != end; it.increment(ec)) names.push_back(narrow(it->path().filename().wstring()));
        std::sort(names.begin(), names.end());
        if (names.empty()) return add({name, "Saved workspace layouts", MigrationAction::Unchanged, "empty"});
        for (auto& n : names) {
            std::string rel = name + "/" + n;
            fs::path p = dir / pathFromUtf8(n);
            if (isLink(p)) {
                add({rel, "Link", MigrationAction::Skip, "symbolic link / junction: skipped"});
                continue;
            }
            std::string l = toLower(n);
            bool layoutFile = fs::is_regular_file(osPath(p), ec) && (endsWith(l, ".ini") || endsWith(l, ".panels.json"));
            if (!layoutFile) {
                add({rel, "Unknown item", MigrationAction::Skip, "not a saved layout; not copied"});
                continue;
            }
            copyFile(rel, "Saved workspace layout");
        }
    }

    bool refuse(std::string why) {
        r.refused = true;
        r.errors.push_back(std::move(why));
        return false;
    }

    bool validate() {
        if (src.empty()) return refuse("no Fledge (Standard) user data folder was found to migrate from");
        if (dst.empty()) return refuse("no destination folder");
        std::error_code ec;
        if (!fs::exists(osPath(src), ec))
            return refuse("the Fledge (Standard) user data folder does not exist: " + narrow(src.wstring()) +
                          ". Nothing to migrate (start Fledge once to create its settings, or there is nothing to bring over).");
        if (!fs::is_directory(osPath(src), ec)) return refuse("the source is not a folder: " + narrow(src.wstring()));
        if (isLink(src)) return refuse("the source folder is a symbolic link / junction; refusing to follow it");
        if (samePath(src, dst)) return refuse("source and destination are the same folder; nothing to migrate");
        if (nested(src, dst)) return refuse("source and destination are inside each other; refusing");
        if (fs::exists(osPath(dst), ec)) {
            if (!fs::is_directory(osPath(dst), ec)) return refuse("the destination is not a folder: " + narrow(dst.wstring()));
            if (isLink(dst)) return refuse("the destination folder is a symbolic link / junction; refusing to write through it");
        }
        return true;
    }

    void run() {
        r.source = src;
        r.destination = dst;
        r.options = opt;
        r.applied = apply;
        if (!validate()) return;
        std::error_code ec;
        if (apply) {
            fs::create_directories(osPath(dst), ec);
            if (ec) {
                refuse("cannot create the destination folder: " + narrow(dst.wstring()));
                return;
            }
        }
        std::vector<std::pair<std::string, bool>> entries;
        for (fs::directory_iterator it(osPath(src), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            entries.push_back({narrow(it->path().filename().wstring()), it->is_directory(e2) && !isLink(it->path())});
        }
        if (ec) r.errors.push_back("cannot list the Fledge folder: " + fileErrorReason(src));
        std::sort(entries.begin(), entries.end());
        for (auto& [name, dir] : entries) {
            fs::path p = src / pathFromUtf8(name);
            if (isLink(p)) {
                add({name, "Link", MigrationAction::Skip, "symbolic link / junction: skipped"});
                continue;
            }
            Known k = classify(name, dir);
            switch (k.rule) {
                case Rule::Settings: jsonFile(name, k.what, true); break;
                case Rule::JsonMerge: jsonFile(name, k.what, false); break;
                case Rule::File: copyFile(name, k.what); break;
                case Rule::LayoutsDir: layouts(name); break;
                case Rule::Exclude: add({name, k.what, MigrationAction::Exclude, k.why}); break;
                case Rule::Unknown: add({name, k.what, MigrationAction::Skip, k.why}); break;
            }
        }
        for (auto& i : r.items)
            if (i.action == MigrationAction::Error) r.errors.push_back(i.path + ": " + i.detail);
        if (apply)
            FLEDGE_LOG_INFO("editor", "settings migration from {} to {} ({}): {}", narrow(src.wstring()), narrow(dst.wstring()), toString(opt.mode),
                            r.summary());
    }
};

MigrationResult migrate(const fs::path& src, const fs::path& dst, const MigrationOptions& opt, bool apply) {
    Runner run{src, dst, opt, apply};
    run.run();
    return std::move(run.r);
}

} // namespace

MigrationPlan planSettingsMigration(const fs::path& source, const fs::path& destination, const MigrationOptions& options) {
    return migrate(source, destination, options, false);
}

MigrationResult runSettingsMigration(const MigrationPlan& plan, const MigrationOptions& options) {
    return migrate(plan.source, plan.destination, options, true);
}

MigrationResult runSettingsMigration(const fs::path& source, const fs::path& destination, const MigrationOptions& options) {
    return migrate(source, destination, options, true);
}

MigrationEndpoints resolveMigrationEndpoints(const EnginePaths& paths, const fs::path& customSettingsDir, const fs::path& appDataDir,
                                             const fs::path& standardOverride, bool userDataOverridden) {
    MigrationEndpoints e;
    e.destination = customSettingsDir.empty() ? paths.userData : customSettingsDir;
    e.sandboxed = paths.buildTree || userDataOverridden || !customSettingsDir.empty();
    if (!standardOverride.empty()) {
        e.source = standardOverride;
        e.note = "Fledge folder from FLEDGE_STANDARD_USER_DATA";
    } else if (e.sandboxed) {
        e.note = "this editor keeps its settings in a test / development folder (build tree, FLEDGE_USER_DATA or --settings-dir), so the real "
                 "%APPDATA%\\Fledge is not read; set FLEDGE_STANDARD_USER_DATA to a test copy to try the migration";
    } else if (appDataDir.empty()) {
        e.note = "%APPDATA% is not set, so the Fledge folder cannot be found";
    } else {
        e.source = appDataDir / pathFromUtf8(editionUserDataFolder(Edition::Standard));
        e.note = "Fledge's settings folder under %APPDATA%";
    }
    return e;
}

MigrationEndpoints migrationEndpointsForThisProcess(const fs::path& customSettingsDir) {
    auto env = [](const char* name) -> fs::path {
#ifdef _WIN32
        if (const wchar_t* v = _wgetenv(widen(name).c_str()); v && *v) return fs::path(v);
#else
        if (const char* v = std::getenv(name); v && *v) return fs::path(v);
#endif
        return {};
    };
    const EnginePaths& p = enginePaths();
    // FLEDGE_USER_DATA is ignored by portable installs (paths.h), so it only counts as an override when it is in effect.
    bool overridden = !env("FLEDGE_USER_DATA").empty() && !p.portable;
    return resolveMigrationEndpoints(p, customSettingsDir, platformAppDataDir(), env("FLEDGE_STANDARD_USER_DATA"), overridden);
}

Status checkMigrationEndpoints(const MigrationPlan& plan, const MigrationEndpoints& e) {
    if (e.source.empty()) return Status::failure(e.note.empty() ? "no Fledge folder to migrate from" : e.note);
    if (!samePath(plan.source, e.source) || !samePath(plan.destination, e.destination))
        return Status::failure("the migration only runs between this install's Fledge and Fledge Advanced user data folders");
    return {};
}

bool migrationOfferApplies(const MigrationEndpoints& e) {
    if (e.source.empty() || samePath(e.source, e.destination)) return false;
    std::error_code ec;
    return fs::is_regular_file(osPath(e.source / "editor_settings.json"), ec) && !fs::exists(osPath(e.destination / "editor_settings.json"), ec);
}

} // namespace fledge::editor
