#include "fledge/net/transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
using socklen_type = int;
#define FLEDGE_NET_CLOSE closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
using socklen_type = socklen_t;
#define FLEDGE_NET_CLOSE close
#endif

namespace fledge::net {

// ---------------------------------------------------------------------------------- address ---
std::string NetAddress::toString() const {
    return std::format("{}.{}.{}.{}:{}", (host >> 24) & 255, (host >> 16) & 255, (host >> 8) & 255, host & 255, port);
}

namespace {
struct WinsockInit {  // WSAStartup is reference counted; one per transport / resolve call is fine
#ifdef _WIN32
    WinsockInit() { WSADATA wsa; ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0; }
    ~WinsockInit() { if (ok) WSACleanup(); }
    bool ok = false;
#else
    bool ok = true;
#endif
};

std::optional<uint16_t> parsePort(const std::string& s) {
    if (s.empty() || s.size() > 5 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    int p = std::stoi(s);
    if (p > 65535) return std::nullopt;
    return static_cast<uint16_t>(p);
}
} // namespace

std::optional<NetAddress> NetAddress::parse(const std::string& text, bool allowAny) {
    auto colon = text.rfind(':');
    std::string hostPart = colon == std::string::npos ? "" : text.substr(0, colon);
    auto port = parsePort(colon == std::string::npos ? text : text.substr(colon + 1));
    if (!port) return std::nullopt;
    if (hostPart.empty()) return NetAddress{allowAny ? 0u : 0x7F000001u, *port};
    if (hostPart == "localhost") return NetAddress{0x7F000001u, *port};
    // dotted quad
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(hostPart.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
        return NetAddress{(a << 24) | (b << 16) | (c << 8) | d, *port};
    WinsockInit wsa;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (!wsa.ok || getaddrinfo(hostPart.c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
    NetAddress out{ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr), *port};
    freeaddrinfo(res);
    return out;
}

// -------------------------------------------------------------------------------------- UDP ---
std::unique_ptr<UdpTransport> UdpTransport::open(const NetAddress& bindAddr, std::string* error) {
    auto fail = [&](std::string msg) -> std::unique_ptr<UdpTransport> {
        if (error) *error = std::move(msg);
        return nullptr;
    };
    std::unique_ptr<UdpTransport> t(new UdpTransport());
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup failed");
#endif
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == static_cast<sock_t>(~0)) {
#ifdef _WIN32
        WSACleanup();
#endif
        return fail("cannot create a UDP socket");
    }
    t->m_socket = static_cast<uintptr_t>(s);  // the destructor closes it (and balances WSAStartup) from here on
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bindAddr.port);
    addr.sin_addr.s_addr = htonl(bindAddr.host);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return fail(std::format("cannot bind UDP {}", bindAddr.toString()));
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // Without this, an ICMP "port unreachable" (a peer that went away) makes the next recvfrom fail.
    BOOL reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &bytes, nullptr, nullptr);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int buf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    sockaddr_in bound{};
    socklen_type len = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
    t->m_local = {bindAddr.host, ntohs(bound.sin_port)};
    return t;
}

UdpTransport::~UdpTransport() {
    if (m_socket == ~uintptr_t(0)) return;
    FLEDGE_NET_CLOSE(static_cast<sock_t>(m_socket));
#ifdef _WIN32
    WSACleanup();
#endif
}

bool UdpTransport::send(const NetAddress& to, const uint8_t* data, size_t size) {
    if (size > kMaxDatagram) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    addr.sin_addr.s_addr = htonl(to.host);
    auto sent = sendto(static_cast<sock_t>(m_socket), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                       reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<decltype(sent)>(size);
}

bool UdpTransport::receive(Datagram& out) {
    uint8_t buf[2048];
    for (int attempt = 0; attempt < 64; ++attempt) {
        sockaddr_in from{};
        socklen_type len = sizeof(from);
        auto n = recvfrom(static_cast<sock_t>(m_socket), reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAECONNRESET || err == WSAEMSGSIZE) continue;  // stale ICMP / oversized datagram: skip it
#endif
            return false;  // would block (nothing waiting) or a real error
        }
        out.from = {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
        out.data.assign(buf, buf + n);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------- simulated network ---
class SimEndpoint final : public Transport {
public:
    SimEndpoint(SimulatedNetwork* net, NetAddress addr) : m_net(net), m_addr(addr) { net->m_endpoints[addr] = this; }
    ~SimEndpoint() override { m_net->m_endpoints.erase(m_addr); }
    bool send(const NetAddress& to, const uint8_t* data, size_t size) override {
        if (size > kMaxDatagram) return false;
        m_net->submit(m_addr, to, data, size);
        return true;
    }
    bool receive(Datagram& out) override {
        if (m_inbox.empty()) return false;
        out = std::move(m_inbox.front());
        m_inbox.pop_front();
        return true;
    }
    NetAddress localAddress() const override { return m_addr; }
    std::deque<Datagram> m_inbox;

private:
    SimulatedNetwork* m_net;
    NetAddress m_addr;
};

SimulatedNetwork::SimulatedNetwork(uint64_t seed) : m_rng(seed ? seed : 0x9E3779B97F4A7C15ull) {}
SimulatedNetwork::~SimulatedNetwork() = default;

std::unique_ptr<Transport> SimulatedNetwork::createEndpoint(uint16_t port) {
    NetAddress a{0x0A000000u | m_nextHost++, port};
    return std::make_unique<SimEndpoint>(this, a);
}

double SimulatedNetwork::random01() {  // xorshift64*: same sequence on every platform
    m_rng ^= m_rng >> 12;
    m_rng ^= m_rng << 25;
    m_rng ^= m_rng >> 27;
    return static_cast<double>((m_rng * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}

void SimulatedNetwork::submit(const NetAddress& from, const NetAddress& to, const uint8_t* data, size_t size) {
    auto it = m_perSender.find(from);
    const LinkConditions& c = it != m_perSender.end() ? it->second : m_default;
    ++m_stats.sent;
    m_stats.bytes += size;
    if (c.loss > 0 && random01() < c.loss) {
        ++m_stats.dropped;
        return;
    }
    int copies = (c.duplicate > 0 && random01() < c.duplicate) ? 2 : 1;
    if (copies == 2) ++m_stats.duplicated;
    for (int i = 0; i < copies; ++i) {
        double delay = c.latency + (c.jitter > 0 ? random01() * c.jitter : 0.0);
        m_inFlight.push_back({m_now + delay, m_order++, from, to, std::vector<uint8_t>(data, data + size)});
    }
    if (c.latency <= 0 && c.jitter <= 0) deliverDue();  // zero-latency link: deliver immediately
}

void SimulatedNetwork::deliverDue() {
    std::stable_sort(m_inFlight.begin(), m_inFlight.end(), [](const InFlight& a, const InFlight& b) {
        return a.deliverAt != b.deliverAt ? a.deliverAt < b.deliverAt : a.order < b.order;
    });
    size_t n = 0;
    while (n < m_inFlight.size() && m_inFlight[n].deliverAt <= m_now + 1e-12) ++n;
    for (size_t i = 0; i < n; ++i) {
        auto& p = m_inFlight[i];
        auto ep = m_endpoints.find(p.to);
        if (ep == m_endpoints.end()) {
            ++m_stats.dropped;  // nobody listening
            continue;
        }
        ++m_stats.delivered;
        ep->second->m_inbox.push_back({p.from, std::move(p.data)});
    }
    m_inFlight.erase(m_inFlight.begin(), m_inFlight.begin() + static_cast<std::ptrdiff_t>(n));
}

void SimulatedNetwork::advance(double seconds) {
    m_now += seconds;
    deliverDue();
}

} // namespace fledge::net
