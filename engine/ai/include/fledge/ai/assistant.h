#pragma once
// AssistantSession: the in-editor AI agent loop. It exposes the same engine
// tools as the MCP server (one ToolRegistry, one ToolPolicy) to the configured
// LLM provider, executes requested tool calls and feeds results back until the
// model stops calling tools (or maxSteps is hit).
#include <functional>
#include <string>
#include <vector>

#include "fledge/ai/llm.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge::ai {

struct AssistantEvent {
    // Notice: something the user should know that is not an error (the reply was cut off and is being redone, ...).
    enum class Kind { AssistantText, Thinking, ToolCall, ToolResult, Notice, Error, Done };
    Kind kind;
    std::string text;
    // ToolCall / ToolResult: structured details for UIs (text stays the one-line summary).
    std::string toolName;
    Json toolArgs;          // ToolCall only (redacted)
    bool isError = false;   // ToolResult
};

class AssistantSession {
public:
    // Lets the host run tools on a specific thread (the editor marshals to its main thread).
    using ToolExecutor = std::function<mcp::ToolResult(const std::string& name, const Json& args)>;

    AssistantSession(LlmProvider& provider, std::vector<ToolSpec> tools, ToolExecutor executor);
    void setSystemPrompt(std::string s) { m_system = std::move(s); }
    void setMaxSteps(int n) { m_maxSteps = n; }

    // Runs one user turn (blocking). Events are reported as they happen.
    Status send(const std::string& userText, const std::function<void(const AssistantEvent&)>& onEvent);
    void reset() { m_history.clear(); }
    const std::vector<ChatMessage>& history() const { return m_history; }

    static std::vector<ToolSpec> toolSpecsFrom(const mcp::ToolRegistry& reg, const mcp::ToolPolicy& policy);
    static std::string defaultSystemPrompt();
    // Used when the configured model cannot call tools: plain chat, and it must not claim to edit anything.
    static std::string chatOnlySystemPrompt();

private:
    LlmProvider& m_provider;
    std::vector<ToolSpec> m_tools;
    ToolExecutor m_exec;
    std::vector<ChatMessage> m_history;
    std::string m_system = defaultSystemPrompt();
    int m_maxSteps = 12;
};

} // namespace fledge::ai
