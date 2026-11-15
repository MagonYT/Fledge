#include "fledge/ai/credentials.h"

#include <cctype>
#include <cstdlib>

#include "fledge/ai/presets.h"
#include "fledge/core/redact.h"

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#endif

namespace fledge::ai {

std::string envVarForProvider(const std::string& provider) {
    if (provider == "anthropic") return "ANTHROPIC_API_KEY";
    if (provider == "openai") return "OPENAI_API_KEY";
    if (const ProviderPreset* p = findPresetByCredentialId(provider); p && !p->envVar.empty()) return p->envVar;
    std::string up;
    for (char c : provider) up += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
    return "FLEDGE_" + up + "_API_KEY";
}

std::string maskKey(const std::string& key) {
    if (key.size() < 12) return "****";
    return key.substr(0, 6) + "..." + key.substr(key.size() - 4);
}

ResolvedKey resolveApiKey(CredentialStore& store, const std::string& provider) {
    ResolvedKey r;
    if (auto k = store.get(provider); k && !k->empty()) {
        r = {*k, KeySource::Store};
    } else if (const char* env = std::getenv(envVarForProvider(provider).c_str()); env && *env) {
        r = {env, KeySource::Environment};
    }
    if (!r.key.empty()) Redactor::get().registerSecret(r.key);
    return r;
}

#ifdef _WIN32
namespace {
class WindowsCredentialStore final : public CredentialStore {
public:
    static std::wstring target(const std::string& provider) { return widen("Fledge/llm/" + provider); }

    std::optional<std::string> get(const std::string& provider) override {
        PCREDENTIALW cred = nullptr;
        if (!CredReadW(target(provider).c_str(), CRED_TYPE_GENERIC, 0, &cred)) return std::nullopt;
        std::string s(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
        CredFree(cred);
        return s;
    }
    Status set(const std::string& provider, const std::string& secret) override {
        std::wstring t = target(provider);
        std::wstring user = L"fledge";
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = t.data();
        c.CredentialBlobSize = static_cast<DWORD>(secret.size());
        c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        c.UserName = user.data();
        if (!CredWriteW(&c, 0)) return Status::failure("CredWrite failed");
        Redactor::get().registerSecret(secret);
        return {};
    }
    Status remove(const std::string& provider) override {
        if (!CredDeleteW(target(provider).c_str(), CRED_TYPE_GENERIC, 0)) return Status::failure("no stored credential");
        return {};
    }
    std::string backendName() const override { return "Windows Credential Manager"; }
};
} // namespace

std::unique_ptr<CredentialStore> createWindowsCredentialStore() { return std::make_unique<WindowsCredentialStore>(); }
std::unique_ptr<CredentialStore> createDefaultCredentialStore() { return createWindowsCredentialStore(); }
#else
std::unique_ptr<CredentialStore> createWindowsCredentialStore() { return nullptr; }
std::unique_ptr<CredentialStore> createDefaultCredentialStore() { return std::make_unique<MemoryCredentialStore>(); }
#endif

} // namespace fledge::ai
