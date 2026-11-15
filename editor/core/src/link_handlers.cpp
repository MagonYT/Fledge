// Per-user URL protocol registration, opt-in store scheme take-over with backup / restore (see link_handlers.h).
#include "fledge/editor/link_handlers.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>

#include "fledge/core/features.h"
#include "fledge/core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fledge::editor {

// ------------------------------------------------------------ tree export ----
Json RegistryStore::exportTree(const std::string& key) const {
    Json values = Json::object(), keys = Json::object();
    for (const auto& n : valueNames(key))
        if (auto v = getString(key, n)) values[n] = *v;
    for (const auto& s : subkeys(key)) keys[s] = exportTree(key + "\\" + s);
    return {{"values", values}, {"keys", keys}};
}

bool RegistryStore::importTree(const std::string& key, const Json& tree) {
    if (!tree.is_object()) return false;
    bool ok = true;
    if (tree.contains("values") && tree["values"].is_object())
        for (auto& [n, v] : tree["values"].items())
            if (v.is_string()) ok &= setString(key, n, v.get<std::string>());
    if (!tree.contains("values") || tree["values"].empty()) ok &= createKey(key);
    if (tree.contains("keys") && tree["keys"].is_object())
        for (auto& [n, sub] : tree["keys"].items()) ok &= importTree(key + "\\" + n, sub);
    return ok;
}

// --------------------------------------------------------------- memory ----
namespace {
std::string lowerKey(std::string k) {
    for (auto& c : k) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!k.empty() && k.back() == '\\') k.pop_back();
    return k;
}
} // namespace

bool MemoryRegistry::keyExists(const std::string& key) const { return m_keys.contains(lowerKey(key)); }
std::optional<std::string> MemoryRegistry::getString(const std::string& key, const std::string& name) const {
    auto it = m_keys.find(lowerKey(key));
    if (it == m_keys.end()) return std::nullopt;
    auto v = it->second.find(toLower(name));
    if (v == it->second.end()) return std::nullopt;
    return v->second;
}
bool MemoryRegistry::setString(const std::string& key, const std::string& name, const std::string& value) {
    std::string k = lowerKey(key);
    for (size_t p = k.find('\\'); p != std::string::npos; p = k.find('\\', p + 1)) m_keys[k.substr(0, p)];  // parents exist
    m_keys[k][toLower(name)] = value;
    return true;
}
bool MemoryRegistry::createKey(const std::string& key) {
    std::string k = lowerKey(key);
    for (size_t p = k.find('\\'); p != std::string::npos; p = k.find('\\', p + 1)) m_keys[k.substr(0, p)];
    m_keys[k];
    return true;
}
bool MemoryRegistry::deleteTree(const std::string& key) {
    std::string k = lowerKey(key);
    for (auto it = m_keys.begin(); it != m_keys.end();)
        it = (it->first == k || it->first.rfind(k + "\\", 0) == 0) ? m_keys.erase(it) : std::next(it);
    return true;
}
std::vector<std::string> MemoryRegistry::subkeys(const std::string& key) const {
    std::string k = lowerKey(key) + "\\";
    std::vector<std::string> out;
    for (auto& [path, v] : m_keys)
        if (path.rfind(k, 0) == 0 && path.find('\\', k.size()) == std::string::npos) out.push_back(path.substr(k.size()));
    return out;
}
std::vector<std::string> MemoryRegistry::valueNames(const std::string& key) const {
    std::vector<std::string> out;
    if (auto it = m_keys.find(lowerKey(key)); it != m_keys.end())
        for (auto& [n, v] : it->second) out.push_back(n);
    return out;
}

// ---------------------------------------------------------------- win32 ----
#ifdef _WIN32
Win32Registry::Win32Registry(std::string root, bool readOnlyMerged, bool machine)
    : m_root(std::move(root)), m_readOnly(readOnlyMerged || machine), m_machine(machine), m_merged(readOnlyMerged) {}

std::wstring Win32Registry::full(const std::string& key) const {
    std::string k = key;
    for (auto& c : k)
        if (c == '/') c = '\\';
    return widen(m_root.empty() ? k : (k.empty() ? m_root : m_root + "\\" + k));
}
namespace {
HKEY rootHive(bool merged, bool machine) { return merged ? HKEY_CLASSES_ROOT : machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }
} // namespace

bool Win32Registry::keyExists(const std::string& key) const {
    HKEY h;
    if (RegOpenKeyExW(rootHive(m_merged, m_machine), full(key).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}
std::optional<std::string> Win32Registry::getString(const std::string& key, const std::string& name) const {
    HKEY h;
    if (RegOpenKeyExW(rootHive(m_merged, m_machine), full(key).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return std::nullopt;
    std::wstring wname = widen(name);
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(h, wname.empty() ? nullptr : wname.c_str(), nullptr, &type, nullptr, &size);
    std::optional<std::string> out;
    if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size < (1u << 20)) {
        std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
        DWORD sz = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
        if (RegQueryValueExW(h, wname.empty() ? nullptr : wname.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &sz) == ERROR_SUCCESS) {
            buf.resize(wcsnlen(buf.c_str(), buf.size()));
            out = narrow(buf);
        }
    }
    RegCloseKey(h);
    return out;
}
bool Win32Registry::setString(const std::string& key, const std::string& name, const std::string& value) {
    if (m_readOnly) return false;
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, full(key).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    std::wstring wname = widen(name), wv = widen(value);
    LONG r = RegSetValueExW(h, wname.empty() ? nullptr : wname.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(wv.c_str()),
                            static_cast<DWORD>((wv.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}
bool Win32Registry::createKey(const std::string& key) {
    if (m_readOnly) return false;
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, full(key).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}
bool Win32Registry::deleteTree(const std::string& key) {
    if (m_readOnly || key.empty()) return false;  // never the store's root itself
    LONG r = RegDeleteTreeW(HKEY_CURRENT_USER, full(key).c_str());
    if (r == ERROR_SUCCESS) RegDeleteKeyW(HKEY_CURRENT_USER, full(key).c_str());
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}
std::vector<std::string> Win32Registry::subkeys(const std::string& key) const {
    std::vector<std::string> out;
    HKEY h;
    if (RegOpenKeyExW(rootHive(m_merged, m_machine), full(key).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(narrow(std::wstring(name, len)));
        if (out.size() > 256) break;
    }
    RegCloseKey(h);
    return out;
}
std::vector<std::string> Win32Registry::valueNames(const std::string& key) const {
    std::vector<std::string> out;
    HKEY h;
    if (RegOpenKeyExW(rootHive(m_merged, m_machine), full(key).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
    wchar_t name[16384];
    for (DWORD i = 0;; ++i) {
        DWORD len = 16384, type = 0;
        if (RegEnumValueW(h, i, name, &len, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (type == REG_SZ || type == REG_EXPAND_SZ) out.push_back(narrow(std::wstring(name, len)));
        if (out.size() > 256) break;
    }
    RegCloseKey(h);
    return out;
}
#endif

// -------------------------------------------------------------- handlers ----
const std::vector<std::string>& LinkHandlers::storeSchemes() {
    static const std::vector<std::string> s = {kUnity, kEpic};
    return s;
}

LinkHandlers::LinkHandlers(RegistryStore& classes, RegistryStore& backup, const RegistryStore* merged, const RegistryStore* machine)
    : m_classes(classes), m_backup(backup), m_merged(merged), m_machine(machine) {}

std::string LinkHandlers::commandFor(const fs::path& exe) { return "\"" + narrow(exe.wstring()) + "\" --open-url \"%1\""; }

bool LinkHandlers::isOurs(const std::string& scheme, const fs::path& exe) const {
    auto cmd = m_classes.getString(scheme + "\\shell\\open\\command", "");
    if (!cmd) return false;
    std::string c = toLower(*cmd), e = toLower(narrow(exe.wstring()));
    return c.find(e) != std::string::npos && c.find("--open-url") != std::string::npos;
}

namespace {
struct SchemeInfo {
    const char* title;
    const char* owner;
};
SchemeInfo infoFor(const std::string& s) {
    if (s == LinkHandlers::kUnity) return {"Unity Asset Store links (\"Open in Unity\", com.unity3d.kharma:)", "the Unity Editor / Unity Hub"};
    if (s == LinkHandlers::kEpic) return {"Epic Games Launcher links (Fab \"Open in Launcher\", com.epicgames.launcher://)", "the Epic Games Launcher"};
    if (s == LinkHandlers::kFledge) return {"Fledge import links (fledge://)", "Fledge"};
    return {"links", "another program"};
}
std::string today() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}
// Case-insensitive lookup in an exported tree.
const Json* child(const Json& tree, const std::string& name) {
    if (!tree.is_object() || !tree.contains("keys")) return nullptr;
    for (auto& [k, v] : tree["keys"].items())
        if (toLower(k) == toLower(name)) return &v;
    return nullptr;
}
} // namespace

std::string LinkHandlers::originalCommand(const std::string& scheme) const {
    if (auto existed = m_backup.getString(scheme, "existed"); existed && *existed == "1") {
        Json tree = Json::parse(m_backup.getString(scheme, "tree").value_or("{}"), nullptr, false);
        const Json* sh = child(tree, "shell");
        const Json* op = sh ? child(*sh, "open") : nullptr;
        const Json* cm = op ? child(*op, "command") : nullptr;
        if (cm && cm->contains("values"))
            for (auto& [n, v] : (*cm)["values"].items())
                if (n.empty() && v.is_string()) return v.get<std::string>();
    }
    if (m_machine)
        if (auto c = m_machine->getString(scheme + "\\shell\\open\\command", "")) return *c;
    return {};
}

HandlerStatus LinkHandlers::status(const std::string& scheme, const fs::path& exe) const {
    HandlerStatus s;
    s.scheme = scheme;
    SchemeInfo i = infoFor(scheme);
    s.title = i.title;
    s.normalOwner = i.owner;
    s.ours = isOurs(scheme, exe);
    s.backupExists = m_backup.keyExists(scheme);
    s.perUserBefore = m_backup.getString(scheme, "existed").value_or("0") == "1";
    const RegistryStore& view = m_merged ? *m_merged : m_classes;
    s.currentCommand = view.getString(scheme + "\\shell\\open\\command", "").value_or("");
    return s;
}

std::string LinkHandlers::consentText(const std::string& scheme, const fs::path& exe) const {
    HandlerStatus s = status(scheme, exe);
    SchemeInfo i = infoFor(scheme);
    std::string now = s.ours ? "Fledge (already taken over)" : s.currentCommand.empty() ? "nothing (no program is registered)" : s.currentCommand;
    std::string text = std::format(
        "Fledge will open {} instead of {}.\n\n"
        "What changes: a per-user entry in HKEY_CURRENT_USER\\Software\\Classes\\{} pointing at\n  {}\n"
        "Only your Windows account is affected; nothing machine-wide changes and no administrator rights are used.\n\n"
        "Today these links are opened by:\n  {}\n\n"
        "Before anything changes, Fledge backs up your current per-user values. Links Fledge cannot import are handed to the "
        "original program, and every Asset Store Import card has \"Open with the original app\".\n\n"
        "To undo: Settings > Asset Store Import > Restore original handlers, the uninstaller, or\n"
        "  Fledge --link-handlers restore-all",
        i.title, i.owner, scheme, commandFor(exe), now);
    if (scheme == kEpic)
        text += "\n\nNote: the Epic Games Launcher also uses these links to start games; Fledge forwards every link that is not an "
                "asset link straight to the launcher.";
    return text;
}

Status LinkHandlers::registerFledge(const fs::path& exe) {
    if (!m_classes.keyExists(kFledge) || !isOurs(kFledge, exe)) {
        if (m_classes.keyExists(kFledge) && !isOurs(kFledge, exe)) {
            // another Fledge install owns it: replace (it is Fledge's own scheme), keeping a note of the old command
            m_backup.setString(kFledge, "previousCommand", m_classes.getString(std::string(kFledge) + "\\shell\\open\\command", "").value_or(""));
            m_classes.deleteTree(kFledge);
        }
    }
    bool ok = m_classes.setString(kFledge, "", "URL:Fledge import link") && m_classes.setString(kFledge, "URL Protocol", "") &&
              m_classes.setString(std::string(kFledge) + "\\DefaultIcon", "", "\"" + narrow(exe.wstring()) + "\",0") &&
              m_classes.setString(std::string(kFledge) + "\\shell\\open\\command", "", commandFor(exe));
    if (!ok || !isOurs(kFledge, exe)) return Status::failure("could not write the per-user fledge:// handler");
    FLEDGE_LOG_INFO("links", "Registered fledge:// for this user -> {}", narrow(exe.wstring()));
    return {};
}

Status LinkHandlers::unregisterFledge(const fs::path& exe) {
    if (!m_classes.keyExists(kFledge)) return {};
    if (!isOurs(kFledge, exe)) return Status::failure("fledge:// belongs to another Fledge install; left unchanged");
    m_classes.deleteTree(kFledge);
    m_backup.deleteTree(kFledge);
    FLEDGE_LOG_INFO("links", "Removed the per-user fledge:// handler");
    return {};
}

Status LinkHandlers::takeOver(const std::string& scheme, const fs::path& exe, bool consent) {
    if (std::find(storeSchemes().begin(), storeSchemes().end(), scheme) == storeSchemes().end())
        return Status::failure("'" + scheme + "' is not a store link scheme Fledge can take over");
    if (!consent) return Status::failure("taking over " + scheme + " needs the user's consent (Settings > Asset Store Import)");
    if (isOurs(scheme, exe)) return {};
    if (!m_backup.keyExists(scheme)) {  // never overwrite an existing backup (it holds the true original)
        bool existed = m_classes.keyExists(scheme);
        Json tree = existed ? m_classes.exportTree(scheme) : Json();
        bool ok = m_backup.setString(scheme, "tree", tree.dump()) && m_backup.setString(scheme, "takenAt", today()) &&
                  m_backup.setString(scheme, "existed", existed ? "1" : "0");
        if (!ok || m_backup.getString(scheme, "existed").value_or("") != (existed ? "1" : "0"))
            return Status::failure("the backup of the current handler could not be written; nothing was changed");
    }
    m_classes.deleteTree(scheme);
    bool ok = m_classes.setString(scheme, "", std::string("URL:") + scheme + " (opened by Fledge)") && m_classes.setString(scheme, "URL Protocol", "") &&
              m_classes.setString(scheme, "FledgeTakeOver", "1") &&
              m_classes.setString(scheme + "\\DefaultIcon", "", "\"" + narrow(exe.wstring()) + "\",0") &&
              m_classes.setString(scheme + "\\shell\\open\\command", "", commandFor(exe));
    if (!ok || !isOurs(scheme, exe)) {
        (void)restore(scheme, exe);
        return Status::failure("could not write the per-user handler for " + scheme + "; the previous one was restored");
    }
    FLEDGE_LOG_INFO("links", "Took over {} for this user (backup stored)", scheme);
    return {};
}

Status LinkHandlers::restore(const std::string& scheme, const fs::path& exe) {
    if (!m_backup.keyExists(scheme)) {
        if (isOurs(scheme, exe)) m_classes.deleteTree(scheme);  // no backup: there were no per-user values before
        return {};
    }
    bool existed = m_backup.getString(scheme, "existed").value_or("0") == "1";
    Json tree = Json::parse(m_backup.getString(scheme, "tree").value_or("null"), nullptr, false);
    if (m_classes.keyExists(scheme) && !isOurs(scheme, exe)) {
        // another program registered itself after the take-over: keep its values, just forget our backup - unless that
        // program is the other Fledge edition, whose backup then holds OUR command: give it the real original instead
        if (m_otherBackup && m_otherBackup->keyExists(scheme)) {
            std::string otherTree = m_otherBackup->getString(scheme, "tree").value_or("");
            std::string me = Json(narrow(exe.wstring())).dump();
            me = me.size() >= 2 ? me.substr(1, me.size() - 2) : me;  // the exe path as it appears inside the JSON tree
            if (!me.empty() && otherTree.find(me) != std::string::npos) {
                m_otherBackup->setString(scheme, "existed", m_backup.getString(scheme, "existed").value_or("0"));
                m_otherBackup->setString(scheme, "tree", m_backup.getString(scheme, "tree").value_or("null"));
                FLEDGE_LOG_INFO("links", "{}: handed the original handler to the other Fledge edition's backup", scheme);
            }
        }
        m_backup.deleteTree(scheme);
        FLEDGE_LOG_WARN("links", "{} was changed by another program since the take-over; left as is", scheme);
        return {};
    }
    m_classes.deleteTree(scheme);
    if (existed && tree.is_object() && !m_classes.importTree(scheme, tree))
        return Status::failure("could not restore the backed-up values of " + scheme + " (the backup is kept)");
    m_backup.deleteTree(scheme);
    FLEDGE_LOG_INFO("links", "Restored the original handler for {}", scheme);
    return {};
}

Status LinkHandlers::restoreAll(const fs::path& exe) {
    std::string errors;
    for (const auto& s : storeSchemes())
        if (Status st = restore(s, exe); !st) errors += st.error + "; ";
    return errors.empty() ? Status::success() : Status::failure(errors);
}

SystemLinkHandlers openSystemLinkHandlers(const std::string& rootOverride) {
    SystemLinkHandlers s;
    std::string root = rootOverride;
    if (root.empty())
        if (const char* env = std::getenv("FLEDGE_LINK_HANDLER_ROOT"); env && *env) root = env;
#ifdef _WIN32
    if (!root.empty()) {
        s.classes = std::make_unique<Win32Registry>(root + "\\Classes");
        s.backup = std::make_unique<Win32Registry>(root + "\\Backup");
    } else {
        s.classes = std::make_unique<Win32Registry>("Software\\Classes");
        // per edition: HKCU\Software\Fledge\LinkHandlers or HKCU\Software\Fledge Advanced\LinkHandlers
        Edition me = currentEdition(), other = me == Edition::Advanced ? Edition::Standard : Edition::Advanced;
        s.backup = std::make_unique<Win32Registry>(std::string("Software\\") + editionUserDataFolder(me) + "\\LinkHandlers");
        s.otherBackup = std::make_unique<Win32Registry>(std::string("Software\\") + editionUserDataFolder(other) + "\\LinkHandlers");
        s.merged = std::make_unique<Win32Registry>("", true);
        s.machine = std::make_unique<Win32Registry>("Software\\Classes", false, true);
    }
#else
    s.classes = std::make_unique<MemoryRegistry>();
    s.backup = std::make_unique<MemoryRegistry>();
#endif
    s.handlers = std::make_unique<LinkHandlers>(*s.classes, *s.backup, s.merged.get(), s.machine.get());
    s.handlers->setOtherEditionBackup(s.otherBackup.get());
    return s;
}

Status launchWithCommand(const std::string& command, const std::string& url, const fs::path& selfExe) {
    if (command.empty()) return Status::failure("no original program is registered for this link");
    if (url.size() > 4096) return Status::failure("link too long");
    for (char c : url)
        if (c == '"' || static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return Status::failure("the link contains characters that cannot be passed on safely");
    std::string lc = toLower(command);
    if (lc.find("--open-url") != std::string::npos || lc.find(toLower(narrow(selfExe.wstring()))) != std::string::npos)
        return Status::failure("the original handler is Fledge itself (nothing to forward to)");
    std::string cmd = command;
    bool replaced = false;
    for (const char* ph : {"%1", "%l", "%L"})
        for (size_t p; (p = cmd.find(ph)) != std::string::npos;) {
            cmd.replace(p, 2, url);
            replaced = true;
        }
    if (!replaced) cmd += " \"" + url + "\"";
#ifdef _WIN32
    std::wstring w = widen(cmd);
    wchar_t expanded[32768];
    DWORD n = ExpandEnvironmentStringsW(w.c_str(), expanded, 32768);
    std::wstring line = (n > 0 && n <= 32768) ? std::wstring(expanded) : w;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return Status::failure(std::format("could not start the original program (error {})", GetLastError()));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
#else
    return Status::failure("not supported on this platform");
#endif
}

int runLinkHandlerCli(const std::vector<std::string>& args, const fs::path& exe) {
    auto sys = openSystemLinkHandlers();
    LinkHandlers& h = *sys.handlers;
    std::string action = args.empty() ? "status" : args[0];
    auto print = [&](const std::string& scheme) {
        HandlerStatus s = h.status(scheme, exe);
        std::printf("%-24s %s%s\n    now: %s\n", scheme.c_str(), s.ours ? "-> this Fledge" : "-> not Fledge",
                    s.backupExists ? " (backup stored)" : "", s.currentCommand.empty() ? "(none)" : s.currentCommand.c_str());
    };
    Status st;
    if (action == "status") {
        print(LinkHandlers::kFledge);
        for (auto& s : LinkHandlers::storeSchemes()) print(s);
        return 0;
    } else if (action == "register-fledge") {
        st = h.registerFledge(exe);
    } else if (action == "unregister-fledge") {
        st = h.unregisterFledge(exe);
    } else if (action == "restore" && args.size() >= 2) {
        st = h.restore(args[1], exe);
    } else if (action == "restore-all") {
        st = h.restoreAll(exe);
    } else if (action == "uninstall") {
        st = h.restoreAll(exe);
        Status u = h.unregisterFledge(exe);
        if (st && !u) st = u;
    } else {
        std::printf("usage: Fledge --link-handlers status | register-fledge | unregister-fledge | restore <scheme> | restore-all | uninstall\n"
                    "(taking over store links is only offered in Settings > Asset Store Import, behind a consent prompt)\n");
        return 2;
    }
    std::printf("%s: %s\n", action.c_str(), st ? "OK" : st.error.c_str());
    std::fflush(stdout);
    return st ? 0 : 1;
}

} // namespace fledge::editor
