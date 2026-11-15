#include "fledge/net/bitstream.h"

#include <cmath>

namespace fledge::net {

namespace {
constexpr float kQuatRange = 0.70710678f;  // the 3 smallest components of a unit quaternion are <= 1/sqrt2
}

void writeQuat(ByteWriter& w, Quat q) {
    q = q.normalized();
    float c[4] = {q.x, q.y, q.z, q.w};
    int largest = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(c[i]) > std::fabs(c[largest])) largest = i;
    float sign = c[largest] < 0 ? -1.0f : 1.0f;  // q and -q are the same rotation: make the dropped one positive
    w.u8(static_cast<uint8_t>(largest));
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        float v = c[i] * sign / kQuatRange;
        v = v > 1 ? 1 : (v < -1 ? -1 : v);
        w.u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767.0f))));
    }
}

Quat readQuat(ByteReader& r) {
    int largest = r.u8();
    if (largest > 3) {
        r.fail();
        return {};
    }
    float c[4] = {};
    float sum = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        c[i] = static_cast<float>(static_cast<int16_t>(r.u16())) / 32767.0f * kQuatRange;
        sum += c[i] * c[i];
    }
    c[largest] = std::sqrt(sum < 1 ? 1 - sum : 0.0f);
    return Quat{c[0], c[1], c[2], c[3]}.normalized();
}

} // namespace fledge::net
