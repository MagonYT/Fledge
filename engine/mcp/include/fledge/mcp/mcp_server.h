#pragma once
// Model Context Protocol server (JSON-RPC 2.0).
//
// Implemented methods: initialize, notifications/initialized, ping,
// tools/list, tools/call, resources/list, resources/read.
// Protocol versions: 2025-06-18 (preferred), 2025-03-26, 2024-11-05.
// Transports (see transports.h): stdio (newline-delimited JSON) and
// Streamable HTTP (POST /mcp, JSON responses) bound to 127.0.0.1 only.
//
// Security model:
//  - Tools carry annotations (readOnlyHint / destructiveHint). Destructive
//    tools are DISABLED unless explicitly allowed by the ToolPolicy.
//  - The policy can deny any tool by name; denied tools are hidden from
//    tools/list and rejected by tools/call.
//  - Every tool result text is passed through fledge::Redactor.
//  - No tool ever reads or returns credentials.
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fledge/core/features.h"
#include "fledge/core/json.h"
#include "fledge/core/version.h"

namespace fledge::mcp {

constexpr const char* kLatestProtocol = "2025-06-18";

namespace rpc {
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
Json makeResult(const Json& id, Json result);
Json makeError(const Json& id, int code, const std::string& message, Json data = nullptr);
} // namespace rpc

struct ToolResult {
    Json content = Json::array();  // MCP content blocks
    bool isError = false;
    Json structured;               // optional structuredContent

    static ToolResult text(const std::string& t);
    static ToolResult json(const Json& j);  // pretty JSON as text + structuredContent
    static ToolResult error(const std::string& t);
    static ToolResult image(const std::string& base64Png, const std::string& caption = {});
};

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    Json inputSchema = {{"type", "object"}, {"properties", Json::object()}};
    bool readOnly = false;
    bool destructive = false;
    std::function<ToolResult(const Json& args)> handler;
    // Edition feature the tool belongs to ("" = core tool, in every edition). Tools of Advanced modules set it.
    std::string feature;
};

struct Resource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType = "application/json";
    std::function<std::optional<std::string>()> read;
};

struct ToolPolicy {
    bool allowDestructive = false;            // global opt-in
    std::set<std::string> allowedDestructive; // per-tool opt-in
    std::set<std::string> denied;             // hard deny
    bool readOnlyMode = false;                // only readOnly tools

    bool permits(const Tool& t) const;
};

class ToolRegistry {
public:
    void add(Tool tool);
    const Tool* find(const std::string& name) const;
    Tool* findMutable(const std::string& name);  // the host patches schemas after registration (1.3.2: Play-mode `target`)
    const std::vector<Tool>& all() const { return m_tools; }
    // Executes with policy + argument checks + redaction. Thread-compatible (caller serializes).
    ToolResult call(const std::string& name, const Json& args, const ToolPolicy& policy) const;
    // Tool list in MCP wire format for permitted tools.
    Json listJson(const ToolPolicy& policy) const;
    // A tool this executable does not have but whose feature is known (FeatureRegistry::declareTool): a structured
    // "requires Fledge Advanced" / "not in this release" error instead of "unknown tool". nullopt for other names.
    static std::optional<ToolResult> unavailableTool(const std::string& name);

    // Optional hook around every handler invocation (the editor uses it to run
    // each mutating tool call inside one undoable transaction tagged as AI).
    std::function<ToolResult(const Tool&, const Json&, const std::function<ToolResult()>&)> callWrapper;

private:
    std::vector<Tool> m_tools;
};

struct ServerInfo {
    std::string name = "fledge-engine";
    std::string title = currentEdition() == Edition::Advanced ? "Fledge Advanced Engine" : "Fledge Engine";
    std::string edition = editionId(currentEdition());  // also sent in serverInfo ("standard" / "advanced")
    std::string version = FLEDGE_VERSION_STRING;  // the engine version (was a stale "0.1.0")
    std::string instructions =
        "Controls the Fledge game engine editor: inspect and edit the open scene (entities, components), "
        "import assets, play/stop, take screenshots and package the game. Use list_entities first; "
        "component property schemas are available via describe_components; describe_edition lists the edition and its features.";
};

class McpServer {
public:
    McpServer(ToolRegistry& tools, ToolPolicy policy = {}, ServerInfo info = {});

    void addResource(Resource r) { m_resources.push_back(std::move(r)); }
    ToolPolicy& policy() { return m_policy; }

    // Handles one JSON-RPC message. Returns the response, or nullopt for notifications.
    std::optional<Json> handle(const Json& message);
    // Text convenience: parse + handle + dump. Returns "" when there is nothing to send.
    std::string handleText(const std::string& text);

    bool initialized() const { return m_initialized; }
    const std::string& negotiatedVersion() const { return m_version; }

private:
    Json onInitialize(const Json& params);
    ToolRegistry& m_tools;
    ToolPolicy m_policy;
    ServerInfo m_info;
    std::vector<Resource> m_resources;
    bool m_initialized = false;
    std::string m_version = kLatestProtocol;
    std::mutex m_mutex;
};

} // namespace fledge::mcp
