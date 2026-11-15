#include <algorithm>
#include <format>

#include "fledge/core/log.h"
#include "fledge/net/net.h"
#include "fledge/scene/components.h"

namespace fledge::net {

namespace {
constexpr size_t kMaxHistory = 64;
constexpr size_t kMaxPendingInputs = 64;
constexpr int kMaxInputsPerPacket = 32;

void header(ByteWriter& w, PacketType type) {
    w.u32(kProtocolId);
    w.u8(static_cast<uint8_t>(type));
}
} // namespace

NetServer::NetServer(Scene& scene, std::unique_ptr<Transport> transport, ServerConfig config)
    : m_scene(scene), m_transport(std::move(transport)), m_config(config), m_schema(ReplicationSchema::get()) {
    m_config.tickRate = std::max(1, m_config.tickRate);
    m_config.snapshotRate = std::clamp(m_config.snapshotRate, 1, m_config.tickRate);
}

NetServer::~NetServer() {
    for (auto& [id, c] : m_clients) {
        ByteWriter b;
        b.u64(c->salt);
        for (int i = 0; i < 3; ++i) sendRaw(c->address, PacketType::Disconnect, b.data());
    }
}

void NetServer::sendRaw(const NetAddress& to, PacketType type, const std::vector<uint8_t>& body) {
    ByteWriter w;
    header(w, type);
    w.append(body);
    m_transport->send(to, w.data());
}

void NetServer::sendData(double now, Client& c, const std::vector<uint8_t>& unreliable) {
    ByteWriter w;
    header(w, PacketType::Data);
    c.conn.writePacket(now, w, unreliable);
    m_transport->send(c.address, w.data());
}

EntityId NetServer::spawnListenHost() {
    if (m_hostPlayer != kNullEntity && m_scene.isValid(m_hostPlayer)) return m_hostPlayer;
    m_hostPlayer = defaultSpawn(m_scene, 0);
    return m_hostPlayer;
}

void NetServer::tickHost(const NetInput& input) {
    if (m_hostPlayer == kNullEntity || !m_scene.isValid(m_hostPlayer)) return;
    float dt = 1.0f / static_cast<float>(m_config.tickRate);
    PlayerState s = readPlayerState(m_scene, m_hostPlayer);
    playerSim(s, readPlayerParams(m_scene, m_hostPlayer), input, dt);
    writePlayerState(m_scene, m_hostPlayer, s);
}

EntityId NetServer::defaultSpawn(Scene& scene, ClientId id) {
    EntityId e = scene.createEntity(id == 0 ? "Host" : std::format("Player {}", id));
    scene.get<TransformComponent>(e)->position = {static_cast<float>(id) * 2.0f, 0.5f, 0};
    scene.add<NetIdentityComponent>(e, {static_cast<int>(id), 0});
    scene.add<NetPlayerComponent>(e, {});
    MeshRendererComponent mr;
    mr.mesh = "builtin:sphere";
    static const Vec3 colors[] = {{0.9f, 0.3f, 0.2f}, {0.2f, 0.5f, 0.9f}, {0.3f, 0.8f, 0.3f}, {0.9f, 0.8f, 0.2f}};
    mr.color = colors[id % 4];
    scene.add<MeshRendererComponent>(e, mr);
    return e;
}

void NetServer::update(double now) {
    m_lastNow = now;
    receive(now);
    std::vector<ClientId> timedOut;
    for (auto& [id, c] : m_clients)
        if (now - c->conn.lastReceiveTime() > m_config.timeout) timedOut.push_back(id);
    for (ClientId id : timedOut) {
        FLEDGE_LOG_INFO("net", "client {} timed out", id);
        removeClient(id, false);
    }

    ++m_tick;
    float dt = 1.0f / static_cast<float>(m_config.tickRate);
    for (auto& [id, c] : m_clients) {
        c->inputCredit = std::min(c->inputCredit + 1, m_config.inputCreditMax);
        while (c->inputCredit > 0 && !c->pendingInputs.empty()) {
            auto it = c->pendingInputs.begin();
            uint32_t tick = it->first;
            NetInput in = it->second;
            c->pendingInputs.erase(it);
            if (tick <= c->lastProcessedInput) continue;
            c->inputsSkipped += tick - c->lastProcessedInput - 1;  // lost on every redundant copy
            c->lastProcessedInput = tick;
            --c->inputCredit;
            ++c->inputsProcessed;
            if (c->player != kNullEntity && m_scene.isValid(c->player) && m_scene.has<NetPlayerComponent>(c->player)) {
                PlayerState s = readPlayerState(m_scene, c->player);
                playerSim(s, readPlayerParams(m_scene, c->player), in, dt);
                writePlayerState(m_scene, c->player, s);
            }
        }
    }

    int interval = std::max(1, m_config.tickRate / m_config.snapshotRate);
    if (m_tick % static_cast<uint32_t>(interval) == 0 && !m_clients.empty()) {
        std::map<EntityId, EntityState> current;
        auto& ids = m_scene.pool<NetIdentityComponent>().entities();
        for (EntityId e : ids) current.emplace(e, captureEntity(m_schema, m_scene, e));
        for (auto& [id, c] : m_clients) sendSnapshot(now, *c, current);
    } else {
        for (auto& [id, c] : m_clients)
            if (c->conn.pendingReliable() > 0) sendData(now, *c, {});
    }
}

void NetServer::receive(double now) {
    Datagram d;
    while (m_transport->receive(d)) {
        ByteReader r(d.data);
        if (r.u32() != kProtocolId || !r.ok()) {
            ++m_malformed;
            continue;
        }
        auto type = static_cast<PacketType>(r.u8());
        Client* c = nullptr;
        for (auto& [id, cl] : m_clients)
            if (cl->address == d.from) c = cl.get();
        switch (type) {
            case PacketType::ConnectRequest: handleConnect(now, d, r); break;
            case PacketType::Data:
                if (c) handleData(now, *c, r);
                break;
            case PacketType::Disconnect:
                if (c && r.u64() == c->salt && r.ok()) {
                    FLEDGE_LOG_INFO("net", "client {} left", c->id);
                    removeClient(c->id, false);
                }
                break;
            default: ++m_malformed; break;
        }
    }
}

void NetServer::handleConnect(double now, const Datagram& d, ByteReader& r) {
    uint8_t version = r.u8();
    uint64_t salt = r.u64();
    uint32_t schemaHash = r.u32();
    if (!r.ok()) {
        ++m_malformed;
        return;
    }
    auto deny = [&](DenyReason why) {
        ByteWriter b;
        b.u64(salt);
        b.u8(static_cast<uint8_t>(why));
        sendRaw(d.from, PacketType::ConnectDeny, b.data());
    };
    if (version != kProtocolVersion) return deny(DenyReason::VersionMismatch);
    if (schemaHash != m_schema.hash) return deny(DenyReason::SchemaMismatch);
    Client* existing = nullptr;
    for (auto& [id, cl] : m_clients)
        if (cl->address == d.from) existing = cl.get();
    if (existing && existing->salt != salt) {  // same address, new session: the old one is gone
        removeClient(existing->id, false);
        existing = nullptr;
    }
    if (!existing) {
        if (static_cast<int>(m_clients.size()) >= m_config.maxClients) return deny(DenyReason::ServerFull);
        auto c = std::make_unique<Client>();
        c->id = m_nextClientId++;
        if (m_nextClientId == 0) m_nextClientId = 1;
        c->address = d.from;
        c->salt = salt;
        c->conn.setLastReceiveTime(now);
        existing = c.get();
        m_clients.emplace(c->id, std::move(c));
        existing->player = onClientJoined ? onClientJoined(m_scene, existing->id) : defaultSpawn(m_scene, existing->id);
        m_peakClients = std::max(m_peakClients, m_clients.size());
        FLEDGE_LOG_INFO("net", "client {} joined from {}", existing->id, d.from.toString());
    }
    ByteWriter b;  // (re)send the accept: the first one may have been lost
    b.u64(salt);
    b.u16(existing->id);
    b.u16(static_cast<uint16_t>(m_config.tickRate));
    b.u16(static_cast<uint16_t>(m_config.snapshotRate));
    b.u32(m_tick);
    sendRaw(d.from, PacketType::ConnectAccept, b.data());
}

void NetServer::handleData(double now, Client& c, ByteReader& r) {
    std::vector<uint8_t> payload;
    if (!c.conn.readPacket(now, r, payload)) return;  // duplicate, too old or malformed
    std::vector<uint8_t> msg;
    while (c.conn.popReliable(msg))
        if (onMessage) onMessage(c.id, msg);
    if (payload.empty()) return;
    ByteReader p(payload);
    auto kind = static_cast<PayloadKind>(p.u8());
    if (kind != PayloadKind::Input) {
        ++m_malformed;
        return;
    }
    uint32_t ack = p.u32();
    int count = p.u8();
    uint32_t first = p.u32();
    if (!p.ok() || count > kMaxInputsPerPacket) {
        ++m_malformed;
        return;
    }
    std::vector<NetInput> inputs(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        inputs[static_cast<size_t>(i)].read(p);
        inputs[static_cast<size_t>(i)].tick = first + static_cast<uint32_t>(i);
    }
    if (!p.ok()) {
        ++m_malformed;
        return;
    }
    if (ack > c.ackedSnapshot && ack <= m_tick) c.ackedSnapshot = ack;
    for (auto& in : inputs) {
        if (in.tick <= c.lastProcessedInput || in.tick > c.lastProcessedInput + 1024) continue;
        if (c.pendingInputs.size() >= kMaxPendingInputs && !c.pendingInputs.contains(in.tick)) break;
        c.pendingInputs[in.tick] = in;
    }
}

bool NetServer::relevant(const Client& c, EntityId e, const Vec3& viewer, bool hasViewer) {
    auto* ni = m_scene.get<NetIdentityComponent>(e);
    if (!ni) return false;
    if (ni->owner == c.id || ni->relevancyRadius <= 0 || !hasViewer) return true;
    auto* t = m_scene.get<TransformComponent>(e);
    return t && length(t->position - viewer) <= ni->relevancyRadius;
}

void NetServer::sendSnapshot(double now, Client& c, const std::map<EntityId, EntityState>& current) {
    const SentSnapshot* base = nullptr;
    if (m_config.deltaCompression && c.ackedSnapshot)
        for (auto& h : c.history)
            if (h.tick == c.ackedSnapshot) base = &h;

    bool hasViewer = c.player != kNullEntity && m_scene.isValid(c.player);
    Vec3 viewer = hasViewer ? m_scene.get<TransformComponent>(c.player)->position : Vec3{};
    std::vector<EntityId> rel;
    for (auto& [e, st] : current)
        if (relevant(c, e, viewer, hasViewer)) rel.push_back(e);
    std::set<EntityId> relSet(rel.begin(), rel.end());
    // Priority: the client's own entities, then (with a baseline) the ones not sent for longest, else by id
    // so that baseline-less snapshots during the join pick the same subset every time.
    auto ownerFirst = [&](EntityId e) { auto* ni = m_scene.get<NetIdentityComponent>(e); return ni && ni->owner == c.id ? 0 : 1; };
    std::stable_sort(rel.begin(), rel.end(), [&](EntityId a, EntityId b) {
        int oa = ownerFirst(a), ob = ownerFirst(b);
        if (oa != ob) return oa < ob;
        if (base) {
            auto ia = c.lastSentTick.find(a), ib = c.lastSentTick.find(b);
            uint32_t ta = ia != c.lastSentTick.end() ? ia->second : 0, tb = ib != c.lastSentTick.end() ? ib->second : 0;
            if (ta != tb) return ta < tb;
        }
        return a < b;
    });

    SentSnapshot snap;
    snap.tick = m_tick;
    ByteWriter body;
    uint32_t count = 0;
    if (base) {
        for (auto& [e, st] : base->entities) {
            if (relSet.contains(e)) continue;
            ByteWriter rec;
            rec.varint((static_cast<uint64_t>(e) << 1) | 1);  // removed (destroyed or no longer relevant)
            if (body.size() + rec.size() > m_config.snapshotBudget) {
                snap.entities.emplace(e, st);  // the client still has it: retry next time
                continue;
            }
            body.append(rec.data());
            ++count;
            c.lastSentTick.erase(e);
        }
    }
    for (EntityId e : rel) {
        const EntityState& cur = current.at(e);
        const EntityState* b = nullptr;
        if (base)
            if (auto it = base->entities.find(e); it != base->entities.end()) b = &it->second;
        ByteWriter rec;
        rec.varint(static_cast<uint64_t>(e) << 1);
        if (!writeEntityDelta(m_schema, rec, b, cur)) {  // unchanged since the baseline
            snap.entities.emplace(e, cur);
            continue;
        }
        if (body.size() + rec.size() > m_config.snapshotBudget) {  // over budget: the client keeps the baseline
            if (b) snap.entities.emplace(e, *b);
            continue;
        }
        body.append(rec.data());
        ++count;
        snap.entities.emplace(e, cur);
        c.lastSentTick[e] = m_tick;
    }

    ByteWriter payload;
    payload.u8(static_cast<uint8_t>(PayloadKind::Snapshot));
    payload.u32(m_tick);
    payload.u32(base ? base->tick : 0);
    payload.u32(c.lastProcessedInput);
    payload.varint(count);
    payload.append(body.data());
    sendData(now, c, payload.data());
    ++m_snapshotsSent;
    m_snapshotBytes += payload.size();
    c.history.push_back(std::move(snap));
    while (c.history.size() > kMaxHistory) c.history.pop_front();
}

bool NetServer::send(ClientId client, std::vector<uint8_t> message) {
    auto it = m_clients.find(client);
    return it != m_clients.end() && it->second->conn.sendReliable(std::move(message));
}

void NetServer::broadcast(const std::vector<uint8_t>& message) {
    for (auto& [id, c] : m_clients) c->conn.sendReliable(message);
}

void NetServer::disconnect(ClientId client) { removeClient(client, true); }

void NetServer::removeClient(ClientId id, bool notify) {
    auto it = m_clients.find(id);
    if (it == m_clients.end()) return;
    if (notify) {
        ByteWriter b;
        b.u64(it->second->salt);
        for (int i = 0; i < 3; ++i) sendRaw(it->second->address, PacketType::Disconnect, b.data());
    }
    m_clients.erase(it);
    if (onClientLeft) onClientLeft(id);
    std::vector<EntityId> owned;
    auto& pool = m_scene.pool<NetIdentityComponent>();
    for (size_t i = 0; i < pool.entities().size(); ++i)
        if (pool.components()[i].owner == static_cast<int>(id)) owned.push_back(pool.entities()[i]);
    for (EntityId e : owned)
        if (m_scene.isValid(e)) m_scene.destroyEntity(e);
}

std::vector<ClientInfo> NetServer::clients() const {
    std::vector<ClientInfo> out;
    for (auto& [id, c] : m_clients)
        out.push_back({c->id, c->address, c->player, c->lastProcessedInput, c->ackedSnapshot, c->inputsProcessed, c->inputsSkipped, &c->conn.stats()});
    return out;
}

} // namespace fledge::net
