#pragma once
// Virtual file system. Paths look like "content:/Scenes/Main.fscene".
// A mount maps a scheme ("content") to a FileProvider: a directory on disk in
// the editor, or a .fpak archive in a packaged game.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/utility.h"

namespace fledge {

class FileProvider {
public:
    virtual ~FileProvider() = default;
    virtual std::optional<Bytes> read(std::string_view relPath) const = 0;
    virtual bool exists(std::string_view relPath) const = 0;
    virtual std::vector<std::string> list() const = 0;  // all files, '/'-separated relative paths
    virtual bool writable() const { return false; }
    virtual bool write(std::string_view, std::span<const uint8_t>) { return false; }
};

class DirectoryProvider final : public FileProvider {
public:
    explicit DirectoryProvider(fs::path root) : m_root(std::move(root)) {}
    std::optional<Bytes> read(std::string_view relPath) const override;
    bool exists(std::string_view relPath) const override;
    std::vector<std::string> list() const override;
    bool writable() const override { return true; }
    bool write(std::string_view relPath, std::span<const uint8_t> data) override;
    const fs::path& root() const { return m_root; }
    fs::path resolve(std::string_view relPath) const;  // rejects ".." escapes (returns empty)

private:
    fs::path m_root;
};

// Read-through stack of providers (first match wins; writes go to the first writable one).
// Used e.g. to mount a cook cache over a content directory.
class OverlayProvider final : public FileProvider {
public:
    explicit OverlayProvider(std::vector<std::shared_ptr<FileProvider>> layers) : m_layers(std::move(layers)) {}
    std::optional<Bytes> read(std::string_view relPath) const override;
    bool exists(std::string_view relPath) const override;
    std::vector<std::string> list() const override;

private:
    std::vector<std::shared_ptr<FileProvider>> m_layers;
};

// Writable in-memory files: the editor's scratch content while no project is open (materials etc.; moved into the project
// by EditorSession::createProject).
class MemoryProvider final : public FileProvider {
public:
    std::optional<Bytes> read(std::string_view relPath) const override;
    bool exists(std::string_view relPath) const override;
    std::vector<std::string> list() const override;
    bool writable() const override { return true; }
    bool write(std::string_view relPath, std::span<const uint8_t> data) override;
    bool empty() const { return m_files.empty(); }

private:
    std::map<std::string, Bytes> m_files;
};

class Vfs {
public:
    void mount(std::string scheme, std::shared_ptr<FileProvider> provider);
    void unmount(const std::string& scheme);
    std::optional<Bytes> read(std::string_view vpath) const;
    std::optional<std::string> readText(std::string_view vpath) const;
    bool exists(std::string_view vpath) const;
    bool write(std::string_view vpath, std::span<const uint8_t> data);
    bool writeText(std::string_view vpath, std::string_view text);
    std::vector<std::string> list(const std::string& scheme) const;
    FileProvider* provider(const std::string& scheme) const;

    // "content:/a/b.txt" -> {"content", "a/b.txt"}
    static bool split(std::string_view vpath, std::string& scheme, std::string& rel);

private:
    std::map<std::string, std::shared_ptr<FileProvider>, std::less<>> m_mounts;
};

} // namespace fledge
