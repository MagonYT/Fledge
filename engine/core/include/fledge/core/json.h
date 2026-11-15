#pragma once
// JSON support (nlohmann/json) plus conversions for core math types.
#include <nlohmann/json.hpp>

#include "fledge/core/math.h"

namespace fledge {
using Json = nlohmann::json;

inline void to_json(Json& j, const Vec2& v) { j = Json::array({v.x, v.y}); }
inline void from_json(const Json& j, Vec2& v) { v = {j.at(0).get<float>(), j.at(1).get<float>()}; }
inline void to_json(Json& j, const Vec3& v) { j = Json::array({v.x, v.y, v.z}); }
inline void from_json(const Json& j, Vec3& v) { v = {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()}; }
inline void to_json(Json& j, const Vec4& v) { j = Json::array({v.x, v.y, v.z, v.w}); }
inline void from_json(const Json& j, Vec4& v) {
    v = {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>(), j.at(3).get<float>()};
}
inline void to_json(Json& j, const Quat& q) { j = Json::array({q.x, q.y, q.z, q.w}); }
inline void from_json(const Json& j, Quat& q) {
    q = {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>(), j.at(3).get<float>()};
}
} // namespace fledge
