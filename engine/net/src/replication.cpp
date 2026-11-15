#include "fledge/net/replication.h"

#include <cmath>

#include "fledge/scene/components.h"

namespace fledge::net {

// ------------------------------------------------------------------------------------ schema ---
ReplicationSchema::ReplicationSchema(const ComponentRegistry& registry) {
    uint32_t h = 2166136261u;
    auto mix = [&](std::string_view s) {
        for (unsigned char ch : s) h = (h ^ ch) * 16777619u;
        h = (h ^ 0xFF) * 16777619u;
    };
    const auto& types = registry.types();
    for (size_t t = 0; t < types.size(); ++t) {
        const auto& props = types[t].type.properties;
        ReplicatedComponent rc;
        for (size_t p = 0; p < props.size(); ++p) {
            if (!props[p].replicated) continue;
            rc.slots.push_back(static_cast<int>(slots.size()));
            slots.push_back({static_cast<int>(components.size()), static_cast<int>(p), &props[p]});
            mix(props[p].name);
            mix(toString(props[p].type));
        }
        if (rc.slots.empty()) continue;
        rc.registryIndex = static_cast<int>(t);
        rc.name = types[t].name;
        mix(rc.name);
        if (rc.name == "NetIdentity") netIdentity = static_cast<int>(components.size());
        components.push_back(std::move(rc));
        if (components.size() == 64) break;  // presence mask limit
    }
    hash = h;
}

const ReplicationSchema& ReplicationSchema::get() {
    static ReplicationSchema s(ComponentRegistry::get());
    return s;
}

int ReplicationSchema::componentOf(int registryIndex) const {
    for (size_t i = 0; i < components.size(); ++i)
        if (components[i].registryIndex == registryIndex) return static_cast<int>(i);
    return -1;
}

// ------------------------------------------------------------------------------------ values ---
namespace {
constexpr size_t kMaxString = 4096;
constexpr size_t kMaxJson = 16384;

std::string toBytes(const ByteWriter& w) { return std::string(w.data().begin(), w.data().end()); }
} // namespace

std::string encodeValue(const PropertyInfo& p, const void* c) {
    ByteWriter w;
    PropertyValue v = p.get(c);
    switch (p.type) {
        case PropertyType::Bool: w.u8(std::get<bool>(v) ? 1 : 0); break;
        case PropertyType::Int:
        case PropertyType::Enum: w.svarint(std::get<int>(v)); break;
        case PropertyType::Float: w.f32(std::get<float>(v)); break;
        case PropertyType::Vec3:
        case PropertyType::Color: w.vec3(std::get<Vec3>(v)); break;
        case PropertyType::Quat: writeQuat(w, std::get<Quat>(v)); break;
        case PropertyType::String:
        case PropertyType::AssetRef: {
            const auto& s = std::get<std::string>(v);
            w.string(s.size() > kMaxString ? std::string_view(s).substr(0, kMaxString) : std::string_view(s));
            break;
        }
        case PropertyType::Json: {
            std::string s = std::get<Json>(v).dump();
            w.string(s.size() > kMaxJson ? std::string("null") : s);
            break;
        }
    }
    return toBytes(w);
}

bool readValue(const PropertyInfo& p, ByteReader& r, std::string& out) {
    const uint8_t* start = r.cursor();
    switch (p.type) {
        case PropertyType::Bool: r.u8(); break;
        case PropertyType::Int:
        case PropertyType::Enum: r.svarint(); break;
        case PropertyType::Float: r.skip(4); break;
        case PropertyType::Vec3:
        case PropertyType::Color: r.skip(12); break;
        case PropertyType::Quat: readQuat(r); break;
        case PropertyType::String:
        case PropertyType::AssetRef: r.string(kMaxString); break;
        case PropertyType::Json: r.string(kMaxJson); break;
    }
    if (!r.ok()) return false;
    out.assign(reinterpret_cast<const char*>(start), reinterpret_cast<const char*>(r.cursor()));
    return true;
}

bool decodeValue(const PropertyInfo& p, const std::string& bytes, void* c) {
    ByteReader r(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    PropertyValue v;
    switch (p.type) {
        case PropertyType::Bool: v = r.u8() != 0; break;
        case PropertyType::Int:
        case PropertyType::Enum: v = static_cast<int>(r.svarint()); break;
        case PropertyType::Float: v = r.f32(); break;
        case PropertyType::Vec3:
        case PropertyType::Color: v = r.vec3(); break;
        case PropertyType::Quat: v = readQuat(r); break;
        case PropertyType::String:
        case PropertyType::AssetRef: v = r.string(kMaxString); break;
        case PropertyType::Json: {
            Json j = Json::parse(r.string(kMaxJson), nullptr, false);
            if (j.is_discarded()) return false;
            v = j;
            break;
        }
    }
    if (!r.ok()) return false;
    p.set(c, v);
    return true;
}

// ------------------------------------------------------------------------------ entity state ---
EntityState captureEntity(const ReplicationSchema& schema, Scene& scene, EntityId e) {
    EntityState s;
    s.values.resize(schema.slots.size());
    for (size_t c = 0; c < schema.components.size(); ++c) {
        const auto& rc = schema.components[c];
        void* comp = scene.getComponent(e, rc.name);
        if (!comp) continue;
        s.mask |= 1ull << c;
        for (int slot : rc.slots) s.values[static_cast<size_t>(slot)] = encodeValue(*schema.slots[static_cast<size_t>(slot)].info, comp);
    }
    return s;
}

void applyEntity(const ReplicationSchema& schema, Scene& scene, EntityId e, const EntityState& state,
                 const std::function<bool(int)>& skipSlot) {
    for (size_t c = 0; c < schema.components.size(); ++c) {
        const auto& rc = schema.components[c];
        bool want = (state.mask >> c) & 1;
        void* comp = scene.getComponent(e, rc.name);
        if (want && !comp) comp = scene.addComponent(e, rc.name);
        if (!want) {
            if (comp) scene.removeComponent(e, rc.name);  // Name / Transform are not removable: kept
            continue;
        }
        if (!comp) continue;
        for (int slot : rc.slots) {
            if (skipSlot && skipSlot(slot)) continue;
            const std::string& bytes = state.values[static_cast<size_t>(slot)];
            if (!bytes.empty()) decodeValue(*schema.slots[static_cast<size_t>(slot)].info, bytes, comp);
        }
    }
}

bool writeEntityDelta(const ReplicationSchema& schema, ByteWriter& w, const EntityState* base, const EntityState& cur) {
    bool full = base == nullptr;
    bool maskChanged = full || base->mask != cur.mask;
    std::vector<int> changed;
    for (size_t slot = 0; slot < schema.slots.size(); ++slot) {
        int c = schema.slots[slot].component;
        if (!((cur.mask >> c) & 1)) continue;
        bool wasPresent = !full && ((base->mask >> c) & 1);
        if (!wasPresent || base->values[slot] != cur.values[slot]) changed.push_back(static_cast<int>(slot));
    }
    if (!full && !maskChanged && changed.empty()) return false;
    w.u8(static_cast<uint8_t>((full ? 1 : 0) | (maskChanged ? 2 : 0)));
    if (maskChanged) w.varint(cur.mask);
    w.varint(changed.size());
    int prev = -1;
    for (int slot : changed) {
        w.varint(static_cast<uint64_t>(slot - prev - 1));
        prev = slot;
        const std::string& v = cur.values[static_cast<size_t>(slot)];
        w.bytes(v.data(), v.size());
    }
    return true;
}

bool readEntityDelta(const ReplicationSchema& schema, ByteReader& r, EntityState& state) {
    uint8_t flags = r.u8();
    if (flags & ~3u) return false;
    if (flags & 1) {
        state.mask = 0;
        state.values.assign(schema.slots.size(), {});
    }
    if (state.values.size() != schema.slots.size()) state.values.resize(schema.slots.size());
    if (flags & 2) {
        uint64_t mask = r.varint();
        if (schema.components.size() < 64 && (mask >> schema.components.size())) return false;
        for (size_t slot = 0; slot < schema.slots.size(); ++slot)  // drop values of removed components
            if (!((mask >> schema.slots[slot].component) & 1)) state.values[slot].clear();
        state.mask = mask;
    }
    uint64_t count = r.varint();
    if (!r.ok() || count > schema.slots.size()) return false;
    int prev = -1;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t gap = r.varint();
        if (!r.ok() || gap >= schema.slots.size()) return false;
        int slot = prev + 1 + static_cast<int>(gap);
        if (slot >= static_cast<int>(schema.slots.size())) return false;
        const auto& s = schema.slots[static_cast<size_t>(slot)];
        if (!((state.mask >> s.component) & 1)) return false;
        if (!readValue(*s.info, r, state.values[static_cast<size_t>(slot)])) return false;
        prev = slot;
    }
    return r.ok();
}

// ------------------------------------------------------------------------------ player input ---
namespace {
constexpr float kTwoPi = 6.28318530718f;
float quantizeAxis(float v) {
    v = v > 1 ? 1 : (v < -1 ? -1 : v);
    return static_cast<float>(std::lround(v * 127.0f)) / 127.0f;
}
uint16_t yawToU16(float yaw) {
    double t = std::fmod(static_cast<double>(yaw) / kTwoPi, 1.0);
    if (t < 0) t += 1.0;
    return static_cast<uint16_t>(std::llround(t * 65536.0) & 0xFFFF);
}
} // namespace

void NetInput::quantize() {
    moveX = quantizeAxis(moveX);
    moveZ = quantizeAxis(moveZ);
    yaw = static_cast<float>(yawToU16(yaw)) * (kTwoPi / 65536.0f);
}

void NetInput::write(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(static_cast<int8_t>(std::lround(quantizeAxis(moveX) * 127.0f))));
    w.u8(static_cast<uint8_t>(static_cast<int8_t>(std::lround(quantizeAxis(moveZ) * 127.0f))));
    w.u16(yawToU16(yaw));
    w.u8(buttons);
}

void NetInput::read(ByteReader& r) {
    moveX = quantizeAxis(static_cast<float>(static_cast<int8_t>(r.u8())) / 127.0f);
    moveZ = quantizeAxis(static_cast<float>(static_cast<int8_t>(r.u8())) / 127.0f);
    yaw = static_cast<float>(r.u16()) * (kTwoPi / 65536.0f);
    buttons = r.u8();
}

void defaultPlayerSim(PlayerState& s, const PlayerParams& p, const NetInput& in, float dt) {
    s.yaw = in.yaw;
    Vec3 wish{in.moveX, 0, in.moveZ};
    if (dot(wish, wish) > 1) wish = normalize(wish);
    Vec3 dir = Quat::fromAxisAngle({0, 1, 0}, s.yaw).rotate(wish);
    s.velocity.x = dir.x * p.moveSpeed;
    s.velocity.z = dir.z * p.moveSpeed;
    bool grounded = s.position.y <= p.groundHeight + 1e-4f;
    if (grounded && (in.buttons & kButtonJump) && s.velocity.y <= 0) s.velocity.y = p.jumpSpeed;
    s.velocity.y -= p.gravity * dt;
    s.position += s.velocity * dt;
    if (s.position.y <= p.groundHeight) {
        s.position.y = p.groundHeight;
        if (s.velocity.y < 0) s.velocity.y = 0;
    }
}

PlayerState readPlayerState(Scene& scene, EntityId e) {
    PlayerState s;
    if (auto* t = scene.get<TransformComponent>(e)) {
        s.position = t->position;
        s.yaw = t->rotation.toEuler().y;
    }
    if (auto* p = scene.get<NetPlayerComponent>(e)) s.velocity = p->velocity;
    return s;
}

void writePlayerState(Scene& scene, EntityId e, const PlayerState& s) {
    if (auto* t = scene.get<TransformComponent>(e)) {
        t->position = s.position;
        t->rotation = Quat::fromAxisAngle({0, 1, 0}, s.yaw);
    }
    if (auto* p = scene.get<NetPlayerComponent>(e)) p->velocity = s.velocity;
}

PlayerParams readPlayerParams(Scene& scene, EntityId e) {
    PlayerParams pp;
    if (auto* p = scene.get<NetPlayerComponent>(e)) {
        pp.moveSpeed = p->moveSpeed;
        pp.jumpSpeed = p->jumpSpeed;
        pp.groundHeight = p->groundHeight;
    }
    return pp;
}

} // namespace fledge::net
