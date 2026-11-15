#pragma once
// One peer-to-peer link on top of a datagram transport: packet sequence numbers with acks (the last
// ack + a 32-bit history in every packet), round-trip time, and two channels:
//   - unreliable: one payload per packet (snapshots, input commands) - newest wins, never resent
//   - reliable ordered: messages are resent until acked and delivered exactly once, in order
// Wire format of a data packet body (after the shared header written by the server / client):
//   u16 seq, u16 ack, u32 ackBits, varint reliableCount, {u16 id, varint len, bytes}*, unreliable payload
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "fledge/net/bitstream.h"

namespace fledge::net {

// a is newer than b (16-bit wrap-around)
inline bool seqGreater(uint16_t a, uint16_t b) {
    return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
}

struct ConnectionStats {
    uint64_t packetsSent = 0, packetsReceived = 0, bytesSent = 0, bytesReceived = 0;
    uint64_t packetsAcked = 0, packetsLost = 0;  // own packets: acked / fell out of the ack window unacked
    uint64_t reliableSent = 0, reliableResent = 0, reliableDelivered = 0;
    uint64_t duplicatesDropped = 0, staleDropped = 0;
    double rtt = 0;  // smoothed round-trip time (s)
};

class Connection {
public:
    // Queues a reliable message (delivered in order on the other side). Max 1024 bytes each.
    bool sendReliable(std::vector<uint8_t> message);
    // Writes the data-packet body: header + acks + due reliable messages, then `unreliable`.
    // Reliable messages are resent after max(0.1 s, 1.5 x RTT) without an ack. maxBytes bounds the body.
    void writePacket(double now, ByteWriter& w, const std::vector<uint8_t>& unreliable, size_t maxBytes = 1300);
    // Reads a data-packet body. Returns false for malformed / duplicate / too-old packets (ignore them).
    // On success `unreliableOut` holds the unreliable payload (may be empty) and new reliable messages queue up.
    bool readPacket(double now, ByteReader& r, std::vector<uint8_t>& unreliableOut);
    bool popReliable(std::vector<uint8_t>& out);
    size_t pendingReliable() const { return m_outgoing.size(); }

    double lastReceiveTime() const { return m_lastReceive; }
    void setLastReceiveTime(double t) { m_lastReceive = t; }
    const ConnectionStats& stats() const { return m_stats; }
    ConnectionStats& stats() { return m_stats; }

private:
    struct SentPacket {
        uint16_t seq = 0;
        bool used = false, acked = false;
        double time = 0;
        std::vector<uint16_t> messages;
    };
    struct OutMessage {
        uint16_t id;
        std::vector<uint8_t> data;
        double lastSent = -1;
    };
    void onAcked(uint16_t seq, double now);

    static constexpr size_t kWindow = 256;
    uint16_t m_localSeq = 0;
    uint16_t m_remoteSeq = 0;
    uint32_t m_receivedBits = 0;  // bit i: m_remoteSeq - 1 - i received
    bool m_anyReceived = false;
    SentPacket m_sent[kWindow];
    std::deque<OutMessage> m_outgoing;  // unacked, in id order
    uint16_t m_nextOutId = 0;
    uint16_t m_nextInId = 0;  // next id to deliver
    std::map<uint16_t, std::vector<uint8_t>> m_inBuffer;  // out-of-order arrivals
    std::deque<std::vector<uint8_t>> m_delivered;
    double m_lastReceive = 0;
    ConnectionStats m_stats;
};

} // namespace fledge::net
