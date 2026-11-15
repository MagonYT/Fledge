// Fledge Script - Fledge native C++ gameplay scripting. Include this one header.
//
//   #include <fledge/script/script.h>
//   using namespace fledge::script;
//
//   class Spinner : public Behaviour {
//   public:
//       FLEDGE_PROPERTY(float, degreesPerSecond, 90.0f, Range(-720, 720), Tooltip("Spin speed"));
//       FLEDGE_PROPERTY(Vec3, axis, Vec3(0, 1, 0));
//       void OnUpdate(float dt) override {
//           Entity e = GetEntity();
//           e.SetRotation(Quat::FromAxisAngle(axis, Radians(degreesPerSecond) * dt) * e.Rotation());
//       }
//   };
//   FLEDGE_BEHAVIOUR(Spinner)
//
// Rules that keep scripts correct and hot-reloadable (see docs/FLEDGE_SCRIPT.md):
//  * Constructors must be cheap and side-effect free (the engine constructs a probe
//    object to read property defaults). Do work in OnCreate / OnStart.
//  * State that must survive hot reload lives in FLEDGE_PROPERTY members; other
//    members are reset (OnCreate runs again with IsHotReloading() == true).
//  * Never keep raw pointers to other behaviours across frames; keep the Entity
//    and call GetBehaviour<T>() when needed (instances are recreated on reload).
//  * Entity / component access goes through the engine: it is cheap but not free;
//    cache values inside one OnUpdate, not across frames.
#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fledge/script/abi.h"
#include "fledge/script/json.h"
#include "fledge/script/math.h"

namespace fledge::script {

class Object;
class Behaviour;
class Service;
class Entity;

namespace detail {
inline const FScriptHostApi* g_host = nullptr;
inline const FScriptHostApi& host() {
    static const FScriptHostApi kNull{};
    return g_host ? *g_host : kNull;
}
inline void* hctx() { return g_host ? g_host->host : nullptr; }
inline FScriptVec3 toC(Vec3 v) { return {v.x, v.y, v.z}; }
inline FScriptQuat toC(Quat q) { return {q.x, q.y, q.z, q.w}; }
inline Vec3 fromC(FScriptVec3 v) { return {v.x, v.y, v.z}; }
inline Quat fromC(FScriptQuat q) { return {q.x, q.y, q.z, q.w}; }
inline std::string str(const char* s) { return s ? std::string(s) : std::string(); }
// The host has the SDK 0.7.0 bone queries (hosts older than Fledge 1.2.1 end at set_time_scale).
inline bool boneApi() { return g_host && g_host->structSize >= offsetof(FScriptHostApi, bone_names) + sizeof(g_host->bone_names); }
void reportError(const std::string& message);  // logs + remembers for last_error
} // namespace detail

// A world-space transform (Animator::BoneWorldTransform). TransformPoint maps a point given in that space to world space.
struct BoneTransform {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1, 1, 1};
    Vec3 TransformPoint(Vec3 p) const { return position + rotation.Rotate(Vec3(p.x * scale.x, p.y * scale.y, p.z * scale.z)); }
    Vec3 TransformDirection(Vec3 d) const { return rotation.Rotate(d); }
};

// ------------------------------------------------------------------ logging --
namespace Log {
inline void Write(int level, std::string_view message) {
    if (detail::host().log) detail::host().log(detail::hctx(), level, std::string(message).c_str());
}
template <typename... A> void Info(std::format_string<A...> f, A&&... a) { Write(FSCRIPT_LOG_INFO, std::format(f, std::forward<A>(a)...)); }
template <typename... A> void Warn(std::format_string<A...> f, A&&... a) { Write(FSCRIPT_LOG_WARN, std::format(f, std::forward<A>(a)...)); }
template <typename... A> void Error(std::format_string<A...> f, A&&... a) { Write(FSCRIPT_LOG_ERROR, std::format(f, std::forward<A>(a)...)); }
template <typename... A> void Debug(std::format_string<A...> f, A&&... a) { Write(FSCRIPT_LOG_DEBUG, std::format(f, std::forward<A>(a)...)); }
} // namespace Log

// --------------------------------------------------------------------- time --
namespace Time {
inline const FScriptFrameInfo& Info() {
    static const FScriptFrameInfo kZero{};
    return detail::host().frameInfo ? *detail::host().frameInfo : kZero;
}
inline float Delta() { return Info().delta; }               // scaled seconds this frame
inline float UnscaledDelta() { return Info().unscaledDelta; }
inline float FixedDelta() { return Info().fixedDelta; }
inline double Elapsed() { return Info().elapsed; }          // scaled seconds since Play
inline uint64_t Frame() { return Info().frame; }
inline float Scale() { return Info().timeScale; }
inline void SetScale(float s) { if (detail::host().set_time_scale) detail::host().set_time_scale(detail::hctx(), s < 0 ? 0 : s); }
} // namespace Time

// -------------------------------------------------------------------- input --
// Actions and axes are named in the project's input map ("input" in the .fledge;
// defaults: MoveX/MoveY (WASD + arrows), LookX/LookY (mouse), Jump (Space), Sprint (LeftShift),
// Interact (E), Fire (MouseLeft), Aim (MouseRight), Pause (Escape)).
namespace Input {
inline bool Down(const char* action) { return detail::host().input_action && detail::host().input_action(detail::hctx(), action, FSCRIPT_INPUT_DOWN); }
inline bool Pressed(const char* action) { return detail::host().input_action && detail::host().input_action(detail::hctx(), action, FSCRIPT_INPUT_PRESSED); }
inline bool Released(const char* action) { return detail::host().input_action && detail::host().input_action(detail::hctx(), action, FSCRIPT_INPUT_RELEASED); }
inline float Axis(const char* axis) { return detail::host().input_axis ? detail::host().input_axis(detail::hctx(), axis) : 0.0f; }
struct Delta2 { float x = 0, y = 0; };
inline Delta2 MouseDelta() {
    Delta2 d;
    if (detail::host().mouse_delta) detail::host().mouse_delta(detail::hctx(), &d.x, &d.y);
    return d;
}
// Rebinds an action to one key ("Space", "E", "F1", "LeftShift", "MouseLeft", ...). Returns false for unknown keys.
inline bool Rebind(const char* action, const char* key) { return detail::host().input_rebind && detail::host().input_rebind(detail::hctx(), action, key); }
} // namespace Input

// ------------------------------------------------------------------- entity --
struct RayHit;

class Entity {
public:
    Entity() = default;
    explicit Entity(FScriptEntity id) : m_id(id) {}
    FScriptEntity Id() const { return m_id; }
    bool IsValid() const { return m_id && detail::host().entity_valid && detail::host().entity_valid(detail::hctx(), m_id); }
    explicit operator bool() const { return IsValid(); }
    bool operator==(const Entity& o) const { return m_id == o.m_id; }

    std::string Name() const { return detail::host().entity_name ? detail::str(detail::host().entity_name(detail::hctx(), m_id)) : ""; }
    void SetName(const std::string& n) const { if (detail::host().set_entity_name) detail::host().set_entity_name(detail::hctx(), m_id, n.c_str()); }

    // transforms (world space unless "Local")
    Vec3 Position() const { return transform(FSCRIPT_SPACE_WORLD).pos; }
    Quat Rotation() const { return transform(FSCRIPT_SPACE_WORLD).rot; }
    Vec3 Scale() const { return transform(FSCRIPT_SPACE_LOCAL).scale; }
    Vec3 LocalPosition() const { return transform(FSCRIPT_SPACE_LOCAL).pos; }
    Quat LocalRotation() const { return transform(FSCRIPT_SPACE_LOCAL).rot; }
    void SetPosition(Vec3 p) const { if (detail::host().set_position) detail::host().set_position(detail::hctx(), m_id, FSCRIPT_SPACE_WORLD, detail::toC(p)); }
    void SetLocalPosition(Vec3 p) const { if (detail::host().set_position) detail::host().set_position(detail::hctx(), m_id, FSCRIPT_SPACE_LOCAL, detail::toC(p)); }
    void SetRotation(Quat q) const { if (detail::host().set_rotation) detail::host().set_rotation(detail::hctx(), m_id, FSCRIPT_SPACE_WORLD, detail::toC(q.Normalized())); }
    void SetLocalRotation(Quat q) const { if (detail::host().set_rotation) detail::host().set_rotation(detail::hctx(), m_id, FSCRIPT_SPACE_LOCAL, detail::toC(q.Normalized())); }
    void SetScale(Vec3 s) const { if (detail::host().set_scale) detail::host().set_scale(detail::hctx(), m_id, detail::toC(s)); }
    Vec3 Forward() const { return Rotation().Rotate(Vec3::Forward()); }
    Vec3 Right() const { return Rotation().Rotate(Vec3::Right()); }
    Vec3 Up() const { return Rotation().Rotate(Vec3::Up()); }
    void Translate(Vec3 worldDelta) const { SetPosition(Position() + worldDelta); }
    void Rotate(Quat worldDelta) const { SetRotation(worldDelta * Rotation()); }
    void LookAt(Vec3 target) const { SetRotation(Quat::LookRotation(target - Position())); }

    // hierarchy
    Entity Parent() const { return Entity(detail::host().parent ? detail::host().parent(detail::hctx(), m_id) : 0); }
    bool SetParent(Entity parent, bool keepWorld = true) const {
        return detail::host().set_parent && detail::host().set_parent(detail::hctx(), m_id, parent.m_id, keepWorld ? 1 : 0);
    }
    std::vector<Entity> Children() const;

    // tags (space separated in the "Tags" component)
    std::string Tags() const { return detail::host().tags ? detail::str(detail::host().tags(detail::hctx(), m_id)) : ""; }
    bool HasTag(std::string_view tag) const;
    void AddTag(std::string_view tag) const;
    void RemoveTag(std::string_view tag) const;

    // components: reflected engine components by type name ("MeshRenderer", "Light", "Camera", ...)
    bool HasComponent(const char* type) const { return detail::host().has_component && detail::host().has_component(detail::hctx(), m_id, type); }
    bool AddComponent(const char* type) const { return detail::host().add_component && detail::host().add_component(detail::hctx(), m_id, type); }
    bool RemoveComponent(const char* type) const { return detail::host().remove_component && detail::host().remove_component(detail::hctx(), m_id, type); }
    json::Value GetComponent(const char* type) const {
        const char* s = detail::host().get_component ? detail::host().get_component(detail::hctx(), m_id, type) : nullptr;
        return s ? json::Value::parse(s) : json::Value();
    }
    // Partial update: {"intensity": 3} only changes that field.
    bool SetComponent(const char* type, const json::Value& fields) const {
        return detail::host().set_component && detail::host().set_component(detail::hctx(), m_id, type, fields.dump().c_str());
    }
    bool SetField(const char* type, const char* field, json::Value value) const {
        json::Value o = json::Value::object();
        o.set(field, std::move(value));
        return SetComponent(type, o);
    }
    float GetFloat(const char* type, const char* field, float def = 0) const { return static_cast<float>(GetComponent(type)[field].asNumber(def)); }
    bool GetBool(const char* type, const char* field, bool def = false) const { return GetComponent(type)[field].asBool(def); }
    // MeshRenderer helpers
    void SetVisible(bool v) const { SetField("MeshRenderer", "visible", v); }
    // 1.3.1 (SDK 0.8.1) per-mesh shadows on the MeshRenderer (Animator: see Animator::SetCastShadows / SetShadowOnly)
    void SetCastShadows(bool cast) const { SetField("MeshRenderer", "castShadows", cast); }
    void SetShadowOnly(bool shadowOnly, const char* ownerCamera = "") const {
        json::Value o = json::Value::object();
        o.set("shadowOnly", json::Value(shadowOnly));
        o.set("ownerCamera", json::Value(std::string(ownerCamera ? ownerCamera : "")));
        SetComponent("MeshRenderer", o);
    }
    void SetColor(Color c) const {
        json::Value a = json::Value::array();
        a.push(c.r);
        a.push(c.g);
        a.push(c.b);
        SetField("MeshRenderer", "color", a);
    }

    // behaviours on this entity (same script module)
    template <typename T> T* GetBehaviour() const;
    template <typename T> std::vector<T*> GetBehaviours() const;
    // Adds a behaviour by class (created at the end of the frame, after property defaults + propertiesJson).
    template <typename T> bool AddBehaviour(const json::Value& properties = json::Value::object()) const;

    void Destroy() const { if (detail::host().destroy_entity) detail::host().destroy_entity(detail::hctx(), m_id); }

private:
    struct Xf {
        Vec3 pos;
        Quat rot;
        Vec3 scale{1, 1, 1};
    };
    Xf transform(int space) const {
        Xf x;
        FScriptVec3 p{}, s{1, 1, 1};
        FScriptQuat r{0, 0, 0, 1};
        if (detail::host().get_transform && detail::host().get_transform(detail::hctx(), m_id, space, &p, &r, &s)) {
            x.pos = detail::fromC(p);
            x.rot = detail::fromC(r);
            x.scale = detail::fromC(s);
        }
        return x;
    }
    FScriptEntity m_id = 0;
};

struct RayHit {
    Entity entity;
    Vec3 point, normal;
    float distance = 0;
};

// Collision data for OnCollision* callbacks (Jolt Physics, docs/PHYSICS.md). Bodies need a Collider; triggers
// (Collider.isTrigger) call OnTriggerEnter / OnTriggerExit instead. point / normal are not filled in yet (ABI v1
// carries only the other entity): use Scene::Raycast towards `other` if you need them.
struct Collision {
    Entity other;
    Vec3 point, normal;
};

// Asset reference property (GUID or content path). AssetKind("Mesh"|"Material"|"Texture"|"Scene"|"Model") filters the editor picker.
struct AssetRef {
    std::string ref;
    bool operator==(const AssetRef&) const = default;
    bool Empty() const { return ref.empty(); }
};

// -------------------------------------------------------------- properties --
struct PropertyOptions {
    float min = 0, max = 0;
    bool hasRange = false;
    std::string tooltip, assetKind;
    bool hidden = false, readOnly = false;
};
struct Range {
    float min, max;
};
struct Tooltip {
    const char* text;
};
struct AssetKind {
    const char* kind;
};
struct Hidden {};
struct ReadOnly {};
namespace detail {
inline void applyOption(PropertyOptions& o, Range r) {
    o.min = r.min;
    o.max = r.max;
    o.hasRange = true;
}
inline void applyOption(PropertyOptions& o, Tooltip t) { o.tooltip = t.text; }
inline void applyOption(PropertyOptions& o, AssetKind a) { o.assetKind = a.kind; }
inline void applyOption(PropertyOptions& o, Hidden) { o.hidden = true; }
inline void applyOption(PropertyOptions& o, ReadOnly) { o.readOnly = true; }
} // namespace detail

// Value <-> JSON for every supported property type. Supported: bool, int, float, double,
// std::string, Vec3, Quat, Color, Entity, AssetRef and std::vector of any of them.
template <typename T> struct PropertyTraits;
template <> struct PropertyTraits<bool> {
    static constexpr const char* name = "bool";
    static json::Value to(const bool& v) { return v; }
    static bool from(const json::Value& j, bool& v) { if (!j.isBool()) return false; v = j.asBool(); return true; }
};
template <> struct PropertyTraits<int> {
    static constexpr const char* name = "int";
    static json::Value to(const int& v) { return v; }
    static bool from(const json::Value& j, int& v) { if (!j.isNumber()) return false; v = static_cast<int>(j.asNumber()); return true; }
};
template <> struct PropertyTraits<float> {
    static constexpr const char* name = "float";
    static json::Value to(const float& v) { return v; }
    static bool from(const json::Value& j, float& v) { if (!j.isNumber()) return false; v = static_cast<float>(j.asNumber()); return true; }
};
template <> struct PropertyTraits<double> {
    static constexpr const char* name = "float";
    static json::Value to(const double& v) { return v; }
    static bool from(const json::Value& j, double& v) { if (!j.isNumber()) return false; v = j.asNumber(); return true; }
};
template <> struct PropertyTraits<std::string> {
    static constexpr const char* name = "string";
    static json::Value to(const std::string& v) { return v; }
    static bool from(const json::Value& j, std::string& v) { if (!j.isString()) return false; v = j.asString(); return true; }
};
namespace detail {
inline bool numbers(const json::Value& j, size_t n, float* out) {
    if (!j.isArray() || j.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!j[i].isNumber()) return false;
        out[i] = static_cast<float>(j[i].asNumber());
    }
    return true;
}
inline json::Value array(std::initializer_list<float> v) {
    json::Value a = json::Value::array();
    for (float f : v) a.push(f);
    return a;
}
} // namespace detail
template <> struct PropertyTraits<Vec3> {
    static constexpr const char* name = "vec3";
    static json::Value to(const Vec3& v) { return detail::array({v.x, v.y, v.z}); }
    static bool from(const json::Value& j, Vec3& v) { float f[3]; if (!detail::numbers(j, 3, f)) return false; v = {f[0], f[1], f[2]}; return true; }
};
template <> struct PropertyTraits<Quat> {
    static constexpr const char* name = "quat";
    static json::Value to(const Quat& v) { return detail::array({v.x, v.y, v.z, v.w}); }
    static bool from(const json::Value& j, Quat& v) {
        float f[4];
        if (detail::numbers(j, 4, f)) { v = Quat{f[0], f[1], f[2], f[3]}.Normalized(); return true; }
        if (detail::numbers(j, 3, f)) { v = Quat::FromEuler(Radians(f[0]), Radians(f[1]), Radians(f[2])); return true; }  // euler degrees
        return false;
    }
};
template <> struct PropertyTraits<Color> {
    static constexpr const char* name = "color";
    static json::Value to(const Color& v) { return detail::array({v.r, v.g, v.b}); }
    static bool from(const json::Value& j, Color& v) {
        float f[4];
        if (detail::numbers(j, 3, f) || detail::numbers(j, 4, f)) { v = {f[0], f[1], f[2]}; return true; }
        return false;
    }
};
template <> struct PropertyTraits<Entity> {
    static constexpr const char* name = "entity";
    static json::Value to(const Entity& v) { return static_cast<double>(v.Id()); }
    static bool from(const json::Value& j, Entity& v) {
        if (j.isNull()) { v = Entity(); return true; }
        if (!j.isNumber() || j.asNumber() < 0) return false;
        v = Entity(static_cast<FScriptEntity>(j.asNumber()));
        return true;
    }
};
template <> struct PropertyTraits<AssetRef> {
    static constexpr const char* name = "asset";
    static json::Value to(const AssetRef& v) { return v.ref; }
    static bool from(const json::Value& j, AssetRef& v) { if (!j.isString()) return false; v.ref = j.asString(); return true; }
};
template <typename T> struct PropertyTraits<std::vector<T>> {
    static constexpr const char* name = PropertyTraits<T>::name;
    static json::Value to(const std::vector<T>& v) {
        json::Value a = json::Value::array();
        for (const T& x : v) a.push(PropertyTraits<T>::to(x));
        return a;
    }
    static bool from(const json::Value& j, std::vector<T>& v) {
        if (!j.isArray()) return false;
        std::vector<T> out(j.size());
        for (size_t i = 0; i < j.size(); ++i)
            if (!PropertyTraits<T>::from(j[i], out[i])) return false;
        v = std::move(out);
        return true;
    }
};
template <typename T> struct IsVector : std::false_type {};
template <typename T> struct IsVector<std::vector<T>> : std::true_type {};

namespace detail {
struct PropertyBinding {
    const char* name;
    const char* type;
    bool isArray;
    PropertyOptions options;
    void* storage;
    json::Value (*get)(const void*);
    bool (*set)(void*, const json::Value&);
};
} // namespace detail

// --------------------------------------------------------------- coroutines --
struct WaitState {
    enum class Kind { NextFrame, Seconds, Frames, Until, Signal } kind = Kind::NextFrame;
    double resumeAt = 0;
    int64_t frames = 0;
    std::function<bool()> predicate;
    bool* signal = nullptr;
};

// Return type of a coroutine: Task MyRoutine() { co_await WaitSeconds(1); ... }
// Start it with StartCoroutine(MyRoutine()); it runs until its first co_await right away.
class Task {
public:
    struct promise_type {
        WaitState wait;
        Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception();
    };
    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : m_h(h) {}
    Task(Task&& o) noexcept : m_h(std::exchange(o.m_h, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (m_h) m_h.destroy();
            m_h = std::exchange(o.m_h, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    ~Task() { if (m_h) m_h.destroy(); }
    std::coroutine_handle<promise_type> release() { return std::exchange(m_h, {}); }

private:
    std::coroutine_handle<promise_type> m_h;
};
using CoroutineHandle = uint64_t;  // 0 = none

namespace detail {
struct WaitAwaiter {
    WaitState state;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) { h.promise().wait = std::move(state); }
    void await_resume() const noexcept {}
};
} // namespace detail
// co_await WaitSeconds(1.5f);  (scaled game time)
inline detail::WaitAwaiter WaitSeconds(float seconds) {
    WaitState s;
    s.kind = WaitState::Kind::Seconds;
    s.resumeAt = Time::Elapsed() + seconds;
    return {std::move(s)};
}
// co_await WaitFrames(3);  co_await NextFrame();
inline detail::WaitAwaiter WaitFrames(int frames) {
    WaitState s;
    s.kind = WaitState::Kind::Frames;
    s.frames = frames < 1 ? 1 : frames;
    return {std::move(s)};
}
inline detail::WaitAwaiter NextFrame() { return WaitFrames(1); }
// co_await WaitUntil([&] { return door.IsOpen(); });  (checked once per frame)
inline detail::WaitAwaiter WaitUntil(std::function<bool()> predicate) {
    WaitState s;
    s.kind = WaitState::Kind::Until;
    s.predicate = std::move(predicate);
    return {std::move(s)};
}

// ------------------------------------------------------------------- events --
using SubscriptionId = uint64_t;
using TimerHandle = uint64_t;

namespace detail {
template <typename E> inline const char eventTypeTag = 0;
template <typename E> const void* eventTypeId() { return &eventTypeTag<E>; }
SubscriptionId subscribe(Object* owner, const void* type, std::function<void(const void*)> fn);
void unsubscribe(SubscriptionId id);
void emit(const void* type, const void* payload);
void post(std::function<void()> deferred);
} // namespace detail

// Typed, synchronous event bus shared by every behaviour and service of the module.
//   struct PlayerDied { Entity who; };
//   Events::Emit(PlayerDied{GetEntity()});           // listeners run now
//   Events::Post(PlayerDied{GetEntity()});           // listeners run at the end of the frame
//   Subscribe<PlayerDied>([this](const PlayerDied& e) { ... });   // inside a Behaviour/Service: auto-unsubscribed
// Ordering (docs/FLEDGE_SCRIPT.md "Start order and events"): every object gets OnCreate + OnEnable when it is created
// (all objects of the scene before the first frame), then on its first frame OnStart runs right before its own OnUpdate,
// object by object in creation order. So an Emit from object A's first OnUpdate reaches B only if B subscribed in
// OnCreate / OnEnable, or B is created before A. Subscribe in OnCreate to be sure, use Events::Post (delivered at the end
// of the frame), or turn on StartAllBeforeUpdate() (SDK 0.8.1).
namespace detail {
inline bool& startAllBeforeUpdate() {
    static bool on = false;
    return on;
}
} // namespace detail
// SDK 0.8.1 (Fledge 1.3.1), opt-in for this script module: in a frame, every pending OnStart runs before any OnUpdate, so
// subscriptions made in OnStart see events emitted in any first OnUpdate. Default off (the 1.3.0 order). Call it from an
// OnCreate (or a Service's OnStart, services start first).
inline void StartAllBeforeUpdate(bool on = true) { detail::startAllBeforeUpdate() = on; }
namespace Events {
template <typename E> void Emit(const E& event) { detail::emit(detail::eventTypeId<E>(), &event); }
template <typename E> void Post(E event) {
    detail::post([e = std::move(event)] { detail::emit(detail::eventTypeId<E>(), &e); });
}
template <typename E, typename F> SubscriptionId Subscribe(F&& fn) {
    return detail::subscribe(nullptr, detail::eventTypeId<E>(),
                             [f = std::forward<F>(fn)](const void* p) { f(*static_cast<const E*>(p)); });
}
inline void Unsubscribe(SubscriptionId id) { detail::unsubscribe(id); }
} // namespace Events

namespace detail {
template <typename E> struct EventAwaiter {
    std::shared_ptr<E> value = std::make_shared<E>();
    std::shared_ptr<bool> fired = std::make_shared<bool>(false);
    SubscriptionId sub = 0;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) {
        auto v = value;
        auto f = fired;
        sub = Events::Subscribe<E>([v, f](const E& e) {
            if (!*f) {
                *v = e;
                *f = true;
            }
        });
        WaitState s;
        s.kind = WaitState::Kind::Signal;
        s.signal = fired.get();
        h.promise().wait = std::move(s);
    }
    E await_resume() {
        Events::Unsubscribe(sub);
        sub = 0;
        return *value;
    }
    ~EventAwaiter() { if (sub) Events::Unsubscribe(sub); }
};
} // namespace detail
// auto e = co_await WaitForEvent<PlayerDied>();
template <typename E> detail::EventAwaiter<E> WaitForEvent() {
    static_assert(std::is_default_constructible_v<E> && std::is_copy_assignable_v<E>, "WaitForEvent<E>: E must be default-constructible and copyable");
    return {};
}

// -------------------------------------------------------------------- objects --
namespace detail {
enum class Kind { Behaviour, Service };
CoroutineHandle startCoroutine(Object* owner, Task task);
void stopCoroutine(CoroutineHandle h);
void stopCoroutines(Object* owner);
TimerHandle addTimer(Object* owner, float delay, float interval, std::function<void()> fn);
void cancelTimer(TimerHandle h);
struct ObjectAccess;
} // namespace detail

// Common base of Behaviour and Service: reflected properties, coroutines, timers, events.
class Object {
public:
    Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    virtual ~Object() = default;

    const std::string& ClassName() const { return m_className; }
    bool IsEnabled() const { return m_enabled; }
    // true while OnCreate / OnEnable / OnHotReload run after a hot reload (properties already restored).
    static bool IsHotReloading();

    CoroutineHandle StartCoroutine(Task task) { return detail::startCoroutine(this, std::move(task)); }
    void StopCoroutine(CoroutineHandle h) { detail::stopCoroutine(h); }
    void StopAllCoroutines() { detail::stopCoroutines(this); }
    // Timers in scaled seconds; cancelled automatically when the object is destroyed.
    TimerHandle After(float seconds, std::function<void()> fn) { return detail::addTimer(this, seconds, 0, std::move(fn)); }
    TimerHandle Every(float seconds, std::function<void()> fn) { return detail::addTimer(this, seconds, seconds, std::move(fn)); }
    void CancelTimer(TimerHandle h) { detail::cancelTimer(h); }
    template <typename E, typename F> SubscriptionId Subscribe(F&& fn) {
        return detail::subscribe(this, detail::eventTypeId<E>(), [f = std::forward<F>(fn)](const void* p) { f(*static_cast<const E*>(p)); });
    }

    // Called after a hot reload, after OnCreate/OnEnable, with properties restored. Default: nothing.
    virtual void OnHotReload() {}

private:
    friend struct detail::ObjectAccess;
    std::vector<detail::PropertyBinding> m_props;
    std::string m_className;
    detail::Kind m_kind = detail::Kind::Behaviour;
    bool m_enabled = false, m_started = false, m_faulted = false;
    FScriptEntity m_entity = 0;
};

namespace detail {
struct ObjectAccess {
    static std::vector<PropertyBinding>& props(Object& o) { return o.m_props; }
    static std::string& className(Object& o) { return o.m_className; }
    static Kind& kind(Object& o) { return o.m_kind; }
    static bool& enabled(Object& o) { return o.m_enabled; }
    static bool& started(Object& o) { return o.m_started; }
    static bool& faulted(Object& o) { return o.m_faulted; }
    static FScriptEntity& entity(Object& o) { return o.m_entity; }
};

template <typename T, typename... Opts>
T declareProperty(Object* self, const char* name, T* storage, T defaultValue, Opts... opts) {
    static_assert(sizeof(PropertyTraits<T>) > 0, "unsupported FLEDGE_PROPERTY type");
    PropertyBinding b{name, PropertyTraits<T>::name, IsVector<T>::value, {}, storage,
                      [](const void* p) { return PropertyTraits<T>::to(*static_cast<const T*>(p)); },
                      [](void* p, const json::Value& j) { return PropertyTraits<T>::from(j, *static_cast<T*>(p)); }};
    (applyOption(b.options, opts), ...);
    ObjectAccess::props(*self).push_back(b);
    return defaultValue;
}
} // namespace detail

// Declares a reflected, serialized, editor-visible member inside a Behaviour or Service:
//   FLEDGE_PROPERTY(float, speed, 5.0f, Range(0, 20), Tooltip("m/s"));
//   FLEDGE_PROPERTY(Entity, target, Entity());
//   FLEDGE_PROPERTY(AssetRef, projectile, AssetRef{}, AssetKind("Scene"));
//   FLEDGE_PROPERTY(std::vector<Vec3>, waypoints, {});
#define FLEDGE_PROPERTY(TYPE, NAME, ...) TYPE NAME = ::fledge::script::detail::declareProperty<TYPE>(this, #NAME, &this->NAME, __VA_ARGS__)

// Per-entity script component. Lifecycle (driven by the engine):
//   OnCreate -> OnEnable -> OnStart (first frame) -> OnFixedUpdate* / OnUpdate / OnLateUpdate ... -> OnDisable -> OnDestroy
class Behaviour : public Object {
public:
    virtual void OnCreate() {}
    virtual void OnStart() {}
    virtual void OnUpdate(float dt) { (void)dt; }
    virtual void OnFixedUpdate(float dt) { (void)dt; }
    virtual void OnLateUpdate(float dt) { (void)dt; }
    virtual void OnEnable() {}
    virtual void OnDisable() {}
    virtual void OnDestroy() {}
    // STUB until physics exists: never called by the engine today.
    virtual void OnCollisionEnter(const Collision& c) { (void)c; }
    virtual void OnCollisionExit(const Collision& c) { (void)c; }
    virtual void OnTriggerEnter(Entity other) { (void)other; }
    virtual void OnTriggerExit(Entity other) { (void)other; }

    Entity GetEntity() const { return Entity(detail::ObjectAccess::entity(const_cast<Behaviour&>(*this))); }
    void SetEnabled(bool enabled);
    template <typename T> T* GetBehaviour() const { return GetEntity().GetBehaviour<T>(); }
};

// Game-wide singleton (missions, wanted level, economy, ...). One instance per class
// while playing, created before behaviours. Access with GetService<T>().
class Service : public Object {
public:
    virtual void OnStart() {}
    virtual void OnUpdate(float dt) { (void)dt; }
    virtual void OnStop() {}
};

// ------------------------------------------------------------------ registry --
namespace detail {
struct ClassEntry {
    std::string name;
    Kind kind;
    Object* (*factory)();
    const void* typeTag;
};
inline std::vector<ClassEntry>& registry() {
    static std::vector<ClassEntry> r;
    return r;
}
template <typename T> inline const char typeTag = 0;
template <typename T> inline std::string registeredName;
inline std::vector<Object*>& liveObjects() {
    static std::vector<Object*> v;
    return v;
}
inline std::unordered_set<Object*>& liveSet() {
    static std::unordered_set<Object*> s;
    return s;
}
template <typename T> bool registerClass(const char* name, Kind kind) {
    for (auto& e : registry())
        if (e.name == name) {
            reportError(std::string("duplicate Fledge Script class name '") + name + "' (each class name must be unique in the module)");
            return false;
        }
    registry().push_back({name, kind, []() -> Object* { return new T(); }, &typeTag<T>});
    registeredName<T> = name;
    return true;
}
} // namespace detail

#define FSCRIPT_DETAIL_REGISTER(TYPE, NAME, KIND, BASE)                                                                    \
    static_assert(std::is_base_of_v<::fledge::script::BASE, TYPE>, #TYPE " must derive from fledge::script::" #BASE);          \
    static_assert(std::is_default_constructible_v<TYPE>, #TYPE " needs a default constructor (no arguments)");           \
    namespace {                                                                                                         \
    [[maybe_unused]] const bool script_registered_##TYPE =                                                               \
        ::fledge::script::detail::registerClass<TYPE>(NAME, ::fledge::script::detail::Kind::KIND);                            \
    }

// Registers a Behaviour subclass (write it once, at global scope, after the class).
#define FLEDGE_BEHAVIOUR(TYPE) FSCRIPT_DETAIL_REGISTER(TYPE, #TYPE, Behaviour, Behaviour)
// Registers a Service subclass.
#define FLEDGE_SERVICE(TYPE) FSCRIPT_DETAIL_REGISTER(TYPE, #TYPE, Service, Service)

template <typename T> T* Entity::GetBehaviour() const {
    for (Object* o : detail::liveObjects())
        if (detail::ObjectAccess::entity(*o) == m_id && detail::ObjectAccess::kind(*o) == detail::Kind::Behaviour)
            if (T* t = dynamic_cast<T*>(o)) return t;
    return nullptr;
}
template <typename T> std::vector<T*> Entity::GetBehaviours() const {
    std::vector<T*> out;
    for (Object* o : detail::liveObjects())
        if (detail::ObjectAccess::entity(*o) == m_id)
            if (T* t = dynamic_cast<T*>(o)) out.push_back(t);
    return out;
}
template <typename T> bool Entity::AddBehaviour(const json::Value& properties) const {
    const std::string& n = detail::registeredName<T>;
    if (n.empty() || !detail::host().add_behaviour) return false;
    return detail::host().add_behaviour(detail::hctx(), m_id, n.c_str(), properties.dump().c_str()) != 0;
}

// Returns the running service of type T, or nullptr (not playing / not registered).
template <typename T> T* GetService() {
    for (Object* o : detail::liveObjects())
        if (detail::ObjectAccess::kind(*o) == detail::Kind::Service)
            if (T* t = dynamic_cast<T*>(o)) return t;
    return nullptr;
}

// -------------------------------------------------------------------- scene --
namespace Scene {
inline Entity Find(const char* name) { return Entity(detail::host().find_by_name ? detail::host().find_by_name(detail::hctx(), name) : 0); }
std::vector<Entity> FindAllWithTag(const char* tag);
std::vector<Entity> FindAllWithComponent(const char* type);
inline Entity FindWithTag(const char* tag) {
    auto v = FindAllWithTag(tag);
    return v.empty() ? Entity() : v.front();
}
// Ray against the physics colliders while playing (exact shapes; terrain, roads), and mesh bounds of entities without colliders.
inline bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RayHit& hit) {
    FScriptRayHit h{};
    if (!detail::host().raycast || !detail::host().raycast(detail::hctx(), detail::toC(origin), detail::toC(direction.Normalized()), maxDistance, &h))
        return false;
    hit.entity = Entity(h.entity);
    hit.point = detail::fromC(h.point);
    hit.normal = detail::fromC(h.normal);
    hit.distance = h.distance;
    return true;
}
inline Entity CreateEntity(const char* name, Entity parent = Entity()) {
    return Entity(detail::host().create_entity ? detail::host().create_entity(detail::hctx(), name, parent.Id()) : 0);
}
// Instantiates a .fscene (a "prefab") under a new root at position/rotation. Behaviours in it start at the end of the frame.
inline Entity Spawn(const char* scenePath, Vec3 position = {}, Quat rotation = {}) {
    return Entity(detail::host().spawn_scene ? detail::host().spawn_scene(detail::hctx(), scenePath, detail::toC(position), detail::toC(rotation)) : 0);
}
// Deferred: the entity (and its children) are removed at the end of the frame.
inline void Destroy(Entity e) { e.Destroy(); }
template <typename T> std::vector<T*> FindBehaviours() {
    std::vector<T*> out;
    for (Object* o : detail::liveObjects())
        if (T* t = dynamic_cast<T*>(o)) out.push_back(t);
    return out;
}
} // namespace Scene

// ---------------------------------------------------------------- physics queries (SDK 0.9.0, Fledge 1.3.2; docs/PHYSICS.md)
namespace detail {
inline bool queryApi() { return g_host && g_host->structSize >= offsetof(FScriptHostApi, sphere_cast) + sizeof(g_host->sphere_cast); }
} // namespace detail
namespace Physics {
// Collision layer bit for a layer mask: Layer(3) | Layer(5). Colliders pick their layer in Collider.layer (0..31).
constexpr uint32_t Layer(int layer) { return layer >= 0 && layer < 32 ? (1u << layer) : 0u; }
constexpr uint32_t kAllLayers = 0xFFFFFFFFu;
struct QueryFilter {
    Entity ignore;                  // never hit (e.g. the player); with ignoreHierarchy also its children (held weapon, arms)
    bool ignoreHierarchy = true;
    uint32_t layerMask = kAllLayers;  // Collider.layer bits; meshes / terrains without a collider are layer 0
};
// Ray against the physics colliders (exact shapes, terrain, roads; triggers are never hit) and the mesh bounds of entities
// without a collider, skipping the filter's entities and layers. False on no hit (or on a host older than Fledge 1.3.2).
inline bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RayHit& hit, const QueryFilter& filter = {}) {
    FScriptRayHit h{};
    if (!detail::queryApi() || !detail::host().raycast_filtered ||
        !detail::host().raycast_filtered(detail::hctx(), detail::toC(origin), detail::toC(direction.Normalized()), maxDistance, filter.ignore.Id(),
                                         filter.layerMask, filter.ignoreHierarchy ? FSCRIPT_QUERY_IGNORE_HIERARCHY : 0, &h))
        return false;
    hit.entity = Entity(h.entity);
    hit.point = detail::fromC(h.point);
    hit.normal = detail::fromC(h.normal);
    hit.distance = h.distance;
    return true;
}
// Sphere sweep (radius in m) along the ray: the first collider the sphere touches. Needs the simulation (play mode).
inline bool SphereCast(Vec3 origin, float radius, Vec3 direction, float maxDistance, RayHit& hit, const QueryFilter& filter = {}) {
    FScriptRayHit h{};
    if (!detail::queryApi() || !detail::host().sphere_cast ||
        !detail::host().sphere_cast(detail::hctx(), detail::toC(origin), radius, detail::toC(direction.Normalized()), maxDistance, filter.ignore.Id(),
                                    filter.layerMask, filter.ignoreHierarchy ? FSCRIPT_QUERY_IGNORE_HIERARCHY : 0, &h))
        return false;
    hit.entity = Entity(h.entity);
    hit.point = detail::fromC(h.point);
    hit.normal = detail::fromC(h.normal);
    hit.distance = h.distance;
    return true;
}
} // namespace Physics

// ---------------------------------------------------------------- audio (docs/AUDIO.md)
// Drives an entity's AudioSource component.
class AudioSource {
public:
    explicit AudioSource(Entity e) : m_e(e) {}
    bool IsValid() const { return m_e.HasComponent("AudioSource"); }
    void Play() const { m_e.SetField("AudioSource", "play", json::Value(true)); }
    void Stop() const { m_e.SetField("AudioSource", "stop", json::Value(true)); }
    bool IsPlaying() const { return m_e.GetBool("AudioSource", "playing"); }
    void SetVolume(float v) const { m_e.SetField("AudioSource", "volume", json::Value(static_cast<double>(v))); }
    void SetPitch(float p) const { m_e.SetField("AudioSource", "pitch", json::Value(static_cast<double>(p))); }
    void SetClip(const char* clip) const { m_e.SetField("AudioSource", "clip", json::Value(std::string(clip))); }

private:
    Entity m_e;
};

namespace Audio {
// Fire-and-forget sound at a position (bus: "Music" | "Sfx" | "Ambient" | "Voice" | "UI"). The temporary entity
// removes itself when the sound ends. spatial = false plays it flat (UI clicks, stingers).
inline Entity PlayOneShot(const char* clip, Vec3 position, float volume = 1.0f, const char* bus = "Sfx", bool spatial = true) {
    Entity e = Scene::CreateEntity("OneShot");
    e.SetPosition(position);
    e.AddComponent("AudioSource");
    json::Value o = json::Value::object();
    o.set("clip", json::Value(std::string(clip)));
    o.set("volume", json::Value(static_cast<double>(volume)));
    o.set("bus", json::Value(std::string(bus)));
    o.set("spatial", json::Value(spatial));
    o.set("playOnStart", json::Value(false));
    o.set("oneShot", json::Value(true));
    o.set("play", json::Value(true));
    e.SetComponent("AudioSource", o);
    return e;
}
// Bus volume 0..2 ("master" | "music" | "sfx" | "ambient" | "voice" | "ui") on the scene's AudioMixer.
inline void SetBusVolume(const char* bus, float volume) {
    std::vector<Entity> m = Scene::FindAllWithComponent("AudioMixer");
    Entity mixer = m.empty() ? Scene::CreateEntity("Audio Mixer") : m.front();
    if (m.empty()) mixer.AddComponent("AudioMixer");
    mixer.SetField("AudioMixer", bus, json::Value(static_cast<double>(volume)));
}
} // namespace Audio

// ---------------------------------------------------------------- gameplay (docs/GAMEPLAY.md)
// A Mission component (graph edited in the Missions panel).
class Mission {
public:
    explicit Mission(Entity e) : m_e(e) {}
    bool IsValid() const { return m_e.HasComponent("Mission"); }
    void Start() const { m_e.SetField("Mission", "start", json::Value(true)); }
    std::string State() const { return m_e.GetComponent("Mission")["state"].asString(); }  // Idle | Running | Succeeded | Failed
    std::string Objective() const { return m_e.GetComponent("Mission")["objective"].asString(); }
    bool IsRunning() const { return State() == "Running"; }

private:
    Entity m_e;
};

namespace Gameplay {
// The entity with the built-in PlayerController.
inline Entity Player() {
    auto v = Scene::FindAllWithComponent("PlayerController");
    return v.empty() ? Entity() : v.front();
}
inline bool IsDriving() { return !Player().GetComponent("PlayerController")["vehicle"].asString().empty(); }
inline std::string Vehicle() { return Player().GetComponent("PlayerController")["vehicle"].asString(); }
inline int WantedLevel() { return static_cast<int>(Player().GetFloat("Wanted", "level")); }
inline void SetWantedLevel(int stars) { Player().SetField("Wanted", "setLevel", json::Value(static_cast<double>(stars))); }
inline int Money() { return static_cast<int>(Player().GetFloat("Inventory", "money")); }
inline void AddMoney(int amount) { Player().SetField("Inventory", "money", json::Value(static_cast<double>(Money() + amount))); }
// Gives a weapon ("pistol" | "rifle" | "shotgun") with rounds and equips it.
inline void GiveWeapon(const char* weapon, int rounds) {
    Entity p = Player();
    json::Value inv = p.GetComponent("Inventory");
    json::Value ammo = inv["ammo"].isObject() ? inv["ammo"] : json::Value::object();
    ammo.set(weapon, json::Value(ammo[weapon].asNumber(0) + rounds));
    json::Value o = json::Value::object();
    o.set("ammo", ammo);
    o.set("weapon", json::Value(std::string(weapon)));
    p.SetComponent("Inventory", o);
}
inline float Health() { return Player().GetFloat("Health", "current"); }
inline void SetHealth(float hp) { Player().SetField("Health", "current", json::Value(static_cast<double>(hp))); }
} // namespace Gameplay

// ------------------------------------------------------------------ animation --
// Drives the entity's "Animator" component (docs/ANIMATION.md). Parameters and requests are applied by the
// engine on its next animation update; State() / HasEvent() report the last update.
// ---------------------------------------------------------------- physics (docs/PHYSICS.md)
namespace detail {
inline Vec3 vec3Of(const json::Value& j) {
    float f[3] = {0, 0, 0};
    numbers(j, 3, f);
    return {f[0], f[1], f[2]};
}
inline json::Value vec3Json(Vec3 v) { return array({v.x, v.y, v.z}); }
} // namespace detail

// Simulated body (RigidBody + Collider components).
class RigidBody {
public:
    explicit RigidBody(Entity e) : m_e(e) {}
    bool IsValid() const { return m_e.HasComponent("RigidBody"); }
    Vec3 Velocity() const { return detail::vec3Of(m_e.GetComponent("RigidBody")["velocity"]); }
    void SetVelocity(Vec3 v) const { m_e.SetField("RigidBody", "velocity", detail::vec3Json(v)); }
    void AddForce(Vec3 f) const { m_e.SetField("RigidBody", "force", detail::vec3Json(f)); }      // applied next step
    void AddImpulse(Vec3 i) const { m_e.SetField("RigidBody", "impulse", detail::vec3Json(i)); }  // applied next step
    void SetKinematic(bool k) const { m_e.SetField("RigidBody", "motion", json::Value(std::string(k ? "Kinematic" : "Dynamic"))); }

private:
    Entity m_e;
};

// Walking character (CharacterController component): slopes, steps, moving platforms.
class CharacterController {
public:
    explicit CharacterController(Entity e) : m_e(e) {}
    bool IsValid() const { return m_e.HasComponent("CharacterController"); }
    // Desired horizontal velocity in world m/s (y is ignored; gravity and jumping are handled for you).
    void Move(Vec3 velocity) const { m_e.SetField("CharacterController", "move", detail::vec3Json(velocity)); }
    void Jump() const { m_e.SetField("CharacterController", "jump", json::Value(true)); }
    bool IsGrounded() const { return m_e.GetBool("CharacterController", "grounded"); }
    Vec3 Velocity() const { return detail::vec3Of(m_e.GetComponent("CharacterController")["velocity"]); }
    // ---- collision filter (SDK 0.8.1, Fledge 1.3.1) ----------------------------------------------------------------------
    // The capsule passes through `other` and its children (a held weapon with a RigidBody, a carried crate...): they
    // neither block nor push it. Stored in the component's "ignore" list (entity names), so it is saved with the scene.
    void IgnoreEntity(Entity other) const {
        std::string name = other.Name();
        if (name.empty()) return;
        json::Value list = m_e.GetComponent("CharacterController")["ignore"];
        json::Value out = json::Value::array();
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].asString() == name) return;  // already ignored
            out.push(list[i]);
        }
        out.push(json::Value(name));
        m_e.SetField("CharacterController", "ignore", out);
    }
    void StopIgnoring(Entity other) const {
        json::Value list = m_e.GetComponent("CharacterController")["ignore"];
        json::Value out = json::Value::array();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].asString() != other.Name()) out.push(list[i]);
        m_e.SetField("CharacterController", "ignore", out);
    }
    void ClearIgnored() const { m_e.SetField("CharacterController", "ignore", json::Value::array()); }
    // false: no dynamic RigidBody blocks or pushes the capsule (default true = collide with everything, as before 1.3.1).
    void SetCollideWithDynamic(bool collide) const { m_e.SetField("CharacterController", "collideWithDynamic", json::Value(collide)); }

private:
    Entity m_e;
};

// Wheeled vehicle (Vehicle component on a RigidBody + box Collider chassis).
class Vehicle {
public:
    explicit Vehicle(Entity e) : m_e(e) {}
    bool IsValid() const { return m_e.HasComponent("Vehicle"); }
    // throttle -1..1 (negative = reverse), steer -1..1 (positive = right), brake / handbrake 0..1
    void SetInput(float throttle, float steer, float brake = 0, float handbrake = 0) const {
        json::Value o = json::Value::object();
        o.set("throttle", json::Value(static_cast<double>(throttle)));
        o.set("steer", json::Value(static_cast<double>(steer)));
        o.set("brake", json::Value(static_cast<double>(brake)));
        o.set("handbrake", json::Value(static_cast<double>(handbrake)));
        m_e.SetComponent("Vehicle", o);
    }
    float SpeedKmh() const { return m_e.GetFloat("Vehicle", "speedKmh"); }
    float Rpm() const { return m_e.GetFloat("Vehicle", "rpm"); }
    float Health() const { return m_e.GetFloat("Vehicle", "health", 100); }
    std::string Driver() const { return m_e.GetComponent("Vehicle")["driver"].asString(); }
    // Enter / exit helpers: records the driver and hides it (the vehicle carries it); Exit puts it beside the door.
    void Enter(Entity driver) const {
        m_e.SetField("Vehicle", "driver", json::Value(driver.Name()));
        driver.SetVisible(false);
        if (driver.HasComponent("CharacterController")) driver.SetField("CharacterController", "move", detail::vec3Json({0, 0, 0}));
    }
    void Exit(Entity driver) const {
        m_e.SetField("Vehicle", "driver", json::Value(std::string()));
        SetInput(0, 0, 1, 1);
        Vec3 p = m_e.Position();
        Vec3 right = m_e.Right();
        driver.SetPosition({p.x - right.x * 1.6f, p.y + 0.2f, p.z - right.z * 1.6f});
        driver.SetVisible(true);
    }

private:
    Entity m_e;
};

class Animator {
public:
    explicit Animator(Entity e) : m_e(e) {}
    bool IsValid() const { return m_e.HasComponent("Animator"); }
    void SetFloat(const char* param, float v) const { set(param, json::Value(static_cast<double>(v))); }
    void SetBool(const char* param, bool v) const { set(param, json::Value(v)); }
    void Trigger(const char* param) const { set(param, json::Value(true)); }
    // Jumps to a state, cross-fading over `fade` seconds.
    void Play(const char* state, float fade = 0.2f) const {
        json::Value o = json::Value::object();
        o.set("play", json::Value(std::string(state)));
        o.set("crossFade", json::Value(static_cast<double>(fade)));
        m_e.SetComponent("Animator", o);
    }
    void CrossFade(const char* state, float fade) const { Play(state, fade); }
    // ---- layers (SDK 0.9.0, Fledge 1.3.2; docs/ANIMATION.md "Layers at run time") -------------------------------------------
    // Jumps a state on any layer of the state machine (by index; 0 = the base layer, same as Play(state, fade)). speed > 0 runs
    // that layer's clock faster / slower while it stays in the state (an upper-body reload at 1.3x, the legs untouched).
    // Several requests in one frame are all applied, in order. A wrong layer / state is reported in LayerError().
    void Play(const char* state, float fade, int layer, float speed = 1.0f) const { queuePlay(json::Value(static_cast<double>(layer)), state, fade, speed); }
    void PlayOnLayer(const char* layer, const char* state, float fade = 0.2f, float speed = 1.0f) const {
        queuePlay(json::Value(std::string(layer ? layer : "")), state, fade, speed);
    }
    // Per-layer playback speed on top of SetSpeed (Animator.speed scales every layer; this only one). 1 = normal.
    void SetLayerSpeed(const char* layer, float speed) const { setLayerValue("layerSpeeds", layer, speed); }
    // Runtime weight of a layer > 0 (0..1); -1 returns to the state machine's own weight.
    void SetLayerWeight(const char* layer, float weight) const { setLayerValue("layerWeights", layer, weight); }
    // Live per-layer values of the last animation update (read them in OnLateUpdate for this frame's values).
    int LayerCount() const { return static_cast<int>(m_e.GetComponent("Animator")["layers"].size()); }
    std::string LayerName(int layer) const { return layerInfo(layer)["name"].asString(); }
    std::string LayerState(const char* layer) const { return layerInfo(layer)["state"].asString(); }
    std::string State(int layer) const { return layerInfo(layer)["state"].asString(); }
    float LayerSpeed(const char* layer) const { return static_cast<float>(layerInfo(layer)["speed"].asNumber(1)); }
    float LayerWeight(const char* layer) const { return static_cast<float>(layerInfo(layer)["weight"].asNumber(0)); }
    float LayerTime(const char* layer) const { return static_cast<float>(layerInfo(layer)["time"].asNumber(0)); }
    bool IsLayerInTransition(const char* layer) const { return layerInfo(layer)["inTransition"].asBool(); }
    std::string LayerError() const { return m_e.GetComponent("Animator")["layerError"].asString(); }
    std::string State() const { return m_e.GetComponent("Animator")["state"].asString(); }
    bool HasEvent(std::string_view name) const {
        json::Value ev = m_e.GetComponent("Animator")["events"];
        for (size_t i = 0; i < ev.size(); ++i)
            if (ev[i].asString() == name) return true;
        return false;
    }
    void SetSpeed(float s) const { m_e.SetField("Animator", "speed", json::Value(static_cast<double>(s))); }
    // ---- shadows (SDK 0.8.1, Fledge 1.3.1; docs/GRAPHICS.md "Per-mesh shadows") ------------------------------------------
    // false: the character casts no shadow (first-person arms / weapon).
    void SetCastShadows(bool cast) const { m_e.SetField("Animator", "castShadows", json::Value(cast)); }
    // true: drawn into the shadow maps only, hidden in the view of ownerCamera ("" = every game camera): the full-body player.
    void SetShadowOnly(bool shadowOnly, const char* ownerCamera = "") const {
        json::Value o = json::Value::object();
        o.set("shadowOnly", json::Value(shadowOnly));
        o.set("ownerCamera", json::Value(std::string(ownerCamera ? ownerCamera : "")));
        m_e.SetComponent("Animator", o);
    }
    void SetLookAt(const char* entityName) const { m_e.SetField("Animator", "lookAt", json::Value(std::string(entityName))); }

    // ---- bones (SDK 0.7.0, Fledge 1.2.1) -------------------------------------------------------------------------
    // World-space pose of a skeleton bone, as evaluated by this frame's animation update: read it in OnLateUpdate (which
    // runs after physics and animation) to attach weapons, effects or cameras to a hand / head. In OnUpdate it is last
    // frame's pose. Unknown bone (or no Animator): zero position / identity rotation / unit scale, logged once.
    bool HasBone(const char* bone) const { BoneTransform t; return query(bone, t); }
    Vec3 BoneWorldPosition(const char* bone) const { BoneTransform t; query(bone, t); return t.position; }
    Quat BoneWorldRotation(const char* bone) const { BoneTransform t; query(bone, t); return t.rotation; }
    BoneTransform BoneWorldTransform(const char* bone) const { BoneTransform t; query(bone, t); return t; }
    std::vector<std::string> BoneNames() const {
        std::vector<std::string> out;
        if (!detail::boneApi() || !detail::host().bone_names) return out;
        std::string all = detail::str(detail::host().bone_names(detail::hctx(), m_e.Id())), cur;
        for (char c : all) {
            if (c == '\n') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

private:
    bool query(const char* bone, BoneTransform& t) const {
        t = {};
        if (!bone || !detail::boneApi() || !detail::host().bone_transform) return false;
        FScriptVec3 p{}, s{1, 1, 1};
        FScriptQuat r{0, 0, 0, 1};
        if (!detail::host().bone_transform(detail::hctx(), m_e.Id(), bone, &p, &r, &s)) return false;
        t.position = detail::fromC(p);
        t.rotation = detail::fromC(r);
        t.scale = detail::fromC(s);
        return true;
    }
    void set(const char* param, json::Value v) const {
        json::Value params = json::Value::object();
        params.set(param, std::move(v));
        m_e.SetField("Animator", "params", params);
    }
    // layerPlay is a queue the engine consumes: append to what this frame already asked for
    void queuePlay(json::Value layer, const char* state, float fade, float speed) const {
        json::Value list = m_e.GetComponent("Animator")["layerPlay"];
        json::Value out = json::Value::array();
        for (size_t i = 0; i < list.size(); ++i) out.push(list[i]);
        json::Value r = json::Value::object();
        r.set("layer", std::move(layer));
        r.set("state", json::Value(std::string(state ? state : "")));
        r.set("fade", json::Value(static_cast<double>(fade)));
        r.set("speed", json::Value(static_cast<double>(speed)));
        out.push(std::move(r));
        m_e.SetField("Animator", "layerPlay", out);
    }
    void setLayerValue(const char* field, const char* layer, float v) const {
        if (!layer) return;
        json::Value cur = m_e.GetComponent("Animator")[field];
        json::Value o = json::Value::object();
        for (const auto& [k, val] : cur.members())
            if (k != layer) o.set(k, val);
        o.set(layer, json::Value(static_cast<double>(v)));
        m_e.SetField("Animator", field, o);
    }
    json::Value layerInfo(int layer) const {
        json::Value list = m_e.GetComponent("Animator")["layers"];
        return layer >= 0 ? list[static_cast<size_t>(layer)] : json::Value();
    }
    json::Value layerInfo(const char* layer) const {
        json::Value list = m_e.GetComponent("Animator")["layers"];
        for (size_t i = 0; i < list.size(); ++i)
            if (layer && list[i]["name"].asString() == layer) return list[i];
        return json::Value();
    }
    Entity m_e;
};

} // namespace fledge::script

// ===========================================================================
// Implementation: compiled once per script module. The Fledge Script build pipeline
// generates a script_module.cpp that defines FLEDGE_SCRIPT_IMPLEMENTATION and includes
// this header; script files never define it themselves.
// ===========================================================================
#ifdef FLEDGE_SCRIPT_IMPLEMENTATION
#include "fledge/script/runtime.inl"
#endif
