#include "fledge/core/guid.h"

#include <mutex>
#include <random>

namespace fledge {

Guid Guid::generate() {
    static std::mutex m;
    static std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    std::lock_guard lock(m);
    Guid g;
    do {
        g.hi = rng();
        g.lo = rng();
    } while (!g.isValid());
    return g;
}

std::optional<Guid> Guid::parse(std::string_view s) {
    if (s.size() != 32) return std::nullopt;
    Guid g;
    for (size_t i = 0; i < 32; ++i) {
        char c = s[i];
        uint64_t v;
        if (c >= '0' && c <= '9') v = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = static_cast<uint64_t>(c - 'A' + 10);
        else return std::nullopt;
        uint64_t& part = i < 16 ? g.hi : g.lo;
        part = (part << 4) | v;
    }
    return g;
}

std::string Guid::toString() const {
    static const char* hex = "0123456789abcdef";
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        s[static_cast<size_t>(i)] = hex[(hi >> (60 - 4 * i)) & 0xF];
        s[static_cast<size_t>(16 + i)] = hex[(lo >> (60 - 4 * i)) & 0xF];
    }
    return s;
}

} // namespace fledge
