// Writing a conversion into a project: unique class / file names, never overwriting anything.
#include <set>

#include "fledge/blueprint/blueprint.h"

namespace fledge::blueprint {

namespace {
// Class names registered by the project's existing scripts (FLEDGE_BEHAVIOUR / FLEDGE_SERVICE / PERCH_*), bounded scan.
std::set<std::string> registeredClasses(const fs::path& scriptsDir) {
    std::set<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(scriptsDir, ec)) return out;
    size_t files = 0;
    for (auto it = fs::recursive_directory_iterator(scriptsDir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator() && files < 5000; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = toLower(narrow(it->path().extension().wstring()));
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inl") continue;
        if (it->file_size(ec) > 2u * 1024 * 1024) continue;
        ++files;
        auto text = readFileText(it->path());
        if (!text) continue;
        for (const char* macro : {"FLEDGE_BEHAVIOUR(", "FLEDGE_SERVICE(", "PERCH_BEHAVIOUR(", "PERCH_SERVICE("}) {
            std::string m = macro;
            for (size_t p = text->find(m); p != std::string::npos; p = text->find(m, p + m.size())) {
                size_t e = text->find(')', p);
                if (e == std::string::npos) break;
                std::string name = text->substr(p + m.size(), e - p - m.size());
                while (!name.empty() && name.front() == ' ') name.erase(name.begin());
                while (!name.empty() && name.back() == ' ') name.pop_back();
                if (!name.empty() && name.size() < 128) out.insert(name);
            }
        }
    }
    return out;
}
fs::path freeReportPath(const fs::path& dir, const std::string& base, const char* ext) {
    std::error_code ec;
    for (int i = 1; i < 1000; ++i) {
        fs::path p = dir / pathFromUtf8(base + (i == 1 ? "" : "_" + std::to_string(i)) + ext);
        if (!fs::exists(p, ec)) return p;
    }
    return dir / pathFromUtf8(base + "_overflow" + ext);
}
} // namespace

std::string uniqueClassName(const fs::path& scriptsDir, const std::string& folder, const std::string& wanted) {
    std::string base = sanitizeIdentifier(wanted, "ConvertedBlueprint");
    std::set<std::string> taken = registeredClasses(scriptsDir);
    fs::path dir = isSafeSubfolder(folder) ? scriptsDir / pathFromUtf8(folder) : scriptsDir;
    std::error_code ec;
    for (int i = 1; i < 10000; ++i) {
        std::string n = i == 1 ? base : base + "_" + std::to_string(i);
        if (taken.contains(n)) continue;
        if (fs::exists(dir / pathFromUtf8(n + ".cpp"), ec) || fs::exists(dir / pathFromUtf8(n + ".h"), ec)) continue;
        if (fs::exists(scriptsDir / pathFromUtf8(n + ".cpp"), ec)) continue;
        return n;
    }
    return base + "_converted";
}

Conversion renamed(const Conversion& c, std::string_view text, const Options& options, const std::string& className) {
    if (c.className == className) return c;
    Options o = options;
    o.className = className;
    return convertText(text, o);
}

Result<WrittenFiles> writeConversion(const Conversion& c, const fs::path& scriptsDir, const std::string& folder, const fs::path& reportDir) {
    if (!c.ok) return Result<WrittenFiles>::failure(c.report.error.empty() ? "nothing to write" : c.report.error);
    if (!isSafeSubfolder(folder)) return Result<WrittenFiles>::failure("target folder must be a relative folder inside Scripts/ (no '..', drive or special names)");
    WrittenFiles w;
    w.className = c.className;
    fs::path dir = scriptsDir / pathFromUtf8(folder);
    std::error_code ec;
    fs::create_directories(osPath(dir), ec);
    w.cpp = dir / pathFromUtf8(c.cppFile);
    if (!c.headerFile.empty()) w.header = dir / pathFromUtf8(c.headerFile);
    if (fs::exists(w.cpp, ec) || (!w.header.empty() && fs::exists(w.header, ec)))
        return Result<WrittenFiles>::failure("Scripts/" + (folder.empty() ? "" : folder + "/") + c.cppFile + " already exists (existing files are never overwritten)");
    if (!writeFileText(w.cpp, c.cpp)) return Result<WrittenFiles>::failure("could not write " + narrow(w.cpp.wstring()));
    if (!w.header.empty() && !writeFileText(w.header, c.header)) {
        fs::remove(osPath(w.cpp), ec);
        return Result<WrittenFiles>::failure("could not write " + narrow(w.header.wstring()));
    }
    if (!reportDir.empty()) {
        fs::create_directories(osPath(reportDir), ec);
        w.reportMarkdown = freeReportPath(reportDir, c.className + ".blueprint-report", ".md");
        w.reportJson = freeReportPath(reportDir, c.className + ".blueprint-report", ".json");
        if (!writeFileText(w.reportMarkdown, c.report.toMarkdown())) w.notes.push_back("could not write the Markdown report");
        if (!writeFileText(w.reportJson, c.report.toJson().dump(2))) w.notes.push_back("could not write the JSON report");
    }
    return w;
}

} // namespace fledge::blueprint
