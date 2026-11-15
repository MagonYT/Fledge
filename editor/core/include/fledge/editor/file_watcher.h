#pragma once
// Directory change notifications for live asset reload.
//   Win32 backend: ReadDirectoryChangesW (overlapped, background thread).
//   Polling backend: periodic size/mtime snapshot diff (portable fallback).
#include <memory>
#include <string>
#include <vector>

#include "fledge/core/utility.h"

namespace fledge {

class FileWatcher {
public:
    virtual ~FileWatcher() = default;
    // Changed/added/removed files since the last call, relative to the root ('/'-separated, deduplicated).
    virtual std::vector<std::string> poll() = 0;
    virtual const char* backend() const = 0;

    // preferNative=false forces the polling backend. Returns nullptr if the directory does not exist.
    static std::unique_ptr<FileWatcher> create(const fs::path& root, bool preferNative = true, int pollIntervalMs = 250);
};

} // namespace fledge
