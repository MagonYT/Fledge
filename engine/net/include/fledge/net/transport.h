#pragma once
// Datagram transports for fledge_net:
//   - UdpTransport: a real non-blocking UDP socket (Winsock on Windows, BSD sockets elsewhere).
//   - SimulatedNetwork: an in-process "internet" for tests and local play. Endpoints exchange datagrams
//     through it with configurable latency, jitter (which reorders), loss and duplication. Time only moves
//     when advance() is called and the random source is seeded, so a test run is fully deterministic.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fledge::net {

struct NetAddress {
    uint32_t host = 0;  // IPv4, host byte order (127.0.0.1 = 0x7F000001)
    uint16_t port = 0;
    bool operator==(const NetAddress&) const = default;
    bool operator<(const NetAddress& o) const { return host != o.host ? host < o.host : port < o.port; }
    bool valid() const { return port != 0; }
    std::string toString() const;
    // "1.2.3.4:5678", "localhost:7777" or "host:port" (resolved with getaddrinfo, IPv4 only).
    // A bare port ("7777") means 0.0.0.0:7777 when allowAny, else 127.0.0.1.
    static std::optional<NetAddress> parse(const std::string& text, bool allowAny = false);
    static NetAddress loopback(uint16_t port) { return {0x7F000001u, port}; }
};

struct Datagram {
    NetAddress from;
    std::vector<uint8_t> data;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool send(const NetAddress& to, const uint8_t* data, size_t size) = 0;
    virtual bool receive(Datagram& out) = 0;  // false when nothing is waiting
    virtual NetAddress localAddress() const = 0;
    bool send(const NetAddress& to, const std::vector<uint8_t>& d) { return send(to, d.data(), d.size()); }
};

constexpr size_t kMaxDatagram = 1400;  // never send more than this in one datagram (below a 1500 MTU)

// ------------------------------------------------------------------------------------- UDP ---
class UdpTransport final : public Transport {
public:
    // Binds to bind (host 0 = all interfaces, port 0 = any free port). nullptr + *error on failure.
    static std::unique_ptr<UdpTransport> open(const NetAddress& bind, std::string* error = nullptr);
    ~UdpTransport() override;
    bool send(const NetAddress& to, const uint8_t* data, size_t size) override;
    bool receive(Datagram& out) override;
    NetAddress localAddress() const override { return m_local; }

private:
    UdpTransport() = default;
    uintptr_t m_socket = ~uintptr_t(0);
    NetAddress m_local;
};

// ------------------------------------------------------------------------- simulated network ---
struct LinkConditions {
    double latency = 0;     // one-way seconds
    double jitter = 0;      // + uniform [0, jitter) seconds per packet (reorders packets)
    double loss = 0;        // probability 0..1 a packet is dropped
    double duplicate = 0;   // probability 0..1 a packet is delivered twice
};

class SimEndpoint;

class SimulatedNetwork {
public:
    explicit SimulatedNetwork(uint64_t seed = 1);
    ~SimulatedNetwork();
    // An endpoint at 10.0.0.<n>:port. The network must outlive its endpoints.
    std::unique_ptr<Transport> createEndpoint(uint16_t port = 7777);
    void setConditions(const LinkConditions& c) { m_default = c; }
    // Conditions for packets sent by `from` (overrides the default), e.g. a bad uplink for one client.
    void setConditions(const NetAddress& from, const LinkConditions& c) { m_perSender[from] = c; }
    void advance(double seconds);  // moves time; due packets land in the receivers' queues
    double now() const { return m_now; }

    struct Stats {
        uint64_t sent = 0, delivered = 0, dropped = 0, duplicated = 0, bytes = 0;
    };
    const Stats& stats() const { return m_stats; }

private:
    friend class SimEndpoint;
    struct InFlight {
        double deliverAt;
        uint64_t order;
        NetAddress from, to;
        std::vector<uint8_t> data;
    };
    void submit(const NetAddress& from, const NetAddress& to, const uint8_t* data, size_t size);
    double random01();
    void deliverDue();

    LinkConditions m_default;
    std::map<NetAddress, LinkConditions> m_perSender;
    std::vector<InFlight> m_inFlight;
    std::map<NetAddress, SimEndpoint*> m_endpoints;
    double m_now = 0;
    uint64_t m_rng, m_order = 0;
    uint32_t m_nextHost = 1;
    Stats m_stats;
};

} // namespace fledge::net
