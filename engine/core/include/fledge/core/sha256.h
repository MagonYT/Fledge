#pragma once
// SHA-256 (FIPS 180-4). Used to verify on-demand downloads (upscaler runtimes) and packaged files.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fledge/core/utility.h"

namespace fledge {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    std::array<uint8_t, 32> finish();
    std::string finishHex();  // lowercase hex

    static std::string hex(const void* data, size_t size);
    static std::string hex(std::string_view s) { return hex(s.data(), s.size()); }
    // Streams a file; returns "" when it cannot be read.
    static std::string fileHex(const fs::path& file);

private:
    void block(const uint8_t* p);
    uint32_t m_h[8];
    uint8_t m_buf[64];
    size_t m_len = 0;
    uint64_t m_total = 0;
};

} // namespace fledge
