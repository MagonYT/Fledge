#include "fledge/editor/autosave.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>

#include "fledge/core/log.h"

namespace fledge::editor {

const char* toString(AutoSaveMode m) {
    switch (m) {
        case AutoSaveMode::AfterDelay: return "delay";
        case AutoSaveMode::OnFocusLoss: return "focus";
        default: return "off";
    }
}

Json AutosaveSettings::toJson() const {
    return {{"recovery", recovery}, {"recoverySeconds", recoverySeconds}, {"recoveryKeep", recoveryKeep},
            {"mode", static_cast<int>(mode)}, {"delaySeconds", delaySeconds}};
}

AutosaveSettings AutosaveSettings::fromJson(const Json& j) {
    AutosaveSettings s;
    if (!j.is_object()) return s;
    auto num = [&](const char* k, int def, int lo, int hi) {
        if (!j.contains(k) || !j[k].is_number()) return def;
        return std::clamp(j[k].get<int>(), lo, hi);
    };
    if (j.contains("recovery") && j["recovery"].is_boolean()) s.recovery = j["recovery"].get<bool>();
    s.recoverySeconds = num("recoverySeconds", s.recoverySeconds, 5, 3600);
    s.recoveryKeep = num("recoveryKeep", s.recoveryKeep, 1, 50);
    s.mode = static_cast<AutoSaveMode>(num("mode", 0, 0, 2));
    s.delaySeconds = num("delaySeconds", s.delaySeconds, 5, 3600);
    return s;
}

// ----------------------------------------------------------------- store ----
namespace {
constexpr const char* kFormat = "fledge-recovery";
constexpr int kVersion = 1;

// snapshot-000012-1767225600000.json -> seq 12
bool parseName(const fs::path& p, uint64_t& seq) {
    std::string n = narrow(p.filename().wstring());
    if (!startsWith(n, "snapshot-") || !endsWith(n, ".json")) return false;
    size_t a = 9, b = n.find('-', a);
    if (b == std::string::npos || b == a || b - a > 18) return false;
    for (size_t i = a; i < b; ++i)
        if (n[i] < '0' || n[i] > '9') return false;
    seq = std::stoull(n.substr(a, b - a));
    return true;
}
} // namespace

bool RecoveryStore::atomicWrite(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(osPath(path.parent_path()), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(osPath(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f.good()) {
            f.close();
            fs::remove(osPath(tmp), ec);
            return false;
        }
    }
    fs::rename(osPath(tmp), osPath(path), ec);
    if (ec) {
        fs::remove(osPath(tmp), ec);
        return false;
    }
    return true;
}

std::vector<fs::path> RecoveryStore::files() const {
    std::vector<std::pair<uint64_t, fs::path>> v;
    std::error_code ec;
    for (fs::directory_iterator it(osPath(m_dir), ec), end; !ec && it != end; it.increment(ec)) {
        uint64_t seq = 0;
        if (it->is_regular_file(ec) && parseName(it->path(), seq)) v.emplace_back(seq, m_dir / it->path().filename());
    }
    std::sort(v.begin(), v.end());
    std::vector<fs::path> out;
    for (auto& [s, p] : v) out.push_back(p);
    return out;
}

Status RecoveryStore::write(const std::string& scenePath, const Json& scene, int64_t createdMs, int keep) {
    uint64_t seq = 1;
    auto existing = files();
    for (auto& f : existing) {
        uint64_t s = 0;
        if (parseName(f, s)) seq = std::max(seq, s + 1);
    }
    Json env = {{"format", kFormat}, {"version", kVersion}, {"scenePath", scenePath}, {"createdMs", createdMs}, {"scene", scene}};
    fs::path file = m_dir / std::format("snapshot-{:06}-{}.json", seq, createdMs);
    std::string data = env.dump();
    bool ok = writer ? writer(file, data) : atomicWrite(file, data);
    if (!ok) return Status::failure("cannot write the recovery snapshot " + narrow(file.wstring()));
    existing.push_back(file);
    std::error_code ec;
    while (static_cast<int>(existing.size()) > std::max(1, keep)) {
        fs::remove(osPath(existing.front()), ec);
        existing.erase(existing.begin());
    }
    return {};
}

Result<RecoverySnapshot> RecoveryStore::load(const fs::path& file) {
    auto fail = [&](const std::string& why) { return Result<RecoverySnapshot>::failure(narrow(file.filename().wstring()) + ": " + why); };
    auto text = readFileText(file);
    if (!text) return fail("cannot be read");
    Json j = Json::parse(*text, nullptr, false);
    if (!j.is_object()) return fail("is not valid JSON (truncated or corrupt)");
    if (j.value("format", "") != kFormat) return fail("is not a recovery snapshot");
    if (j.value("version", 0) != kVersion) return fail("has an unsupported version");
    if (!j.contains("scene") || !j["scene"].is_object() || !j.contains("scenePath") || !j["scenePath"].is_string())
        return fail("is incomplete");
    RecoverySnapshot s;
    s.file = file;
    parseName(file, s.seq);
    s.createdMs = j.value("createdMs", int64_t{0});
    s.scenePath = j["scenePath"].get<std::string>();
    s.scene = std::move(j["scene"]);
    if (s.scene.contains("entities") && s.scene["entities"].is_array()) s.entities = s.scene["entities"].size();
    return s;
}

std::vector<RecoverySnapshot> RecoveryStore::loadAll(std::vector<std::string>* warnings) const {
    std::vector<RecoverySnapshot> out;
    auto all = files();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        auto r = load(*it);
        if (!r) {
            FLEDGE_LOG_WARN("editor", "Ignoring recovery snapshot {}", r.error());
            if (warnings) warnings->push_back(r.error());
            continue;
        }
        out.push_back(std::move(*r));
    }
    return out;
}

int RecoveryStore::clear() {
    int n = 0;
    std::error_code ec;
    std::vector<fs::path> victims = files();
    for (fs::directory_iterator it(osPath(m_dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".tmp" && startsWith(narrow(it->path().filename().wstring()), "snapshot-"))
            victims.push_back(m_dir / it->path().filename());
    for (auto& f : victims) {
        ec.clear();
        if (fs::remove(osPath(f), ec) && !ec) ++n;
    }
    return n;
}

// ------------------------------------------------------------- decision ----
std::optional<RecoveryOffer> decideRecovery(const std::vector<RecoverySnapshot>& snaps,
                                            const std::function<SavedScene(const std::string&)>& lookup) {
    if (snaps.empty()) return std::nullopt;
    const RecoverySnapshot& newest = snaps.front();  // written in order: the newest one holds the latest work
    SavedScene saved = lookup ? lookup(newest.scenePath) : SavedScene{};
    if (saved.exists) {
        if (newest.createdMs <= saved.mtimeMs) return std::nullopt;                     // saved after the snapshot
        if (saved.json.is_object() && saved.json == newest.scene) return std::nullopt;  // nothing to recover
    }
    RecoveryOffer o;
    o.snapshot = newest;
    o.snapshotCount = snaps.size();
    o.saved = std::move(saved);
    o.summary = std::format("{} entit{}", newest.entities, newest.entities == 1 ? "y" : "ies");
    return o;
}

// --------------------------------------------------------------- timing ----
void AutosaveScheduler::reset() {
    m_wasDirty = false;
    m_focusLost = false;
    m_snapshotSerial.reset();
}

AutosaveActions AutosaveScheduler::tick(double now, const AutosaveSettings& s, const AutosaveContext& c) {
    AutosaveActions a;
    if (!c.hasProject) {
        reset();
        return a;
    }
    if (c.changeSerial != m_seenSerial) {
        m_seenSerial = c.changeSerial;
        m_lastChange = now;
    }
    if (m_wasFocused && !c.focused) m_focusLost = true;
    if (c.focused) m_focusLost = false;
    m_wasFocused = c.focused;
    if (!c.dirty) {
        m_wasDirty = false;
        m_focusLost = false;
        m_snapshotSerial.reset();
        return a;
    }
    if (!m_wasDirty) {  // the scene just became dirty: both timers start now
        m_wasDirty = true;
        m_lastSnapshot = m_lastChange = now;
        m_snapshotSerial.reset();
    }
    // Play mode edits are discarded on stop and a half-built scene is not worth saving: nothing happens until they are over.
    if (c.playing || c.transient) return a;

    if (s.recovery && now - m_lastSnapshot >= s.recoverySeconds && m_snapshotSerial != c.changeSerial) {
        a.snapshot = true;
        m_lastSnapshot = now;
        m_snapshotSerial = c.changeSerial;
    }
    if (c.allowRealSave && !c.busyUi) {
        if (s.mode == AutoSaveMode::AfterDelay && now - m_lastChange >= s.delaySeconds) {
            a.autoSave = true;
            m_lastChange = now;  // a failed save retries after another delay, not every frame
        } else if (s.mode == AutoSaveMode::OnFocusLoss && m_focusLost) {
            a.autoSave = true;
            m_focusLost = false;
        }
    }
    return a;
}

int64_t wallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t fileTimeMs(const fs::path& file) {
    std::error_code ec;
    auto t = fs::last_write_time(osPath(file), ec);
    if (ec) return 0;
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

} // namespace fledge::editor
