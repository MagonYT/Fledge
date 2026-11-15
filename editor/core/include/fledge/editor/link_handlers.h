#pragma once
// Per-user URL protocol handlers (Windows registry, HKCU only - never machine-wide, never administrator rights).
//
//   fledge://                 Fledge's own scheme ("import this URL or file"): opt-in, installer page or Settings.
//   com.unity3d.kharma:       Unity Asset Store "Open in Unity"            } opt-in TAKE-OVER, off by default, only after a
//   com.epicgames.launcher:   Epic Games Launcher (Fab "Open in launcher") } consent prompt. The previous per-user values
//                                                                            are backed up first, and Restore puts them
//                                                                            back (Settings, the uninstaller, the CLI).
// A store scheme taken over keeps working for its owner: links Fledge does not import (Epic game launches, or any link
// the user sends on) are handed to the backed-up original command.
//
// All registry access goes through RegistryStore, so tests run against MemoryRegistry or a throw-away test key
// (Win32Registry with a test root) and never touch the real user's handlers.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge::editor {

// A registry subtree. Key paths are '\'-separated and relative to the store's root; value name "" = (Default).
class RegistryStore {
public:
    virtual ~RegistryStore() = default;
    virtual bool keyExists(const std::string& key) const = 0;
    virtual std::optional<std::string> getString(const std::string& key, const std::string& name) const = 0;
    virtual bool setString(const std::string& key, const std::string& name, const std::string& value) = 0;
    virtual bool createKey(const std::string& key) = 0;
    virtual bool deleteTree(const std::string& key) = 0;
    virtual std::vector<std::string> subkeys(const std::string& key) const = 0;
    virtual std::vector<std::string> valueNames(const std::string& key) const = 0;
    // The whole subtree as JSON {"values": {name: value}, "keys": {name: {...}}} (string values only).
    Json exportTree(const std::string& key) const;
    bool importTree(const std::string& key, const Json& tree);
};

class MemoryRegistry final : public RegistryStore {
public:
    bool keyExists(const std::string& key) const override;
    std::optional<std::string> getString(const std::string& key, const std::string& name) const override;
    bool setString(const std::string& key, const std::string& name, const std::string& value) override;
    bool createKey(const std::string& key) override;
    bool deleteTree(const std::string& key) override;
    std::vector<std::string> subkeys(const std::string& key) const override;
    std::vector<std::string> valueNames(const std::string& key) const override;
private:
    std::map<std::string, std::map<std::string, std::string>> m_keys;  // lower-case key path -> values
};

#ifdef _WIN32
// HKEY_CURRENT_USER\<root> (writable) or, with readOnlyMerged, HKEY_CLASSES_ROOT (the merged per-user + machine view,
// used only to show who owns a scheme today and to find a machine-wide original command).
class Win32Registry final : public RegistryStore {
public:
    explicit Win32Registry(std::string hkcuRoot, bool readOnlyMerged = false, bool machine = false);
    bool keyExists(const std::string& key) const override;
    std::optional<std::string> getString(const std::string& key, const std::string& name) const override;
    bool setString(const std::string& key, const std::string& name, const std::string& value) override;
    bool createKey(const std::string& key) override;
    bool deleteTree(const std::string& key) override;
    std::vector<std::string> subkeys(const std::string& key) const override;
    std::vector<std::string> valueNames(const std::string& key) const override;
private:
    std::string m_root;
    bool m_readOnly = false, m_machine = false, m_merged = false;
    std::wstring full(const std::string& key) const;
};
#endif

struct HandlerStatus {
    std::string scheme;
    std::string title;           // "Unity Asset Store (Open in Unity)"
    std::string normalOwner;     // "Unity Hub / Unity Editor"
    bool ours = false;           // HKCU points at this Fledge
    bool backupExists = false;   // a take-over backup is stored
    std::string currentCommand;  // effective command today (merged view), "" = no handler
    bool perUserBefore = false;  // the backup holds per-user values (else the owner is machine-wide or absent)
};

class LinkHandlers {
public:
    static constexpr const char* kFledge = "fledge";
    static constexpr const char* kUnity = "com.unity3d.kharma";
    static constexpr const char* kEpic = "com.epicgames.launcher";
    static const std::vector<std::string>& storeSchemes();  // kUnity, kEpic

    // classes: HKCU\Software\Classes; backup: HKCU\Software\Fledge\LinkHandlers; merged: HKCR (optional);
    // machine: HKLM\Software\Classes read-only (optional; original command of machine-wide handlers).
    LinkHandlers(RegistryStore& classes, RegistryStore& backup, const RegistryStore* merged = nullptr, const RegistryStore* machine = nullptr);

    static std::string commandFor(const fs::path& exe);  // "\"<exe>\" --open-url \"%1\""
    bool isOurs(const std::string& scheme, const fs::path& exe) const;
    HandlerStatus status(const std::string& scheme, const fs::path& exe) const;
    // The consent text shown before a take-over (what is taken over, who owns it now, how to undo it).
    std::string consentText(const std::string& scheme, const fs::path& exe) const;

    Status registerFledge(const fs::path& exe);
    Status unregisterFledge(const fs::path& exe);  // only removes it when it points at this exe
    // Store scheme take-over. `consent` must come from the user's answer to consentText(); refuses otherwise.
    // Backs up the previous per-user values first (never twice: an existing backup is kept).
    Status takeOver(const std::string& scheme, const fs::path& exe, bool consent);
    // Puts the backed-up values back (or removes Fledge's per-user key when there were none) and deletes the backup.
    Status restore(const std::string& scheme, const fs::path& exe);
    Status restoreAll(const fs::path& exe);  // every store scheme with a backup or pointing at this exe
    // The command the scheme's original owner registered (backup, else the machine-wide value), "" = unknown.
    std::string originalCommand(const std::string& scheme) const;
    // Fledge and Fledge Advanced keep separate backups (HKCU\Software\Fledge\LinkHandlers and
    // HKCU\Software\Fledge Advanced\LinkHandlers). When both took over a scheme and the one that took it over first is
    // restored first, its backup (the real original handler) is handed to the other edition's backup instead of being
    // forgotten, so uninstalling the editions in any order ends with the original handler (docs/EDITIONS.md).
    void setOtherEditionBackup(RegistryStore* other) { m_otherBackup = other; }

private:
    RegistryStore* m_otherBackup = nullptr;
    RegistryStore& m_classes;
    RegistryStore& m_backup;
    const RegistryStore* m_merged;
    const RegistryStore* m_machine;
};

// The production stores (HKCU\Software\Classes, HKCU\Software\Fledge\LinkHandlers, HKCR, HKLM\Software\Classes).
// rootOverride (tests / FLEDGE_LINK_HANDLER_ROOT): use HKCU\<rootOverride>\Classes and ...\Backup instead, no merged view.
struct SystemLinkHandlers {
    std::unique_ptr<RegistryStore> classes, backup, merged, machine, otherBackup;
    std::unique_ptr<LinkHandlers> handlers;
};
SystemLinkHandlers openSystemLinkHandlers(const std::string& rootOverride = {});

// Launches a URL with a registered command line ("<exe>" ... "%1"): %1 is replaced by the URL, which must not contain
// quotes or control characters. Refuses commands that point back at Fledge (no loops).
Status launchWithCommand(const std::string& command, const std::string& url, const fs::path& selfExe);

// `Fledge --link-handlers <action> [scheme]`: status | register-fledge | unregister-fledge | restore <scheme> |
// restore-all | uninstall (restore-all + unregister-fledge). Take-over is only offered in Settings, behind its consent
// prompt. Prints a report; returns the process exit code.
int runLinkHandlerCli(const std::vector<std::string>& args, const fs::path& exe);

} // namespace fledge::editor
