#pragma once
// Replication of reflected properties.
//
// ReplicationSchema is built from the ComponentRegistry: every property registered with .replicated()
// (TypeBuilder) becomes a slot. Server and client build the same schema from the same binary; its hash
// is checked during the handshake so mismatched builds are refused instead of decoding garbage.
//
// EntityState is one entity's replicated data: a component presence mask plus every replicated
// property's value, encoded to bytes. Encoded bytes are what deltas compare, so "changed" means
// "changed on the wire" (a quaternion moving below the quantisation step is not resent).
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fledge/net/bitstream.h"
#include "fledge/scene/scene.h"

namespace fledge::net {

constexpr uint32_t kProtocolId = 0x314E4C46;  // "FLN1"
constexpr uint8_t kProtocolVersion = 1;
enum class PacketType : uint8_t { ConnectRequest = 1, ConnectAccept = 2, ConnectDeny = 3, Data = 4, Disconnect = 5 };
enum class DenyReason : uint8_t { ServerFull = 1, SchemaMismatch = 2, VersionMismatch = 3 };
enum class PayloadKind : uint8_t { None = 0, Snapshot = 1, Input = 2 };

struct ReplicatedSlot {
    int component = 0;         // index into ReplicationSchema::components
    int property = 0;          // index into the component's TypeInfo::properties
    const PropertyInfo* info = nullptr;
};

struct ReplicatedComponent {
    int registryIndex = -1;    // ComponentRegistry::types() index
    std::string name;
    std::vector<int> slots;    // indices into ReplicationSchema::slots
};

class ReplicationSchema {
public:
    static const ReplicationSchema& get();  // built once from ComponentRegistry::get()
    explicit ReplicationSchema(const ComponentRegistry& registry);

    std::vector<ReplicatedComponent> components;  // at most 64 (presence mask)
    std::vector<ReplicatedSlot> slots;
    uint32_t hash = 0;                            // FNV-1a over component / property names and types
    int netIdentity = -1;                         // component index of "NetIdentity"
    int componentOf(int registryIndex) const;
};

struct EntityState {
    uint64_t mask = 0;                 // bit c: component c present
    std::vector<std::string> values;   // per slot, encoded value ("" when the component is absent)
    bool operator==(const EntityState&) const = default;
};

// Value encoding per PropertyType: bool 1 B, int / enum zig-zag varint, float 4 B, vec3 / color 12 B,
// quat 7 B (smallest three), string / asset varint length + UTF-8, json compact text.
std::string encodeValue(const PropertyInfo& p, const void* component);
bool decodeValue(const PropertyInfo& p, const std::string& bytes, void* component);
// Copies one encoded value of type p from r into out (for skipping / storing without applying).
bool readValue(const PropertyInfo& p, ByteReader& r, std::string& out);

EntityState captureEntity(const ReplicationSchema& schema, Scene& scene, EntityId e);
// Adds / removes components to match state.mask and sets every replicated value. skipSlot(slot) == true
// leaves that property alone (the client routes interpolated / predicted transforms itself).
void applyEntity(const ReplicationSchema& schema, Scene& scene, EntityId e, const EntityState& state,
                 const std::function<bool(int slot)>& skipSlot = {});

// Delta record of one entity against a baseline state (nullptr = full). Writes nothing and returns false
// when the states are equal.
bool writeEntityDelta(const ReplicationSchema& schema, ByteWriter& w, const EntityState* baseline, const EntityState& current);
// Applies a delta record (as written above) on top of `state` (which holds the baseline, or is empty).
bool readEntityDelta(const ReplicationSchema& schema, ByteReader& r, EntityState& state);

// ------------------------------------------------------------------------------ player input ---
// One fixed-step input command. Quantised before use on both sides, so client prediction and the
// server simulate bit-identical inputs.
struct NetInput {
    uint32_t tick = 0;      // client input sequence (1, 2, 3, ...)
    float moveX = 0;        // strafe -1..1 (int8 on the wire)
    float moveZ = 0;        // forward is -1 (camera looks down -Z), -1..1
    float yaw = 0;          // radians (uint16 on the wire)
    uint8_t buttons = 0;    // bit 0 = jump; others game-defined
    void quantize();
    void write(ByteWriter& w) const;
    void read(ByteReader& r);
};
constexpr uint8_t kButtonJump = 1;

struct PlayerState {
    Vec3 position;
    Vec3 velocity;
    float yaw = 0;
};
struct PlayerParams {
    float moveSpeed = 5, jumpSpeed = 6, groundHeight = 0.5f, gravity = 20;
};
// Deterministic kinematic movement shared by the server and the predicting client. Games can replace it
// (NetServer::playerSim / NetClient::playerSim) - both sides must use the same function.
using PlayerSimFn = std::function<void(PlayerState&, const PlayerParams&, const NetInput&, float dt)>;
void defaultPlayerSim(PlayerState& s, const PlayerParams& p, const NetInput& in, float dt);

PlayerState readPlayerState(Scene& scene, EntityId e);
void writePlayerState(Scene& scene, EntityId e, const PlayerState& s);
PlayerParams readPlayerParams(Scene& scene, EntityId e);

} // namespace fledge::net
