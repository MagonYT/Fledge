#include "fledge/mcp/transports.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <iostream>
#include <map>
#include <random>

#include "fledge/core/json.h"
#include "fledge/core/log.h"
#include "fledge/core/utility.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>
using socklen_type = int;
using sock_t = SOCKET;
#define FLEDGE_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_type = socklen_t;
using sock_t = int;
#define FLEDGE_CLOSESOCK close
#endif

namespace fledge::mcp {

void runStdio(const MessageHandler& handler, std::istream& in, std::ostream& out) {
#ifdef _WIN32
    if (&out == &std::cout) _setmode(_fileno(stdout), _O_BINARY);  // no CRLF translation
#endif
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string resp = handler(line);
        if (!resp.empty()) {
            out << resp << '\n';
            out.flush();
        }
    }
}

// ------------------------------------------------------------------ HTTP ---
namespace {

struct HttpRequest {
    std::string method, target;
    std::map<std::string, std::string> headers;  // lower-case keys
    std::string body;
};

bool recvRequest(uintptr_t s, HttpRequest& req) {
    std::string buf;
    char tmp[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        int n = recv(static_cast<sock_t>(s), tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
        headerEnd = buf.find("\r\n\r\n");
        if (buf.size() > 64 * 1024 && headerEnd == std::string::npos) return false;
    }
    std::string head = buf.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    std::string requestLine = head.substr(0, lineEnd);
    auto sp1 = requestLine.find(' '), sp2 = requestLine.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return false;
    req.method = requestLine.substr(0, sp1);
    req.target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        std::string line = head.substr(pos, e - pos);
        auto c = line.find(':');
        if (c != std::string::npos) {
            std::string v = line.substr(c + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            req.headers[toLower(line.substr(0, c))] = v;
        }
        pos = e + 2;
    }
    size_t contentLength = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) contentLength = std::stoul(it->second);
    if (contentLength > 16 * 1024 * 1024) return false;
    req.body = buf.substr(headerEnd + 4);
    while (req.body.size() < contentLength) {
        int n = recv(static_cast<sock_t>(s), tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        req.body.append(tmp, static_cast<size_t>(n));
    }
    req.body.resize(contentLength);
    return true;
}

void sendResponse(uintptr_t s, int status, const char* reason, const std::string& body, const std::string& contentType,
                  const std::string& extraHeaders = {}) {
    std::string resp = std::format("HTTP/1.1 {} {}\r\nContent-Length: {}\r\nConnection: close\r\n", status, reason, body.size());
    if (!contentType.empty()) resp += "Content-Type: " + contentType + "\r\n";
    resp += extraHeaders;
    resp += "\r\n";
    resp += body;
    send(static_cast<sock_t>(s), resp.data(), static_cast<int>(resp.size()), 0);
}

bool originAllowed(const std::string& origin) {
    for (const char* ok : {"http://localhost", "https://localhost", "http://127.0.0.1", "https://127.0.0.1"}) {
        std::string_view o(origin), p(ok);
        if (o.substr(0, p.size()) == p && (o.size() == p.size() || o[p.size()] == ':')) return true;
    }
    return false;
}

std::string randomHex(size_t n) {
    std::random_device rd;
    std::string s;
    const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) s += hex[rd() & 15];
    return s;
}

thread_local std::string t_sessionTag;
} // namespace

std::string currentHttpSessionTag() { return t_sessionTag; }

HttpServer::HttpServer() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

HttpServer::~HttpServer() {
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool HttpServer::start(const HttpServerOptions& options, MessageHandler handler) {
    stop();
    m_options = options;
    m_handler = std::move(handler);
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == static_cast<sock_t>(~0)) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(options.port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only - never expose to the network
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 8) != 0) {
        FLEDGE_CLOSESOCK(s);
        FLEDGE_LOG_ERROR("mcp", "Cannot bind MCP HTTP server to 127.0.0.1:{}", options.port);
        return false;
    }
    if (options.port == 0) {  // a free port was chosen by the OS
        sockaddr_in got{};
        socklen_type gl = sizeof(got);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&got), &gl) == 0) m_options.port = ntohs(got.sin_port);
    }
    m_listen = static_cast<uintptr_t>(s);
    m_running = true;
    m_thread = std::thread([this] { acceptLoop(); });
    FLEDGE_LOG_INFO("mcp", "MCP Streamable HTTP listening on http://127.0.0.1:{}{}{}", m_options.port, options.path,
                  options.bearerToken.empty() ? "" : " (bearer token required)");
    return true;
}

void HttpServer::stop() {
    if (!m_running) return;
    m_running = false;
    FLEDGE_CLOSESOCK(static_cast<sock_t>(m_listen));
    if (m_thread.joinable()) m_thread.join();
    m_listen = ~uintptr_t(0);
}

void HttpServer::acceptLoop() {
    while (m_running) {
        sockaddr_in peer{};
        socklen_type len = sizeof(peer);
        auto c = accept(static_cast<sock_t>(m_listen), reinterpret_cast<sockaddr*>(&peer), &len);
        if (!m_running) {
            if (c != static_cast<sock_t>(~0)) FLEDGE_CLOSESOCK(c);
            break;
        }
        if (c == static_cast<sock_t>(~0)) continue;
        serveClient(static_cast<uintptr_t>(c));  // one request per connection, sequential
        FLEDGE_CLOSESOCK(c);
    }
}

void HttpServer::serveClient(uintptr_t s) {
    HttpRequest req;
    if (!recvRequest(s, req)) return;
    auto header = [&](const char* k) {
        auto it = req.headers.find(k);
        return it == req.headers.end() ? std::string() : it->second;
    };
    std::string target = req.target.substr(0, req.target.find('?'));
    if (target != m_options.path) return sendResponse(s, 404, "Not Found", "", "");
    if (std::string origin = header("origin"); !origin.empty() && !originAllowed(origin))
        return sendResponse(s, 403, "Forbidden", "origin not allowed", "text/plain");
    if (!m_options.bearerToken.empty() && header("authorization") != "Bearer " + m_options.bearerToken)
        return sendResponse(s, 401, "Unauthorized", "", "", "WWW-Authenticate: Bearer\r\n");

    if (req.method == "GET") return sendResponse(s, 405, "Method Not Allowed", "", "", "Allow: POST, DELETE\r\n");
    std::string sid = header("mcp-session-id");
    auto known = [&] { return std::find(m_sessions.begin(), m_sessions.end(), sid) != m_sessions.end(); };
    if (req.method == "DELETE") {  // ends that session (unknown ids: nothing to end)
        if (known()) m_sessions.erase(std::find(m_sessions.begin(), m_sessions.end(), sid));
        return sendResponse(s, 200, "OK", "", "");
    }
    if (req.method != "POST") return sendResponse(s, 405, "Method Not Allowed", "", "", "Allow: POST, DELETE\r\n");

    // initialize = the JSON-RPC method (a single message or inside a batch), not any body that mentions the word
    bool isInitialize = false;
    {
        Json msg = Json::parse(req.body, nullptr, false);
        auto isInit = [](const Json& m) { return m.is_object() && m.value("method", "") == "initialize"; };
        if (isInit(msg)) isInitialize = true;
        if (msg.is_array())
            for (auto& m : msg) isInitialize = isInitialize || isInit(m);
    }
    if (!isInitialize && !m_sessions.empty() && !known())
        return sendResponse(s, sid.empty() ? 400 : 404, sid.empty() ? "Bad Request" : "Not Found", "missing or unknown Mcp-Session-Id",
                            "text/plain");
    t_sessionTag = known() ? sid.substr(0, 8) : std::string();
    std::string resp = m_handler(req.body);
    t_sessionTag.clear();
    std::string extra;
    if (isInitialize && resp.find("\"result\"") != std::string::npos) {
        // several clients may be connected (an editor assistant + Claude Code ...): a new session never ends another one
        m_sessions.push_back(randomHex(32));
        while (m_sessions.size() > kMaxSessions) m_sessions.pop_front();
        extra = "Mcp-Session-Id: " + m_sessions.back() + "\r\n";
    }
    if (resp.empty()) return sendResponse(s, 202, "Accepted", "", "", extra);
    sendResponse(s, 200, "OK", resp, "application/json", extra);
}

} // namespace fledge::mcp
