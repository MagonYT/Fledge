#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fledge {

// 128-bit random identifier, printed as 32 lowercase hex digits.
struct Guid {
    uint64_t hi = 0, lo = 0;

    static Guid generate();
    static std::optional<Guid> parse(std::string_view s);
    std::string toString() const;
    bool isValid() const { return hi != 0 || lo != 0; }
    auto operator<=>(const Guid&) const = default;
};

} // namespace fledge

template <>
struct std::hash<fledge::Guid> {
    size_t operator()(const fledge::Guid& g) const noexcept { return std::hash<uint64_t>()(g.hi ^ (g.lo * 0x9E3779B97F4A7C15ull)); }
};
