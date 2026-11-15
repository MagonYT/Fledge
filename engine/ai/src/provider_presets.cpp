#include "fledge/ai/presets.h"

namespace fledge::ai {

const PresetModel* ProviderPreset::findModel(const std::string& mid) const {
    for (auto& m : models)
        if (m.id == mid) return &m;
    return nullptr;
}

ProviderConfig ProviderPreset::makeConfig(const std::string& model) const {
    ProviderConfig c;
    c.kind = kind;
    c.presetId = id;
    c.baseUrl = baseUrl;
    c.credentialId = credentialId;
    c.model = model.empty() ? defaultModel : model;
    c.custom.path = path;
    c.custom.format = format;
    c.custom.auth = needsKey ? auth : AuthStyle::None;
    const PresetModel* m = findModel(c.model);
    c.custom.toolsEnabled = m ? m->toolCapable : true;
    return c;
}

namespace {
const char* kVerify = "URL, model ids and env var written from memory: VERIFY AGAINST THE PROVIDER DOCS before relying on it.";

ProviderPreset make(const char* id, const char* name, const char* base, const char* env, bool local, const char* defModel,
                    std::vector<PresetModel> models, std::string notes) {
    ProviderPreset p;
    p.id = id;
    p.displayName = name;
    p.baseUrl = base;
    p.envVar = env;
    p.credentialId = id;
    p.needsKey = env[0] != '\0';
    p.local = local;
    p.defaultModel = defModel;
    p.models = std::move(models);
    p.notes = std::move(notes) + " " + kVerify;
    return p;
}

std::vector<ProviderPreset> buildPresets() {
    std::vector<ProviderPreset> v;
    v.push_back(make("vestra", "VestraAI", "https://ai.vestra.systems/v1", "VESTRA_API_KEY", false, "robot-1.1-coding",
                     {{"robot-1.1-coding", "robot-1.1-coding (tools)", true,
                       "The model that can use tools. Its tool-call wire format is UNCONFIRMED: Fledge assumes OpenAI-style "
                       "tools / tool_calls. If the assistant cannot call tools, the format differs."},
                      {"vestra", "vestra (chat only)", false, "Plain chat model, no tool access: the assistant can talk but cannot edit the scene."}},
                     "VestraAI, OpenAI-style POST /chat/completions with a Bearer key. Pick robot-1.1-coding for the assistant (the "
                     "'vestra' model has no tool access). The tool-call wire format of robot-1.1-coding is UNCONFIRMED (assumed OpenAI-style)."));
    v.push_back(make("openrouter", "OpenRouter", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", false, "anthropic/claude-sonnet-4.5",
                     {{"anthropic/claude-sonnet-4.5", "Claude Sonnet (via OpenRouter)", true, "example id, check the OpenRouter model list"},
                      {"openai/gpt-4o", "GPT-4o (via OpenRouter)", true, "example id, check the OpenRouter model list"},
                      {"meta-llama/llama-3.3-70b-instruct", "Llama 3.3 70B", true, "tool support varies by routed provider"}},
                     "OpenRouter aggregator. Model ids look like vendor/model; tool support depends on the model and routed provider."));
    v.push_back(make("9router", "9Router (self-hosted gateway)", "http://localhost:20128/v1", "NINEROUTER_API_KEY", true, "",
                     {}, "Self-hosted OpenAI-compatible gateway at <host>/v1, default port 20128. The local URL and default port are "
                         "UNCONFIRMED: edit the Base URL to match your install. It needs a 9Router key. Enter the model or combo name "
                         "configured in your 9Router (no default). Tool support depends on the model it routes to."));
    v.push_back(make("groq", "Groq", "https://api.groq.com/openai/v1", "GROQ_API_KEY", false, "llama-3.3-70b-versatile",
                     {{"llama-3.3-70b-versatile", "Llama 3.3 70B Versatile", true, "example id"}}, "Groq OpenAI-compatible API."));
    v.push_back(make("together", "Together AI", "https://api.together.xyz/v1", "TOGETHER_API_KEY", false, "meta-llama/Llama-3.3-70B-Instruct-Turbo",
                     {{"meta-llama/Llama-3.3-70B-Instruct-Turbo", "Llama 3.3 70B Turbo", true, "example id"}}, "Together AI OpenAI-compatible API."));
    v.push_back(make("mistral", "Mistral", "https://api.mistral.ai/v1", "MISTRAL_API_KEY", false, "mistral-large-latest",
                     {{"mistral-large-latest", "Mistral Large", true, "example id"}, {"codestral-latest", "Codestral", true, "tool support unknown"}},
                     "Mistral La Plateforme."));
    v.push_back(make("deepseek", "DeepSeek", "https://api.deepseek.com", "DEEPSEEK_API_KEY", false, "deepseek-chat",
                     {{"deepseek-chat", "deepseek-chat", true, "example id"},
                      {"deepseek-reasoner", "deepseek-reasoner", true, "tool support unknown; may reject tools"}},
                     "DeepSeek API (base has no /v1 here)."));
    v.push_back(make("xai", "xAI", "https://api.x.ai/v1", "XAI_API_KEY", false, "grok-4", {{"grok-4", "Grok 4", true, "example id"}},
                     "xAI Grok API."));
    v.push_back(make("gemini", "Google Gemini (OpenAI-compatible)", "https://generativelanguage.googleapis.com/v1beta/openai/", "GEMINI_API_KEY",
                     false, "gemini-2.5-flash",
                     {{"gemini-2.5-flash", "Gemini 2.5 Flash", true, "example id"}, {"gemini-2.5-pro", "Gemini 2.5 Pro", true, "example id"}},
                     "Gemini through Google's OpenAI-compatibility endpoint."));
    v.push_back(make("ollama", "Ollama (local)", "http://localhost:11434/v1", "", true, "llama3.1",
                     {{"llama3.1", "llama3.1", true, "tool support depends on the model"}, {"qwen2.5-coder", "qwen2.5-coder", true, "tool support depends on the model"}},
                     "Local Ollama server, no key. Models must be pulled first."));
    v.push_back(make("lmstudio", "LM Studio (local)", "http://localhost:1234/v1", "", true, "local-model",
                     {{"local-model", "whatever model is loaded", true, "tool support depends on the loaded model"}},
                     "LM Studio local server, no key. Start the server in LM Studio first."));
    return v;
}
} // namespace

const std::vector<ProviderPreset>& providerPresets() {
    static const std::vector<ProviderPreset> presets = buildPresets();
    return presets;
}

const ProviderPreset* findPreset(const std::string& id) {
    for (auto& p : providerPresets())
        if (p.id == id) return &p;
    return nullptr;
}

const ProviderPreset* findPresetByCredentialId(const std::string& cid) {
    for (auto& p : providerPresets())
        if (p.credentialId == cid) return &p;
    return nullptr;
}

} // namespace fledge::ai
