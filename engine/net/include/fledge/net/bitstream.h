#pragma once
// Byte-level packet writer / reader. Little-endian, varints (LEB128) and zig-zag signed varints.
// The reader never reads past the end: an overrun returns zeros and clears ok(), so a malformed or
// truncated packet is rejected by checking ok() once at the end.
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/math.h"

namespace fledge::net {

class ByteWriter {
public:
    void u8(uint8_t v) { m_buf.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void varint(uint64_t v) {
        while (v >= 0x80) {
            m_buf.push_back(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        m_buf.push_back(static_cast<uint8_t>(v));
    }
    void svarint(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void vec3(Vec3 v) { f32(v.x); f32(v.y); f32(v.z); }
    void string(std::string_view s) {
        varint(s.size());
        raw(s.data(), s.size());
    }
    void bytes(const void* p, size_t n) { raw(p, n); }
    void append(const std::vector<uint8_t>& b) { m_buf.insert(m_buf.end(), b.begin(), b.end()); }

    size_t size() const { return m_buf.size(); }
    const std::vector<uint8_t>& data() const { return m_buf; }
    std::vector<uint8_t>& data() { return m_buf; }
    void clear() { m_buf.clear(); }

private:
    void raw(const void* p, size_t n) {  // little-endian hosts only (x86 / ARM)
        auto* b = static_cast<const uint8_t*>(p);
        m_buf.insert(m_buf.end(), b, b + n);
    }
    std::vector<uint8_t> m_buf;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : m_p(data), m_n(size) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : m_p(v.data()), m_n(v.size()) {}

    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            uint8_t b = u8();
            if (!m_ok) return 0;
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        m_ok = false;  // more than 10 bytes
        return 0;
    }
    int64_t svarint() {
        uint64_t u = varint();
        return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
    }
    Vec3 vec3() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    std::string string(size_t maxLen = 65536) {
        uint64_t n = varint();
        if (n > maxLen || n > remaining()) { m_ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(m_p + m_pos), static_cast<size_t>(n));
        m_pos += static_cast<size_t>(n);
        return s;
    }
    // Copies n bytes (or fails).
    bool bytes(void* out, size_t n) { raw(out, n); return m_ok; }
    const uint8_t* cursor() const { return m_p + m_pos; }
    bool skip(size_t n) {
        if (n > remaining()) { m_ok = false; return false; }
        m_pos += n;
        return true;
    }

    size_t remaining() const { return m_n - m_pos; }
    size_t position() const { return m_pos; }
    bool ok() const { return m_ok; }
    void fail() { m_ok = false; }

private:
    void raw(void* out, size_t n) {
        if (!m_ok || n > remaining()) {
            m_ok = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, m_p + m_pos, n);
        m_pos += n;
    }
    const uint8_t* m_p;
    size_t m_n, m_pos = 0;
    bool m_ok = true;
};

// Quaternion "smallest three": the largest component is dropped (re-derived from unit length), the other
// three are stored as int16 in [-1/sqrt2, 1/sqrt2]. 7 bytes, error < 3e-5 per component.
void writeQuat(ByteWriter& w, Quat q);
Quat readQuat(ByteReader& r);

} // namespace fledge::net
