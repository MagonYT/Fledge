#include "fledge/assets/compression.h"

#include <cstring>
#include <vector>

namespace fledge::lz {

namespace {
constexpr size_t kMinMatch = 3;
constexpr size_t kMaxMatch = 130;
constexpr size_t kMaxOffset = 65535;
constexpr size_t kHashBits = 14;

uint32_t hash3(const uint8_t* p) {
    uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
    return (v * 2654435761u) >> (32 - kHashBits);
}
} // namespace

Bytes compress(std::span<const uint8_t> in) {
    Bytes out;
    out.reserve(in.size() / 2 + 16);
    std::vector<int64_t> table(size_t(1) << kHashBits, -1);
    size_t litStart = 0;
    size_t i = 0;
    auto flushLiterals = [&](size_t end) {
        while (litStart < end) {
            size_t n = std::min<size_t>(128, end - litStart);
            out.push_back(static_cast<uint8_t>(n - 1));
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(litStart),
                       in.begin() + static_cast<std::ptrdiff_t>(litStart + n));
            litStart += n;
        }
    };
    while (i + kMinMatch <= in.size()) {
        uint32_t h = hash3(&in[i]);
        int64_t cand = table[h];
        table[h] = static_cast<int64_t>(i);
        if (cand >= 0 && i - static_cast<size_t>(cand) <= kMaxOffset &&
            std::memcmp(&in[static_cast<size_t>(cand)], &in[i], kMinMatch) == 0) {
            size_t c = static_cast<size_t>(cand);
            size_t len = kMinMatch;
            while (len < kMaxMatch && i + len < in.size() && in[c + len] == in[i + len]) ++len;
            flushLiterals(i);
            out.push_back(static_cast<uint8_t>(0x80 | (len - kMinMatch)));
            size_t off = i - c;
            out.push_back(static_cast<uint8_t>(off & 0xFF));
            out.push_back(static_cast<uint8_t>(off >> 8));
            for (size_t k = 1; k < len && i + k + kMinMatch <= in.size(); ++k) table[hash3(&in[i + k])] = static_cast<int64_t>(i + k);
            i += len;
            litStart = i;
        } else {
            ++i;
        }
    }
    flushLiterals(in.size());
    return out;
}

std::optional<Bytes> decompress(std::span<const uint8_t> in, size_t expectedSize) {
    Bytes out;
    out.reserve(expectedSize);
    size_t i = 0;
    while (i < in.size()) {
        uint8_t c = in[i++];
        if (c < 0x80) {
            size_t n = static_cast<size_t>(c) + 1;
            if (i + n > in.size() || out.size() + n > expectedSize) return std::nullopt;
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(i), in.begin() + static_cast<std::ptrdiff_t>(i + n));
            i += n;
        } else {
            size_t len = static_cast<size_t>(c & 0x7F) + kMinMatch;
            if (i + 2 > in.size()) return std::nullopt;
            size_t off = static_cast<size_t>(in[i]) | (static_cast<size_t>(in[i + 1]) << 8);
            i += 2;
            if (off == 0 || off > out.size() || out.size() + len > expectedSize) return std::nullopt;
            size_t src = out.size() - off;
            for (size_t k = 0; k < len; ++k) out.push_back(out[src + k]);  // overlapping copy is intended
        }
    }
    if (out.size() != expectedSize) return std::nullopt;
    return out;
}

} // namespace fledge::lz
