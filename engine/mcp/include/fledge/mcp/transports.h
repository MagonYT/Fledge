#pragma once
// MCP transports.
//
// stdio: one JSON-RPC message per line on stdin; responses one per line on
//        stdout. All logging goes to stderr (the Logger default).
//
// Streamable HTTP (subset of the 2025-06-18 transport):
//   - binds 127.0.0.1 ONLY (never 0.0.0.0)
//   - POST <path> with a JSON-RPC message -> 200 application/json response,
//     or 202 Accepted for notifications
//   - GET <path> -> 405 (no server-initiated SSE stream offered)
//   - DELETE <path> -> ends the session
//   - Origin header, when present, must be http(s)://localhost or 127.0.0.1
//     (DNS-rebinding protection) else 403
//   - Mcp-Session-Id issued on each initialize, required afterwards; several clients can be connected at once (1.2.1: up
//     to 16 sessions; before 1.2.1 a second initialize invalidated the first session: 404 on its next request)
//   - optional bearer token (FLEDGE_MCP_TOKEN env var) required when set
//   - port 0 binds a free port (port() then reports it)
#include <atomic>
#include <deque>
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <thread>

namespace fledge::mcp {

// Called with a raw request body, returns the raw response body ("" = no response).
using MessageHandler = std::function<std::string(const std::string&)>;

// While an HttpServer handler runs: the request's Mcp-Session-Id (first 8 characters, "" for initialize / no session).
// For audit logs only (it is not a secret by itself, but only a prefix is exposed). Thread-local to the server thread.
std::string currentHttpSessionTag();

// Blocks until EOF on `in`.
void runStdio(const MessageHandler& handler, std::istream& in, std::ostream& out);

struct HttpServerOptions {
    int port = 8765;
    std::string path = "/mcp";
    std::string bearerToken;  // empty = no auth (still localhost-only)
};

class HttpServer {
public:
    HttpServer();
    ~HttpServer();
    // Starts a background accept thread. Returns false if the port can't be bound.
    bool start(const HttpServerOptions& options, MessageHandler handler);
    void stop();
    bool running() const { return m_running; }
    int port() const { return m_options.port; }

private:
    void acceptLoop();
    void serveClient(uintptr_t sock);
    HttpServerOptions m_options;
    MessageHandler m_handler;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    uintptr_t m_listen = ~uintptr_t(0);
    // Streamable HTTP sessions (1.2.1): every successful `initialize` adds one (Mcp-Session-Id); up to kMaxSessions stay valid
    // at once (the oldest is dropped beyond that), DELETE ends one. Requests from all sessions run one at a time against
    // the same project (the editor's main thread).
    static constexpr size_t kMaxSessions = 16;
    std::deque<std::string> m_sessions;
public:
    size_t sessionCount() const { return m_sessions.size(); }
};

} // namespace fledge::mcp
