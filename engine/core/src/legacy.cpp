#include "fledge/core/legacy.h"

#include <array>
#include <utility>

namespace fledge::legacy {

namespace {
// Pre-0.8 extension, current extension.
constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kExtensions{{
    {".cageproject", ".fledge"},
    {".cagescene", ".fscene"},
    {".cagemat", ".fmat"},
    {".cagemodel", ".fmodel"},
    {".cagepak", ".fpak"},
}};
// Current format tag, pre-0.8 tag.
constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kFormats{{
    {"fledgeproject", "cageproject"},
    {"fscene", "cagescene"},
    {"fmat", "cagemat"},
    {"fmodel", "cagemodel"},
    {"fledgemeta", "cagemeta"},
    {"fledgeassets", "cageassets"},
    {"fledgetemplate", "cagetemplate"},
}};
// Text rewrites, most specific first.
constexpr std::array<std::pair<std::string_view, std::string_view>, 17> kText{{
    {".cageproject", ".fledge"},
    {"cageproject", "fledgeproject"},
    {".cagescene", ".fscene"},
    {"cagescene", "fscene"},
    {".cagemat", ".fmat"},
    {"cagemat", "fmat"},
    {".cagemodel", ".fmodel"},
    {"cagemodel", "fmodel"},
    {".cagepak", ".fpak"},
    {"cagemeta", "fledgemeta"},
    {"cageassets", "fledgeassets"},
    {"cagetemplate", "fledgetemplate"},
    {"<cage/", "<fledge/"},
    {"\"cage/", "\"fledge/"},
    {"cage::", "fledge::"},
    {"namespace cage", "namespace fledge"},
    {"CAGE_", "FLEDGE_"},
}};

void replaceAll(std::string& s, std::string_view a, std::string_view b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
}
bool isText(const fs::path& p) {
    std::string e = toLower(narrow(p.extension().wstring()));
    for (const char* t : {".json", ".meta", ".cpp", ".h", ".hpp", ".txt", ".md", ".cageproject", ".cagescene", ".cagemat", ".cagemodel", ".fledge",
                          ".fscene", ".fmat", ".fmodel"})
        if (e == t) return true;
    return false;
}
} // namespace

std::string_view newExtension(std::string_view oldExtension) {
    std::string e = toLower(std::string(oldExtension));
    for (auto& [o, n] : kExtensions)
        if (e == o) return n;
    return {};
}

bool isLegacyProjectFile(const fs::path& file) { return toLower(narrow(file.extension().wstring())) == kExtensions[0].first; }

std::string_view legacyFormat(std::string_view currentTag) {
    for (auto& [n, o] : kFormats)
        if (n == currentTag) return o;
    return {};
}

bool formatIs(const Json& j, std::string_view tag) {
    if (!j.is_object() || !j.contains("format") || !j["format"].is_string()) return false;
    std::string f = j["format"].get<std::string>();
    std::string_view old = legacyFormat(tag);
    return f == tag || (!old.empty() && f == old);
}

std::string migrateText(std::string text) {
    for (auto& [a, b] : kText) replaceAll(text, a, b);
    return text;
}

Result<MigrationResult> migrateProject(const fs::path& legacyProjectFile) {
    std::error_code ec;
    if (!isLegacyProjectFile(legacyProjectFile) || !fs::is_regular_file(legacyProjectFile, ec))
        return Result<MigrationResult>::failure("not a pre-0.8 project file: " + narrow(legacyProjectFile.wstring()));
    fs::path src = fs::absolute(legacyProjectFile, ec).parent_path();
    fs::path dst = src.parent_path() / (src.filename().wstring() + L"-Fledge");
    for (int n = 2; fs::exists(dst, ec); ++n) dst = src.parent_path() / (src.filename().wstring() + L"-Fledge-" + std::to_wstring(n));
    fs::create_directories(dst, ec);
    if (ec) return Result<MigrationResult>::failure("cannot create " + narrow(dst.wstring()) + ": " + ec.message());
    MigrationResult res;
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        fs::path rel = fs::relative(it->path(), src, ec);
        std::string first = narrow(rel.begin()->wstring());
        if (first == "Intermediate" || first == "Build") {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        // rename every path component with a legacy extension (".x.cagemat.meta" -> ".x.fmat.meta")
        std::string relText = narrow(rel.generic_wstring());
        std::string newRel = relText;
        for (auto& [o, n] : kExtensions) replaceAll(newRel, o, n);
        fs::path out = dst / pathFromUtf8(newRel);
        if (it->is_directory()) {
            fs::create_directories(out, ec);
            continue;
        }
        fs::create_directories(out.parent_path(), ec);
        if (newRel != relText) ++res.filesRenamed;
        if (isText(it->path())) {
            auto text = readFileText(it->path());
            if (!text) return Result<MigrationResult>::failure("cannot read " + narrow(it->path().wstring()));
            std::string conv = migrateText(*text);
            if (conv != *text) ++res.filesRewritten;
            if (!writeFileBytes(out, Bytes(conv.begin(), conv.end()))) return Result<MigrationResult>::failure("cannot write " + narrow(out.wstring()));
        } else {
            fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
            if (ec) return Result<MigrationResult>::failure("cannot copy " + narrow(it->path().wstring()) + ": " + ec.message());
        }
    }
    fs::path proj = legacyProjectFile.filename();
    proj.replace_extension(".fledge");
    res.projectFile = dst / proj;
    if (!fs::is_regular_file(res.projectFile, ec)) return Result<MigrationResult>::failure("migration produced no project file");
    return res;
}

} // namespace fledge::legacy
