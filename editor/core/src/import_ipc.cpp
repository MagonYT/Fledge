// Authenticated localhost IPC for fledge:// links (see import_ipc.h).
#include "fledge/editor/import_ipc.h"

#include <cstring>
#include <format>
#include <random>

#include "fledge/core/json.h"
#include "fledge/core/log.h"
#include "fledge/importers/store_import.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <sddl.h>
using ipc_sock = SOCKET;
#define IPC_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using ipc_sock = int;
#define IPC_CLOSE close
#endif

namespace fledge::editor {

namespace {
constexpr size_t kMaxRequest = 16 * 1024;
constexpr size_t kMaxBody = 8 * 1024;

struct WinsockInit {
    WinsockInit() {
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
    }
    ~WinsockInit() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

void setTimeout(ipc_sock s, int ms) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void reply(ipc_sock s, int status, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 413 ? "Payload Too Large" : "Error";
    std::string r = std::format("HTTP/1.1 {} {}\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n"
                                "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n{}",
                                status, reason, body.size(), body);
    send(s, r.data(), static_cast<int>(r.size()), 0);
}

#ifdef _WIN32
// The account that owns the process at the other end of a loopback connection is the current user.
bool peerIsSameUser(ipc_sock s) {
    sockaddr_in local{}, peer{};
    int len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) != 0) return false;
    len = sizeof(peer);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &len) != 0) return false;
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    std::vector<uint8_t> buf(size + 4096);
    size = static_cast<ULONG>(buf.size());
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR) return false;
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    DWORD pid = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        // the client's row: its local port is our peer's port, its remote port is our listening port
        if (static_cast<u_short>(row.dwLocalPort) == peer.sin_port && static_cast<u_short>(row.dwRemotePort) == local.sin_port &&
            row.dwLocalAddr == peer.sin_addr.s_addr) {
            pid = row.dwOwningPid;
            break;
        }
    }
    if (pid == 0) return false;
    auto userSid = [](HANDLE process, std::vector<uint8_t>& out) {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) return false;
        DWORD n = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
        out.resize(n);
        bool ok = n && GetTokenInformation(tok, TokenUser, out.data(), n, &n);
        CloseHandle(tok);
        return ok;
    };
    std::vector<uint8_t> mine, theirs;
    if (!userSid(GetCurrentProcess(), mine)) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;  // another user's process cannot even be opened
    bool ok = userSid(p, theirs);
    CloseHandle(p);
    return ok && EqualSid(reinterpret_cast<TOKEN_USER*>(mine.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(theirs.data())->User.Sid);
}

bool writeOwnerOnlyFile(const fs::path& file, const std::string& text) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    // protected DACL: full access for the owner only (no inherited ACEs)
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE h = CreateFileW(osPath(file).c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(h);
    return ok;
}
#endif
} // namespace

std::string secureRandomHex(size_t bytes) {
    std::vector<uint8_t> b(bytes);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};  // no token = no server
#else
    std::random_device rd;
    for (auto& x : b) x = static_cast<uint8_t>(rd());
#endif
    std::string s;
    for (uint8_t x : b) s += std::format("{:02x}", x);
    return s;
}

fs::path ipcEndpointFile(const fs::path& userData) { return userData / "ipc" / "import-endpoint.json"; }

std::optional<IpcEndpoint> readIpcEndpoint(const fs::path& userData) {
    auto text = readFileText(ipcEndpointFile(userData));
    if (!text) return std::nullopt;
    Json j = Json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    IpcEndpoint e;
    e.port = j.value("port", 0);
    e.token = j.value("token", "");
    e.pid = j.value("pid", 0u);
    if (e.port <= 0 || e.port > 65535 || e.token.size() < 32) return std::nullopt;
    return e;
}

bool parseIpcRequest(std::string_view raw, IpcHttpRequest& out) {
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos || headerEnd > kMaxRequest) return false;
    std::string_view head = raw.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    std::string_view line = head.substr(0, lineEnd);
    size_t a = line.find(' '), b = line.rfind(' ');
    if (a == std::string_view::npos || b == a) return false;
    out.method = std::string(line.substr(0, a));
    out.target = std::string(line.substr(a + 1, b - a - 1));
    size_t pos = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
    int count = 0;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string_view::npos) e = head.size();
        std::string_view h = head.substr(pos, e - pos);
        pos = e + 2;
        if (++count > 64) return false;
        size_t c = h.find(':');
        if (c == std::string_view::npos) return false;
        std::string v(h.substr(c + 1));
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        out.headers[toLower(h.substr(0, c))] = v;
    }
    out.body = std::string(raw.substr(headerEnd + 4));
    return true;
}

std::pair<int, std::string> checkIpcRequest(const IpcHttpRequest& r, const std::string& token, int port, const FileAllowed& fileAllowed) {
    auto header = [&](const char* k) {
        auto it = r.headers.find(k);
        return it == r.headers.end() ? std::string() : it->second;
    };
    for (const char* browserHeader : {"origin", "referer", "sec-fetch-site", "sec-fetch-mode", "sec-fetch-dest"})
        if (r.headers.contains(browserHeader)) return {403, "requests from web pages are refused"};
    if (r.target != kIpcPath) return {404, "unknown path"};
    if (r.method != "POST") return {405, "POST only"};
    if (header("host") != std::format("127.0.0.1:{}", port)) return {403, "bad Host header"};
    if (token.empty() || !constantTimeEquals(header("authorization"), "Bearer " + token)) return {401, "missing or wrong session token"};
    if (toLower(header("content-type")) != kIpcContentType) return {400, std::string("Content-Type must be ") + kIpcContentType};
    if (r.body.size() > kMaxBody) return {413, "link too long"};
    store::StoreLink l = store::parseLink(r.body);
    switch (l.kind) {
        case store::LinkKind::ImportUrl:
        case store::LinkKind::UnityOpenInEditor:
        case store::LinkKind::EpicLauncher:
        case store::LinkKind::EpicPassThrough: return {200, "accepted"};
        case store::LinkKind::ImportFile:
            if (!fileAllowed || !fileAllowed(pathFromUtf8(l.filePath)))
                return {403, "files are only accepted from the Downloads folder, the Unity Asset Store cache and the watch folders"};
            return {200, "accepted"};
        default: break;
    }
    if (startsWith(toLower(r.body), "fledge:")) return {400, "invalid fledge:// link: " + l.error};
    return {400, "only fledge:// links are accepted"};
}

ImportIpcServer::~ImportIpcServer() { stop(); }

Status ImportIpcServer::start(const fs::path& userData, Handler handler, FileAllowed fileAllowed, bool verifyPeerUser) {
    stop();
    static WinsockInit winsock;
    m_token = secureRandomHex(32);
    if (m_token.empty()) return Status::failure("no secure random source for the IPC token");
    ipc_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == static_cast<ipc_sock>(~0)) return Status::failure("cannot create a socket");
#ifdef _WIN32
    BOOL excl = TRUE;  // no other process may bind the same port
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 4) != 0) {
        IPC_CLOSE(s);
        return Status::failure("cannot bind the IPC port on 127.0.0.1");
    }
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    m_port = ntohs(addr.sin_port);
    m_listen = static_cast<uintptr_t>(s);
    m_handler = std::move(handler);
    m_fileAllowed = std::move(fileAllowed);
    m_verifyPeer = verifyPeerUser;
    m_endpointFile = ipcEndpointFile(userData);
    std::error_code ec;
    fs::create_directories(osPath(m_endpointFile.parent_path()), ec);
    std::string text = Json{{"port", m_port}, {"token", m_token}, {"pid",
#ifdef _WIN32
                                                                    static_cast<uint32_t>(GetCurrentProcessId())
#else
                                                                    0u
#endif
                                                                   }, {"version", 1}}.dump();
#ifdef _WIN32
    bool written = writeOwnerOnlyFile(m_endpointFile, text);
#else
    bool written = writeFileText(m_endpointFile, text);
#endif
    if (!written) {
        IPC_CLOSE(s);
        return Status::failure("cannot write the IPC endpoint file");
    }
    m_running = true;
    m_thread = std::thread([this] { loop(); });
    FLEDGE_LOG_INFO("links", "Link IPC listening on 127.0.0.1:{} (token in {})", m_port, narrow(m_endpointFile.filename().wstring()));
    return {};
}

void ImportIpcServer::stop() {
    if (!m_running) return;
    m_running = false;
    IPC_CLOSE(static_cast<ipc_sock>(m_listen));
    if (m_thread.joinable()) m_thread.join();
    std::error_code ec;
    // only remove the endpoint file if it is still ours (a newer editor may have replaced it)
    if (auto e = readIpcEndpoint(m_endpointFile.parent_path().parent_path()); e && e->token == m_token) fs::remove(osPath(m_endpointFile), ec);
    m_token.clear();
}

void ImportIpcServer::loop() {
    while (m_running) {
        ipc_sock c = accept(static_cast<ipc_sock>(m_listen), nullptr, nullptr);
        if (!m_running) {
            if (c != static_cast<ipc_sock>(~0)) IPC_CLOSE(c);
            break;
        }
        if (c == static_cast<ipc_sock>(~0)) continue;
        serve(static_cast<uintptr_t>(c));
        IPC_CLOSE(c);
    }
}

void ImportIpcServer::serve(uintptr_t sock) {
    ipc_sock s = static_cast<ipc_sock>(sock);
    setTimeout(s, 2000);
#ifdef _WIN32
    if (m_verifyPeer && !peerIsSameUser(s)) {
        FLEDGE_LOG_WARN("links", "IPC: refused a connection from another user's process");
        return reply(s, 403, "other users' processes are refused");
    }
#endif
    std::string buf;
    char tmp[4096];
    size_t need = std::string::npos;
    while (buf.size() < kMaxRequest + kMaxBody) {
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        size_t he = buf.find("\r\n\r\n");
        if (he != std::string::npos && need == std::string::npos) {
            IpcHttpRequest probe;
            if (!parseIpcRequest(buf, probe)) return reply(s, 400, "malformed request");
            size_t cl = static_cast<size_t>(std::strtoull(probe.headers["content-length"].c_str(), nullptr, 10));
            if (cl > kMaxBody) return reply(s, 413, "link too long");
            need = he + 4 + cl;
        }
        if (need != std::string::npos && buf.size() >= need) break;
    }
    IpcHttpRequest req;
    if (need == std::string::npos || buf.size() < need || !parseIpcRequest(std::string_view(buf).substr(0, need), req))
        return reply(s, 400, "incomplete request");
    auto [status, why] = checkIpcRequest(req, m_token, m_port, m_fileAllowed);
    if (status != 200) {
        FLEDGE_LOG_WARN("links", "IPC: refused ({}): {}", status, why);
        return reply(s, status, why);
    }
    reply(s, 200, m_handler ? m_handler(req.body) : "queued");
}

Result<std::pair<int, std::string>> ipcSendRaw(int port, const std::string& raw, int timeoutMs) {
    using R = Result<std::pair<int, std::string>>;
    static WinsockInit winsock;
    ipc_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == static_cast<ipc_sock>(~0)) return R::failure("socket");
    setTimeout(s, timeoutMs);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        IPC_CLOSE(s);
        return R::failure("no editor is listening");
    }
    send(s, raw.data(), static_cast<int>(raw.size()), 0);
    std::string resp;
    char tmp[4096];
    for (int n; (n = recv(s, tmp, sizeof(tmp), 0)) > 0 && resp.size() < 65536;) resp.append(tmp, static_cast<size_t>(n));
    IPC_CLOSE(s);
    if (resp.rfind("HTTP/1.1 ", 0) != 0 || resp.size() < 12) return R::failure("bad response");
    int status = std::atoi(resp.substr(9, 3).c_str());
    size_t he = resp.find("\r\n\r\n");
    return std::pair<int, std::string>{status, he == std::string::npos ? std::string() : resp.substr(he + 4)};
}

bool forwardToRunningEditor(const fs::path& userData, const std::string& link, std::string* replyOut, int timeoutMs) {
    auto ep = readIpcEndpoint(userData);
    if (!ep) {
        if (replyOut) *replyOut = "no running editor";
        return false;
    }
    std::string req = std::format("POST {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nAuthorization: Bearer {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                                  "Connection: close\r\n\r\n{}",
                                  kIpcPath, ep->port, ep->token, kIpcContentType, link.size(), link);
    auto r = ipcSendRaw(ep->port, req, timeoutMs);
    if (!r) {
        if (replyOut) *replyOut = r.error();
        return false;
    }
    if (replyOut) *replyOut = r->second;
    return r->first == 200;
}

} // namespace fledge::editor
