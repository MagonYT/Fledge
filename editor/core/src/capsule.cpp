#include "fledge/editor/capsule.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "fledge/core/sha256.h"
#include "fledge/core/version.h"

namespace fledge {

namespace {
constexpr char kMagic[9] = "FLDGCAP1";
constexpr char kEnd[9] = "FLDGEND1";

bool skipDir(const std::string& n) { return n == "Intermediate" || n == "Build" || n == ".git" || n == ".vs"; }

struct Loaded {
    CapsuleInfo info;
    std::vector<std::pair<std::string, Bytes>> files;  // rel path, data
};

Result<Loaded> load(const fs::path& capsule, bool keepData) {
    auto fail = [](std::string e) { return Result<Loaded>::failure(std::move(e)); };
    auto bytes = readFileBytes(capsule);
    if (!bytes) return fail("cannot read " + narrow(capsule.wstring()));
    const Bytes& b = *bytes;
    if (b.size() < 16 + 8 || std::memcmp(b.data(), kMagic, 8) != 0) return fail("not a Fledge capsule");
    uint64_t msize = 0;
    std::memcpy(&msize, b.data() + 8, 8);
    if (msize > b.size() - 16) return fail("truncated capsule (manifest)");
    std::string mtext(reinterpret_cast<const char*>(b.data() + 16), static_cast<size_t>(msize));
    Json m = Json::parse(mtext, nullptr, false);
    if (!m.is_object() || m.value("format", "") != "fledgecapsule") return fail("damaged capsule manifest");
    // manifest hash covers everything but itself
    std::string claimed = m.value("manifestSha256", "");
    Json copy = m;
    copy.erase("manifestSha256");
    if (claimed.empty() || Sha256::hex(copy.dump()) != claimed) return fail("capsule manifest hash mismatch (modified or damaged)");
    Loaded out;
    out.info.name = m.value("name", "");
    out.info.engineVersion = m.value("engineVersion", "");
    out.info.createdBy = m.value("createdBy", "");
    out.info.manifest = m;
    size_t pos = 16 + static_cast<size_t>(msize);
    for (auto& f : m.value("files", Json::array())) {
        std::string path = f.value("path", "");
        uint64_t size = f.value("size", uint64_t{0});
        if (path.empty() || path.find("..") != std::string::npos || path[0] == '/' || path.find(':') != std::string::npos)
            return fail("unsafe path in capsule: " + path);
        if (size > b.size() - pos) return fail("truncated capsule (" + path + ")");
        if (Sha256::hex(b.data() + pos, static_cast<size_t>(size)) != f.value("sha256", "")) return fail("hash mismatch for " + path);
        if (keepData) out.files.push_back({path, Bytes(b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(pos + size))});
        pos += static_cast<size_t>(size);
        out.info.bytes += size;
        ++out.info.files;
    }
    if (b.size() - pos != 8 || std::memcmp(b.data() + pos, kEnd, 8) != 0) return fail("capsule has trailing or missing data");
    return out;
}
} // namespace

Result<CapsuleInfo> exportCapsule(const fs::path& root, const fs::path& outFile) {
    std::error_code ec;
    std::vector<fs::path> files;
    fs::path projectFile;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::string n = narrow(it->path().filename().wstring());
        if (it->is_directory(ec)) {
            if (skipDir(n)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || fs::equivalent(it->path(), outFile, ec)) continue;
        if (it->path().extension() == ".fledgecapsule") continue;
        if (it->path().extension() == ".fledge" && it->path().parent_path() == root) projectFile = it->path();
        files.push_back(it->path());
    }
    if (projectFile.empty()) return Result<CapsuleInfo>::failure("no .fledge project file in " + narrow(root.wstring()));
    std::sort(files.begin(), files.end());
    Json list = Json::array();
    std::vector<Bytes> blobs;
    CapsuleInfo info;
    for (auto& f : files) {
        auto data = readFileBytes(f);
        if (!data) return Result<CapsuleInfo>::failure("cannot read " + narrow(f.wstring()));
        list.push_back({{"path", narrow(f.lexically_relative(root).generic_wstring())}, {"size", data->size()}, {"sha256", Sha256::hex(data->data(), data->size())}});
        info.bytes += data->size();
        blobs.push_back(std::move(*data));
    }
    info.name = narrow(projectFile.stem().wstring());
    info.engineVersion = FLEDGE_VERSION_STRING;
    info.createdBy = "Fledge " FLEDGE_VERSION_STRING;
    info.files = files.size();
    Json m = {{"format", "fledgecapsule"}, {"version", 1}, {"name", info.name}, {"projectFile", narrow(projectFile.filename().wstring())},
              {"engineVersion", info.engineVersion}, {"createdBy", info.createdBy}, {"files", list}};
    m["manifestSha256"] = Sha256::hex(m.dump());
    info.manifest = m;
    std::string mtext = m.dump();
    fs::create_directories(outFile.parent_path(), ec);
    std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
    if (!out) return Result<CapsuleInfo>::failure("cannot write " + narrow(outFile.wstring()));
    uint64_t msize = mtext.size();
    out.write(kMagic, 8);
    out.write(reinterpret_cast<const char*>(&msize), 8);
    out.write(mtext.data(), static_cast<std::streamsize>(mtext.size()));
    for (auto& b : blobs) out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    out.write(kEnd, 8);
    if (!out.good()) return Result<CapsuleInfo>::failure("write failed: " + narrow(outFile.wstring()));
    return info;
}

Result<CapsuleInfo> inspectCapsule(const fs::path& capsule) {
    auto l = load(capsule, false);
    if (!l) return Result<CapsuleInfo>::failure(l.error());
    return l->info;
}

Result<fs::path> importCapsule(const fs::path& capsule, const fs::path& destDir) {
    auto l = load(capsule, true);
    if (!l) return Result<fs::path>::failure(l.error());
    std::string base = l->info.name.empty() ? "Imported" : l->info.name;
    std::error_code ec;
    fs::path dir = destDir / pathFromUtf8(base);
    for (int i = 2; fs::exists(dir, ec); ++i) dir = destDir / pathFromUtf8(base + "-" + std::to_string(i));
    for (auto& [rel, data] : l->files) {
        fs::path p = dir / pathFromUtf8(rel);
        fs::create_directories(p.parent_path(), ec);
        if (!writeFileBytes(p, data)) return Result<fs::path>::failure("cannot write " + narrow(p.wstring()));
    }
    return dir / pathFromUtf8(l->info.manifest.value("projectFile", base + ".fledge"));
}

} // namespace fledge
