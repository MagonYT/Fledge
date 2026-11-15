#include "fledge/editor/command_stack.h"

#include <algorithm>

#include "fledge/core/log.h"

namespace fledge {

const char* toString(EditSource s) {
    switch (s) {
        case EditSource::User: return "user";
        case EditSource::AI: return "AI";
        case EditSource::System: return "system";
    }
    return "?";
}

CommandStack::CommandStack(Scene& scene) : m_scene(scene) {}

std::optional<Json> CommandStack::capture(EntityId e) {
    if (!m_scene.isValid(e)) return std::nullopt;
    return m_scene.entityState(e);
}

void CommandStack::begin(std::string label, EditSource source, std::string mergeKey) {
    if (m_depth++ > 0 || !m_enabled || m_applying) return;
    m_pending = Command{};
    m_pending.label = std::move(label);
    m_pending.source = source;
    m_pending.mergeKey = std::move(mergeKey);
    m_aliveAtBegin = {m_scene.entities().begin(), m_scene.entities().end()};
}

void CommandStack::touch(EntityId e) {
    if (m_depth == 0 || !m_enabled || m_applying) return;
    if (m_pending.before.contains(e)) return;
    // Created inside this transaction -> it did not exist before it.
    m_pending.before[e] = m_aliveAtBegin.contains(e) ? capture(e) : std::nullopt;
}

void CommandStack::touchSubtree(EntityId e) {
    touch(e);
    for (EntityId d : m_scene.descendants(e)) touch(d);
}

void CommandStack::touchFile(const std::string& vpath) {
    if (m_depth == 0 || !m_enabled || m_applying || !fileIO.read) return;
    if (!m_pending.filesBefore.contains(vpath)) m_pending.filesBefore[vpath] = fileIO.read(vpath);
}

void CommandStack::applyFiles(const std::map<std::string, std::optional<std::string>>& files) {
    if (files.empty() || !fileIO.write) return;
    std::vector<std::string> paths;
    for (auto& [p, content] : files) {
        fileIO.write(p, content);
        paths.push_back(p);
    }
    if (fileIO.applied) fileIO.applied(paths);
}

void CommandStack::touchAll() {
    for (EntityId e : m_scene.entities()) touch(e);
}

bool CommandStack::commit() {
    if (m_depth == 0) return false;
    if (--m_depth > 0 || !m_enabled || m_applying) return false;
    // Entities created inside the transaction did not exist before it.
    for (EntityId e : m_scene.entities())
        if (!m_aliveAtBegin.contains(e) && !m_pending.before.contains(e)) m_pending.before[e] = std::nullopt;
    bool changed = false;
    for (auto& [e, b] : m_pending.before) {
        auto a = capture(e);
        if (a != b) changed = true;
        m_pending.after[e] = std::move(a);
    }
    for (auto& [p, b] : m_pending.filesBefore) {
        auto a = fileIO.read ? fileIO.read(p) : std::nullopt;
        if (a != b) changed = true;
        m_pending.filesAfter[p] = std::move(a);
    }
    if (!changed) return false;
    m_pending.timeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();

    bool merge = !m_pending.mergeKey.empty() && m_mergeOpen && m_cursor == m_commands.size() && m_cursor > 0 &&
                 m_commands.back().mergeKey == m_pending.mergeKey && m_commands.back().source == m_pending.source;
    if (merge) {
        Command& top = m_commands.back();
        for (auto& [e, b] : m_pending.before)
            if (!top.before.contains(e)) top.before[e] = b;
        for (auto& [e, a] : m_pending.after) top.after[e] = a;
        for (auto& [p, b] : m_pending.filesBefore)
            if (!top.filesBefore.contains(p)) top.filesBefore[p] = b;
        for (auto& [p, a] : m_pending.filesAfter) top.filesAfter[p] = a;
        top.timeSeconds = m_pending.timeSeconds;
    } else {
        m_commands.resize(m_cursor);  // drop the redo tail
        m_commands.push_back(std::move(m_pending));
        if (m_commands.size() > maxCommands) m_commands.erase(m_commands.begin());
        m_cursor = m_commands.size();
    }
    m_mergeOpen = !m_commands.back().mergeKey.empty();
    m_pending = Command{};
    if (onChanged) onChanged();
    return true;
}

void CommandStack::cancel() {
    if (m_depth == 0) return;
    if (--m_depth > 0 || !m_enabled || m_applying) return;
    std::map<EntityId, std::optional<Json>> restore = m_pending.before;
    for (EntityId e : m_scene.entities())
        if (!m_aliveAtBegin.contains(e)) restore[e] = std::nullopt;
    (void)apply(restore);
    m_applying = true;
    applyFiles(m_pending.filesBefore);
    m_applying = false;
    m_pending = Command{};
}

Status CommandStack::apply(const std::map<EntityId, std::optional<Json>>& states) {
    m_applying = true;
    Status result;
    // 1) delete entities that must not exist (their surviving children are fixed in step 3)
    for (auto& [e, st] : states)
        if (!st && m_scene.isValid(e)) m_scene.destroySingle(e);
    // 2) create/update components (parents may not exist yet)
    std::vector<std::pair<EntityId, const Json*>> present;
    for (auto& [e, st] : states) {
        if (!st) continue;
        Json flat = *st;
        flat["parent"] = kNullEntity;
        flat["index"] = -1;
        bool existed = m_scene.isValid(e);
        if (auto s = m_scene.applyEntityState(e, existed ? Json{{"components", flat["components"]}, {"parent", m_scene.parentOf(e)},
                                                                 {"index", m_scene.siblingIndex(e)}}
                                                         : flat);
            !s && result.ok)
            result = s;
        present.emplace_back(e, &*st);
    }
    // 3) restore hierarchy links in ascending sibling index so positions come out right
    std::sort(present.begin(), present.end(),
              [](auto& a, auto& b) { return a.second->value("index", -1) < b.second->value("index", -1); });
    for (auto& [e, st] : present) {
        EntityId parent = st->value("parent", kNullEntity);
        if (auto s = m_scene.setParent(e, m_scene.isValid(parent) ? parent : kNullEntity, st->value("index", -1), false); !s && result.ok)
            result = s;
    }
    m_applying = false;
    if (!result) FLEDGE_LOG_ERROR("history", "undo/redo apply failed: {}", result.error);
    return result;
}

bool CommandStack::undo() {
    if (!m_enabled || m_depth > 0 || !canUndo()) return false;
    m_mergeOpen = false;
    const Command& c = m_commands[--m_cursor];
    (void)apply(c.before);
    m_applying = true;
    applyFiles(c.filesBefore);
    m_applying = false;
    FLEDGE_LOG_INFO("history", "Undo: {} [{}]", c.label, toString(c.source));
    if (onChanged) onChanged();
    return true;
}

bool CommandStack::redo() {
    if (!m_enabled || m_depth > 0 || !canRedo()) return false;
    m_mergeOpen = false;
    const Command& c = m_commands[m_cursor++];
    (void)apply(c.after);
    m_applying = true;
    applyFiles(c.filesAfter);
    m_applying = false;
    FLEDGE_LOG_INFO("history", "Redo: {} [{}]", c.label, toString(c.source));
    if (onChanged) onChanged();
    return true;
}

void CommandStack::clear() {
    m_commands.clear();
    m_cursor = 0;
    m_mergeOpen = false;
}

void CommandStack::setEnabled(bool enabled) {
    m_enabled = enabled;
    m_mergeOpen = false;
}

} // namespace fledge
