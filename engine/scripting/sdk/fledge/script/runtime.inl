// Fledge Script module runtime: object table, scheduler (coroutines + timers), event bus and
// the C entry point. Included exactly once per module through FLEDGE_SCRIPT_IMPLEMENTATION.
#include <algorithm>
#include <unordered_map>

namespace fledge::script {
namespace detail {

struct RuntimeState {
    std::string lastError;
    std::string scratch;
    std::string description;
    bool reloading = false;
    // coroutines
    struct Running {
        CoroutineHandle id;
        Object* owner;
        std::coroutine_handle<Task::promise_type> h;
        bool stopped = false;
    };
    std::vector<Running> coroutines, pendingCoroutines;
    CoroutineHandle nextCoroutine = 1;
    bool tickingCoroutines = false;
    // timers
    struct Timer {
        TimerHandle id;
        Object* owner;
        double fireAt;
        float interval;
        std::function<void()> fn;
        bool cancelled = false;
    };
    std::vector<Timer> timers;
    TimerHandle nextTimer = 1;
    // events
    struct Sub {
        SubscriptionId id;
        Object* owner;
        const void* type;
        std::function<void(const void*)> fn;
        bool removed = false;
    };
    std::vector<Sub> subs;
    SubscriptionId nextSub = 1;
    std::vector<std::function<void()>> posted;
    int emitting = 0;
};
inline RuntimeState& rt() {
    static RuntimeState s;
    return s;
}

void reportError(const std::string& message) {
    rt().lastError = message;
    Log::Write(FSCRIPT_LOG_ERROR, message);
}

// ---- events
SubscriptionId subscribe(Object* owner, const void* type, std::function<void(const void*)> fn) {
    SubscriptionId id = rt().nextSub++;
    rt().subs.push_back({id, owner, type, std::move(fn)});
    return id;
}
void unsubscribe(SubscriptionId id) {
    for (auto& s : rt().subs)
        if (s.id == id) s.removed = true;
    if (!rt().emitting) std::erase_if(rt().subs, [](const RuntimeState::Sub& s) { return s.removed; });
}
void emit(const void* type, const void* payload) {
    auto& st = rt();
    ++st.emitting;
    size_t n = st.subs.size();  // listeners added while emitting see the next event
    for (size_t i = 0; i < n; ++i) {
        if (st.subs[i].removed || st.subs[i].type != type) continue;
        Object* owner = st.subs[i].owner;
        if (owner && ObjectAccess::faulted(*owner)) continue;
        auto fn = st.subs[i].fn;  // copy: the vector may grow during the call
        try {
            fn(payload);
        } catch (const std::exception& e) {
            reportError(std::format("event listener{}{} threw: {}", owner ? " of " : "", owner ? ObjectAccess::className(*owner) : "", e.what()));
        } catch (...) {
            reportError("event listener threw an unknown exception");
        }
    }
    if (--st.emitting == 0) std::erase_if(st.subs, [](const RuntimeState::Sub& s) { return s.removed; });
}
void post(std::function<void()> deferred) { rt().posted.push_back(std::move(deferred)); }

// ---- coroutines
CoroutineHandle startCoroutine(Object* owner, Task task) {
    auto h = task.release();
    if (!h) return 0;
    CoroutineHandle id = rt().nextCoroutine++;
    // Run immediately until the first co_await (or completion).
    try {
        h.resume();
    } catch (...) {
        reportError("coroutine threw while starting");
    }
    if (h.done()) {
        h.destroy();
        return id;
    }
    auto& list = rt().tickingCoroutines ? rt().pendingCoroutines : rt().coroutines;
    list.push_back({id, owner, h});
    return id;
}
void stopCoroutine(CoroutineHandle id) {
    for (auto* list : {&rt().coroutines, &rt().pendingCoroutines})
        for (auto& c : *list)
            if (c.id == id) c.stopped = true;
}
void stopCoroutines(Object* owner) {
    for (auto* list : {&rt().coroutines, &rt().pendingCoroutines})
        for (auto& c : *list)
            if (c.owner == owner) c.stopped = true;
}
inline bool ready(WaitState& w) {
    switch (w.kind) {
        case WaitState::Kind::NextFrame: return true;
        case WaitState::Kind::Seconds: return Time::Elapsed() + 1e-9 >= w.resumeAt;
        case WaitState::Kind::Frames: return --w.frames <= 0;
        case WaitState::Kind::Until: {
            try {
                return !w.predicate || w.predicate();
            } catch (...) {
                reportError("WaitUntil predicate threw");
                return true;
            }
        }
        case WaitState::Kind::Signal: return w.signal && *w.signal;
    }
    return true;
}
inline void sweepCoroutines() {
    auto& list = rt().coroutines;
    for (auto& c : list)
        if (c.stopped && c.h) {
            c.h.destroy();
            c.h = {};
        }
    std::erase_if(list, [](const RuntimeState::Running& c) { return !c.h; });
}
inline void tickCoroutines() {
    auto& st = rt();
    st.tickingCoroutines = true;
    for (size_t i = 0; i < st.coroutines.size(); ++i) {
        auto& c = st.coroutines[i];
        if (c.stopped || !c.h) continue;
        if (c.owner && (!ObjectAccess::enabled(*c.owner) || ObjectAccess::faulted(*c.owner))) continue;  // paused while disabled
        if (!ready(c.h.promise().wait)) continue;
        auto h = c.h;
        try {
            h.resume();
        } catch (...) {
            reportError("coroutine threw");
        }
        if (st.coroutines[i].h.done()) st.coroutines[i].stopped = true;
    }
    st.tickingCoroutines = false;
    sweepCoroutines();
    for (auto& p : st.pendingCoroutines) st.coroutines.push_back(p);
    st.pendingCoroutines.clear();
}

// ---- timers
TimerHandle addTimer(Object* owner, float delay, float interval, std::function<void()> fn) {
    TimerHandle id = rt().nextTimer++;
    rt().timers.push_back({id, owner, Time::Elapsed() + (delay < 0 ? 0 : delay), interval, std::move(fn)});
    return id;
}
void cancelTimer(TimerHandle id) {
    for (auto& t : rt().timers)
        if (t.id == id) t.cancelled = true;
}
inline void tickTimers() {
    auto& st = rt();
    double now = Time::Elapsed();
    size_t n = st.timers.size();
    for (size_t i = 0; i < n; ++i) {
        if (st.timers[i].cancelled || now + 1e-9 < st.timers[i].fireAt) continue;
        Object* owner = st.timers[i].owner;
        if (owner && (!ObjectAccess::enabled(*owner) || ObjectAccess::faulted(*owner))) continue;
        auto fn = st.timers[i].fn;
        if (st.timers[i].interval > 0) st.timers[i].fireAt += st.timers[i].interval;
        else st.timers[i].cancelled = true;
        try {
            fn();
        } catch (const std::exception& e) {
            reportError(std::format("timer threw: {}", e.what()));
        } catch (...) {
            reportError("timer threw an unknown exception");
        }
    }
    std::erase_if(st.timers, [](const RuntimeState::Timer& t) { return t.cancelled; });
}

inline void releaseOwned(Object* o) {
    stopCoroutines(o);
    for (auto& t : rt().timers)
        if (t.owner == o) t.cancelled = true;
    for (auto& s : rt().subs)
        if (s.owner == o) s.removed = true;
    if (!rt().tickingCoroutines) sweepCoroutines();
    if (!rt().emitting) std::erase_if(rt().subs, [](const RuntimeState::Sub& s) { return s.removed; });
}

// ---- guarded calls into script code
template <typename F> void guarded(Object* o, const char* what, F&& f) {
    if (ObjectAccess::faulted(*o)) return;
    try {
        f();
    } catch (const std::exception& e) {
        ObjectAccess::faulted(*o) = true;
        reportError(std::format("{}::{} threw: {} (object disabled until the next Play)", ObjectAccess::className(*o), what, e.what()));
    } catch (...) {
        ObjectAccess::faulted(*o) = true;
        reportError(std::format("{}::{} threw an unknown exception (object disabled until the next Play)", ObjectAccess::className(*o), what));
    }
}

inline Object* fromHandle(FScriptInstance h) {
    auto* o = reinterpret_cast<Object*>(static_cast<uintptr_t>(h));
    return liveSet().contains(o) ? o : nullptr;
}

inline void setEnabled(Object* o, bool enabled) {
    if (ObjectAccess::enabled(*o) == enabled) return;
    ObjectAccess::enabled(*o) = enabled;
    if (auto* b = dynamic_cast<Behaviour*>(o)) {
        if (enabled) guarded(o, "OnEnable", [&] { b->OnEnable(); });
        else guarded(o, "OnDisable", [&] { b->OnDisable(); });
    }
}

inline void destroyObject(Object* o, bool callHooks) {
    if (callHooks) {
        if (auto* b = dynamic_cast<Behaviour*>(o)) {
            if (ObjectAccess::enabled(*o)) guarded(o, "OnDisable", [&] { b->OnDisable(); });
            guarded(o, "OnDestroy", [&] { b->OnDestroy(); });
        } else if (auto* s = dynamic_cast<Service*>(o)) {
            if (ObjectAccess::started(*o)) guarded(o, "OnStop", [&] { s->OnStop(); });
        }
    }
    ObjectAccess::enabled(*o) = false;
    releaseOwned(o);
    liveSet().erase(o);
    std::erase(liveObjects(), o);
    delete o;
}

inline json::Value propertiesOf(Object& o) {
    json::Value j = json::Value::object();
    for (auto& p : ObjectAccess::props(o)) j.set(p.name, p.get(p.storage));
    return j;
}

inline const char* describeAll() {
    json::Value classes = json::Value::array();
    for (auto& c : registry()) {
        json::Value cls = json::Value::object();
        cls.set("name", c.name);
        cls.set("kind", c.kind == Kind::Service ? "service" : "behaviour");
        json::Value props = json::Value::array();
        Object* probe = nullptr;
        try {
            probe = c.factory();
        } catch (...) {
            reportError(std::format("constructing {} threw (constructors must not do work)", c.name));
        }
        if (probe) {
            for (auto& p : ObjectAccess::props(*probe)) {
                json::Value d = json::Value::object();
                d.set("name", p.name);
                d.set("type", p.type);
                d.set("array", p.isArray);
                d.set("default", p.get(p.storage));
                if (p.options.hasRange) {
                    d.set("min", p.options.min);
                    d.set("max", p.options.max);
                }
                if (!p.options.tooltip.empty()) d.set("tooltip", p.options.tooltip);
                if (!p.options.assetKind.empty()) d.set("assetKind", p.options.assetKind);
                if (p.options.hidden) d.set("hidden", true);
                if (p.options.readOnly) d.set("readOnly", true);
                props.push(std::move(d));
            }
            delete probe;
        }
        cls.set("properties", std::move(props));
        classes.push(std::move(cls));
    }
    json::Value root = json::Value::object();
    root.set("classes", std::move(classes));
    root.set("sdkVersion", FSCRIPT_SDK_VERSION);
    rt().description = root.dump();
    return rt().description.c_str();
}

// ---- C entry points
inline int api_init(const FScriptHostApi* h) {
    // Hosts older than SDK 0.7.0 end at set_time_scale: they still load this module (the bone queries then return
    // zero / identity, see detail::boneApi()).
    if (!h || h->abiVersion != FSCRIPT_ABI_VERSION || h->structSize < offsetof(FScriptHostApi, bone_transform)) return 1;
    g_host = h;
    return 0;
}
inline void api_shutdown() {
    auto objs = liveObjects();
    for (Object* o : objs) destroyObject(o, false);
    auto& st = rt();
    for (auto* list : {&st.coroutines, &st.pendingCoroutines})
        for (auto& c : *list)
            if (c.h) c.h.destroy();
    st.coroutines.clear();
    st.pendingCoroutines.clear();
    st.timers.clear();
    st.subs.clear();
    st.posted.clear();
}
inline FScriptInstance api_create(const char* className, FScriptEntity owner) {
    for (auto& c : registry()) {
        if (c.name != className) continue;
        Object* o = nullptr;
        try {
            o = c.factory();
        } catch (const std::exception& e) {
            reportError(std::format("constructing {} threw: {}", c.name, e.what()));
            return 0;
        } catch (...) {
            reportError(std::format("constructing {} threw", c.name));
            return 0;
        }
        ObjectAccess::className(*o) = c.name;
        ObjectAccess::kind(*o) = c.kind;
        ObjectAccess::entity(*o) = owner;
        liveObjects().push_back(o);
        liveSet().insert(o);
        return static_cast<FScriptInstance>(reinterpret_cast<uintptr_t>(o));
    }
    rt().lastError = std::string("unknown class '") + (className ? className : "") + "'";
    return 0;
}
inline int api_set_properties(FScriptInstance h, const char* text) {
    Object* o = fromHandle(h);
    if (!o || !text) return 0;
    bool ok = true;
    json::Value j = json::Value::parse(text, &ok);
    if (!ok || !j.isObject()) return 0;
    int applied = 0;
    for (auto& p : ObjectAccess::props(*o))
        if (const json::Value* v = j.find(p.name)) {
            if (p.set(p.storage, *v)) ++applied;
            else Log::Write(FSCRIPT_LOG_WARN, std::format("{}.{}: value {} does not fit type {}{}", ObjectAccess::className(*o), p.name, v->dump(),
                                                        p.isArray ? "array of " : "", p.type));
        }
    return applied + 1;
}
inline const char* api_get_properties(FScriptInstance h) {
    Object* o = fromHandle(h);
    rt().scratch = o ? propertiesOf(*o).dump() : "{}";
    return rt().scratch.c_str();
}
inline void api_hook(FScriptInstance h, int hook) {
    Object* o = fromHandle(h);
    if (!o) return;
    auto* b = dynamic_cast<Behaviour*>(o);
    switch (hook) {
        case FSCRIPT_HOOK_CREATE:
            if (b) guarded(o, "OnCreate", [&] { b->OnCreate(); });
            break;
        case FSCRIPT_HOOK_ENABLE: setEnabled(o, true); break;
        case FSCRIPT_HOOK_DISABLE: setEnabled(o, false); break;
        case FSCRIPT_HOOK_DESTROY: destroyObject(o, true); break;
        case FSCRIPT_HOOK_HOT_RELOAD: guarded(o, "OnHotReload", [&] { o->OnHotReload(); }); break;
        default: break;
    }
}
inline void api_set_reloading(int r) { rt().reloading = r != 0; }
inline void api_frame(int phase, float dt) {
    auto objs = liveObjects();  // snapshot: objects may be added/destroyed during callbacks
    auto alive = [](Object* o) { return liveSet().contains(o) && ObjectAccess::enabled(*o) && !ObjectAccess::faulted(*o); };
    switch (phase) {
        case FSCRIPT_PHASE_UPDATE:
            if (startAllBeforeUpdate())  // SDK 0.8.1 opt-in: all pending OnStart first (see StartAllBeforeUpdate)
                for (Object* o : objs) {
                    if (!alive(o) || ObjectAccess::started(*o)) continue;
                    ObjectAccess::started(*o) = true;
                    if (auto* b = dynamic_cast<Behaviour*>(o)) guarded(o, "OnStart", [&] { b->OnStart(); });
                    else if (auto* s = dynamic_cast<Service*>(o)) guarded(o, "OnStart", [&] { s->OnStart(); });
                }
            for (Object* o : objs) {
                if (!alive(o)) continue;
                if (!ObjectAccess::started(*o)) {
                    ObjectAccess::started(*o) = true;
                    if (auto* b = dynamic_cast<Behaviour*>(o)) guarded(o, "OnStart", [&] { b->OnStart(); });
                    else if (auto* s = dynamic_cast<Service*>(o)) guarded(o, "OnStart", [&] { s->OnStart(); });
                }
                if (!alive(o)) continue;
                if (auto* b = dynamic_cast<Behaviour*>(o)) guarded(o, "OnUpdate", [&] { b->OnUpdate(dt); });
                else if (auto* s = dynamic_cast<Service*>(o)) guarded(o, "OnUpdate", [&] { s->OnUpdate(dt); });
            }
            tickTimers();
            tickCoroutines();
            break;
        case FSCRIPT_PHASE_FIXED:
            for (Object* o : objs)
                if (alive(o) && ObjectAccess::started(*o))
                    if (auto* b = dynamic_cast<Behaviour*>(o)) guarded(o, "OnFixedUpdate", [&] { b->OnFixedUpdate(dt); });
            break;
        case FSCRIPT_PHASE_LATE:
            for (Object* o : objs)
                if (alive(o) && ObjectAccess::started(*o))
                    if (auto* b = dynamic_cast<Behaviour*>(o)) guarded(o, "OnLateUpdate", [&] { b->OnLateUpdate(dt); });
            break;
        case FSCRIPT_PHASE_END_FRAME: {
            auto posted = std::move(rt().posted);
            rt().posted.clear();
            for (auto& f : posted) f();
            break;
        }
        default: break;
    }
}
inline void api_collision(FScriptInstance h, int kind, FScriptEntity other) {
    Object* o = fromHandle(h);
    auto* b = o ? dynamic_cast<Behaviour*>(o) : nullptr;
    if (!b || !ObjectAccess::enabled(*o)) return;
    Collision c{Entity(other), {}, {}};
    switch (kind) {
        case FSCRIPT_COLLISION_ENTER: guarded(o, "OnCollisionEnter", [&] { b->OnCollisionEnter(c); }); break;
        case FSCRIPT_COLLISION_EXIT: guarded(o, "OnCollisionExit", [&] { b->OnCollisionExit(c); }); break;
        case FSCRIPT_TRIGGER_ENTER: guarded(o, "OnTriggerEnter", [&] { b->OnTriggerEnter(Entity(other)); }); break;
        case FSCRIPT_TRIGGER_EXIT: guarded(o, "OnTriggerExit", [&] { b->OnTriggerExit(Entity(other)); }); break;
        default: break;
    }
}
inline int api_instance_count() { return static_cast<int>(liveObjects().size()); }
inline const char* api_last_error() { return rt().lastError.c_str(); }

inline const FScriptModuleApi& moduleApi() {
    static const FScriptModuleApi api = {
        FSCRIPT_ABI_VERSION, sizeof(FScriptModuleApi), FSCRIPT_SDK_VERSION,
#if defined(__clang__)
        "clang " __clang_version__,
#elif defined(__GNUC__)
        "gcc " __VERSION__,
#elif defined(_MSC_VER)
        "msvc",
#else
        "unknown",
#endif
        &api_init, &api_shutdown, &describeAll, &api_create, &api_set_properties, &api_get_properties, &api_hook, &api_set_reloading,
        &api_frame, &api_collision, &api_instance_count, &api_last_error};
    return api;
}

} // namespace detail

bool Object::IsHotReloading() { return detail::rt().reloading; }

void Behaviour::SetEnabled(bool enabled) { detail::setEnabled(this, enabled); }

void Task::promise_type::unhandled_exception() {
    try {
        throw;
    } catch (const std::exception& e) {
        detail::reportError(std::format("coroutine threw: {}", e.what()));
    } catch (...) {
        detail::reportError("coroutine threw an unknown exception");
    }
}

std::vector<Entity> Entity::Children() const {
    std::vector<Entity> out;
    if (!detail::host().children) return out;
    FScriptEntity buf[256];
    int n = detail::host().children(detail::hctx(), m_id, buf, 256);
    for (int i = 0; i < n && i < 256; ++i) out.emplace_back(buf[i]);
    return out;
}
namespace detail {
inline std::vector<std::string> splitTags(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == ',' || c == ';') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
} // namespace detail
bool Entity::HasTag(std::string_view tag) const {
    for (auto& t : detail::splitTags(Tags()))
        if (t == tag) return true;
    return false;
}
void Entity::AddTag(std::string_view tag) const {
    if (HasTag(tag) || !detail::host().set_tags) return;
    std::string t = Tags();
    if (!t.empty()) t += ' ';
    t += tag;
    detail::host().set_tags(detail::hctx(), m_id, t.c_str());
}
void Entity::RemoveTag(std::string_view tag) const {
    if (!detail::host().set_tags) return;
    std::string out;
    for (auto& t : detail::splitTags(Tags()))
        if (t != tag) out += (out.empty() ? "" : " ") + t;
    detail::host().set_tags(detail::hctx(), m_id, out.c_str());
}
namespace Scene {
static std::vector<Entity> collect(int (*fn)(void*, const char*, FScriptEntity*, int), const char* arg) {
    std::vector<Entity> out;
    if (!fn) return out;
    std::vector<FScriptEntity> buf(64);
    int n = fn(detail::hctx(), arg, buf.data(), static_cast<int>(buf.size()));
    if (n > static_cast<int>(buf.size())) {
        buf.resize(static_cast<size_t>(n));
        n = fn(detail::hctx(), arg, buf.data(), static_cast<int>(buf.size()));
    }
    for (int i = 0; i < n && i < static_cast<int>(buf.size()); ++i) out.emplace_back(buf[static_cast<size_t>(i)]);
    return out;
}
std::vector<Entity> FindAllWithTag(const char* tag) { return collect(detail::host().find_with_tag, tag); }
std::vector<Entity> FindAllWithComponent(const char* type) { return collect(detail::host().find_with_component, type); }
} // namespace Scene

} // namespace fledge::script

#ifndef FLEDGE_SCRIPT_NO_MODULE_ENTRY
extern "C" FSCRIPT_EXPORT const FScriptModuleApi* fledge_script_module_entry(void) { return &fledge::script::detail::moduleApi(); }
#endif
