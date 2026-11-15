#pragma once
// Provider presets: data that fills a ProviderConfig (URL, request format, auth style, key env var, model list).
//
// HONESTY NOTE: every URL, model id and env-var name below was written from memory and has NOT been verified against the
// provider's documentation or a live endpoint. ProviderPreset::verified is false for all of them. Check the provider docs
// (and use "Test connection" in Settings > AI Providers) before relying on a preset. docs/AI_HANDBOOK.md repeats this.
#include <string>
#include <vector>

#include "fledge/ai/llm.h"

namespace fledge::ai {

struct PresetModel {
    std::string id;
    std::string label;
    bool toolCapable = true;  // unknown = true (see note)
    std::string note;
};

struct ProviderPreset {
    std::string id;           // "vestra", "openrouter", ... (also the default credential id)
    std::string displayName;
    ProviderKind kind = ProviderKind::Custom;
    std::string baseUrl;      // e.g. https://ai.vestra.systems/v1
    std::string path = "/chat/completions";
    RequestFormat format = RequestFormat::OpenAIChat;
    AuthStyle auth = AuthStyle::Bearer;
    std::string envVar;       // key environment variable ("" = none)
    std::string credentialId; // credential-store id
    bool needsKey = true;
    bool local = false;       // runs on the user's machine / LAN
    bool verified = false;    // URL + models confirmed against the provider's docs (always false today)
    std::string defaultModel; // may be empty (user must pick)
    std::vector<PresetModel> models;
    std::string notes;

    const PresetModel* findModel(const std::string& id) const;
    // A ready ProviderConfig (kind, URLs, auth, credential id, model). model empty = defaultModel.
    ProviderConfig makeConfig(const std::string& model = {}) const;
};

const std::vector<ProviderPreset>& providerPresets();
const ProviderPreset* findPreset(const std::string& id);
// Preset whose credentialId equals this id, or nullptr.
const ProviderPreset* findPresetByCredentialId(const std::string& credentialId);

} // namespace fledge::ai
