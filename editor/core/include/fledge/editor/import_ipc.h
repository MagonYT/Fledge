#pragma once
// Single-instance IPC for links: `Fledge --open-url <link>` (what the fledge:// handler runs) hands the link to an
// editor that is already running, or starts the editor when none is.
//
// Transport: HTTP on 127.0.0.1 (an ephemeral port), one POST per link. Authentication and isolation:
//   * a random 256-bit token, created per editor session, in <user data>/ipc/import-endpoint.json; on Windows that
//     file gets an owner-only ACL (other accounts cannot read it, and the user profile is private anyway);
//   * Windows: the connecting process must belong to the same user (TCP owner PID -> process token SID);
//   * requests carrying Origin, Referer or Sec-Fetch-* headers (anything a browser sends) are refused, the Host must be
//     127.0.0.1:<port> (DNS rebinding), the content type is a custom one (cross-site requests need a CORS preflight,
//     which is refused), and the token travels in Authorization: Bearer;
//   * the body must be a fledge:// link (or a store link Fledge was allowed to take over); an import-file link must
//     point at an expected location (the caller's fileAllowed check: Downloads, the Unity cache, watch folders);
//   * at most 8 KiB per request, 2 s to send it, one request per connection.
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>

#include "fledge/core/utility.h"

namespace fledge::editor {

inline constexpr const char* kIpcPath = "/fledge/v1/link";
inline constexpr const char* kIpcContentType = "application/x-fledge-link";

struct IpcEndpoint {
    int port = 0;
    std::string token;
    uint32_t pid = 0;
};
fs::path ipcEndpointFile(const fs::path& userData);
std::optional<IpcEndpoint> readIpcEndpoint(const fs::path& userData);

struct IpcHttpRequest {
    std::string method, target;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
};
// Pure request validation (unit-tested): returns {HTTP status, reason}; 200 = accept the link in body.
using FileAllowed = std::function<bool(const fs::path&)>;
std::pair<int, std::string> checkIpcRequest(const IpcHttpRequest& r, const std::string& token, int port, const FileAllowed& fileAllowed);
// Parses raw request bytes (headers + body); false when malformed or over the limits.
bool parseIpcRequest(std::string_view raw, IpcHttpRequest& out);

class ImportIpcServer {
public:
    using Handler = std::function<std::string(const std::string& link)>;  // runs on the IPC thread; return a short reply
    ~ImportIpcServer();
    // Binds 127.0.0.1:0, writes the endpoint file, serves until stop(). verifyPeerUser: Windows same-user check.
    Status start(const fs::path& userData, Handler handler, FileAllowed fileAllowed, bool verifyPeerUser = true);
    void stop();
    bool running() const { return m_running; }
    int port() const { return m_port; }
    const std::string& token() const { return m_token; }  // tests only

private:
    void loop();
    void serve(uintptr_t sock);
    std::atomic<bool> m_running{false};
    uintptr_t m_listen = ~uintptr_t(0);
    int m_port = 0;
    std::string m_token;
    fs::path m_endpointFile;
    Handler m_handler;
    FileAllowed m_fileAllowed;
    bool m_verifyPeer = true;
    std::thread m_thread;
};

// Sends a link to the running editor. false when no editor is listening or it refused (reply says why).
bool forwardToRunningEditor(const fs::path& userData, const std::string& link, std::string* reply = nullptr, int timeoutMs = 3000);
// Test helper: sends raw bytes to 127.0.0.1:port, returns {status, body}.
Result<std::pair<int, std::string>> ipcSendRaw(int port, const std::string& raw, int timeoutMs = 3000);
// 32 random bytes as hex (BCryptGenRandom on Windows).
std::string secureRandomHex(size_t bytes = 32);

} // namespace fledge::editor
