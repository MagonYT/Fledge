#include "fledge/render/upscalers.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "fledge/core/json.h"
#include "fledge/core/paths.h"

#ifdef _WIN32
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <dxgi1_6.h>
#endif

namespace fledge::upscalers {

namespace {
std::mutex g_mutex;
std::filesystem::path g_override;

std::filesystem::path rootOr(const std::filesystem::path& root) { return root.empty() ? cacheRoot() : root; }

// "310.9.16" > "3.7.20": numeric component compare
bool newer(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        std::vector<int> v;
        int cur = 0;
        bool any = false;
        for (char c : s) {
            if (c >= '0' && c <= '9') {
                cur = cur * 10 + (c - '0');
                any = true;
            } else if (any) {
                v.push_back(cur);
                cur = 0;
                any = false;
            }
        }
        if (any) v.push_back(cur);
        return v;
    };
    return parts(a) > parts(b);
}

std::string primaryAdapter() {
#ifdef _WIN32
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return {};
    using PFN = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
    std::string name;
    IDXGIFactory1* f = nullptr;
    if (create && SUCCEEDED(create(0, __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            a->Release();
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            char buf[160];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, buf, sizeof(buf), nullptr, nullptr);
            name = buf;
            break;
        }
        f->Release();
    }
    FreeLibrary(dxgi);
    return name;
#else
    return {};
#endif
}
} // namespace

uint64_t VersionEntry::downloadSize() const {
    uint64_t n = license.size;
    for (const auto& f : files) n += f.size;
    return n;
}

const VersionEntry* findVersion(std::string_view id) {
    for (const auto& v : catalog())
        if (v.id == id) return &v;
    return nullptr;
}

std::vector<const VersionEntry*> versionsFor(UpscalerKind kind) {
    std::vector<const VersionEntry*> out;
    for (const auto& v : catalog())
        if (v.kind == kind) out.push_back(&v);
    // newest first by release date (vendor numbering is not monotonic: DLSS 3.7.20 -> 310.1.0);
    // unpublished entries (no date) stay on top so their reason is visible
    std::stable_sort(out.begin(), out.end(), [](auto* a, auto* b) {
        if (a->date.empty() || b->date.empty()) return a->date.empty() && !b->date.empty();
        if (a->date != b->date) return a->date > b->date;
        return newer(a->version, b->version);
    });
    return out;
}

std::filesystem::path cacheRoot() {
    {
        std::lock_guard l(g_mutex);
        if (!g_override.empty()) return g_override;
    }
    const EnginePaths& p = enginePaths();
    // portable installs keep everything next to the executable; otherwise the per-user data folder
    return p.portable ? p.root / "Upscalers" : p.userData / "Upscalers";
}

void setCacheRootOverride(const std::filesystem::path& p) {
    std::lock_guard l(g_mutex);
    g_override = p;
}

std::filesystem::path versionDir(const VersionEntry& v, const std::filesystem::path& root) { return rootOr(root) / v.id; }

bool isInstalled(const VersionEntry& v, std::string* why, const std::filesystem::path& root) {
    auto no = [&](std::string w) {
        if (why) *why = std::move(w);
        return false;
    };
    if (v.files.empty()) return no("nothing to install");
    std::filesystem::path dir = versionDir(v, root);
    auto text = readFileText(dir / "fledge_upscaler.json");
    if (!text) return no("not installed");
    Json m = Json::parse(*text, nullptr, false);
    if (!m.is_object() || m.value("id", "") != v.id || !m.value("licenseAccepted", false)) return no("incomplete installation");
    std::error_code ec;
    for (const auto& f : v.files) {
        auto size = std::filesystem::file_size(dir / f.name, ec);
        if (ec || size != f.size) return no(f.name + " missing or damaged");
    }
    return true;
}

std::vector<const VersionEntry*> installedVersions(UpscalerKind kind, const std::filesystem::path& root) {
    std::vector<const VersionEntry*> out;
    for (const VersionEntry* v : versionsFor(kind))
        if (isInstalled(*v, nullptr, root)) out.push_back(v);
    return out;
}

const VersionEntry* selectVersion(UpscalerKind kind, std::string_view requested, const std::filesystem::path& root) {
    if (!requested.empty())
        if (const VersionEntry* v = findVersion(requested); v && v->kind == kind && isInstalled(*v, nullptr, root)) return v;
    auto inst = installedVersions(kind, root);
    return inst.empty() ? nullptr : inst.front();  // newest first
}

Check hardwareCheck(UpscalerKind kind, const std::string& adapterDescription) {
    std::string a = adapterDescription.empty() ? primaryAdapter() : adapterDescription;
    std::string lower = a;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    switch (kind) {
        case UpscalerKind::Dlss:
            if (lower.find("nvidia") == std::string::npos) return {false, "DLSS needs an NVIDIA RTX GPU (found: " + (a.empty() ? "unknown" : a) + ")"};
            if (lower.find("rtx") == std::string::npos) return {false, "DLSS needs an NVIDIA RTX GPU (tensor cores); found " + a};
            return {true, {}};
        case UpscalerKind::XeSS:
            return {true, {}};  // XeSS SR runs on any Shader Model 6.4 GPU (DP4a path on non-Intel)
        default: return {true, {}};
    }
}

Check runtimeIntegration(UpscalerKind kind, rhi::Backend backend) {
    switch (kind) {
        case UpscalerKind::Dlss:
            return {false,
                    "DLSS evaluation is not integrated in this build (NGX needs MSVC-built static libraries; Streamline is not integrated) - "
                    "versions can be downloaded and packaged"};
        case UpscalerKind::XeSS:
#ifdef FLEDGE_HAVE_XESS
            if (backend != rhi::Backend::D3D12) return {false, "XeSS is integrated for Direct3D 12 only"};
            return {true, {}};
#else
            (void)backend;
            return {false, "built without the XeSS headers"};
#endif
        default: return {true, {}};
    }
}

std::filesystem::path runtimeLibrary(const VersionEntry& v, const std::filesystem::path& exeDir, const std::filesystem::path& root) {
    if (v.files.empty()) return {};
    std::error_code ec;
    const FileEntry& f = v.files.front();
    if (!exeDir.empty()) {
        std::filesystem::path local = exeDir / f.name;  // packaged next to the game
        if (std::filesystem::file_size(local, ec) == f.size && !ec) return local;
    }
    if (isInstalled(v, nullptr, root)) return versionDir(v, root) / f.name;
    return {};
}

Resolution resolve(const GraphicsSettings& g, rhi::Backend backend, const std::string& adapter, bool fsr1Available, const std::filesystem::path& exeDir,
                   const std::filesystem::path& root) {
    Resolution r;
    r.kind = g.upscaler;
    auto fallback = [&](UpscalerKind from, const std::string& why) {
        r.note += (r.note.empty() ? "" : "; ") + std::string(toString(from)) + ": " + why;
    };
    auto tryVendor = [&](UpscalerKind k) -> bool {
        Check hw = hardwareCheck(k, adapter);
        if (!hw.ok) return fallback(k, hw.reason), false;
        Check in = runtimeIntegration(k, backend);
        if (!in.ok) return fallback(k, in.reason), false;
        const VersionEntry* v = nullptr;
        // packaged games: a version folder may be absent but the DLL sits next to the exe
        if (const VersionEntry* req = findVersion(g.upscalerVersion); req && req->kind == k && !runtimeLibrary(*req, exeDir, root).empty()) v = req;
        if (!v) v = selectVersion(k, g.upscalerVersion, root);
        if (!v) {
            for (const VersionEntry* c : versionsFor(k))
                if (!runtimeLibrary(*c, exeDir, root).empty()) {
                    v = c;
                    break;
                }
        }
        if (!v) return fallback(k, "no version installed (Graphics panel > Upscaler versions)"), false;
        r.kind = k;
        r.version = v;
        return true;
    };
    if (r.kind == UpscalerKind::Dlss) {
        if (tryVendor(UpscalerKind::Dlss)) return r;
        r.kind = UpscalerKind::XeSS;
    }
    if (r.kind == UpscalerKind::XeSS) {
        if (tryVendor(UpscalerKind::XeSS)) return r;
        r.kind = UpscalerKind::Fsr;
    }
    if (r.kind == UpscalerKind::Fsr && !fsr1Available) {
        fallback(UpscalerKind::Fsr, "FSR 1 runs on Direct3D 11/12 only");
        r.kind = UpscalerKind::Taa;
    }
    return r;
}

} // namespace fledge::upscalers
