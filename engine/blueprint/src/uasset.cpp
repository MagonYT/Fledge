// .uasset / .umap probe (best effort, read-only): package tag, versions, flags and the name table of UNCOOKED packages.
// The export table and the serialized graph objects are NOT decoded; cooked / compressed packages are out of scope.
#include <cstring>
#include <set>

#include "fledge/blueprint/blueprint.h"

namespace fledge::blueprint {

namespace {
constexpr uint32_t kPackageTag = 0x9E2A83C1u;
constexpr uint32_t kFilterEditorOnly = 0x80000000u;

struct Reader {
    std::string_view b;
    size_t pos = 0;
    bool ok = true;
    int32_t i32() {
        if (pos + 4 > b.size()) {
            ok = false;
            return 0;
        }
        uint32_t v = static_cast<uint8_t>(b[pos]) | (static_cast<uint32_t>(static_cast<uint8_t>(b[pos + 1])) << 8) |
                     (static_cast<uint32_t>(static_cast<uint8_t>(b[pos + 2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(b[pos + 3])) << 24);
        pos += 4;
        return static_cast<int32_t>(v);
    }
    void skip(size_t n) {
        if (pos + n > b.size()) ok = false;
        else pos += n;
    }
    std::string fstring() {
        int32_t len = i32();
        if (!ok || len == 0) return {};
        if (len > 1024 || len < -1024) {
            ok = false;
            return {};
        }
        std::string s;
        if (len > 0) {
            if (pos + static_cast<size_t>(len) > b.size()) {
                ok = false;
                return {};
            }
            s.assign(b.data() + pos, static_cast<size_t>(len));
            pos += static_cast<size_t>(len);
        } else {
            size_t n = static_cast<size_t>(-len);
            if (pos + n * 2 > b.size()) {
                ok = false;
                return {};
            }
            for (size_t i = 0; i < n; ++i) {
                unsigned c = static_cast<uint8_t>(b[pos + i * 2]) | (static_cast<unsigned>(static_cast<uint8_t>(b[pos + i * 2 + 1])) << 8);
                s += c < 0x80 ? static_cast<char>(c) : '?';
            }
            pos += n * 2;
        }
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
};

bool plausibleName(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c >= 0x7f) return false;
    return true;
}
} // namespace

Json UassetProbe::toJson() const {
    Json j = {{"isPackage", isPackage}, {"cooked", cooked}, {"unversioned", unversioned}, {"headerParsed", headerParsed},
              {"legacyFileVersion", legacyVersion}, {"fileVersionUE4", ue4Version}, {"fileVersionUE5", ue5Version},
              {"licenseeVersion", licenseeVersion}, {"packageFlags", packageFlags}, {"nameCount", nameCount}, {"method", method},
              {"blueprintNodeClasses", blueprintNodes}, {"mappedFunctionsOrEvents", functionsFound}, {"message", message}};
    Json n = Json::array();
    for (size_t i = 0; i < names.size() && i < 400; ++i) n.push_back(names[i]);
    j["names"] = n;
    j["exportTable"] = "not decoded";
    return j;
}

UassetProbe probeUasset(std::string_view bytes, const MappingTable& table) {
    UassetProbe p;
    Reader r{bytes};
    uint32_t tag = static_cast<uint32_t>(r.i32());
    p.isPackage = r.ok && tag == kPackageTag;
    if (!p.isPackage) {
        p.method = "none";
        p.message = "not an Unreal package (no package file tag). Paste T3D text instead: select the nodes in the Unreal Blueprint editor, Ctrl+C.";
        return p;
    }
    p.legacyVersion = r.i32();
    bool known = p.legacyVersion < 0 && p.legacyVersion >= -9;
    if (known) {
        if (p.legacyVersion != -4) r.i32();  // LegacyUE3Version
        p.ue4Version = r.i32();
        if (p.legacyVersion <= -8) p.ue5Version = r.i32();
        p.licenseeVersion = r.i32();
        p.unversioned = p.ue4Version == 0 && p.ue5Version == 0 && p.licenseeVersion == 0;
        if (p.legacyVersion <= -2) {
            int32_t n = r.i32();
            if (n < 0 || n > 10000) r.ok = false;
            for (int32_t i = 0; r.ok && i < n; ++i) {
                if (p.legacyVersion < -5) r.skip(20);  // FGuid + int32
                else if (p.legacyVersion < -2) {
                    r.skip(20);
                    (void)r.fstring();
                } else r.skip(8);
            }
        }
        if (p.ue5Version >= 1016) r.skip(20);  // PACKAGE_SAVED_HASH
        int32_t totalHeader = r.i32();
        (void)r.fstring();  // folder / package name
        p.packageFlags = static_cast<uint32_t>(r.i32());
        p.nameCount = r.i32();
        int32_t nameOffset = r.i32();
        p.cooked = (p.packageFlags & kFilterEditorOnly) != 0 || p.unversioned;
        if (r.ok && p.nameCount > 0 && p.nameCount < 1000000 && nameOffset > 0 && static_cast<size_t>(nameOffset) < bytes.size() &&
            (totalHeader <= 0 || nameOffset < totalHeader || static_cast<size_t>(totalHeader) > bytes.size())) {
            Reader nr{bytes, static_cast<size_t>(nameOffset)};
            std::vector<std::string> names;
            for (int32_t i = 0; nr.ok && i < p.nameCount && i < 100000; ++i) {
                std::string s = nr.fstring();
                if (!nr.ok || !plausibleName(s)) {
                    nr.ok = false;
                    break;
                }
                names.push_back(std::move(s));
                if (p.ue4Version >= 504 || p.ue5Version > 0) nr.skip(4);  // name hashes
            }
            if (nr.ok && !names.empty()) {
                p.names = std::move(names);
                p.headerParsed = true;
                p.method = "name table";
            }
        }
    }
    if (!p.headerParsed) {  // fall back: printable identifier runs anywhere in the file
        p.method = "string scan";
        std::set<std::string> seen;
        std::string cur;
        for (size_t i = 0; i <= bytes.size() && p.names.size() < 5000; ++i) {
            char c = i < bytes.size() ? bytes[i] : '\0';
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '/' || c == '.') {
                if (cur.size() < 128) cur += c;
            } else {
                if (cur.size() >= 4 && seen.insert(cur).second) p.names.push_back(cur);
                cur.clear();
            }
        }
    }
    std::set<std::string> nodes, fns;
    for (auto& n : p.names) {
        if (startsWith(n, "K2Node_") && nodes.size() < 200) nodes.insert(n);
        else if ((table.function(n) || table.event(n)) && fns.size() < 200) fns.insert(n);
    }
    p.blueprintNodes.assign(nodes.begin(), nodes.end());
    p.functionsFound.assign(fns.begin(), fns.end());
    if (p.cooked)
        p.message = "cooked package: not supported (cooking strips the editor-only Blueprint graph). Convert from the project's uncooked "
                    "Blueprint by copying its nodes as text in the Unreal editor.";
    else if (!known)
        p.message = "unknown package version: only an inventory of strings was read. Graph conversion from .uasset is not supported: "
                    "copy the nodes as text in the Unreal editor (Ctrl+C) and paste them.";
    else
        p.message = std::string("uncooked package: ") + (p.headerParsed ? "the header and name table were read" : "the header could not be fully read") +
                    "; the export table and the graph objects are not decoded, so no code was generated. Open the Blueprint in Unreal, "
                    "select its nodes (Ctrl+A), copy (Ctrl+C) and paste the text into the converter.";
    return p;
}

namespace {
std::string utf16ToUtf8(std::string_view b, bool bigEndian) {
    std::string o;
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        unsigned c = bigEndian ? (static_cast<unsigned>(static_cast<uint8_t>(b[i])) << 8) | static_cast<uint8_t>(b[i + 1])
                               : static_cast<uint8_t>(b[i]) | (static_cast<unsigned>(static_cast<uint8_t>(b[i + 1])) << 8);
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < b.size()) {
            unsigned d = bigEndian ? (static_cast<unsigned>(static_cast<uint8_t>(b[i + 2])) << 8) | static_cast<uint8_t>(b[i + 3])
                                   : static_cast<uint8_t>(b[i + 2]) | (static_cast<unsigned>(static_cast<uint8_t>(b[i + 3])) << 8);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) {
            o += static_cast<char>(0xC0 | (c >> 6));
            o += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            o += static_cast<char>(0xE0 | (c >> 12));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (c >> 18));
            o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return o;
}
std::string decodeText(std::string_view b) {
    if (b.size() >= 2 && static_cast<uint8_t>(b[0]) == 0xFF && static_cast<uint8_t>(b[1]) == 0xFE) return utf16ToUtf8(b.substr(2), false);
    if (b.size() >= 2 && static_cast<uint8_t>(b[0]) == 0xFE && static_cast<uint8_t>(b[1]) == 0xFF) return utf16ToUtf8(b.substr(2), true);
    return std::string(b);
}
} // namespace

Conversion convertFile(const fs::path& file, const Options& options) {
    Conversion c;
    Options o = options;
    if (o.sourceName.empty()) o.sourceName = narrow(file.filename().wstring());
    c.report.sourceName = commentText(o.sourceName, 200);
    c.report.fledgeVersion = o.fledgeVersion;
    std::error_code ec;
    auto size = fs::file_size(osPath(file), ec);
    if (ec) {
        c.report.error = fileErrorReason(file);
        return c;
    }
    c.report.sourceBytes = static_cast<size_t>(size);
    if (size > o.limits.maxInputBytes) {
        c.report.error = "the file is " + std::to_string(size / 1024) + " KiB; the limit is " + std::to_string(o.limits.maxInputBytes / 1024) + " KiB";
        return c;
    }
    std::string err;
    auto bytes = readFileBytes(file, &err);
    if (!bytes) {
        c.report.error = err;
        return c;
    }
    std::string_view b(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    std::string ext = toLower(narrow(file.extension().wstring()));
    if (isUnrealPackage(b) || ext == ".uasset" || ext == ".umap") {
        UassetProbe p = probeUasset(b, o.mappings ? *o.mappings : MappingTable::builtin());
        c.report.sourceKind = "uasset";
        c.report.uasset = p.toJson();
        c.report.error = ".uasset conversion is not supported - " + p.message;
        c.className = sanitizeIdentifier(narrow(file.stem().wstring()), "ConvertedBlueprint");
        c.report.className = c.className;
        return c;
    }
    // class name: Options::className, else the Blueprint object of a whole export, else the file name
    return convertText(decodeText(b), o);
}

bool isUnrealPackage(std::string_view b) {
    return b.size() >= 4 && static_cast<uint8_t>(b[0]) == 0xC1 && static_cast<uint8_t>(b[1]) == 0x83 && static_cast<uint8_t>(b[2]) == 0x2A &&
           static_cast<uint8_t>(b[3]) == 0x9E;
}

Result<std::string> readT3DText(const fs::path& file, const Limits& limits) {
    std::error_code ec;
    auto size = fs::file_size(osPath(file), ec);
    if (ec) return Result<std::string>::failure(fileErrorReason(file));
    if (size > limits.maxInputBytes)
        return Result<std::string>::failure("the file is " + std::to_string(size / 1024) + " KiB; the limit is " + std::to_string(limits.maxInputBytes / 1024) + " KiB");
    std::string err;
    auto bytes = readFileBytes(file, &err);
    if (!bytes) return Result<std::string>::failure(err);
    std::string_view b(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (isUnrealPackage(b)) return Result<std::string>::failure("this is an Unreal package (.uasset / .umap), not T3D text");
    return decodeText(b);
}

} // namespace fledge::blueprint
