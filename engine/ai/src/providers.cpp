#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

#include "fledge/ai/llm.h"
#include "fledge/ai/presets.h"
#include "fledge/core/redact.h"

namespace fledge::ai {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string stripTrailingSlashes(std::string b) {
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

struct UrlParts {
    std::string scheme, host;
    bool hasCredentials = false, hasQuery = false, valid = false;
};
UrlParts parseUrl(const std::string& url) {
    UrlParts u;
    size_t p = url.find("://");
    if (p == std::string::npos) return u;
    u.scheme = lower(url.substr(0, p));
    if (u.scheme != "http" && u.scheme != "https") return u;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return u;  // spaces / control characters (incl. CR LF)
    size_t a = p + 3;
    size_t e = url.find_first_of("/?#", a);
    std::string auth = url.substr(a, e == std::string::npos ? std::string::npos : e - a);
    u.hasQuery = url.find('?', a) != std::string::npos;
    if (size_t at = auth.rfind('@'); at != std::string::npos) {
        u.hasCredentials = true;
        auth = auth.substr(at + 1);
    }
    if (!auth.empty() && auth[0] == '[') {
        size_t rb = auth.find(']');
        if (rb == std::string::npos) return u;
        u.host = auth.substr(1, rb - 1);
    } else {
        u.host = auth.substr(0, auth.find(':'));
    }
    u.host = lower(u.host);
    u.valid = !u.host.empty();
    return u;
}

bool isTokenChar(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}
bool validHeaderName(const std::string& n) {
    return !n.empty() && n.size() <= 128 && std::all_of(n.begin(), n.end(), isTokenChar);
}
bool validHeaderValue(const std::string& v) {  // no CR / LF / NUL / other control chars (tab allowed)
    return v.size() <= 4096 && std::all_of(v.begin(), v.end(), [](char c) {
        unsigned char u = static_cast<unsigned char>(c);
        return u == '\t' || (u >= 0x20 && u != 0x7f);
    });
}
bool looksSecretName(const std::string& n) { return looksLikeSecretName(n); }

const char* defaultPath(RequestFormat f) { return f == RequestFormat::AnthropicMessages ? "/v1/messages" : "/chat/completions"; }
} // namespace

bool looksLikeSecretName(const std::string& n) {
    std::string l = lower(n);
    for (const char* w : {"key", "token", "secret", "password", "passwd", "auth", "cookie", "signature"})
        if (l.find(w) != std::string::npos) return true;
    return false;
}

const char* toString(ProviderKind k) {
    switch (k) {
        case ProviderKind::Anthropic: return "anthropic";
        case ProviderKind::OpenAI: return "openai";
        case ProviderKind::OpenAICompatible: return "openai-compatible";
        case ProviderKind::Custom: return "custom";
        case ProviderKind::ClaudeCode: return "claude-code";
    }
    return "?";
}

ProviderKind providerKindFromString(const std::string& s) {
    if (s == "openai") return ProviderKind::OpenAI;
    if (s == "openai-compatible") return ProviderKind::OpenAICompatible;
    if (s == "custom") return ProviderKind::Custom;
    if (s == "claude-code") return ProviderKind::ClaudeCode;
    return ProviderKind::Anthropic;
}

const char* toString(RequestFormat f) { return f == RequestFormat::AnthropicMessages ? "anthropic-messages" : "openai-chat"; }
const char* toString(AuthStyle a) {
    switch (a) {
        case AuthStyle::Bearer: return "bearer";
        case AuthStyle::XApiKey: return "x-api-key";
        case AuthStyle::Header: return "header";
        case AuthStyle::None: return "none";
    }
    return "bearer";
}
const char* toString(MaxTokensField f) {
    switch (f) {
        case MaxTokensField::MaxTokens: return "max_tokens";
        case MaxTokensField::MaxCompletionTokens: return "max_completion_tokens";
        case MaxTokensField::None: return "none";
    }
    return "max_tokens";
}
RequestFormat requestFormatFromString(const std::string& s) {
    return s == "anthropic-messages" || s == "anthropic" ? RequestFormat::AnthropicMessages : RequestFormat::OpenAIChat;
}
AuthStyle authStyleFromString(const std::string& s) {
    if (s == "x-api-key") return AuthStyle::XApiKey;
    if (s == "header") return AuthStyle::Header;
    if (s == "none") return AuthStyle::None;
    return AuthStyle::Bearer;
}
MaxTokensField maxTokensFieldFromString(const std::string& s) {
    if (s == "max_completion_tokens") return MaxTokensField::MaxCompletionTokens;
    if (s == "none") return MaxTokensField::None;
    return MaxTokensField::MaxTokens;
}

std::string ProviderConfig::resolvedCredentialId() const { return credentialId.empty() ? toString(kind) : credentialId; }

std::string ProviderConfig::resolvedModel() const {
    if (!model.empty()) return model;
    switch (kind) {
        case ProviderKind::Anthropic: return "claude-opus-5-5";
        case ProviderKind::OpenAI: return "gpt-5";          // placeholder default - set your model in Settings
        case ProviderKind::OpenAICompatible: return "llama3.1";  // e.g. an Ollama model name
        case ProviderKind::Custom: return {};  // no default: set the model your endpoint expects
        case ProviderKind::ClaudeCode: return {};  // empty = whatever model Claude Code is set to use
    }
    return {};
}

std::string ProviderConfig::resolvedBaseUrl() const {
    std::string b = trim(baseUrl);
    if (b.empty()) {
        switch (kind) {
            case ProviderKind::Anthropic: b = "https://api.anthropic.com"; break;
            case ProviderKind::OpenAI: b = "https://api.openai.com/v1"; break;
            case ProviderKind::OpenAICompatible: b = "http://localhost:11434/v1"; break;
            case ProviderKind::Custom: break;
            case ProviderKind::ClaudeCode: break;
        }
    }
    return stripTrailingSlashes(b);
}

std::string ProviderConfig::requestUrl() const {
    switch (kind) {
        case ProviderKind::Anthropic: return resolvedBaseUrl() + "/v1/messages";
        case ProviderKind::OpenAI:
        case ProviderKind::OpenAICompatible: return resolvedBaseUrl() + "/chat/completions";
        case ProviderKind::Custom: break;
        case ProviderKind::ClaudeCode: return {};  // no HTTP request leaves the editor
    }
    std::string full = trim(custom.url);
    if (!full.empty()) return full;
    std::string base = resolvedBaseUrl();
    if (base.empty()) return {};
    std::string path = trim(custom.path);
    if (path.empty()) path = defaultPath(custom.format);
    if (path[0] != '/') path = "/" + path;
    return base + path;
}

bool ProviderConfig::needsKey() const {
    switch (kind) {
        case ProviderKind::Anthropic:
        case ProviderKind::OpenAI: return true;
        case ProviderKind::OpenAICompatible: return false;  // local servers (Ollama / LM Studio) usually need none; a stored key is still sent
        case ProviderKind::Custom: return custom.auth != AuthStyle::None;
        case ProviderKind::ClaudeCode: return false;  // Claude Code holds its own sign-in; the editor never sees it
    }
    return true;
}

bool ProviderConfig::supportsTools() const { return kind != ProviderKind::Custom || custom.toolsEnabled; }

bool isLoopbackHost(const std::string& hostIn) {
    std::string h = lower(hostIn);
    if (h == "localhost" || h == "::1" || h == "0:0:0:0:0:0:0:1") return true;
    if (h.size() > 10 && h.compare(h.size() - 10, 10, ".localhost") == 0) return true;
    if (h.rfind("127.", 0) == 0) {  // 127.x.x.x, digits and dots only
        return std::all_of(h.begin(), h.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
    }
    return false;
}

EndpointCheck validateEndpoint(const ProviderConfig& c) {
    EndpointCheck r;
    if (c.kind == ProviderKind::ClaudeCode) return r;  // nothing to send; the CLI is checked when it starts
    std::string url = c.requestUrl();
    if (url.empty()) {
        r.errors.push_back(c.kind == ProviderKind::Custom ? "No endpoint: set the full request URL, or a base URL." : "No base URL.");
    } else {
        UrlParts u = parseUrl(url);
        if (!u.valid) r.errors.push_back("The URL must be a valid http:// or https:// address without spaces.");
        else {
            if (u.hasCredentials) r.errors.push_back("The URL must not contain a user name or password (use the credential store for the key).");
            if (u.hasQuery) r.warnings.push_back("The URL has a query string. Never put an API key in the URL: it would be stored in the settings file.");
            if (u.scheme == "http" && !isLoopbackHost(u.host))
                r.warnings.push_back("Plain http:// to '" + u.host + "': the key and your prompts are sent unencrypted. Use https:// unless this is a trusted private network.");
        }
    }
    if (c.kind != ProviderKind::Custom) return r;

    const CustomEndpoint& e = c.custom;
    if (c.resolvedModel().empty()) r.warnings.push_back("No model id set; the request is sent without a \"model\" field.");
    if (e.timeoutSeconds < 1 || e.timeoutSeconds > 600) r.errors.push_back("Timeout must be between 1 and 600 seconds.");
    if (e.auth == AuthStyle::Header) {
        if (!validHeaderName(e.authHeaderName)) r.errors.push_back("The custom auth header name is empty or has invalid characters.");
        if (!validHeaderValue(e.authHeaderPrefix)) r.errors.push_back("The auth header prefix contains a line break or control character.");
    }
    if (e.format == RequestFormat::AnthropicMessages && e.maxTokensField == MaxTokensField::None)
        r.warnings.push_back("The Anthropic messages format requires max_tokens; most servers reject a request without it.");
    if (e.extraHeaders.size() > CustomEndpoint::kMaxExtraHeaders)
        r.errors.push_back(std::format("At most {} extra headers are allowed.", CustomEndpoint::kMaxExtraHeaders));
    for (auto& h : e.extraHeaders) {
        if (!validHeaderName(h.name)) {
            r.errors.push_back("Extra header name '" + redact(h.name.substr(0, 40)) + "' is empty or has invalid characters (line breaks and ':' are rejected).");
            continue;
        }
        if (!validHeaderValue(h.value)) {
            r.errors.push_back("Extra header '" + h.name + "' has a value with a line break or control character.");
            continue;
        }
        std::string ln = lower(h.name);
        if (ln == "host" || ln == "content-length" || ln == "transfer-encoding" || ln == "connection")
            r.errors.push_back("Extra header '" + h.name + "' cannot be set (managed by the HTTP transport).");
        else if (looksSecretName(h.name))
            r.warnings.push_back("Extra header '" + h.name + "' looks like a secret, but extra header values are stored in the plain settings file. "
                                 "Put the key in the credential store and use the auth style (Bearer / x-api-key / custom header) instead.");
    }
    if (!e.extraBody.is_object()) r.errors.push_back("Extra body fields must be a JSON object.");
    else {
        if (e.extraBody.dump().size() > CustomEndpoint::kMaxExtraBodyBytes)
            r.errors.push_back(std::format("Extra body fields are larger than {} KB.", CustomEndpoint::kMaxExtraBodyBytes / 1024));
        if (e.extraBody.contains("messages")) r.warnings.push_back("The extra body field \"messages\" is ignored (the conversation is built by Fledge).");
        for (auto it = e.extraBody.begin(); it != e.extraBody.end(); ++it)
            if (looksSecretName(it.key()))
                r.warnings.push_back("Extra body field \"" + it.key() + "\" looks like a secret; extra body is stored in the plain settings file.");
    }
    return r;
}

Json ProviderConfig::toJson() const {
    Json hdrs = Json::array();
    for (auto& h : custom.extraHeaders) hdrs.push_back({{"name", h.name}, {"value", h.value}});
    Json cj = {{"url", custom.url},
               {"path", custom.path},
               {"format", toString(custom.format)},
               {"auth", toString(custom.auth)},
               {"authHeaderName", custom.authHeaderName},
               {"authHeaderPrefix", custom.authHeaderPrefix},
               {"extraHeaders", hdrs},
               {"extraBody", custom.extraBody},
               {"maxTokensField", toString(custom.maxTokensField)},
               {"timeoutSeconds", custom.timeoutSeconds},
               {"toolsEnabled", custom.toolsEnabled}};
    return {{"kind", toString(kind)}, {"model", model},   {"baseUrl", baseUrl}, {"effort", effort},
            {"serverFallbacks", serverFallbacks}, {"credentialId", credentialId}, {"presetId", presetId}, {"custom", cj},
            {"showThinking", showThinking}, {"cliPath", cliPath}, {"promptCaching", promptCaching}};
}

ProviderConfig ProviderConfig::fromJson(const Json& j) {
    ProviderConfig c;
    if (!j.is_object()) return c;
    auto str = [](const Json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
    c.kind = providerKindFromString(str(j, "kind").empty() ? "anthropic" : str(j, "kind"));
    c.model = str(j, "model");
    c.baseUrl = str(j, "baseUrl");
    c.effort = str(j, "effort");
    c.serverFallbacks = j.value("serverFallbacks", true);
    c.credentialId = str(j, "credentialId");
    c.presetId = str(j, "presetId");
    c.showThinking = j.value("showThinking", true);
    c.promptCaching = j.value("promptCaching", true);
    c.cliPath = str(j, "cliPath");
    if (j.contains("custom") && j["custom"].is_object()) {
        const Json& e = j["custom"];
        CustomEndpoint& x = c.custom;
        x.url = str(e, "url");
        x.path = str(e, "path");
        x.format = requestFormatFromString(str(e, "format"));
        x.auth = authStyleFromString(str(e, "auth"));
        x.authHeaderName = str(e, "authHeaderName");
        x.authHeaderPrefix = str(e, "authHeaderPrefix");
        if (e.contains("extraHeaders") && e["extraHeaders"].is_array())
            for (auto& h : e["extraHeaders"])
                if (h.is_object()) x.extraHeaders.push_back({str(h, "name"), str(h, "value")});
        if (e.contains("extraBody") && e["extraBody"].is_object()) x.extraBody = e["extraBody"];
        x.maxTokensField = maxTokensFieldFromString(str(e, "maxTokensField"));
        x.timeoutSeconds = std::clamp(e.value("timeoutSeconds", 120), 1, 600);
        x.toolsEnabled = e.value("toolsEnabled", true);
    }
    return c;
}

Json ProviderConfig::describe() const {
    std::string host;
    if (std::string u = requestUrl(); !u.empty()) host = parseUrl(u).host;
    Json d = {{"kind", toString(kind)}, {"preset", presetId}, {"model", resolvedModel()}, {"host", host},
              {"needsKey", needsKey()}, {"toolCapable", supportsTools()}};
    if (kind == ProviderKind::Custom) {
        d["requestFormat"] = toString(custom.format);
        d["auth"] = toString(custom.auth);
    }
    return d;
}

ChatMessage ChatResponse::asAssistantMessage() const {
    ChatMessage m;
    m.role = ChatMessage::Role::Assistant;
    m.text = text;
    m.toolCalls = toolCalls;
    m.providerRaw = providerRaw;
    return m;
}

namespace {

std::string errorFromBody(int status, const std::string& body) {
    Json j = Json::parse(body, nullptr, false);
    std::string msg;
    if (j.is_object() && j.contains("error")) {
        const Json& e = j["error"];
        msg = e.is_object() ? (e.contains("message") && e["message"].is_string() ? e["message"].get<std::string>() : e.dump())
                            : (e.is_string() ? e.get<std::string>() : e.dump());
        if (msg.size() > 500) msg = msg.substr(0, 500) + "...";
    } else {
        msg = body.substr(0, 300);
    }
    return redact(std::format("HTTP {}: {}", status, msg));
}

std::string providerLabel(const ProviderConfig& c, const char* fallback) {
    std::string base = fallback;
    if (c.kind == ProviderKind::Custom) {
        const ProviderPreset* p = c.presetId.empty() ? nullptr : findPreset(c.presetId);
        base = p ? p->displayName : "Custom endpoint";
    }
    std::string m = c.resolvedModel();
    return m.empty() ? base : base + " (" + m + ")";
}

void setHeader(std::map<std::string, std::string>& h, const std::string& name, const std::string& value) {
    std::string ln = lower(name);
    for (auto it = h.begin(); it != h.end();) it = lower(it->first) == ln ? h.erase(it) : std::next(it);
    h[ln] = value;
}

// Custom endpoint: auth style, then extra headers (which may override defaults except the auth header's value if empty).
void applyCustomHeaders(const ProviderConfig& cfg, const std::string& key, HttpRequest& h) {
    const CustomEndpoint& e = cfg.custom;
    if (!key.empty()) {
        switch (e.auth) {
            case AuthStyle::Bearer: setHeader(h.headers, "authorization", "Bearer " + key); break;
            case AuthStyle::XApiKey: setHeader(h.headers, "x-api-key", key); break;
            case AuthStyle::Header:
                if (validHeaderName(e.authHeaderName)) setHeader(h.headers, e.authHeaderName, e.authHeaderPrefix + key);
                break;
            case AuthStyle::None: break;
        }
    }
    for (auto& x : e.extraHeaders)
        if (validHeaderName(x.name) && validHeaderValue(x.value)) setHeader(h.headers, x.name, x.value);
    h.timeoutMs = std::clamp(e.timeoutSeconds, 1, 600) * 1000;
}

// Custom endpoint: max-token field variant and extra body fields. `body` already has the format's defaults.
void applyCustomBody(const ProviderConfig& cfg, int maxTokens, Json& body) {
    if (cfg.kind != ProviderKind::Custom) return;
    const CustomEndpoint& e = cfg.custom;
    body.erase("max_tokens");
    body.erase("max_completion_tokens");
    if (e.maxTokensField != MaxTokensField::None) body[toString(e.maxTokensField)] = maxTokens;
    if (cfg.resolvedModel().empty()) body.erase("model");
    if (e.extraBody.is_object())
        for (auto it = e.extraBody.begin(); it != e.extraBody.end(); ++it)
            if (it.key() != "messages") body[it.key()] = it.value();
}

// Refuse to send a request that failed validation (e.g. header injection) before any byte leaves the process.
std::string sendBlocker(const ProviderConfig& cfg) {
    EndpointCheck c = validateEndpoint(cfg);
    return c.ok() ? std::string() : c.errors.front();
}

// ------------------------------------------------------------ Anthropic ---
class AnthropicProvider final : public LlmProvider {
public:
    AnthropicProvider(ProviderConfig c, std::string key, HttpTransport& t) : m_cfg(std::move(c)), m_key(std::move(key)), m_http(t) {}
    std::string name() const override { return providerLabel(m_cfg, "Anthropic"); }

    bool firstParty() const { return m_cfg.kind != ProviderKind::Custom && m_cfg.resolvedBaseUrl() == "https://api.anthropic.com"; }

    // Adaptive thinking exists from the 4.6 family on; older ids (claude-3-*, *-4-0, *-4-1, *-4-5, dated 4.x snapshots) do not take it.
    static bool adaptiveThinkingModel(const std::string& m) {
        for (const char* old : {"claude-3", "-4-0", "-4-1", "-4-5", "-4-2025", "claude-2", "claude-instant"})
            if (m.find(old) != std::string::npos) return false;
        return m.rfind("claude-", 0) == 0;
    }

    Json buildRequestBody(const ChatRequest& req) const override {
        Json msgs = Json::array();
        for (size_t i = 0; i < req.messages.size(); ++i) {
            const auto& m = req.messages[i];
            if (m.role == ChatMessage::Role::User) {
                msgs.push_back({{"role", "user"}, {"content", m.text}});
            } else if (m.role == ChatMessage::Role::Assistant) {
                Json content;
                if (m.providerRaw.is_array()) {
                    content = m.providerRaw;  // replay verbatim (keeps thinking blocks intact)
                } else {
                    content = Json::array();
                    if (!m.text.empty()) content.push_back({{"type", "text"}, {"text", m.text}});
                    for (auto& tc : m.toolCalls)
                        content.push_back({{"type", "tool_use"}, {"id", tc.id}, {"name", tc.name}, {"input", tc.arguments}});
                }
                msgs.push_back({{"role", "assistant"}, {"content", content}});
            } else {
                // Group consecutive tool results into ONE user message.
                Json blocks = Json::array();
                for (; i < req.messages.size() && req.messages[i].role == ChatMessage::Role::Tool; ++i) {
                    const auto& t = req.messages[i];
                    Json b = {{"type", "tool_result"}, {"tool_use_id", t.toolCallId}, {"content", t.text}};
                    if (t.toolError) b["is_error"] = true;
                    blocks.push_back(b);
                }
                --i;
                msgs.push_back({{"role", "user"}, {"content", blocks}});
            }
        }
        // Thinking counts against max_tokens. With the reasoning summary on, 16000 left too little for a large tool call
        // (a whole script): the reply was cut off mid-call. First-party current models get twice the room.
        const bool roomy = m_cfg.showThinking && firstParty() && adaptiveThinkingModel(m_cfg.resolvedModel());
        Json body = {{"model", m_cfg.resolvedModel()}, {"max_tokens", roomy ? std::max(req.maxTokens, 32000) : req.maxTokens}, {"messages", msgs}};
        if (!req.system.empty()) body["system"] = req.system;
        if (!req.tools.empty()) {
            Json tools = Json::array();
            for (auto& t : req.tools) tools.push_back({{"name", t.name}, {"description", t.description}, {"input_schema", t.inputSchema}});
            body["tools"] = tools;
        }
        if (!m_cfg.effort.empty()) body["output_config"] = {{"effort", m_cfg.effort}};
        if (m_cfg.serverFallbacks && firstParty()) body["fallbacks"] = "default";
        // Automatic caching: one top-level marker and the API places the breakpoint at the end of the request, so the next
        // turn (same tools, same system prompt, same earlier messages) reads that prefix from the cache. Nothing in the
        // prefix may change between turns: the tool list keeps registry order and the system prompt holds no time or ids.
        // Other servers that speak this format are left alone (they may reject the field); use Extra body there.
        if (m_cfg.promptCaching && firstParty()) body["cache_control"] = {{"type", "ephemeral"}};
        // Reasoning summary for the thinking view. Current models think adaptively and return nothing readable unless
        // display is "summarized"; models before the 4.6 family reject adaptive thinking, so they are left alone.
        if (m_cfg.showThinking && firstParty() && adaptiveThinkingModel(m_cfg.resolvedModel()))
            body["thinking"] = {{"type", "adaptive"}, {"display", "summarized"}};
        applyCustomBody(m_cfg, req.maxTokens, body);
        return body;
    }

    Result<ChatResponse> parseResponse(int status, const std::string& body) const override {
        if (status != 200) return Result<ChatResponse>::failure(errorFromBody(status, body));
        Json j = Json::parse(body, nullptr, false);
        if (!j.is_object() || !j.contains("content") || !j["content"].is_array()) {
            if (j.is_object() && j.contains("error")) return Result<ChatResponse>::failure(errorFromBody(status, body));
            return Result<ChatResponse>::failure("malformed Anthropic response");
        }
        ChatResponse r;
        for (auto& block : j["content"]) {
            if (!block.is_object()) continue;
            std::string type = block.value("type", "");
            if (type == "text" && block.contains("text") && block["text"].is_string()) r.text += block["text"].get<std::string>();
            else if (type == "thinking" && block.contains("thinking") && block["thinking"].is_string()) {
                std::string th = block["thinking"].get<std::string>();
                if (!th.empty()) r.thinking += (r.thinking.empty() ? "" : "\n\n") + th;
            } else if (type == "tool_use")
                r.toolCalls.push_back({block.value("id", ""), block.value("name", ""), block.value("input", Json::object())});
        }
        std::string sr = j.contains("stop_reason") && j["stop_reason"].is_string() ? j["stop_reason"].get<std::string>() : "";
        r.stopReason = sr == "end_turn" ? "end" : sr;
        if (sr == "refusal" && r.text.empty()) r.text = "(the model declined this request)";
        if (j.contains("usage") && j["usage"].is_object()) {
            const Json& u = j["usage"];
            auto num = [&](const char* k) { return u.contains(k) && u[k].is_number_integer() ? u[k].get<int>() : 0; };
            r.inputTokens = num("input_tokens");
            r.outputTokens = num("output_tokens");
            r.cacheReadTokens = num("cache_read_input_tokens");
            r.cacheWriteTokens = num("cache_creation_input_tokens");
        }
        r.providerRaw = j["content"];
        return r;
    }

    Result<ChatResponse> chat(const ChatRequest& req) override {
        if (std::string why = sendBlocker(m_cfg); !why.empty()) return Result<ChatResponse>::failure("endpoint settings rejected: " + why);
        HttpRequest h;
        h.url = m_cfg.requestUrl();
        h.headers["content-type"] = "application/json";
        h.headers["anthropic-version"] = "2026-10-21";
        if (m_cfg.kind == ProviderKind::Custom) {
            applyCustomHeaders(m_cfg, m_key, h);
        } else {
            h.headers["x-api-key"] = m_key;
        }
        if (m_cfg.serverFallbacks && firstParty()) h.headers["anthropic-beta"] = "server-side-fallback-2026-07-01";
        h.body = buildRequestBody(req).dump(-1, ' ', false, Json::error_handler_t::replace);
        HttpResponse res = m_http.send(h);
        if (res.status == 0) return Result<ChatResponse>::failure(redact("network error: " + res.error));
        return parseResponse(res.status, res.body);
    }

private:
    ProviderConfig m_cfg;
    std::string m_key;
    HttpTransport& m_http;
};

// --------------------------------------------------------------- OpenAI ---
class OpenAIProvider final : public LlmProvider {
public:
    OpenAIProvider(ProviderConfig c, std::string key, HttpTransport& t) : m_cfg(std::move(c)), m_key(std::move(key)), m_http(t) {}
    std::string name() const override {
        return providerLabel(m_cfg, m_cfg.kind == ProviderKind::OpenAI ? "OpenAI" : "OpenAI-compatible");
    }

    Json buildRequestBody(const ChatRequest& req) const override {
        Json msgs = Json::array();
        if (!req.system.empty()) msgs.push_back({{"role", "system"}, {"content", req.system}});
        for (const auto& m : req.messages) {
            switch (m.role) {
                case ChatMessage::Role::User: msgs.push_back({{"role", "user"}, {"content", m.text}}); break;
                case ChatMessage::Role::Assistant: {
                    Json a = {{"role", "assistant"}, {"content", m.text.empty() ? Json(nullptr) : Json(m.text)}};
                    if (!m.toolCalls.empty()) {
                        Json calls = Json::array();
                        for (auto& tc : m.toolCalls)
                            calls.push_back({{"id", tc.id}, {"type", "function"},
                                             {"function", {{"name", tc.name}, {"arguments", tc.arguments.dump()}}}});
                        a["tool_calls"] = calls;
                    }
                    msgs.push_back(a);
                    break;
                }
                case ChatMessage::Role::Tool:
                    msgs.push_back({{"role", "tool"}, {"tool_call_id", m.toolCallId}, {"content", m.text}});
                    break;
            }
        }
        Json body = {{"model", m_cfg.resolvedModel()}, {"messages", msgs}};
        // OpenAI's current models take max_completion_tokens; most compatible servers still expect max_tokens.
        body[m_cfg.kind == ProviderKind::OpenAI ? "max_completion_tokens" : "max_tokens"] = req.maxTokens;
        if (!req.tools.empty()) {
            Json tools = Json::array();
            for (auto& t : req.tools)
                tools.push_back({{"type", "function"},
                                 {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.inputSchema}}}});
            body["tools"] = tools;
        }
        applyCustomBody(m_cfg, req.maxTokens, body);
        return body;
    }

    Result<ChatResponse> parseResponse(int status, const std::string& body) const override {
        if (status != 200) return Result<ChatResponse>::failure(errorFromBody(status, body));
        Json j = Json::parse(body, nullptr, false);
        if (!j.is_object()) return Result<ChatResponse>::failure("malformed OpenAI response (not a JSON object)");
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty() || !j["choices"][0].is_object()) {
            if (j.contains("error")) return Result<ChatResponse>::failure(errorFromBody(status, body));  // some gateways answer errors with 200
            return Result<ChatResponse>::failure("malformed OpenAI response (no choices)");
        }
        const Json& choice = j["choices"][0];
        const Json msg = choice.contains("message") && choice["message"].is_object() ? choice["message"] : Json::object();
        ChatResponse r;
        if (msg.contains("content")) {
            const Json& c = msg["content"];
            if (c.is_string()) r.text = c.get<std::string>();
            else if (c.is_array())  // content parts: [{"type":"text","text":"..."}]
                for (auto& part : c)
                    if (part.is_object() && part.contains("text") && part["text"].is_string()) r.text += part["text"].get<std::string>();
        }
        // Reasoning text, where a server returns it (DeepSeek / vLLM / Ollama: reasoning_content; OpenRouter: reasoning).
        for (const char* key : {"reasoning_content", "reasoning"})
            if (r.thinking.empty() && msg.contains(key) && msg[key].is_string()) r.thinking = msg[key].get<std::string>();
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array())
            for (auto& tc : msg["tool_calls"]) {
                if (!tc.is_object()) continue;
                const Json fn = tc.contains("function") && tc["function"].is_object() ? tc["function"] : Json::object();
                Json args = Json::object();
                if (fn.contains("arguments")) {
                    const Json& a = fn["arguments"];
                    if (a.is_object()) args = a;  // some servers return the object, not a JSON string
                    else if (a.is_string()) {
                        std::string raw = a.get<std::string>();
                        if (!trim(raw).empty()) {
                            args = Json::parse(raw, nullptr, false);
                            if (args.is_discarded()) args = {{"_raw", raw}};
                        }
                    }
                }
                std::string id = tc.contains("id") && tc["id"].is_string() ? tc["id"].get<std::string>() : std::string();
                std::string nm = fn.contains("name") && fn["name"].is_string() ? fn["name"].get<std::string>() : std::string();
                r.toolCalls.push_back({id, nm, args});
            }
        std::string fr = choice.contains("finish_reason") && choice["finish_reason"].is_string() ? choice["finish_reason"].get<std::string>() : "";
        r.stopReason = fr == "stop" ? "end" : fr == "tool_calls" ? "tool_use" : fr == "length" ? "max_tokens"
                     : fr == "content_filter" ? "refusal" : fr;
        if (j.contains("usage") && j["usage"].is_object()) {
            r.inputTokens = j["usage"].value("prompt_tokens", 0);
            r.outputTokens = j["usage"].value("completion_tokens", 0);
        }
        return r;
    }

    Result<ChatResponse> chat(const ChatRequest& req) override {
        if (std::string why = sendBlocker(m_cfg); !why.empty()) return Result<ChatResponse>::failure("endpoint settings rejected: " + why);
        HttpRequest h;
        h.url = m_cfg.requestUrl();
        h.headers["content-type"] = "application/json";
        if (m_cfg.kind == ProviderKind::Custom) {
            applyCustomHeaders(m_cfg, m_key, h);
        } else if (!m_key.empty()) {
            h.headers["authorization"] = "Bearer " + m_key;  // local servers often need no key
        }
        h.body = buildRequestBody(req).dump(-1, ' ', false, Json::error_handler_t::replace);
        HttpResponse res = m_http.send(h);
        if (res.status == 0) return Result<ChatResponse>::failure(redact("network error: " + res.error));
        return parseResponse(res.status, res.body);
    }

private:
    ProviderConfig m_cfg;
    std::string m_key;
    HttpTransport& m_http;
};

// Records the last status so testConnection can report it without changing the provider interface.
class RecordingTransport final : public HttpTransport {
public:
    explicit RecordingTransport(HttpTransport& inner) : m_inner(inner) {}
    HttpResponse send(const HttpRequest& request) override {
        HttpResponse r = m_inner.send(request);
        status = r.status;
        return r;
    }
    int status = 0;

private:
    HttpTransport& m_inner;
};

} // namespace

std::unique_ptr<LlmProvider> createProvider(const ProviderConfig& config, std::string apiKey, HttpTransport& transport) {
    if (config.kind == ProviderKind::ClaudeCode) return nullptr;  // made by createClaudeCodeProvider (claude_code.h): no HTTP here
    if (!apiKey.empty()) Redactor::get().registerSecret(apiKey);
    if (config.kind == ProviderKind::Anthropic || (config.kind == ProviderKind::Custom && config.custom.format == RequestFormat::AnthropicMessages))
        return std::make_unique<AnthropicProvider>(config, std::move(apiKey), transport);
    return std::make_unique<OpenAIProvider>(config, std::move(apiKey), transport);
}

ConnectionTestResult testConnection(const ProviderConfig& config, const std::string& apiKey, HttpTransport& transport) {
    ConnectionTestResult out;
    EndpointCheck chk = validateEndpoint(config);
    if (!chk.ok()) {
        out.message = "Not sent: " + chk.errors.front();
        return out;
    }
    if (config.needsKey() && apiKey.empty()) {
        out.message = "Not sent: no API key is configured for credential '" + config.resolvedCredentialId() + "'.";
        return out;
    }
    if (config.kind == ProviderKind::ClaudeCode) {
        out.message = "Not sent: Claude Code runs as its own program. Use the Claude Code check in AI settings.";
        return out;
    }
    RecordingTransport rec(transport);
    auto provider = createProvider(config, apiKey, rec);
    ChatRequest req;
    req.messages.push_back(ChatMessage::user("Reply with the single word: ok"));
    req.maxTokens = 16;
    auto t0 = std::chrono::steady_clock::now();
    auto res = provider->chat(req);
    out.latencyMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    out.httpStatus = rec.status;
    if (res) {
        out.ok = true;
        out.message = std::format("OK: HTTP {} in {} ms ({}).", out.httpStatus, out.latencyMs, config.describe().value("host", std::string()));
    } else if (rec.status == 200) {
        out.message = redact(std::format("The server answered HTTP 200 in {} ms but the reply is not usable: {}", out.latencyMs, res.error()));
    } else {
        out.message = redact(std::format("Failed after {} ms: {}", out.latencyMs, res.error()));
    }
    return out;
}

} // namespace fledge::ai
