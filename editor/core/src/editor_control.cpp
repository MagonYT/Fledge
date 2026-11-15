// Full MCP Control policy, limits and the audit log (editor_control.h, docs/MCP.md "Full MCP Control").
#include "fledge/editor/editor_control.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <format>

#include "fledge/ai/llm.h"
#include "fledge/core/redact.h"

namespace fledge::editor {

EditorCommandPolicy editorCommandPolicy(const std::string& id) {
    auto starts = [&](const char* p) { return id.rfind(p, 0) == 0; };
    // AI permission switches belong to the user, whatever the tier.
    if (starts("ai.level.") || id == "ai.toggle" || id == "mcp.toggle")
        return {CommandGate::Blocked, "changes the AI's own permissions or the MCP server; only the user can do that"};
    // Native OS dialogs block the editor's main thread until a person answers.
    if (id == "store.import_file") return {CommandGate::Blocked, "opens an OS file dialog; use import_asset_file {path} instead"};
    // Builds run the C++ compiler and load native code; packaging runs fledgepack and the compiler.
    if (id == "scripts.build" || id == "scripts.rebuild" || id == "scripts.reload")
        return {CommandGate::NeedsConfirm, "compiles / loads native code (external program)", AiCategory::ScriptBuild};
    if (id == "file.package" || id == "file.cook" || id == "file.cook_force")
        return {CommandGate::NeedsConfirm, "runs the packager / cooker over the whole project", AiCategory::Packaging};
    static const std::vector<std::pair<const char*, const char*>> confirm = {
        {"edit.delete", "deletes the selected entities"},
        {"file.exit", "closes the editor (and its MCP server)"},
        {"file.revert_autosave", "replaces the scene with the previous auto-save backup"},
        {"store.restore_handlers", "changes the system's link handlers (registry)"},
        {"scripts.open_ide", "starts an external program"},
        {"code.new_terminal", "starts a shell (external program)"},
        {"graphics.preview", "starts the game as a separate program"},
        {"graphics.path_trace", "starts the game as a separate program"},
        {"help.docs", "starts Explorer (external program)"},
        {"help.licenses", "starts Explorer (external program)"},
        {"help.userdata", "starts Explorer (external program)"},
        {"help.script", "opens a document in an external program"},
        {"file.reveal_project", "starts Explorer (external program)"},
    };
    for (auto& [c, why] : confirm)
        if (id == c) return {CommandGate::NeedsConfirm, why};
    return {};
}

const std::vector<std::string>& editorPreferenceKeys() {
    static const std::vector<std::string> k = {"theme",     "uiScale",  "fontSize",        "cameraSpeed", "mouseSensitivity",
                                               "invertY",   "lockMouseInPlay", "showGrid",  "showStats",   "showOrientation",
                                               "showWelcome", "restoreLastProject"};
    return k;
}

Status validateEditorPreference(const std::string& key, const Json& v) {
    auto keys = editorPreferenceKeys();
    if (std::find(keys.begin(), keys.end(), key) == keys.end())
        return Status::failure("'" + key + "' is not an editor preference an AI may change (AI, MCP and path settings are the user's)");
    auto range = [&](double lo, double hi, bool zeroOk = false) -> Status {
        if (!v.is_number()) return Status::failure(key + " must be a number");
        double d = v.get<double>();
        if ((zeroOk && d == 0) || (d >= lo && d <= hi)) return {};
        return Status::failure(std::format("{} must be between {} and {}{}", key, lo, hi, zeroOk ? " (or 0 = follow the monitor)" : ""));
    };
    if (key == "theme") {
        if (v.is_string() && (v == "dark" || v == "light")) return {};
        return Status::failure("theme must be 'dark' or 'light'");
    }
    if (key == "uiScale") return range(0.75, 2.5, true);
    if (key == "fontSize") return range(12, 20);
    if (key == "cameraSpeed") return range(0.5, 50);
    if (key == "mouseSensitivity") return range(0.2, 3);
    if (!v.is_boolean()) return Status::failure(key + " must be true or false");
    return {};
}

Status checkProjectPath(const fs::path& file, const std::vector<fs::path>& roots) {
    if (file.empty() || !file.is_absolute()) return Status::failure("path must be an absolute path to a .fledge file");
    if (toLower(narrow(file.extension().wstring())) != ".fledge") return Status::failure("path must be a .fledge project file");
    auto norm = [](const fs::path& p) {
        std::wstring s = p.lexically_normal().wstring();
        std::replace(s.begin(), s.end(), L'\\', L'/');
        while (s.size() > 1 && s.back() == L'/') s.pop_back();
#ifdef _WIN32
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
#endif
        return s;
    };
    std::wstring f = norm(file);
    if (f.find(L"/../") != std::wstring::npos) return Status::failure("path must not contain '..'");
    for (auto& r : roots)
        if (!r.empty() && f.starts_with(norm(r) + L"/")) {
            std::error_code ec;
            if (!fs::is_regular_file(osPath(file), ec)) return Status::failure("no such project file");
            return {};
        }
    return Status::failure("open_project only opens projects inside the projects folder or a recent project's folder");
}

bool RateLimiter::allow(double now) {
    std::lock_guard l(m_mutex);
    while (!m_times.empty() && now - m_times.front() >= m_seconds) m_times.pop_front();
    if (m_times.size() >= m_calls) return false;
    m_times.push_back(now);
    return true;
}

namespace {
Json redactValue(const Json& v, int depth) {
    if (depth > 8) return "[...]";
    if (v.is_object()) {
        Json o = Json::object();
        for (auto it = v.begin(); it != v.end(); ++it)
            o[it.key()] = ai::looksLikeSecretName(it.key()) ? Json(Redactor::kMask) : redactValue(it.value(), depth + 1);
        return o;
    }
    if (v.is_array()) {
        Json a = Json::array();
        for (auto& x : v) a.push_back(redactValue(x, depth + 1));
        return a;
    }
    if (v.is_string()) {
        std::string s = redact(v.get<std::string>());
        if (s.size() > 512) s = s.substr(0, 512) + "...";
        return s;
    }
    return v;
}

std::string utcStamp(const char* fmt) {
    std::time_t t = std::time(nullptr);
    const std::tm* tm = std::gmtime(&t);
    char buf[40] = "";
    if (tm) std::strftime(buf, sizeof buf, fmt, tm);
    return buf;
}
} // namespace

Json McpAuditLog::redactArgs(const Json& args) {
    Json r = redactValue(args, 0);
    std::string d = r.dump();
    if (d.size() > 4096) return Json({{"truncated", true}, {"bytes", d.size()}});
    return r;
}

fs::path McpAuditLog::currentFile() const { return m_dir / pathFromUtf8("mcp-audit-" + utcStamp("%Y-%m-%d") + ".jsonl"); }

void McpAuditLog::record(const std::string& caller, const std::string& tool, const std::string& category, const Json& args, bool ok,
                         const std::string& error) {
    Json line = {{"time", utcStamp("%Y-%m-%dT%H:%M:%SZ")}, {"caller", caller}, {"tool", tool}, {"category", category},
                 {"args", redactArgs(args)}, {"ok", ok}};
    if (!error.empty()) line["error"] = redact(error.size() > 300 ? error.substr(0, 300) + "..." : error);
    std::string text = line.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
    std::lock_guard l(m_mutex);
    std::error_code ec;
    fs::create_directories(osPath(m_dir), ec);
    fs::path f = currentFile();
    if (fs::exists(osPath(f), ec) && fs::file_size(osPath(f), ec) > kMaxFileBytes) return;  // capped; the next day starts a new file
    FILE* out = nullptr;
#ifdef _WIN32
    out = _wfopen(osPath(f).wstring().c_str(), L"ab");
#else
    out = std::fopen(f.string().c_str(), "ab");
#endif
    if (!out) return;
    std::fwrite(text.data(), 1, text.size(), out);
    std::fclose(out);
}

} // namespace fledge::editor
