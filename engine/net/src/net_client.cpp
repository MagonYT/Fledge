#include <algorithm>
#include <chrono>
#include <random>

#include "fledge/core/log.h"
#include "fledge/net/net.h"
#include "fledge/scene/components.h"

namespace fledge::net {

namespace {
constexpr size_t kMaxReceived = 64;
constexpr size_t kMaxHistory = 256;
constexpr size_t kMaxSamples = 32;

int findSlot(const ReplicationSchema& s, std::string_view component, std::string_view property) {
    for (size_t i = 0; i < s.slots.size(); ++i) {
        const auto& slot = s.slots[i];
        if (s.components[static_cast<size_t>(slot.component)].name == component && slot.info->name == property) return static_cast<int>(i);
    }
    return -1;
}

ByteReader readerOf(const std::string& b) { return ByteReader(reinterpret_cast<const uint8_t*>(b.data()), b.size()); }

Quat nlerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) b = {-b.x, -b.y, -b.z, -b.w};
    return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
}
} // namespace

const char* clientStateName(ClientState s) {
    switch (s) {
        case ClientState::Disconnected: return "disconnected";
        case ClientState::Connecting: return "connecting";
        case ClientState::Connected: return "connected";
        case ClientState::Denied: return "denied";
        case ClientState::TimedOut: return "timed out";
    }
    return "?";
}

NetClient::NetClient(Scene& scene, std::unique_ptr<Transport> transport, NetAddress server, ClientConfig config)
    : m_scene(scene), m_transport(std::move(transport)), m_server(server), m_config(config), m_schema(ReplicationSchema::get()) {
    m_config.tickRate = std::max(1, m_config.tickRate);
    m_config.inputRedundancy = std::clamp(m_config.inputRedundancy, 1, 32);
    m_slotPosition = findSlot(m_schema, "Transform", "position");
    m_slotRotation = findSlot(m_schema, "Transform", "rotation");
    m_slotVelocity = findSlot(m_schema, "NetPlayer", "velocity");
}

NetClient::~NetClient() { disconnect(); }

void NetClient::sendRaw(PacketType type, const std::vector<uint8_t>& body) {
    ByteWriter w;
    w.u32(kProtocolId);
    w.u8(static_cast<uint8_t>(type));
    w.append(body);
    m_transport->send(m_server, w.data());
}

void NetClient::connect(double now) {
    clearMirror();
    m_conn = Connection{};
    std::random_device rd;
    m_salt = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    m_state = ClientState::Connecting;
    m_connectStart = now;
    m_lastConnectSend = -1;
    m_id = 0;
}

void NetClient::disconnect() {
    if (m_state == ClientState::Connected || m_state == ClientState::Connecting) {
        ByteWriter b;
        b.u64(m_salt);
        for (int i = 0; i < 3; ++i) sendRaw(PacketType::Disconnect, b.data());
    }
    m_state = ClientState::Disconnected;
    clearMirror();
}

void NetClient::clearMirror() {
    for (auto& [netId, m] : m_entities)
        if (m.local != kNullEntity && m_scene.isValid(m.local)) m_scene.destroyEntity(m.local);
    m_entities.clear();
    m_received.clear();
    m_history.clear();
    m_latestTick = 0;
    m_lastProcessedInput = 0;
    m_localPlayerNet = kNullEntity;
}

bool NetClient::send(std::vector<uint8_t> message) { return m_state == ClientState::Connected && m_conn.sendReliable(std::move(message)); }

EntityId NetClient::localEntity(EntityId netId) const {
    auto it = m_entities.find(netId);
    return it == m_entities.end() ? kNullEntity : it->second.local;
}

EntityId NetClient::netIdOf(EntityId local) const {
    for (auto& [netId, m] : m_entities)
        if (m.local == local) return netId;
    return kNullEntity;
}

EntityId NetClient::localPlayer() const { return m_localPlayerNet ? localEntity(m_localPlayerNet) : kNullEntity; }

void NetClient::update(double now) {
    if (m_state == ClientState::Connecting) {
        if (now - m_connectStart > m_config.connectTimeout) {
            m_state = ClientState::TimedOut;
            FLEDGE_LOG_WARN("net", "connecting to {} timed out", m_server.toString());
            return;
        }
        if (m_lastConnectSend < 0 || now - m_lastConnectSend >= 0.1) {
            ByteWriter b;
            b.u8(kProtocolVersion);
            b.u64(m_salt);
            b.u32(m_schema.hash);
            sendRaw(PacketType::ConnectRequest, b.data());
            m_lastConnectSend = now;
        }
    }
    receive(now);
    if (m_state != ClientState::Connected) return;
    if (now - m_conn.lastReceiveTime() > m_config.timeout) {
        FLEDGE_LOG_WARN("net", "server {} timed out", m_server.toString());
        m_state = ClientState::TimedOut;
        clearMirror();
        return;
    }
    predictAndSend(now);
    interpolate(now);
}

void NetClient::receive(double now) {
    Datagram d;
    while (m_transport->receive(d)) {
        if (!(d.from == m_server)) continue;
        ByteReader r(d.data);
        if (r.u32() != kProtocolId || !r.ok()) continue;
        auto type = static_cast<PacketType>(r.u8());
        switch (type) {
            case PacketType::ConnectAccept: {
                uint64_t salt = r.u64();
                ClientId id = r.u16();
                int tickRate = r.u16();
                r.u16();  // snapshot rate (informational)
                uint32_t serverTick = r.u32();
                if (!r.ok() || salt != m_salt || m_state != ClientState::Connecting) break;
                if (tickRate != m_config.tickRate)
                    FLEDGE_LOG_WARN("net", "server ticks at {} Hz, client at {} Hz: prediction will drift", tickRate, m_config.tickRate);
                m_id = id;
                m_state = ClientState::Connected;
                m_conn.setLastReceiveTime(now);
                m_latestRecvTime = now;
                (void)serverTick;
                FLEDGE_LOG_INFO("net", "connected to {} as client {}", m_server.toString(), id);
                break;
            }
            case PacketType::ConnectDeny: {
                uint64_t salt = r.u64();
                uint8_t why = r.u8();
                if (!r.ok() || salt != m_salt || m_state != ClientState::Connecting) break;
                m_denyReason = static_cast<DenyReason>(why);
                m_state = ClientState::Denied;
                FLEDGE_LOG_WARN("net", "server {} refused the connection (reason {})", m_server.toString(), why);
                break;
            }
            case PacketType::Disconnect: {
                if (r.u64() != m_salt || !r.ok() || m_state != ClientState::Connected) break;
                FLEDGE_LOG_INFO("net", "server closed the connection");
                m_state = ClientState::Disconnected;
                clearMirror();
                return;
            }
            case PacketType::Data: {
                if (m_state != ClientState::Connected) break;
                std::vector<uint8_t> payload;
                if (!m_conn.readPacket(now, r, payload)) break;
                std::vector<uint8_t> msg;
                while (m_conn.popReliable(msg))
                    if (onMessage) onMessage(msg);
                if (payload.empty()) break;
                ByteReader p(payload);
                if (static_cast<PayloadKind>(p.u8()) == PayloadKind::Snapshot) handleSnapshot(now, p);
                break;
            }
            default: break;
        }
    }
}

void NetClient::handleSnapshot(double now, ByteReader& r) {
    uint32_t tick = r.u32();
    uint32_t baseTick = r.u32();
    uint32_t lastProcessed = r.u32();
    uint64_t count = r.varint();
    if (!r.ok() || count > 100000) return;
    ++m_stats.snapshotsReceived;
    if (tick <= m_latestTick) {  // reordered: an older snapshot arrived after a newer one
        ++m_stats.snapshotsStale;
        return;
    }
    ReceivedSnapshot snap;
    snap.tick = tick;
    if (baseTick != 0) {
        auto it = std::find_if(m_received.begin(), m_received.end(), [&](const ReceivedSnapshot& s) { return s.tick == baseTick; });
        if (it == m_received.end()) {
            ++m_stats.snapshotsDroppedNoBaseline;
            return;
        }
        snap.entities = it->entities;
    }
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = r.varint();
        if (!r.ok()) return;
        auto netId = static_cast<EntityId>(key >> 1);
        if (key & 1) {
            snap.entities.erase(netId);
            continue;
        }
        bool existed = snap.entities.contains(netId);
        if (!existed && (r.remaining() == 0 || !(r.cursor()[0] & 1))) return;  // a delta for an entity we never had
        if (!readEntityDelta(m_schema, r, snap.entities[netId])) return;
    }
    if (!r.ok()) return;
    m_latestTick = tick;
    m_latestRecvTime = now;
    ++m_stats.snapshotsApplied;
    applySnapshot(now, snap, lastProcessed);
    m_received.push_back(std::move(snap));
    while (m_received.size() > kMaxReceived) m_received.pop_front();
}

bool NetClient::isLocalPlayer(const Mirrored& m) {
    auto* ni = m_scene.get<NetIdentityComponent>(m.local);
    return ni && ni->owner == static_cast<int>(m_id) && m_scene.has<NetPlayerComponent>(m.local);
}

void NetClient::applySnapshot(double /*now*/, const ReceivedSnapshot& snap, uint32_t lastProcessed) {
    for (auto it = m_entities.begin(); it != m_entities.end();) {
        if (snap.entities.contains(it->first)) {
            ++it;
            continue;
        }
        if (m_scene.isValid(it->second.local)) m_scene.destroyEntity(it->second.local);
        if (it->first == m_localPlayerNet) m_localPlayerNet = kNullEntity;
        ++m_stats.destroyed;
        it = m_entities.erase(it);
    }
    for (auto& [netId, st] : snap.entities) {
        Mirrored& m = m_entities[netId];
        bool isNew = m.local == kNullEntity || !m_scene.isValid(m.local);
        if (isNew) {
            m.local = m_scene.createEntity("net");
            m.samples.clear();
            ++m_stats.spawned;
        }
        bool changed = isNew || !(m.state == st);
        m.state = st;
        auto decodeVec = [&](int slot, Vec3 fallback) {
            if (slot < 0 || st.values[static_cast<size_t>(slot)].empty()) return fallback;
            auto r = readerOf(st.values[static_cast<size_t>(slot)]);
            return r.vec3();
        };
        auto decodeQuat = [&](int slot) {
            if (slot < 0 || st.values[static_cast<size_t>(slot)].empty()) return Quat{};
            auto r = readerOf(st.values[static_cast<size_t>(slot)]);
            return readQuat(r);
        };
        Vec3 pos = decodeVec(m_slotPosition, {});
        Quat rot = decodeQuat(m_slotRotation);
        if (changed) {
            // Transform position / rotation (and the local player's velocity) are routed below.
            applyEntity(m_schema, m_scene, m.local, st, [&](int slot) {
                return slot == m_slotPosition || slot == m_slotRotation || slot == m_slotVelocity;
            });
        }
        if (isLocalPlayer(m)) {
            if (m_localPlayerNet != netId) {
                m_localPlayerNet = netId;
                isNew = true;
            }
            PlayerState server;
            server.position = pos;
            server.velocity = decodeVec(m_slotVelocity, {});
            server.yaw = rot.toEuler().y;
            m_lastServerPlayer = server;
            if (isNew) writePlayerState(m_scene, m.local, server);
            reconcile(server, lastProcessed);
        } else {
            if (auto* p = m_scene.get<NetPlayerComponent>(m.local)) p->velocity = decodeVec(m_slotVelocity, p->velocity);
            if (isNew)
                if (auto* t = m_scene.get<TransformComponent>(m.local)) {
                    t->position = pos;
                    t->rotation = rot;
                }
            if (m.samples.empty() || m.samples.back().tick < snap.tick) m.samples.push_back({static_cast<double>(snap.tick), pos, rot});
            while (m.samples.size() > kMaxSamples) m.samples.pop_front();
        }
    }
}

void NetClient::reconcile(const PlayerState& server, uint32_t lastProcessed) {
    if (lastProcessed < m_lastProcessedInput) return;  // older information than we already used
    m_lastProcessedInput = lastProcessed;
    EntityId local = localPlayer();
    if (local == kNullEntity) return;
    auto acked = std::find_if(m_history.begin(), m_history.end(), [&](const PredictedInput& p) { return p.input.tick == lastProcessed; });
    // Without a prediction for that input (first state, nothing processed yet) the server state is adopted
    // and the pending inputs are replayed on top of it - not counted as a misprediction.
    bool mispredicted = true;
    if (acked != m_history.end()) {
        float err = length(acked->after.position - server.position);
        float velErr = length(acked->after.velocity - server.velocity);
        mispredicted = err > m_config.correctionEpsilon || velErr > 0.01f;
        if (mispredicted) {
            ++m_stats.corrections;
            m_stats.lastCorrection = err;
            m_stats.maxCorrection = std::max(m_stats.maxCorrection, err);
        }
    }
    // Keep the entry for lastProcessed itself: the next snapshot may report the same input again.
    while (!m_history.empty() && m_history.front().input.tick < lastProcessed) m_history.pop_front();
    if (!mispredicted) return;
    // Rewind to the server state and replay every input the server has not processed yet.
    PlayerState s = server;
    if (!m_history.empty()) s.yaw = m_history.front().input.yaw;
    PlayerParams params = readPlayerParams(m_scene, local);
    float dt = 1.0f / static_cast<float>(m_config.tickRate);
    for (auto& h : m_history) {
        if (h.input.tick == lastProcessed) {  // already contained in the server state
            h.after = server;
            continue;
        }
        playerSim(s, params, h.input, dt);
        h.after = s;
        ++m_stats.replayedInputs;
    }
    writePlayerState(m_scene, local, s);
}

void NetClient::predictAndSend(double now) {
    EntityId local = localPlayer();
    if (local != kNullEntity) {
        NetInput in = m_input;
        in.tick = ++m_inputTick;
        in.quantize();
        PlayerState s = readPlayerState(m_scene, local);
        playerSim(s, readPlayerParams(m_scene, local), in, 1.0f / static_cast<float>(m_config.tickRate));
        writePlayerState(m_scene, local, s);
        m_history.push_back({in, s});
        while (m_history.size() > kMaxHistory) m_history.pop_front();
    }
    // Input packet: snapshot ack + the newest commands (redundant copies cover lost packets).
    size_t n = std::min(m_history.size(), static_cast<size_t>(m_config.inputRedundancy));
    ByteWriter p;
    p.u8(static_cast<uint8_t>(PayloadKind::Input));
    p.u32(m_latestTick);
    p.u8(static_cast<uint8_t>(n));
    p.u32(n ? m_history[m_history.size() - n].input.tick : 0);
    for (size_t i = m_history.size() - n; i < m_history.size(); ++i) m_history[i].input.write(p);
    ByteWriter w;
    w.u32(kProtocolId);
    w.u8(static_cast<uint8_t>(PacketType::Data));
    m_conn.writePacket(now, w, p.data());
    m_transport->send(m_server, w.data());
}

void NetClient::interpolate(double now) {
    double renderTick = static_cast<double>(m_latestTick) + (now - m_latestRecvTime) * m_config.tickRate -
                        m_config.interpolationDelay * m_config.tickRate;
    for (auto& [netId, m] : m_entities) {
        if (netId == m_localPlayerNet || m.samples.empty()) continue;
        auto* t = m_scene.get<TransformComponent>(m.local);
        if (!t) continue;
        while (m.samples.size() >= 2 && m.samples[1].tick <= renderTick) m.samples.pop_front();  // keep one sample <= renderTick
        const Sample& a = m.samples.front();
        if (m.samples.size() == 1 || renderTick <= a.tick) {
            t->position = a.position;
            t->rotation = a.rotation;
            continue;
        }
        const Sample& b = m.samples[1];
        float k = static_cast<float>((renderTick - a.tick) / (b.tick - a.tick));
        k = std::clamp(k, 0.0f, 1.0f);
        t->position = lerp(a.position, b.position, k);
        t->rotation = nlerp(a.rotation, b.rotation, k);
    }
}

} // namespace fledge::net
