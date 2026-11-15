// Conversion report: JSON (schema fledge.blueprint-report/1) and Markdown.
#include <format>

#include "fledge/blueprint/blueprint.h"

namespace fledge::blueprint {

int Report::count(Severity s) const {
    int n = 0;
    for (auto& e : nodes)
        if (e.severity == s) ++n;
    return n;
}

std::string Report::summary() const {
    if (!error.empty()) return "Blueprint conversion failed: " + error;
    return std::format("{}: {} nodes - {} converted, {} approximated, {} stubbed, {} ignored; {} unrecognised constructs", className, nodes.size(),
                       count(Severity::Converted), count(Severity::Approximated), count(Severity::Stubbed), count(Severity::Ignored),
                       unrecognised.size());
}

namespace {
// Report strings come from the input: invalid UTF-8 would make Json::dump throw, so it is replaced with '?'.
std::string validUtf8(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) >> 6) == 2;
        if (ok && n == 2 && c < 0xC2) ok = false;
        if (ok && n == 3) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;
        }
        if (ok && n == 4) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if (c > 0xF4 || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false;
        }
        if (!ok) {
            o += '?';
            ++i;
            continue;
        }
        o.append(s, i, n);
        i += n;
    }
    return o;
}
void fixStrings(Json& j) {
    if (j.is_string()) j = validUtf8(j.get_ref<const std::string&>());
    else if (j.is_array() || j.is_object())
        for (auto& v : j) fixStrings(v);
}
} // namespace

Json Report::toJson() const {
    Json j = toJsonRaw();
    fixStrings(j);
    return j;
}

Json Report::toJsonRaw() const {
    Json j;
    j["schema"] = "fledge.blueprint-report/1";
    j["fledgeVersion"] = fledgeVersion;
    j["ok"] = error.empty();
    if (!error.empty()) j["error"] = error;
    j["source"] = {{"name", sourceName}, {"kind", sourceKind}, {"bytes", sourceBytes}};
    j["className"] = className;
    j["files"] = files;
    j["counts"] = {{"nodes", nodes.size()},
                   {"converted", count(Severity::Converted)},
                   {"approximated", count(Severity::Approximated)},
                   {"stubbed", count(Severity::Stubbed)},
                   {"ignored", count(Severity::Ignored)},
                   {"unrecognised", unrecognised.size()}};
    Json ns = Json::array();
    for (auto& n : nodes)
        ns.push_back({{"node", n.node}, {"class", n.cls}, {"title", n.title}, {"graph", n.graph}, {"severity", toString(n.severity)}, {"detail", n.detail}, {"line", n.line}});
    j["nodes"] = ns;
    Json un = Json::array();
    for (auto& u : unrecognised) un.push_back({{"construct", u.construct}, {"name", u.name}, {"where", u.where}, {"detail", u.detail}, {"line", u.line}});
    j["unrecognised"] = un;
    Json vs = Json::array();
    for (auto& v : variables)
        vs.push_back({{"name", v.name}, {"cppName", v.cppName}, {"unrealType", v.unrealType}, {"cppType", v.cppType}, {"default", v.defaultValue},
                      {"property", v.property}, {"declared", v.declared}});
    j["variables"] = vs;
    Json nm = Json::array();
    for (auto& n : names) nm.push_back({{"unreal", n.unreal}, {"cpp", n.cpp}, {"kind", n.kind}});
    j["nameMap"] = nm;
    j["warnings"] = warnings;
    j["licence"] = kLicenceNotice;
    if (!uasset.is_null()) j["uasset"] = uasset;
    j["notice"] = "Tested on hand-written samples modelled on the Unreal T3D format; please report mismatches with real exports "
                  "(attach this report: it lists every unrecognised construct).";
    return j;
}

namespace {
std::string md(std::string_view s) {  // one table cell: no pipes / newlines / HTML
    std::string o;
    for (char c : s) {
        if (c == '|') o += "\\|";
        else if (c == '\n' || c == '\r') o += ' ';
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '`') o += '\'';
        else o += c;
    }
    return o.substr(0, 300);
}
} // namespace

std::string Report::toMarkdown() const {
    std::string s = "# Blueprint conversion report: " + md(className) + "\n\n";
    s += "> " + std::string(kLicenceNotice) + "\n\n";
    s += "Source: " + md(sourceName) + " (" + md(sourceKind) + ", " + std::to_string(sourceBytes) + " bytes)";
    if (!fledgeVersion.empty()) s += ", Fledge " + md(fledgeVersion);
    s += "\n\n";
    if (!error.empty()) s += "**Not converted:** " + md(error) + "\n\n";
    if (!files.empty()) {
        s += "Generated files:";
        for (auto& f : files) s += " `" + md(f) + "`";
        s += "\n\n";
    }
    s += std::format("| Nodes | Converted | Approximated | Stubbed | Ignored | Unrecognised |\n|---|---|---|---|---|---|\n| {} | {} | {} | {} | {} | {} |\n\n",
                     nodes.size(), count(Severity::Converted), count(Severity::Approximated), count(Severity::Stubbed), count(Severity::Ignored),
                     unrecognised.size());
    s += "Converted = same behaviour; approximated = works, with the difference noted; stubbed = a `TODO(blueprint)` placeholder that compiles "
         "but does nothing (or returns a default value); ignored = comments, reroutes and nodes no event reaches.\n\n";
    if (!warnings.empty()) {
        s += "## Warnings\n\n";
        for (auto& w : warnings) s += "- " + md(w) + "\n";
        s += "\n";
    }
    if (!unrecognised.empty()) {
        s += "## Unrecognised constructs\n\nPlease report these if they come from a real Unreal export.\n\n| Construct | Name | Where | Detail | Line |\n|---|---|---|---|---|\n";
        for (auto& u : unrecognised) s += "| " + md(u.construct) + " | " + md(u.name) + " | " + md(u.where) + " | " + md(u.detail) + " | " + std::to_string(u.line) + " |\n";
        s += "\n";
    }
    if (!variables.empty()) {
        s += "## Variables\n\n| Unreal | C++ | Unreal type | C++ type | Default | Inspector |\n|---|---|---|---|---|---|\n";
        for (auto& v : variables)
            s += "| " + md(v.name) + " | `" + md(v.cppName) + "` | " + md(v.unrealType) + " | `" + md(v.cppType) + "` | " + md(v.defaultValue) + " | " +
                 (v.property ? "yes" : "no") + " |\n";
        s += "\n";
    }
    if (!names.empty()) {
        s += "## Name map\n\n| Unreal | C++ | Kind |\n|---|---|---|\n";
        for (auto& n : names) s += "| " + md(n.unreal) + " | `" + md(n.cpp) + "` | " + md(n.kind) + " |\n";
        s += "\n";
    }
    if (!nodes.empty()) {
        s += "## Nodes\n\n| Node | Class | Severity | Detail | Line |\n|---|---|---|---|---|\n";
        for (auto& n : nodes)
            s += "| " + md(n.title) + " | " + md(n.cls) + " | " + toString(n.severity) + " | " + md(n.detail) + " | " + std::to_string(n.line) + " |\n";
        s += "\n";
    }
    if (!uasset.is_null()) s += "## .uasset probe\n\n```json\n" + uasset.dump(2, ' ', false, Json::error_handler_t::replace) +"\n```\n\n";
    s += "_Tested on hand-written samples modelled on the Unreal T3D clipboard format. If a real export converts wrongly, report it with this "
         "report attached._\n";
    return s;
}

} // namespace fledge::blueprint
