#include "fledge/net/connection.h"

#include <algorithm>

namespace fledge::net {

namespace {
constexpr size_t kMaxReliableMessage = 1024;
constexpr size_t kMaxQueuedReliable = 4096;
constexpr uint16_t kReceiveWindow = 1024;  // reliable ids further ahead than this are rejected
} // namespace

bool Connection::sendReliable(std::vector<uint8_t> message) {
    if (message.size() > kMaxReliableMessage || m_outgoing.size() >= kMaxQueuedReliable) return false;
    m_outgoing.push_back({m_nextOutId++, std::move(message), -1});
    return true;
}

void Connection::writePacket(double now, ByteWriter& w, const std::vector<uint8_t>& unreliable, size_t maxBytes) {
    uint16_t seq = m_localSeq++;
    SentPacket& sp = m_sent[seq % kWindow];
    if (sp.used && !sp.acked) ++m_stats.packetsLost;
    sp.seq = seq;
    sp.used = true;
    sp.acked = false;
    sp.time = now;
    sp.messages.clear();

    size_t start = w.size();
    w.u16(seq);
    w.u8(m_anyReceived ? 1 : 0);
    w.u16(m_remoteSeq);
    w.u32(m_receivedBits);

    double resendDelay = std::max(0.1, m_stats.rtt * 1.5);
    ByteWriter rel;
    uint32_t count = 0;
    size_t budget = maxBytes > unreliable.size() + 16 ? maxBytes - unreliable.size() - 16 : 0;
    for (auto& m : m_outgoing) {
        if (m.lastSent >= 0 && now - m.lastSent < resendDelay) continue;
        size_t need = 2 + 2 + m.data.size();
        if (rel.size() + need > budget) break;  // the rest waits for the next packet
        rel.u16(m.id);
        rel.varint(m.data.size());
        rel.append(m.data);
        if (m.lastSent >= 0) ++m_stats.reliableResent;
        else ++m_stats.reliableSent;
        m.lastSent = now;
        sp.messages.push_back(m.id);
        ++count;
    }
    w.varint(count);
    w.append(rel.data());
    w.append(unreliable);
    ++m_stats.packetsSent;
    m_stats.bytesSent += w.size() - start;
}

void Connection::onAcked(uint16_t seq, double now) {
    SentPacket& sp = m_sent[seq % kWindow];
    if (!sp.used || sp.seq != seq || sp.acked) return;
    sp.acked = true;
    ++m_stats.packetsAcked;
    double sample = now - sp.time;
    m_stats.rtt = m_stats.rtt <= 0 ? sample : m_stats.rtt + 0.1 * (sample - m_stats.rtt);
    for (uint16_t id : sp.messages) {
        auto it = std::find_if(m_outgoing.begin(), m_outgoing.end(), [id](const OutMessage& m) { return m.id == id; });
        if (it != m_outgoing.end()) m_outgoing.erase(it);
    }
}

bool Connection::readPacket(double now, ByteReader& r, std::vector<uint8_t>& unreliableOut) {
    size_t start = r.position();
    uint16_t seq = r.u16();
    bool hasAck = r.u8() & 1;
    uint16_t ack = r.u16();
    uint32_t ackBits = r.u32();
    uint64_t count = r.varint();
    if (!r.ok() || count > 256) return false;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> messages;
    messages.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint16_t id = r.u16();
        uint64_t len = r.varint();
        if (!r.ok() || len > kMaxReliableMessage || len > r.remaining()) return false;
        std::vector<uint8_t> data(r.cursor(), r.cursor() + len);
        r.skip(static_cast<size_t>(len));
        messages.emplace_back(id, std::move(data));
    }
    if (!r.ok()) return false;

    // Sequence bookkeeping (duplicates and packets older than the 32-packet ack window are dropped).
    if (!m_anyReceived) {
        m_anyReceived = true;
        m_remoteSeq = seq;
        m_receivedBits = 0;
    } else if (seq == m_remoteSeq) {
        ++m_stats.duplicatesDropped;
        return false;
    } else if (seqGreater(seq, m_remoteSeq)) {
        uint16_t shift = static_cast<uint16_t>(seq - m_remoteSeq);
        m_receivedBits = shift >= 32 ? 0 : (m_receivedBits << shift);
        if (shift <= 32) m_receivedBits |= 1u << (shift - 1);  // the previous newest
        m_remoteSeq = seq;
    } else {
        uint16_t back = static_cast<uint16_t>(m_remoteSeq - seq);
        if (back > 32) {
            ++m_stats.staleDropped;
            return false;
        }
        uint32_t bit = 1u << (back - 1);
        if (m_receivedBits & bit) {
            ++m_stats.duplicatesDropped;
            return false;
        }
        m_receivedBits |= bit;
    }

    if (hasAck) {
        onAcked(ack, now);
        for (int i = 0; i < 32; ++i)
            if (ackBits & (1u << i)) onAcked(static_cast<uint16_t>(ack - 1 - i), now);
    }
    for (auto& [id, data] : messages) {
        uint16_t ahead = static_cast<uint16_t>(id - m_nextInId);
        if (ahead >= kReceiveWindow || m_inBuffer.contains(id)) {  // already delivered (or absurdly far ahead)
            ++m_stats.duplicatesDropped;
            continue;
        }
        m_inBuffer.emplace(id, std::move(data));
    }
    for (auto it = m_inBuffer.find(m_nextInId); it != m_inBuffer.end(); it = m_inBuffer.find(m_nextInId)) {
        m_delivered.push_back(std::move(it->second));
        m_inBuffer.erase(it);
        ++m_nextInId;
        ++m_stats.reliableDelivered;
    }
    unreliableOut.assign(r.cursor(), r.cursor() + r.remaining());
    r.skip(r.remaining());
    m_lastReceive = now;
    ++m_stats.packetsReceived;
    m_stats.bytesReceived += r.position() - start;
    return true;
}

bool Connection::popReliable(std::vector<uint8_t>& out) {
    if (m_delivered.empty()) return false;
    out = std::move(m_delivered.front());
    m_delivered.pop_front();
    return true;
}

} // namespace fledge::net
