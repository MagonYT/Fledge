#pragma once
// fledge_net: server-authoritative client/server networking (docs/NETWORKING.md).
//
//   NetServer  owns the authoritative Scene. Every fixed tick it applies the clients' input commands to their
//              NetPlayer entities and, at the snapshot rate, sends each client a snapshot of the entities
//              that carry NetIdentity and are relevant to it, delta-compressed against the newest snapshot
//              that client acknowledged.
//   NetClient  mirrors those entities into its own Scene, interpolates remote ones between snapshots,
//              predicts its own NetPlayer from local input and reconciles when the server disagrees.
//
// Both are driven by the caller: update(now, dt) once per fixed simulation step. Nothing blocks and
// nothing runs on another thread, which keeps tests on SimulatedNetwork deterministic.
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "fledge/net/connection.h"
#include "fledge/net/replication.h"
#include "fledge/net/transport.h"

namespace fledge::net {

using ClientId = uint16_t;

// ------------------------------------------------------------------------------------ server ---
struct ServerConfig {
    int maxClients = 8;
    int tickRate = 60;             // fixed simulation steps per second (update() calls)
    int snapshotRate = 20;         // snapshots per second per client
    size_t snapshotBudget = 1000;  // bytes of entity records per snapshot; the rest waits (priority by age)
    bool deltaCompression = true;  // false: every snapshot is a full one (bandwidth comparison)
    double timeout = 5.0;          // seconds without a packet before a client is dropped
    int inputCreditMax = 10;       // input commands a client may bank (one more per tick): speed-hack cap
};

struct ClientInfo {
    ClientId id = 0;
    NetAddress address;
    EntityId player = kNullEntity;
    uint32_t lastProcessedInput = 0;
    uint32_t ackedSnapshot = 0;
    uint64_t inputsProcessed = 0, inputsSkipped = 0;  // skipped = lost beyond the redundancy window
    const ConnectionStats* stats = nullptr;
};

class NetServer {
public:
    NetServer(Scene& scene, std::unique_ptr<Transport> transport, ServerConfig config = {});
    ~NetServer();

    // One fixed step: receive packets, handle joins / leaves / timeouts, run queued input commands,
    // and send snapshots when due. `now` is the caller's clock in seconds.
    void update(double now);

    // Called when a client joins: return the entity it controls (kNullEntity = none). The default spawns
    // "Player <id>" with NetIdentity{owner}, NetPlayer and a MeshRenderer sphere.
    // Listen-server host: a NetPlayer on this scene that remote clients see, driven by tickHost() instead of UDP.
    EntityId spawnListenHost();
    void tickHost(const NetInput& input);
    EntityId hostPlayer() const { return m_hostPlayer; }

    std::function<EntityId(Scene&, ClientId)> onClientJoined;
    // Called after a client left or timed out (its owned entities are destroyed afterwards).
    std::function<void(ClientId)> onClientLeft;
    // Reliable messages from clients.
    std::function<void(ClientId, const std::vector<uint8_t>&)> onMessage;
    PlayerSimFn playerSim = defaultPlayerSim;

    bool send(ClientId client, std::vector<uint8_t> message);  // reliable, ordered
    void broadcast(const std::vector<uint8_t>& message);
    void disconnect(ClientId client);

    std::vector<ClientInfo> clients() const;
    size_t clientCount() const { return m_clients.size(); }
    uint32_t tick() const { return m_tick; }
    NetAddress address() const { return m_transport->localAddress(); }
    const ServerConfig& config() const { return m_config; }
    uint64_t snapshotsSent() const { return m_snapshotsSent; }
    uint64_t snapshotBytes() const { return m_snapshotBytes; }
    uint64_t malformedPackets() const { return m_malformed; }
    size_t peakClients() const { return m_peakClients; }

private:
    struct SentSnapshot {
        uint32_t tick = 0;
        std::map<EntityId, EntityState> entities;  // what the client has once it receives this snapshot
    };
    struct Client {
        ClientId id = 0;
        NetAddress address;
        uint64_t salt = 0;
        Connection conn;
        EntityId player = kNullEntity;
        uint32_t lastProcessedInput = 0, ackedSnapshot = 0;
        std::map<uint32_t, NetInput> pendingInputs;
        int inputCredit = 0;
        uint64_t inputsProcessed = 0, inputsSkipped = 0;
        std::deque<SentSnapshot> history;           // newest last, at most 64
        std::map<EntityId, uint32_t> lastSentTick;  // priority: entities not sent for longest go first
        bool pendingPacket = false;                 // reliable data waiting even without a snapshot
    };

    void receive(double now);
    void handleConnect(double now, const Datagram& d, ByteReader& r);
    void handleData(double now, Client& c, ByteReader& r);
    void sendRaw(const NetAddress& to, PacketType type, const std::vector<uint8_t>& body);
    void sendSnapshot(double now, Client& c, const std::map<EntityId, EntityState>& current);
    void sendData(double now, Client& c, const std::vector<uint8_t>& unreliable);
    void removeClient(ClientId id, bool notify);
    bool relevant(const Client& c, EntityId e, const Vec3& viewer, bool hasViewer);
    EntityId defaultSpawn(Scene& scene, ClientId id);

    Scene& m_scene;
    EntityId m_hostPlayer = kNullEntity;
    std::unique_ptr<Transport> m_transport;
    ServerConfig m_config;
    const ReplicationSchema& m_schema;
    std::map<ClientId, std::unique_ptr<Client>> m_clients;
    ClientId m_nextClientId = 1;
    uint32_t m_tick = 0;
    double m_lastNow = 0;
    uint64_t m_snapshotsSent = 0, m_snapshotBytes = 0, m_malformed = 0;
    size_t m_peakClients = 0;
};

// ------------------------------------------------------------------------------------ client ---
struct ClientConfig {
    int tickRate = 60;                 // must match the server
    double interpolationDelay = 0.1;   // remote entities are shown this far in the past (s)
    int inputRedundancy = 12;          // every input packet repeats the last N commands (loss cover)
    double connectTimeout = 5.0;
    double timeout = 5.0;
    float correctionEpsilon = 1e-3f;   // prediction error (m) that counts as a misprediction
};

enum class ClientState { Disconnected, Connecting, Connected, Denied, TimedOut };
const char* clientStateName(ClientState s);

struct ClientStats {
    uint64_t snapshotsReceived = 0, snapshotsApplied = 0, snapshotsDroppedNoBaseline = 0, snapshotsStale = 0;
    uint64_t corrections = 0, replayedInputs = 0;
    float lastCorrection = 0, maxCorrection = 0;  // prediction error (m) when a correction happened
    uint64_t spawned = 0, destroyed = 0;
};

class NetClient {
public:
    NetClient(Scene& scene, std::unique_ptr<Transport> transport, NetAddress server, ClientConfig config = {});
    ~NetClient();

    void connect(double now);
    void disconnect();  // tells the server (best effort) and clears the mirrored entities
    // One fixed step: receive, apply the newest snapshot, predict the local player with the current input,
    // send input commands, interpolate remote entities.
    void update(double now);
    void setInput(const NetInput& input) { m_input = input; }  // tick is assigned by the client

    std::function<void(const std::vector<uint8_t>&)> onMessage;  // reliable messages from the server
    PlayerSimFn playerSim = defaultPlayerSim;
    bool send(std::vector<uint8_t> message);

    ClientState state() const { return m_state; }
    ClientId id() const { return m_id; }
    DenyReason denyReason() const { return m_denyReason; }
    EntityId localPlayer() const;                 // the predicted entity (kNullEntity until it arrives)
    EntityId localEntity(EntityId netId) const;  // netId = server entity id
    EntityId netIdOf(EntityId local) const;
    size_t entityCount() const { return m_entities.size(); }
    uint32_t latestSnapshot() const { return m_latestTick; }
    uint32_t inputTick() const { return m_inputTick; }
    const ClientStats& stats() const { return m_stats; }
    const ConnectionStats& connectionStats() const { return m_conn.stats(); }
    // The server state of the local player at the last snapshot (tests compare it with the prediction).
    PlayerState lastServerPlayerState() const { return m_lastServerPlayer; }

private:
    struct ReceivedSnapshot {
        uint32_t tick = 0;
        std::map<EntityId, EntityState> entities;
    };
    struct Sample {
        double tick;
        Vec3 position;
        Quat rotation;
    };
    struct Mirrored {
        EntityId local = kNullEntity;
        EntityState state;
        std::deque<Sample> samples;
    };
    struct PredictedInput {
        NetInput input;
        PlayerState after;
    };

    void receive(double now);
    void handleSnapshot(double now, ByteReader& r);
    void applySnapshot(double now, const ReceivedSnapshot& snap, uint32_t lastProcessedInput);
    void reconcile(const PlayerState& server, uint32_t lastProcessedInput);
    void predictAndSend(double now);
    void interpolate(double now);
    void sendRaw(PacketType type, const std::vector<uint8_t>& body);
    void clearMirror();
    bool isLocalPlayer(const Mirrored& m);

    Scene& m_scene;
    std::unique_ptr<Transport> m_transport;
    NetAddress m_server;
    ClientConfig m_config;
    const ReplicationSchema& m_schema;
    Connection m_conn;
    ClientState m_state = ClientState::Disconnected;
    DenyReason m_denyReason{};
    ClientId m_id = 0;
    uint64_t m_salt = 0;
    double m_connectStart = 0, m_lastConnectSend = -1;
    int m_slotPosition = -1, m_slotRotation = -1, m_slotVelocity = -1;

    std::deque<ReceivedSnapshot> m_received;  // decoded snapshots usable as baselines (newest last)
    uint32_t m_latestTick = 0;
    uint32_t m_lastProcessedInput = 0;
    double m_latestRecvTime = 0;
    std::map<EntityId, Mirrored> m_entities;  // by netId
    EntityId m_localPlayerNet = kNullEntity;

    NetInput m_input;
    uint32_t m_inputTick = 0;
    std::deque<PredictedInput> m_history;  // unacknowledged inputs, oldest first
    PlayerState m_lastServerPlayer;
    ClientStats m_stats;
};

} // namespace fledge::net
