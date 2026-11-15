#pragma once
// Provider-neutral chat API with tool calling.
//
// Implementations:
//   AnthropicProvider  - Messages API  (POST {base}/v1/messages)
//   OpenAIProvider     - Chat Completions (POST {base}/chat/completions);
//                        also used for any OpenAI-compatible server (Ollama,
//                        LM Studio, vLLM, OpenRouter...) via baseUrl.
//   Custom             - user-configured endpoint (full URL or base+path, OpenAI
//                        chat or Anthropic messages body, any auth style, extra
//                        headers / body fields); see CustomEndpoint and presets.h.
//   OpenAI Responses API - STUB (not implemented; Chat Completions is used).
// Streaming: interface exists (chatStream); implementations currently fall
// back to a single non-streaming response (STUB).
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fledge/ai/http.h"
#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge::ai {

struct ToolCall {
    std::string id;
    std::string name;
    Json arguments = Json::object();
};

struct ChatMessage {
    enum class Role { User, Assistant, Tool };
    Role role = Role::User;
    std::string text;
    std::vector<ToolCall> toolCalls;  // assistant
    std::string toolCallId;           // tool result
    bool toolError = false;
    // Provider-native assistant content (e.g. Anthropic content blocks incl.
    // thinking blocks) replayed verbatim on the next request when the same
    // provider is used.
    Json providerRaw;

    static ChatMessage user(std::string t) { ChatMessage m; m.text = std::move(t); return m; }
    static ChatMessage toolResult(std::string id, std::string t, bool err = false) {
        ChatMessage m;
        m.role = Role::Tool;
        m.toolCallId = std::move(id);
        m.text = std::move(t);
        m.toolError = err;
        return m;
    }
};

struct ToolSpec {
    std::string name;
    std::string description;
    Json inputSchema;
};

// Something a provider did while answering, reported as it happens (ChatRequest::onActivity). Only agent-style providers
// that run their own tool loop (Claude Code) report Text / ToolCall / ToolResult; API providers return those in ChatResponse.
struct ProviderActivity {
    enum class Kind { Thinking, Text, ToolCall, ToolResult } kind = Kind::Text;
    std::string text;       // Thinking / Text: the content; ToolResult: the (shortened) result
    std::string toolName;   // ToolCall / ToolResult
    Json toolArgs;          // ToolCall
    bool isError = false;   // ToolResult
};

struct ChatRequest {
    std::string system;
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;
    int maxTokens = 16000;
    std::function<void(const ProviderActivity&)> onActivity;  // optional; called on the calling thread
};

struct ChatResponse {
    std::string text;
    std::string thinking;     // the model's reasoning summary when the provider returns one ("" = none)
    bool activityStreamed = false;  // text / tools were already reported through onActivity (do not show `text` again)
    std::vector<ToolCall> toolCalls;
    std::string stopReason;   // normalized: "end", "tool_use", "max_tokens", "refusal", or provider value
    int inputTokens = 0, outputTokens = 0;
    int cacheReadTokens = 0, cacheWriteTokens = 0;  // prompt caching: input read from / written to the cache (0 = not reported)
    Json providerRaw;         // assistant content to replay
    ChatMessage asAssistantMessage() const;
};

// ClaudeCode: no HTTP from the editor at all. The user's own Claude Code CLI runs as a child process, signed in with
// their own Claude account, and reaches the editor tools through the MCP server (see claude_code.h).
enum class ProviderKind { Anthropic, OpenAI, OpenAICompatible, Custom, ClaudeCode };
const char* toString(ProviderKind k);
ProviderKind providerKindFromString(const std::string& s);

// ---- Custom endpoint ("custom" kind) -------------------------------------------------------------
enum class RequestFormat { OpenAIChat, AnthropicMessages };
enum class AuthStyle { Bearer, XApiKey, Header, None };
enum class MaxTokensField { MaxTokens, MaxCompletionTokens, None };
const char* toString(RequestFormat f);   // "openai-chat" | "anthropic-messages"
const char* toString(AuthStyle a);       // "bearer" | "x-api-key" | "header" | "none"
const char* toString(MaxTokensField f);  // "max_tokens" | "max_completion_tokens" | "none"
RequestFormat requestFormatFromString(const std::string& s);
AuthStyle authStyleFromString(const std::string& s);
MaxTokensField maxTokensFieldFromString(const std::string& s);

struct HeaderField {
    std::string name, value;  // NOT secret: stored in the settings file (see docs/AI_HANDBOOK.md)
};

struct CustomEndpoint {
    // Full request URL. If empty, ProviderConfig::baseUrl + path (or the format's default path) is used.
    std::string url;
    std::string path;                 // overrides the default path when url is empty ("/chat/completions", "/v1/messages")
    RequestFormat format = RequestFormat::OpenAIChat;
    AuthStyle auth = AuthStyle::Bearer;
    std::string authHeaderName;       // AuthStyle::Header: header that carries the key
    std::string authHeaderPrefix;     // AuthStyle::Header: text placed before the key, e.g. "Token "
    std::vector<HeaderField> extraHeaders;
    Json extraBody = Json::object();  // merged into the request body (top-level keys override defaults, except "messages")
    MaxTokensField maxTokensField = MaxTokensField::MaxTokens;
    int timeoutSeconds = 120;         // 1..600
    bool toolsEnabled = true;         // false: the assistant sends no tool definitions (chat only)
    static constexpr size_t kMaxExtraBodyBytes = 16 * 1024;
    static constexpr size_t kMaxExtraHeaders = 16;
};

struct ProviderConfig {
    ProviderKind kind = ProviderKind::Anthropic;
    std::string model;      // empty = provider default
    std::string baseUrl;    // empty = provider default
    std::string effort;     // Anthropic output_config.effort (optional: low|medium|high|xhigh|max)
    bool serverFallbacks = true;  // Anthropic first-party: "fallbacks": "default" on refusal
    bool showThinking = true;     // ask for / show the model's reasoning summary where the provider supports it
    // Anthropic first-party: automatic prompt caching. Every turn resends the tool list, the system prompt and the whole
    // conversation; with this on, the unchanged start of the request is read from the cache at a fraction of the input price.
    bool promptCaching = true;
    std::string cliPath;          // ClaudeCode: path of the claude executable ("" = find it on PATH)
    // Credential-store id; defaults to "anthropic"/"openai"/"openai-compatible"/"custom".
    std::string credentialId;
    std::string presetId;   // informational: preset this config was filled from ("" = none)
    CustomEndpoint custom;  // used when kind == Custom
    std::string resolvedCredentialId() const;
    std::string resolvedModel() const;
    std::string resolvedBaseUrl() const;
    // The exact URL requests are POSTed to (no secrets).
    std::string requestUrl() const;
    // Whether a key must be configured before a provider can be created.
    bool needsKey() const;
    // Whether the assistant may send tool definitions (preset model table / custom.toolsEnabled; unknown = true).
    bool supportsTools() const;

    Json toJson() const;  // never contains secrets (keys live in the CredentialStore only)
    static ProviderConfig fromJson(const Json& j);
    // Safe summary for status displays / MCP: kind, preset, model, host, format, auth style, tools. Never a key.
    Json describe() const;
};

// Whether a header / body field / query parameter name looks like it carries a secret ("key", "token", "auth", ...).
// Used for the plain-settings warnings below and by the settings migration's "AI keys" option.
bool looksLikeSecretName(const std::string& name);

// Validation for Custom endpoints (other kinds: only URL checks). Warnings never block.
struct EndpointCheck {
    std::vector<std::string> errors, warnings;
    bool ok() const { return errors.empty(); }
};
EndpointCheck validateEndpoint(const ProviderConfig& c);
bool isLoopbackHost(const std::string& host);

// Result of a connection test: one tiny request through the normal provider/transport path.
struct ConnectionTestResult {
    bool ok = false;
    int httpStatus = 0;      // 0 = transport failure / not sent
    int latencyMs = 0;
    std::string message;     // redacted; never contains the key
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual std::string name() const = 0;
    virtual Result<ChatResponse> chat(const ChatRequest& request) = 0;
    // STUB streaming: default delivers the final text once.
    virtual Result<ChatResponse> chatStream(const ChatRequest& request, const std::function<void(const std::string&)>& onDelta) {
        auto r = chat(request);
        if (r && onDelta) onDelta(r->text);
        return r;
    }

    // Exposed for tests.
    virtual Json buildRequestBody(const ChatRequest& request) const = 0;
    virtual Result<ChatResponse> parseResponse(int status, const std::string& body) const = 0;
};

// Sends one tiny "ping" request (no tools, small max tokens) through createProvider + transport.
// Validation errors are returned without sending anything.
ConnectionTestResult testConnection(const ProviderConfig& config, const std::string& apiKey, HttpTransport& transport);

// apiKey is held in memory only. transport must outlive the provider.
std::unique_ptr<LlmProvider> createProvider(const ProviderConfig& config, std::string apiKey, HttpTransport& transport);

} // namespace fledge::ai
