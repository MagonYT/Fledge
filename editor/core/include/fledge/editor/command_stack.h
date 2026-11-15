#pragma once
// Undo/redo for scene edits.
//
// Model: every command stores the full state (components + parent + sibling
// index, see Scene::entityState) of each entity it touched, before and after
// ("absent" when the entity did not exist). Undo applies the before-states,
// redo the after-states. This one mechanism covers property edits, create /
// delete (whole subtrees), rename, reparent, gizmo drags and multi-step AI tool
// calls, and it cannot drift out of sync with what the tool actually changed.
//
// Usage (all editor mutations go through EditorSession ops which do this):
//   stack.begin("Move Cube", EditSource::User, "gizmo:5");  // nestable
//   stack.touch(e);   // BEFORE mutating e (captures its before-state)
//   ... mutate scene ...
//   stack.commit();   // entities created inside the transaction are detected automatically
//
// Continuous edits (drags, typing) pass a merge key; consecutive commits with
// the same key merge into one command until closeMerge() is called (mouse up /
// widget deactivated). While disabled (play mode) nothing is recorded.
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/scene/scene.h"

namespace fledge {

enum class EditSource { User, AI, System };
const char* toString(EditSource s);

struct Command {
    std::string label;
    EditSource source = EditSource::User;
    std::string mergeKey;
    double timeSeconds = 0;  // since stack creation
    std::map<EntityId, std::optional<Json>> before, after;
    // Asset files (e.g. .fmat, their .meta, AssetRegistry.json) touched by the
    // command: full contents before/after, nullopt = file absent.
    std::map<std::string, std::optional<std::string>> filesBefore, filesAfter;
};

// File access used for asset-file snapshots (provided by EditorSession over the VFS).
struct CommandFileIO {
    std::function<std::optional<std::string>(const std::string& vpath)> read;
    std::function<void(const std::string& vpath, const std::optional<std::string>& content)> write;  // nullopt = delete
    std::function<void(const std::vector<std::string>& vpaths)> applied;                             // after undo/redo/cancel wrote files
};

class CommandStack {
public:
    explicit CommandStack(Scene& scene);

    void begin(std::string label, EditSource source = EditSource::User, std::string mergeKey = {});
    void touch(EntityId e);
    void touchSubtree(EntityId e);  // e and all descendants (use before deleting)
    void touchAll();
    void touchFile(const std::string& vpath);  // BEFORE writing an asset file inside a transaction
    CommandFileIO fileIO;
    bool commit();   // true if a command was recorded or merged
    void cancel();   // outermost: rolls the scene back to the transaction start
    bool inTransaction() const { return m_depth > 0; }
    void closeMerge() { m_mergeOpen = false; }

    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < m_commands.size(); }
    bool undo();
    bool redo();
    void clear();

    void setEnabled(bool enabled);  // disabled during play mode
    bool enabled() const { return m_enabled; }

    const std::vector<Command>& commands() const { return m_commands; }
    size_t cursor() const { return m_cursor; }  // number of applied commands
    size_t maxCommands = 1000;
    std::function<void()> onChanged;  // called after commit/undo/redo

private:
    Status apply(const std::map<EntityId, std::optional<Json>>& states);
    void applyFiles(const std::map<std::string, std::optional<std::string>>& files);
    std::optional<Json> capture(EntityId e);

    Scene& m_scene;
    std::vector<Command> m_commands;
    size_t m_cursor = 0;
    int m_depth = 0;
    Command m_pending;
    std::unordered_set<EntityId> m_aliveAtBegin;
    bool m_mergeOpen = false;
    bool m_enabled = true;
    bool m_applying = false;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

} // namespace fledge
