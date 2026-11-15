#pragma once
// Memory utilities: a bump/linear arena for per-frame scratch allocations.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace fledge {

class LinearArena {
public:
    explicit LinearArena(size_t capacity) : m_buffer(new std::byte[capacity]), m_capacity(capacity) {}
    void* allocate(size_t size, size_t align = alignof(std::max_align_t)) {
        size_t p = (m_offset + align - 1) & ~(align - 1);
        if (p + size > m_capacity) return nullptr;
        m_offset = p + size;
        return m_buffer.get() + p;
    }
    template <typename T, typename... Args>
    T* make(Args&&... args) {  // NOTE: destructors are not run on reset()
        void* mem = allocate(sizeof(T), alignof(T));
        return mem ? new (mem) T(std::forward<Args>(args)...) : nullptr;
    }
    void reset() { m_offset = 0; }
    size_t used() const { return m_offset; }
    size_t capacity() const { return m_capacity; }

private:
    std::unique_ptr<std::byte[]> m_buffer;
    size_t m_capacity;
    size_t m_offset = 0;
};

} // namespace fledge
