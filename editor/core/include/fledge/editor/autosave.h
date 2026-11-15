#pragma once
// Scene auto-save and crash recovery (UI-free; the editor, the headless MCP server and the tests share it).
//
//   * Recovery snapshots (default ON): while a project is open and its scene has unsaved changes, a rolling snapshot of the
//     edit-mode scene is written every N seconds into <project>/Intermediate/Recovery/ (kept: the last K). Written atomically
//     (temp file + rename), never over the user's real files. Cleared by a clean save and a clean close.
//   * Recovery offer: opening a project whose snapshots are newer than, and differ from, the saved scene offers Restore /
//     Open saved version / Discard. decideRecovery() is the pure decision, RecoveryStore::loadAll() reads the snapshots.
//   * Auto-save (default OFF): Off | After a delay (idle since the last change) | On focus loss. AutosaveScheduler holds the
//     timing with an injectable clock; EditorSession performs the writes (docs/UI.md "Auto-save and recovery").
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge::editor {

enum class AutoSaveMode { Off = 0, AfterDelay = 1, OnFocusLoss = 2 };
const char* toString(AutoSaveMode m);  // "off" | "delay" | "focus"

// Per user (editor_settings.json "autosave"). Values are clamped when read.
struct AutosaveSettings {
    bool recovery = true;          // write recovery snapshots of unsaved changes
    int recoverySeconds = 60;      // 5 .. 3600
    int recoveryKeep = 5;          // 1 .. 50 snapshots kept
    AutoSaveMode mode = AutoSaveMode::Off;  // true auto-save of the scene file (editor GUI only)
    int delaySeconds = 60;         // idle seconds after the last change (mode == AfterDelay), 5 .. 3600
    Json toJson() const;
    static AutosaveSettings fromJson(const Json& j);
};

// One recovery snapshot (a JSON envelope around the scene: format, version, scene path, creation time, the scene).
struct RecoverySnapshot {
    fs::path file;
    uint64_t seq = 0;
    int64_t createdMs = 0;     // wall clock, ms since the Unix epoch
    std::string scenePath;     // "content:/Scenes/Main.fscene"
    Json scene;
    size_t entities = 0;
};

class RecoveryStore {
public:
    explicit RecoveryStore(fs::path dir) : m_dir(std::move(dir)) {}
    const fs::path& dir() const { return m_dir; }

    // Writes one file. The default writes <name>.tmp and renames it; tests replace it to simulate a failing disk.
    using WriteFn = std::function<bool(const fs::path& path, const std::string& data)>;
    WriteFn writer;
    static bool atomicWrite(const fs::path& path, const std::string& data);

    // New snapshot, then drops the oldest beyond `keep`. A failed write changes nothing: the older snapshots stay.
    Status write(const std::string& scenePath, const Json& scene, int64_t createdMs, int keep);
    std::vector<fs::path> files() const;  // snapshot files, oldest first (stray .tmp files and foreign names are ignored)
    size_t count() const { return files().size(); }
    // Newest first. Unreadable / corrupt / wrong-format files are skipped; each one adds a line to *warnings (and the log).
    std::vector<RecoverySnapshot> loadAll(std::vector<std::string>* warnings = nullptr) const;
    static Result<RecoverySnapshot> load(const fs::path& file);
    int clear();  // removes every snapshot and stray temp file; returns how many files were removed

private:
    fs::path m_dir;
};

// What is on disk for a scene: used by decideRecovery().
struct SavedScene {
    bool exists = false;
    Json json;            // parsed; null when unreadable
    int64_t mtimeMs = 0;  // last write time, ms since the Unix epoch
};

struct RecoveryOffer {
    RecoverySnapshot snapshot;  // the newest valid snapshot
    size_t snapshotCount = 0;   // valid snapshots found
    SavedScene saved;
    std::string summary;        // "12 entities" shown by the dialog
};

// Pure decision. `snapshotsNewestFirst` are the valid snapshots (RecoveryStore::loadAll). A recovery is offered when the
// newest snapshot is newer than the saved scene file AND its content differs from it (or the file is gone). Never throws.
std::optional<RecoveryOffer> decideRecovery(const std::vector<RecoverySnapshot>& snapshotsNewestFirst,
                                            const std::function<SavedScene(const std::string& scenePath)>& lookup);

// ------------------------------------------------------------------ timing --
struct AutosaveContext {
    bool hasProject = false;
    bool dirty = false;
    bool playing = false;
    bool transient = false;     // loading / importing / inside an undo transaction: the scene may be half-built
    bool busyUi = false;        // modal dialog, gizmo or camera drag, import / cook / package running ...
    bool focused = true;        // the editor window has focus
    bool allowRealSave = false; // only the editor GUI session may write the real scene file
    uint64_t changeSerial = 0;  // bumps on every edit (commit / undo / redo / restore)
};
struct AutosaveActions {
    bool snapshot = false;
    bool autoSave = false;
};

// The timing rules, free of any file access and of real time (`now` is passed in, seconds on any monotonic clock).
class AutosaveScheduler {
public:
    AutosaveActions tick(double now, const AutosaveSettings& s, const AutosaveContext& c);
    void reset();  // new project / scene loaded / saved

private:
    bool m_wasDirty = false, m_wasFocused = true, m_focusLost = false;
    double m_lastSnapshot = 0, m_lastChange = 0;
    uint64_t m_seenSerial = 0;
    std::optional<uint64_t> m_snapshotSerial;
};

int64_t wallClockMs();                     // system clock, ms since the Unix epoch
int64_t fileTimeMs(const fs::path& file);  // last write time of a file, 0 when it does not exist

} // namespace fledge::editor
