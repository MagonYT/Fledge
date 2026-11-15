#pragma once
// Credential storage for LLM provider keys/tokens.
//
// Rules (see docs/AI_PROVIDERS.md):
//  - Keys live in the OS credential vault (Windows Credential Manager,
//    target "Fledge/llm/<provider>") or in environment variables.
//  - Keys are NEVER written to project files, scenes, logs, paks or packaged
//    games. Every key that is loaded is registered with fledge::Redactor so it
//    is masked if it ever reaches a log line or tool result.
//  - The shipping runtime does not link this code at all.
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "fledge/core/utility.h"

namespace fledge::ai {

class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    virtual std::optional<std::string> get(const std::string& provider) = 0;
    virtual Status set(const std::string& provider, const std::string& secret) = 0;
    virtual Status remove(const std::string& provider) = 0;
    virtual std::string backendName() const = 0;
};

// Windows Credential Manager (CredReadW/CredWriteW, CRED_TYPE_GENERIC,
// CRED_PERSIST_LOCAL_MACHINE). Protected by the user's Windows logon (DPAPI).
std::unique_ptr<CredentialStore> createWindowsCredentialStore();

// In-memory store for tests / ephemeral sessions.
class MemoryCredentialStore final : public CredentialStore {
public:
    std::optional<std::string> get(const std::string& p) override {
        auto it = m_map.find(p);
        if (it == m_map.end()) return std::nullopt;
        return it->second;
    }
    Status set(const std::string& p, const std::string& s) override { m_map[p] = s; return {}; }
    Status remove(const std::string& p) override { m_map.erase(p); return {}; }
    std::string backendName() const override { return "memory"; }

private:
    std::unordered_map<std::string, std::string> m_map;
};

// Default platform store (Credential Manager on Windows, memory elsewhere).
std::unique_ptr<CredentialStore> createDefaultCredentialStore();

// Environment variable consulted for a provider: anthropic -> ANTHROPIC_API_KEY,
// openai -> OPENAI_API_KEY, a preset's credential id (vestra, openrouter, ...) -> the preset's
// variable (VESTRA_API_KEY, ...), anything else -> FLEDGE_<UPPER>_API_KEY.
std::string envVarForProvider(const std::string& provider);

enum class KeySource { None, Store, Environment };

struct ResolvedKey {
    std::string key;
    KeySource source = KeySource::None;
};

// Store first, then environment. Registers the key with the Redactor.
ResolvedKey resolveApiKey(CredentialStore& store, const std::string& provider);

// "sk-ant-...abcd" style hint for UIs (never the full key).
std::string maskKey(const std::string& key);

} // namespace fledge::ai
