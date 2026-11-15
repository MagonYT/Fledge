#include "fledge/mcp/mcp_server.h"

#include <format>

#include "fledge/core/log.h"
#include "fledge/core/redact.h"

namespace fledge::mcp {

namespace rpc {
Json makeResult(const Json& id, Json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }
Json makeError(const Json& id, int code, const std::string& message, Json data) {
    Json err = {{"code", code}, {"message", message}};
    if (!data.is_null()) err["data"] = std::move(data);
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", err}};
}
} // namespace rpc

ToolResult ToolResult::text(const std::string& t) {
    ToolResult r;
    r.content.push_back({{"type", "text"}, {"text", t}});
    return r;
}
ToolResult ToolResult::json(const Json& j) {
    ToolResult r = text(j.dump(2));
    r.structured = j.is_object() ? j : Json{{"result", j}};
    return r;
}
ToolResult ToolResult::error(const std::string& t) {
    ToolResult r = text(t);
    r.isError = true;
    return r;
}
ToolResult ToolResult::image(const std::string& base64Png, const std::string& caption) {
    ToolResult r;
    r.content.push_back({{"type", "image"}, {"data", base64Png}, {"mimeType", "image/png"}});
    if (!caption.empty()) r.content.push_back({{"type", "text"}, {"text", caption}});
    return r;
}

bool ToolPolicy::permits(const Tool& t) const {
    if (denied.contains(t.name)) return false;
    if (readOnlyMode && !t.readOnly) return false;
    if (t.destructive && !allowDestructive && !allowedDestructive.contains(t.name)) return false;
    return true;
}

void ToolRegistry::add(Tool tool) { m_tools.push_back(std::move(tool)); }

const Tool* ToolRegistry::find(const std::string& name) const {
    for (auto& t : m_tools)
        if (t.name == name) return &t;
    return nullptr;
}
Tool* ToolRegistry::findMutable(const std::string& name) {
    for (auto& t : m_tools)
        if (t.name == name) return &t;
    return nullptr;
}

Json ToolRegistry::listJson(const ToolPolicy& policy) const {
    Json tools = Json::array();
    for (auto& t : m_tools) {
        if (!policy.permits(t)) continue;
        Json j = {{"name", t.name}, {"description", t.description}, {"inputSchema", t.inputSchema}};
        if (!t.title.empty()) j["title"] = t.title;
        j["annotations"] = {{"readOnlyHint", t.readOnly}, {"destructiveHint", t.destructive}};
        if (!t.title.empty()) j["annotations"]["title"] = t.title;
        tools.push_back(j);
    }
    return tools;
}

// Minimal JSON-schema check: required keys + primitive types of declared properties.
static std::string checkArgs(const Json& schema, const Json& args) {
    if (!args.is_object()) return "arguments must be an object";
    for (auto& req : schema.value("required", Json::array()))
        if (!args.contains(req.get<std::string>())) return std::format("missing required argument '{}'", req.get<std::string>());
    const Json props = schema.value("properties", Json::object());
    for (auto& [k, v] : args.items()) {
        if (!props.contains(k)) {
            if (schema.value("additionalProperties", true) == false) return std::format("unknown argument '{}'", k);
            continue;
        }
        std::string type = props[k].value("type", "");
        bool ok = type.empty() || (type == "string" && v.is_string()) || (type == "integer" && v.is_number_integer()) ||
                  (type == "number" && v.is_number()) || (type == "boolean" && v.is_boolean()) ||
                  (type == "array" && v.is_array()) || (type == "object" && v.is_object());
        if (!ok) return std::format("argument '{}' must be of type {}", k, type);
    }
    return {};
}

static void redactContent(Json& content) {
    for (auto& block : content)
        if (block.value("type", "") == "text") block["text"] = redact(block["text"].get<std::string>());
}

std::optional<ToolResult> ToolRegistry::unavailableTool(const std::string& name) {
    const FeatureRegistry& reg = FeatureRegistry::get();
    std::string featureId = reg.toolFeature(name);
    if (featureId.empty() || reg.provided(featureId)) return std::nullopt;
    FeatureRequirement need = requirementFor(reg, featureId);
    auto missing = missingFeatures({need}, reg);
    std::string reason = missing.empty() ? std::string("not available") : missing.front().reason;
    bool advanced = need.edition == Edition::Advanced && currentEdition() == Edition::Standard;
    std::string code = advanced ? "requires_fledge_advanced" : "feature_unavailable";
    ToolResult r = ToolResult::error(std::format("tool '{}' {} ({}: {}). This is {}; see describe_edition.", name,
                                                 advanced ? "requires Fledge Advanced" : "is not available in this release",
                                                 need.title, reason, editionName(currentEdition())));
    r.structured = {{"error", code},
                    {"tool", name},
                    {"feature", need.id},
                    {"featureTitle", need.title},
                    {"requiredEdition", editionId(need.edition)},
                    {"edition", editionId(currentEdition())},
                    {"reason", reason}};
    return r;
}

ToolResult ToolRegistry::call(const std::string& name, const Json& args, const ToolPolicy& policy) const {
    const Tool* t = find(name);
    if (!t) {
        if (auto u = unavailableTool(name)) return *u;
        return ToolResult::error("unknown tool: " + name);
    }
    if (!policy.permits(*t))
        return ToolResult::error(std::format("tool '{}' is not permitted by the current MCP policy{}", name,
                                             t->destructive ? " (destructive tools require explicit opt-in)" : ""));
    if (auto err = checkArgs(t->inputSchema, args); !err.empty()) return ToolResult::error(err);
    ToolResult r;
    try {
        if (callWrapper) r = callWrapper(*t, args, [&] { return t->handler(args); });
        else r = t->handler(args);
    } catch (const std::exception& e) {
        r = ToolResult::error(std::string("tool failed: ") + e.what());
    }
    redactContent(r.content);
    if (!r.structured.is_null()) r.structured = Json::parse(redact(r.structured.dump(-1, ' ', false, Json::error_handler_t::replace)), nullptr, false);
    return r;
}

McpServer::McpServer(ToolRegistry& tools, ToolPolicy policy, ServerInfo info)
    : m_tools(tools), m_policy(std::move(policy)), m_info(std::move(info)) {}

Json McpServer::onInitialize(const Json& params) {
    std::string requested = params.value("protocolVersion", kLatestProtocol);
    static const std::set<std::string> supported = {"2025-06-18", "2025-03-26", "2024-11-05"};
    m_version = supported.contains(requested) ? requested : kLatestProtocol;
    m_initialized = true;
    std::string client = params.contains("clientInfo") ? params["clientInfo"].value("name", "?") : "?";
    FLEDGE_LOG_INFO("mcp", "Client '{}' initialized (protocol {})", client, m_version);
    Json serverInfo = {{"name", m_info.name}, {"version", m_info.version}, {"edition", m_info.edition}};
    if (m_version != "2024-11-05") serverInfo["title"] = m_info.title;
    return {{"protocolVersion", m_version},
            {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}, {"subscribe", false}}}}},
            {"serverInfo", serverInfo},
            {"instructions", m_info.instructions}};
}

std::optional<Json> McpServer::handle(const Json& msg) {
    std::lock_guard lock(m_mutex);
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0")
        return rpc::makeError(nullptr, rpc::kInvalidRequest, "expected a JSON-RPC 2.0 object");
    bool isNotification = !msg.contains("id");
    if (!msg.contains("method")) {
        return std::nullopt;  // a response from the client (we never send requests) - ignore
    }
    const Json id = isNotification ? Json(nullptr) : msg["id"];
    if (!isNotification && !(id.is_string() || id.is_number_integer()))
        return rpc::makeError(nullptr, rpc::kInvalidRequest, "id must be a string or integer");
    const std::string method = msg.value("method", "");
    const Json params = msg.contains("params") ? msg["params"] : Json::object();

    if (isNotification) {
        // notifications/initialized, notifications/cancelled, ... - nothing to do
        return std::nullopt;
    }
    try {
        if (method == "initialize") return rpc::makeResult(id, onInitialize(params));
        if (method == "ping") return rpc::makeResult(id, Json::object());
        if (!m_initialized) return rpc::makeError(id, rpc::kInvalidRequest, "server not initialized: send 'initialize' first");
        if (method == "tools/list") return rpc::makeResult(id, {{"tools", m_tools.listJson(m_policy)}});
        if (method == "tools/call") {
            if (!params.contains("name") || !params["name"].is_string())
                return rpc::makeError(id, rpc::kInvalidParams, "tools/call requires 'name'");
            std::string name = params["name"];
            if (!m_tools.find(name)) {
                // tools of a feature this edition lacks: a structured tool error, so clients can tell the user why
                if (auto u = ToolRegistry::unavailableTool(name)) {
                    Json result = {{"content", u->content}, {"isError", true}};
                    if (m_version == "2025-06-18") result["structuredContent"] = u->structured;
                    return rpc::makeResult(id, result);
                }
                return rpc::makeError(id, rpc::kInvalidParams, "unknown tool: " + name);
            }
            Json args = params.contains("arguments") ? params["arguments"] : Json::object();
            FLEDGE_LOG_INFO("mcp", "tools/call {}", name);
            ToolResult r = m_tools.call(name, args, m_policy);
            Json result = {{"content", r.content}, {"isError", r.isError}};
            if (!r.structured.is_null() && m_version == "2025-06-18") result["structuredContent"] = r.structured;
            return rpc::makeResult(id, result);
        }
        if (method == "resources/list") {
            Json list = Json::array();
            for (auto& r : m_resources)
                list.push_back({{"uri", r.uri}, {"name", r.name}, {"description", r.description}, {"mimeType", r.mimeType}});
            return rpc::makeResult(id, {{"resources", list}});
        }
        if (method == "resources/templates/list") return rpc::makeResult(id, {{"resourceTemplates", Json::array()}});
        if (method == "resources/read") {
            std::string uri = params.value("uri", "");
            for (auto& r : m_resources) {
                if (r.uri != uri) continue;
                auto text = r.read();
                if (!text) return rpc::makeError(id, rpc::kInternalError, "resource unavailable: " + uri);
                return rpc::makeResult(id, {{"contents", Json::array({{{"uri", uri}, {"mimeType", r.mimeType}, {"text", redact(*text)}}})}});
            }
            return rpc::makeError(id, -32002, "resource not found", {{"uri", uri}});
        }
        return rpc::makeError(id, rpc::kMethodNotFound, "method not found: " + method);
    } catch (const std::exception& e) {
        return rpc::makeError(id, rpc::kInternalError, e.what());
    }
}

std::string McpServer::handleText(const std::string& text) {
    Json msg = Json::parse(text, nullptr, false);
    if (msg.is_discarded()) return rpc::makeError(nullptr, rpc::kParseError, "parse error").dump();
    if (msg.is_array()) return rpc::makeError(nullptr, rpc::kInvalidRequest, "JSON-RPC batches are not supported").dump();
    auto r = handle(msg);
    return r ? r->dump(-1, ' ', false, Json::error_handler_t::replace) : std::string();
}

} // namespace fledge::mcp
