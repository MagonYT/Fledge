#pragma once
// Typed, synchronous publish/subscribe event bus.
//   bus.subscribe<SceneLoaded>([](const SceneLoaded& e){ ... });
//   bus.publish(SceneLoaded{...});
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace fledge {

class EventBus {
public:
    using Handle = uint64_t;

    template <typename E>
    Handle subscribe(std::function<void(const E&)> fn) {
        std::lock_guard lock(m_mutex);
        Handle h = m_next++;
        m_handlers[typeid(E)].push_back({h, [fn = std::move(fn)](const void* e) { fn(*static_cast<const E*>(e)); }});
        return h;
    }

    void unsubscribe(Handle h) {
        std::lock_guard lock(m_mutex);
        for (auto& [t, list] : m_handlers) std::erase_if(list, [h](auto& e) { return e.handle == h; });
    }

    template <typename E>
    void publish(const E& event) {
        std::vector<std::function<void(const void*)>> copy;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_handlers.find(typeid(E));
            if (it == m_handlers.end()) return;
            for (auto& e : it->second) copy.push_back(e.fn);
        }
        for (auto& fn : copy) fn(&event);
    }

private:
    struct Entry {
        Handle handle;
        std::function<void(const void*)> fn;
    };
    std::mutex m_mutex;
    std::unordered_map<std::type_index, std::vector<Entry>> m_handlers;
    Handle m_next = 1;
};

} // namespace fledge
