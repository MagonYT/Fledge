#include "fledge/anim/anim.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <sstream>
#include <unordered_map>

#include "fledge/core/utility.h"

namespace fledge::anim {

// ============================================================================== skeleton / pose ===
int Skeleton::find(std::string_view name) const {
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i].name == name) return static_cast<int>(i);
    return -1;
}

void Skeleton::computeInverseBind() {
    auto model = modelSpace(*this, restPose(*this));
    for (size_t i = 0; i < bones.size(); ++i) bones[i].inverseBind = model[i].inverse();
}

Pose restPose(const Skeleton& s) {
    Pose p;
    p.reserve(s.bones.size());
    for (auto& b : s.bones) p.push_back(b.rest);
    return p;
}

std::vector<Mat4> modelSpace(const Skeleton& s, const Pose& p) {
    std::vector<Mat4> m(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) {
        Mat4 local = (i < p.size() ? p[i] : s.bones[i].rest).matrix();
        int par = s.bones[i].parent;
        m[i] = par >= 0 ? m[static_cast<size_t>(par)] * local : local;
    }
    return m;
}

std::vector<Mat4> skinMatrices(const Skeleton& s, const std::vector<Mat4>& model) {
    std::vector<Mat4> out(model.size());
    for (size_t i = 0; i < model.size(); ++i) out[i] = model[i] * s.bones[i].inverseBind;
    return out;
}

Quat nlerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) b = {-b.x, -b.y, -b.z, -b.w};
    return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
}

Quat slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) return nlerp(a, b, t);
    float th = std::acos(std::clamp(d, -1.0f, 1.0f)), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
}

Transform lerp(const Transform& a, const Transform& b, float t) {
    return {fledge::lerp(a.position, b.position, t), nlerp(a.rotation, b.rotation, t), fledge::lerp(a.scale, b.scale, t)};
}

void blendPoses(const Pose& a, const Pose& b, float t, Pose& out, const std::vector<float>& mask) {
    size_t n = std::min(a.size(), b.size());
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = lerp(a[i], b[i], t * (mask.empty() ? 1.0f : (i < mask.size() ? mask[i] : 0.0f)));
}

Pose makeAdditive(const Pose& pose, const Pose& ref) {
    Pose d(pose.size());
    for (size_t i = 0; i < pose.size() && i < ref.size(); ++i) {
        d[i].position = pose[i].position - ref[i].position;
        d[i].rotation = (ref[i].rotation.conjugate() * pose[i].rotation).normalized();
        d[i].scale = {pose[i].scale.x / std::max(1e-6f, ref[i].scale.x), pose[i].scale.y / std::max(1e-6f, ref[i].scale.y),
                      pose[i].scale.z / std::max(1e-6f, ref[i].scale.z)};
    }
    return d;
}

void applyAdditive(Pose& base, const Pose& add, float w, const std::vector<float>& mask) {
    for (size_t i = 0; i < base.size() && i < add.size(); ++i) {
        float k = w * (mask.empty() ? 1.0f : (i < mask.size() ? mask[i] : 0.0f));
        if (k <= 0) continue;
        base[i].position += add[i].position * k;
        base[i].rotation = (base[i].rotation * nlerp(Quat{}, add[i].rotation, k)).normalized();
        base[i].scale = base[i].scale * fledge::lerp(Vec3{1, 1, 1}, add[i].scale, k);
    }
}

// ============================================================================== clips ===
size_t Clip::keyCount() const {
    size_t n = 0;
    for (auto& t : tracks) n += t.tTimes.size() + t.rTimes.size() + t.sTimes.size();
    return n;
}

namespace {
template <typename T, typename F>
T sampleChannel(const std::vector<float>& times, const std::vector<T>& vals, float t, F interp) {
    if (times.size() == 1 || t <= times.front()) return vals.front();
    if (t >= times.back()) return vals.back();
    size_t i = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
    float t0 = times[i - 1], t1 = times[i];
    float k = t1 > t0 ? (t - t0) / (t1 - t0) : 0;
    return interp(vals[i - 1], vals[i], k);
}
float wrapTime(const Clip& c, float t) {
    if (c.duration <= 0) return 0;
    if (c.loop) {
        t = std::fmod(t, c.duration);
        return t < 0 ? t + c.duration : t;
    }
    return std::clamp(t, 0.0f, c.duration);
}
float quatAngle(Quat a, Quat b) {
    float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * std::acos(std::min(1.0f, d));
}
template <typename T, typename Interp, typename Err>
size_t reduce(std::vector<float>& times, std::vector<T>& vals, float tol, Interp interp, Err err) {
    if (times.size() <= 2) return 0;
    std::vector<float> nt{times.front()};
    std::vector<T> nv{vals.front()};
    size_t removed = 0;
    for (size_t i = 1; i + 1 < times.size(); ++i) {
        // can key i be dropped? check it (and every key dropped since the last kept one) against prev-kept -> next
        float t0 = nt.back(), t1 = times[i + 1];
        bool ok = true;
        for (size_t j = i; j > 0 && times[j] > t0 && ok; --j) {
            float k = (times[j] - t0) / (t1 - t0);
            if (err(interp(nv.back(), vals[i + 1], k), vals[j]) > tol) ok = false;
        }
        if (ok) {
            ++removed;
            continue;
        }
        nt.push_back(times[i]);
        nv.push_back(vals[i]);
    }
    nt.push_back(times.back());
    nv.push_back(vals.back());
    times.swap(nt);
    vals.swap(nv);
    return removed;
}
} // namespace

void sampleClip(const Clip& c, float t, Pose& pose) {
    t = wrapTime(c, t);
    for (const Track& tr : c.tracks) {
        if (tr.bone < 0 || static_cast<size_t>(tr.bone) >= pose.size()) continue;
        Transform& x = pose[static_cast<size_t>(tr.bone)];
        if (!tr.translations.empty()) x.position = sampleChannel(tr.tTimes, tr.translations, t, [](Vec3 a, Vec3 b, float k) { return fledge::lerp(a, b, k); });
        if (!tr.rotations.empty()) x.rotation = sampleChannel(tr.rTimes, tr.rotations, t, [](Quat a, Quat b, float k) { return nlerp(a, b, k); });
        if (!tr.scales.empty()) x.scale = sampleChannel(tr.sTimes, tr.scales, t, [](Vec3 a, Vec3 b, float k) { return fledge::lerp(a, b, k); });
    }
}

size_t compressClip(Clip& c, float tt, float rt) {
    size_t removed = 0;
    auto lerpV = [](Vec3 a, Vec3 b, float k) { return fledge::lerp(a, b, k); };
    auto errV = [](Vec3 a, Vec3 b) { return length(a - b); };
    for (Track& tr : c.tracks) {
        removed += reduce(tr.tTimes, tr.translations, tt, lerpV, errV);
        removed += reduce(tr.rTimes, tr.rotations, rt, [](Quat a, Quat b, float k) { return nlerp(a, b, k); }, quatAngle);
        removed += reduce(tr.sTimes, tr.scales, tt, lerpV, errV);
    }
    return removed;
}

Vec3 rootMotion(const Clip& c, float t0, float t1, bool includeY) {
    const Track* root = nullptr;
    for (auto& tr : c.tracks)
        if (tr.bone == c.rootBone && !tr.translations.empty()) root = &tr;
    if (!root) return {0, 0, 0};
    auto at = [&](float t) { return sampleChannel(root->tTimes, root->translations, wrapTime(c, t), [](Vec3 a, Vec3 b, float k) { return fledge::lerp(a, b, k); }); };
    Vec3 d;
    float a = wrapTime(c, t0), b = wrapTime(c, t1);
    if (c.loop && t1 - t0 >= c.duration) {  // whole cycles + remainder
        int cycles = static_cast<int>((t1 - t0) / c.duration);
        Vec3 full = root->translations.back() - root->translations.front();
        d = full * static_cast<float>(cycles) + (b >= a ? at(b) - at(a) : (root->translations.back() - at(a)) + (at(b) - root->translations.front()));
    } else if (b >= a) {
        d = at(b) - at(a);
    } else {
        d = (root->translations.back() - at(a)) + (at(b) - root->translations.front());
    }
    if (!includeY) d.y = 0;
    return d;
}

void eventsBetween(const Clip& c, float t0, float t1, std::vector<std::string>& out) {
    if (t1 <= t0) return;
    float a = wrapTime(c, t0), b = wrapTime(c, t1);
    bool wrapped = c.loop && (t1 - t0 >= c.duration || b < a);
    for (auto& e : c.events) {
        bool hit = wrapped ? (e.time > a || e.time <= b) : (e.time > a && e.time <= b);
        if (hit) out.push_back(e.name);
    }
}

// ============================================================================== skinning ===
void skinMesh(const MeshData& bind, const SkinData& skin, const std::vector<Mat4>& mats, MeshData& out) {
    out.vertices.resize(bind.vertices.size());
    out.indices = bind.indices;
    out.tangents.clear();
    for (size_t v = 0; v < bind.vertices.size(); ++v) {
        const Vertex& src = bind.vertices[v];
        Vertex& dst = out.vertices[v];
        dst.uv = src.uv;
        if (v >= skin.joints.size()) {
            dst = src;
            continue;
        }
        Vec3 p{0, 0, 0}, n{0, 0, 0};
        const auto& j = skin.joints[v];
        const Vec4& w = skin.weights[v];
        for (int k = 0; k < 4; ++k) {
            float wk = w[k];
            if (wk <= 0 || j[static_cast<size_t>(k)] >= mats.size()) continue;
            const Mat4& m = mats[j[static_cast<size_t>(k)]];
            p += m.transformPoint(src.position) * wk;
            n += (m * Vec4(src.normal, 0)).xyz() * wk;
        }
        if (v < skin.joints1.size() && v < skin.weights1.size()) {  // 1.3.2: influences 5..8 (absent on 4-influence skins)
            const auto& j1 = skin.joints1[v];
            const Vec4& w1 = skin.weights1[v];
            for (int k = 0; k < 4; ++k) {
                float wk = w1[k];
                if (wk <= 0 || j1[static_cast<size_t>(k)] >= mats.size()) continue;
                const Mat4& m = mats[j1[static_cast<size_t>(k)]];
                p += m.transformPoint(src.position) * wk;
                n += (m * Vec4(src.normal, 0)).xyz() * wk;
            }
        }
        dst.position = p;
        // 1.3.3: a vertex skinned only by bones keyed to scale 0 has n = 0 (and NaN bone matrices give NaN): such normals
        // become exactly (0, 0, 0), never NaN, like the GPU path's adjugate. The lit shaders then use the face normal
        // (docs/GRAPHICS.md "Robustness rules"); partially weighted vertices keep their (renormalised) direction.
        const bool finite = std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z);
        dst.normal = finite ? normalize(n) : Vec3{0, 0, 0};
    }
}

int renormalizeWeights(SkinData& skin) {
    int changed = 0;
    const bool eight = !skin.joints1.empty() && skin.weights1.size() == skin.weights.size();
    for (size_t v = 0; v < skin.weights.size(); ++v) {
        Vec4& a = skin.weights[v];
        Vec4 zero{0, 0, 0, 0};
        Vec4& b = eight ? skin.weights1[v] : zero;
        bool dirty = false;
        for (Vec4* w : {&a, &b})
            for (int k = 0; k < 4; ++k)
                if (!std::isfinite((*w)[k]) || (*w)[k] < 0) {
                    (*w)[k] = 0;
                    dirty = true;
                }
        float sum = a.x + a.y + a.z + a.w + b.x + b.y + b.z + b.w;
        if (sum > 0 && std::fabs(sum - 1.0f) > 1e-6f) {
            float inv = 1.0f / sum;
            a = a * inv;
            if (eight) b = b * inv;
            dirty = true;
        }
        if (dirty) ++changed;
    }
    return changed;
}

std::vector<uint32_t> zeroWeightVertices(const SkinData& skin) {
    std::vector<uint32_t> out;
    const bool eight = !skin.weights1.empty() && skin.weights1.size() == skin.weights.size();
    for (size_t v = 0; v < skin.weights.size(); ++v) {
        const Vec4& a = skin.weights[v];
        float sum = std::max(a.x, 0.0f) + std::max(a.y, 0.0f) + std::max(a.z, 0.0f) + std::max(a.w, 0.0f);
        if (eight) {
            const Vec4& b = skin.weights1[v];
            sum += std::max(b.x, 0.0f) + std::max(b.y, 0.0f) + std::max(b.z, 0.0f) + std::max(b.w, 0.0f);
        }
        if (!(sum > 0)) out.push_back(static_cast<uint32_t>(v));
    }
    return out;
}

int fixZeroWeights(const MeshData& mesh, SkinData& skin) {
    std::vector<uint32_t> zero = zeroWeightVertices(skin);
    if (zero.empty() || skin.weights.size() != mesh.vertices.size()) return 0;
    const bool eight = !skin.joints1.empty() && skin.joints1.size() == skin.joints.size() && skin.weights1.size() == skin.weights.size();
    std::vector<char> isZero(skin.weights.size(), 0);
    for (uint32_t v : zero) isZero[v] = 1;
    // uniform grid over the weighted vertices (cell ~ the mean spacing), searched in growing rings
    Vec3 mn{1e30f}, mx{-1e30f};
    size_t weighted = 0;
    for (size_t v = 0; v < mesh.vertices.size(); ++v) {
        if (isZero[v]) continue;
        ++weighted;
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], mesh.vertices[v].position[k]);
            mx[k] = std::max(mx[k], mesh.vertices[v].position[k]);
        }
    }
    if (weighted == 0) return 0;
    Vec3 ext = mx - mn;
    float vol = std::max(ext.x, 1e-4f) * std::max(ext.y, 1e-4f) * std::max(ext.z, 1e-4f);
    float cell = std::max(1e-4f, std::cbrt(vol / static_cast<float>(weighted)) * 2.0f);
    auto key = [&](Vec3 p, int& x, int& y, int& z) {
        x = static_cast<int>(std::floor((p.x - mn.x) / cell));
        y = static_cast<int>(std::floor((p.y - mn.y) / cell));
        z = static_cast<int>(std::floor((p.z - mn.z) / cell));
    };
    auto pack = [](int x, int y, int z) { return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFF) << 42) |
                                                 (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFF) << 21) |
                                                 static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFF); };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (size_t v = 0; v < mesh.vertices.size(); ++v) {
        if (isZero[v]) continue;
        int x, y, z;
        key(mesh.vertices[v].position, x, y, z);
        grid[pack(x, y, z)].push_back(static_cast<uint32_t>(v));
    }
    int maxRing = static_cast<int>(std::ceil(std::max({ext.x, ext.y, ext.z}) / cell)) + 2;
    int fixed = 0;
    for (uint32_t v : zero) {
        Vec3 p = mesh.vertices[v].position;
        int cx, cy, cz;
        key(p, cx, cy, cz);
        float best = 1e30f;
        int64_t found = -1;
        for (int r = 0; r <= maxRing; ++r) {
            for (int x = cx - r; x <= cx + r; ++x)
                for (int y = cy - r; y <= cy + r; ++y)
                    for (int z = cz - r; z <= cz + r; ++z) {
                        if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) != r) continue;  // ring shell only
                        auto it = grid.find(pack(x, y, z));
                        if (it == grid.end()) continue;
                        for (uint32_t o : it->second) {
                            Vec3 d = mesh.vertices[o].position - p;
                            float dd = dot(d, d);
                            if (dd < best || (dd == best && o < static_cast<uint32_t>(found))) {
                                best = dd;
                                found = o;
                            }
                        }
                    }
            // a hit in ring r may still lose to one in ring r + 1 (cube vs sphere): stop one ring later
            if (found >= 0 && std::sqrt(best) <= static_cast<float>(r) * cell) break;
        }
        if (found < 0) continue;
        skin.joints[v] = skin.joints[static_cast<size_t>(found)];
        skin.weights[v] = skin.weights[static_cast<size_t>(found)];
        if (eight) {
            skin.joints1[v] = skin.joints1[static_cast<size_t>(found)];
            skin.weights1[v] = skin.weights1[static_cast<size_t>(found)];
        }
        ++fixed;
    }
    return fixed;
}

// ============================================================================== IK ===
namespace {
Quat fromTo(Vec3 a, Vec3 b) {
    a = normalize(a);
    b = normalize(b);
    float d = dot(a, b);
    if (d > 0.99999f) return {};
    if (d < -0.99999f) {
        Vec3 axis = cross({1, 0, 0}, a);
        if (length(axis) < 1e-4f) axis = cross({0, 1, 0}, a);
        return Quat::fromAxisAngle(normalize(axis), 3.14159265f);
    }
    Vec3 c = cross(a, b);
    return Quat{c.x, c.y, c.z, 1 + d}.normalized();
}
Quat rotOf(const Mat4& m) {
    Mat4 r;
    for (int k = 0; k < 3; ++k) r.c[k] = Vec4(normalize(m.c[k].xyz()), 0);
    return quatFromMatrix(r);
}
// Sets bone i so that its MODEL rotation becomes `modelRot`.
void setModelRot(const Skeleton& s, Pose& pose, const std::vector<Mat4>& model, int i, Quat modelRot) {
    int par = s.bones[static_cast<size_t>(i)].parent;
    Quat parentRot = par >= 0 ? rotOf(model[static_cast<size_t>(par)]) : Quat{};
    pose[static_cast<size_t>(i)].rotation = (parentRot.conjugate() * modelRot).normalized();
}
} // namespace

bool solveTwoBone(const Skeleton& s, Pose& pose, int upper, int mid, int end, Vec3 target, Vec3 pole, float weight) {
    if (upper < 0 || mid < 0 || end < 0) return false;
    Pose original = pose;
    auto model = modelSpace(s, pose);
    Vec3 a = model[static_cast<size_t>(upper)].c[3].xyz(), b = model[static_cast<size_t>(mid)].c[3].xyz(), c = model[static_cast<size_t>(end)].c[3].xyz();
    float la = length(b - a), lb = length(c - b);
    if (la < 1e-5f || lb < 1e-5f) return false;
    float d = std::clamp(length(target - a), std::fabs(la - lb) + 1e-4f, la + lb - 1e-4f);
    // 1) bend the middle joint to the angle that gives distance d
    float cur = std::acos(std::clamp(dot(normalize(a - b), normalize(c - b)), -1.0f, 1.0f));
    float want = std::acos(std::clamp((la * la + lb * lb - d * d) / (2 * la * lb), -1.0f, 1.0f));
    Vec3 axis = cross(a - b, c - b);
    if (length(axis) < 1e-6f) axis = cross(a - b, pole - b);
    if (length(axis) < 1e-6f) axis = {1, 0, 0};
    axis = normalize(axis);
    Quat midRot = rotOf(model[static_cast<size_t>(mid)]);
    setModelRot(s, pose, model, mid, (Quat::fromAxisAngle(axis, cur - want) * midRot).normalized());
    // 2) swing the upper bone so the chain points at the target
    model = modelSpace(s, pose);
    c = model[static_cast<size_t>(end)].c[3].xyz();
    Quat swing = fromTo(c - a, target - a);
    setModelRot(s, pose, model, upper, (swing * rotOf(model[static_cast<size_t>(upper)])).normalized());
    // 3) twist around the chain axis so the middle joint faces the pole
    model = modelSpace(s, pose);
    b = model[static_cast<size_t>(mid)].c[3].xyz();
    Vec3 ax = normalize(target - a);
    Vec3 pb = (b - a) - ax * dot(b - a, ax), pp = (pole - a) - ax * dot(pole - a, ax);
    if (length(pb) > 1e-5f && length(pp) > 1e-5f) {
        float ang = std::atan2(dot(cross(normalize(pb), normalize(pp)), ax), dot(normalize(pb), normalize(pp)));
        setModelRot(s, pose, model, upper, (Quat::fromAxisAngle(ax, ang) * rotOf(model[static_cast<size_t>(upper)])).normalized());
    }
    if (weight < 1)
        for (int i : {upper, mid}) pose[static_cast<size_t>(i)].rotation = nlerp(original[static_cast<size_t>(i)].rotation, pose[static_cast<size_t>(i)].rotation, weight);
    return true;
}

void lookAt(const Skeleton& s, Pose& pose, int bone, Vec3 target, Vec3 forward, float weight) {
    if (bone < 0) return;
    auto model = modelSpace(s, pose);
    Quat r = rotOf(model[static_cast<size_t>(bone)]);
    Vec3 cur = r.rotate(forward), pos = model[static_cast<size_t>(bone)].c[3].xyz();
    Quat q = nlerp(Quat{}, fromTo(cur, target - pos), std::clamp(weight, 0.0f, 1.0f));
    setModelRot(s, pose, model, bone, (q * r).normalized());
}

int footPlacement(const Skeleton& s, Pose& pose, const std::vector<Leg>& legs, const std::function<std::optional<float>(float, float)>& ground, float footHeight) {
    int adjusted = 0;
    for (const Leg& l : legs) {
        auto model = modelSpace(s, pose);
        Vec3 ankle = model[static_cast<size_t>(l.ankle)].c[3].xyz(), knee = model[static_cast<size_t>(l.knee)].c[3].xyz();
        auto h = ground(ankle.x, ankle.z);
        if (!h) continue;
        float want = *h + footHeight;
        if (std::fabs(want - ankle.y) < 1e-3f || want < ankle.y - 0.5f) continue;  // already there, or ground far below (airborne)
        Vec3 pole = knee + Vec3{0, 0, -1};  // knees point forward (-Z)
        if (solveTwoBone(s, pose, l.hip, l.knee, l.ankle, {ankle.x, want, ankle.z}, pole)) ++adjusted;
    }
    return adjusted;
}

// ============================================================================== retargeting ===
namespace {
std::string normName(std::string n) {
    n = toLower(n);
    for (const char* p : {"mixamorig:", "mixamorig_", "bip01_", "bip01 ", "armature|", "def-"})
        if (startsWith(n, p)) n = n.substr(std::strlen(p));
    std::string out;
    for (char c : n)
        if (std::isalnum(static_cast<unsigned char>(c))) out += c;
    return out;
}
} // namespace

std::map<std::string, std::string> autoBoneMap(const Skeleton& from, const Skeleton& to) {
    std::map<std::string, std::string> m;
    for (auto& t : to.bones)
        for (auto& f : from.bones)
            if (normName(f.name) == normName(t.name)) {
                m[t.name] = f.name;
                break;
            }
    return m;
}

Clip retarget(const Clip& c, const Skeleton& from, const Skeleton& to, const std::map<std::string, std::string>& mapIn) {
    std::map<std::string, std::string> map = mapIn.empty() ? autoBoneMap(from, to) : mapIn;
    auto fromModel = modelSpace(from, restPose(from)), toModel = modelSpace(to, restPose(to));
    float hf = from.bones.empty() ? 1 : fromModel[0].c[3].y, ht = to.bones.empty() ? 1 : toModel[0].c[3].y;
    float ratio = std::fabs(hf) > 1e-4f ? ht / hf : 1.0f;
    Clip out;
    out.name = c.name;
    out.duration = c.duration;
    out.loop = c.loop;
    out.events = c.events;
    out.rootBone = 0;
    for (size_t ti = 0; ti < to.bones.size(); ++ti) {
        auto it = map.find(to.bones[ti].name);
        std::string srcName = it != map.end() ? it->second : to.bones[ti].name;
        int sb = from.find(srcName);
        if (sb < 0) continue;
        for (const Track& tr : c.tracks) {
            if (tr.bone != sb) continue;
            Track nt;
            nt.bone = static_cast<int>(ti);
            const Transform& sr = from.bones[static_cast<size_t>(sb)].rest;
            const Transform& dr = to.bones[ti].rest;
            nt.rTimes = tr.rTimes;
            for (Quat q : tr.rotations) nt.rotations.push_back((dr.rotation * (sr.rotation.conjugate() * q)).normalized());
            if (sb == c.rootBone) {  // root translation, scaled by the hip-height ratio
                nt.tTimes = tr.tTimes;
                for (Vec3 p : tr.translations) nt.translations.push_back(dr.position + (p - sr.position) * ratio);
            }
            out.tracks.push_back(std::move(nt));
        }
    }
    return out;
}

// ============================================================================== graph ===
int AnimGraph::Layer::find(std::string_view n) const {
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i].name == n) return static_cast<int>(i);
    return -1;
}

AnimGraph AnimGraph::fromJson(const Json& j) {
    AnimGraph g;
    if (!j.is_object()) return g;
    for (auto& p : j.value("params", Json::array())) g.params.push_back({p.value("name", ""), p.value("type", "float"), p.value("default", 0.0f)});
    auto parseLayer = [&](const Json& l) {
        Layer L;
        L.name = l.value("name", "Base");
        L.defaultState = l.value("default", "");
        L.weight = l.value("weight", 1.0f);
        L.additive = l.value("additive", false);
        for (auto& m : l.value("mask", Json::array()))
            if (m.is_string()) L.mask.push_back(m.get<std::string>());
        for (auto& s : l.value("states", Json::array())) {
            State st;
            st.name = s.value("name", "");
            st.clip = s.value("clip", "");
            st.speed = s.value("speed", 1.0f);
            if (s.contains("blend1d")) {
                st.blend = 1;
                st.paramX = s["blend1d"].value("param", "");
                for (auto& c : s["blend1d"].value("children", Json::array())) st.children.push_back({c.value("clip", ""), c.value("pos", 0.0f), 0, 0});
                std::sort(st.children.begin(), st.children.end(), [](auto& a, auto& b) { return a.pos < b.pos; });
            } else if (s.contains("blend2d")) {
                st.blend = 2;
                st.paramX = s["blend2d"].value("x", "");
                st.paramY = s["blend2d"].value("y", "");
                for (auto& c : s["blend2d"].value("children", Json::array())) st.children.push_back({c.value("clip", ""), 0, c.value("x", 0.0f), c.value("y", 0.0f)});
            }
            L.states.push_back(st);
        }
        for (auto& t : l.value("transitions", Json::array())) {
            Transition tr;
            tr.from = t.value("from", "*");
            tr.to = t.value("to", "");
            tr.duration = t.value("duration", 0.2f);
            tr.exitTime = t.value("exitTime", -1.0f);
            tr.curve = t.value("curve", "linear");
            for (auto& c : t.value("conditions", Json::array())) tr.conditions.push_back({c.value("param", ""), c.value("op", ">"), c.value("value", 0.0f)});
            L.transitions.push_back(tr);
        }
        if (L.defaultState.empty() && !L.states.empty()) L.defaultState = L.states[0].name;
        return L;
    };
    if (j.contains("events") && j["events"].is_object())
        for (auto& [clip, list] : j["events"].items())
            for (auto& e : list) g.events[clip].push_back({e.value("time", 0.0f), e.value("name", "")});
    if (j.contains("editor")) g.editorData = j["editor"];
    if (j.contains("layers"))
        for (auto& l : j["layers"]) g.layers.push_back(parseLayer(l));
    else if (j.contains("states"))
        g.layers.push_back(parseLayer(j));
    return g;
}

Json AnimGraph::toJson() const {
    Json j;
    Json ps = Json::array();
    for (auto& p : params) ps.push_back({{"name", p.name}, {"type", p.type}, {"default", p.value}});
    j["params"] = ps;
    Json ls = Json::array();
    for (auto& L : layers) {
        Json l = {{"name", L.name}, {"default", L.defaultState}, {"weight", L.weight}, {"additive", L.additive}, {"mask", L.mask}};
        Json ss = Json::array();
        for (auto& s : L.states) {
            Json sj = {{"name", s.name}, {"speed", s.speed}};
            if (s.blend == 0) sj["clip"] = s.clip;
            else if (s.blend == 1) {
                Json ch = Json::array();
                for (auto& c : s.children) ch.push_back({{"clip", c.clip}, {"pos", c.pos}});
                sj["blend1d"] = {{"param", s.paramX}, {"children", ch}};
            } else {
                Json ch = Json::array();
                for (auto& c : s.children) ch.push_back({{"clip", c.clip}, {"x", c.x}, {"y", c.y}});
                sj["blend2d"] = {{"x", s.paramX}, {"y", s.paramY}, {"children", ch}};
            }
            ss.push_back(sj);
        }
        l["states"] = ss;
        Json ts = Json::array();
        for (auto& t : L.transitions) {
            Json cs = Json::array();
            for (auto& c : t.conditions) cs.push_back({{"param", c.param}, {"op", c.op}, {"value", c.value}});
            Json tj = {{"from", t.from}, {"to", t.to}, {"duration", t.duration}, {"curve", t.curve}, {"conditions", cs}};
            if (t.exitTime >= 0) tj["exitTime"] = t.exitTime;
            ts.push_back(tj);
        }
        l["transitions"] = ts;
        ls.push_back(l);
    }
    j["layers"] = ls;
    if (!events.empty()) {
        Json ev = Json::object();
        for (auto& [clip, list] : events) {
            Json arr = Json::array();
            for (auto& e : list) arr.push_back({{"time", e.time}, {"name", e.name}});
            ev[clip] = arr;
        }
        j["events"] = ev;
    }
    if (!editorData.empty()) j["editor"] = editorData;
    return j;
}

Json AnimGraph::defaultLocomotion() {
    return Json::parse(R"({"params":[{"name":"speed","type":"float","default":0},{"name":"jump","type":"trigger"},{"name":"wave","type":"trigger"}],
 "layers":[{"name":"Base","default":"Idle","states":[
   {"name":"Idle","clip":"Idle"},
   {"name":"Move","blend1d":{"param":"speed","children":[{"clip":"Idle","pos":0},{"clip":"Walk","pos":1.4},{"clip":"Run","pos":4.0}]}},
   {"name":"Jump","clip":"Jump"},
   {"name":"Wave","clip":"Wave"}],
  "transitions":[
   {"from":"Idle","to":"Move","duration":0.2,"curve":"ease","conditions":[{"param":"speed","op":">","value":0.1}]},
   {"from":"Move","to":"Idle","duration":0.25,"curve":"ease","conditions":[{"param":"speed","op":"<","value":0.05}]},
   {"from":"*","to":"Jump","duration":0.1,"conditions":[{"param":"jump","op":"trigger"}]},
   {"from":"Jump","to":"Idle","duration":0.2,"exitTime":0.95},
   {"from":"Idle","to":"Wave","duration":0.2,"conditions":[{"param":"wave","op":"trigger"}]},
   {"from":"Wave","to":"Idle","duration":0.3,"exitTime":0.95}]}]})");
}

// ============================================================================== animator ===
void Animator::setGraph(const AnimGraph& g) {
    m_graph = g;
    m_layers.assign(g.layers.size(), {});
    for (auto& p : g.params)
        if (!m_params.count(p.name)) m_params[p.name] = p.value;
}

void Animator::setFloat(const std::string& p, float v) { m_params[p] = v; }
float Animator::param(const std::string& p) const {
    auto it = m_params.find(p);
    return it == m_params.end() ? 0.0f : it->second;
}

bool Animator::play(const std::string& state, float fade, int layer, float playSpeed) {
    if (m_layers.size() != m_graph.layers.size()) m_layers.assign(m_graph.layers.size(), {});
    if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return false;
    int idx = m_graph.layers[static_cast<size_t>(layer)].find(state);
    if (idx < 0) return false;
    LayerState& L = m_layers[static_cast<size_t>(layer)];
    L.previous = fade > 0 ? L.current : -1;
    L.prevTime = L.time;
    L.current = idx;
    L.time = 0;
    L.fade = 0;
    L.fadeDuration = fade;
    L.curve = "linear";
    L.playSpeed = std::isfinite(playSpeed) && playSpeed > 0 ? std::min(playSpeed, 10.0f) : 1.0f;
    return true;
}

void Animator::setLayerSpeed(int layer, float speed) {
    if (m_layers.size() != m_graph.layers.size()) m_layers.assign(m_graph.layers.size(), {});
    if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return;
    m_layers[static_cast<size_t>(layer)].speed = std::isfinite(speed) ? std::clamp(speed, 0.0f, 10.0f) : 1.0f;
}
float Animator::layerSpeed(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return layer >= 0 && layer < layerCount() ? 1.0f : 0.0f;
    const LayerState& L = m_layers[static_cast<size_t>(layer)];
    return L.speed * L.playSpeed;
}

std::string Animator::currentState(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(m_layers.size()) || m_layers[static_cast<size_t>(layer)].current < 0) return {};
    return m_graph.layers[static_cast<size_t>(layer)].states[static_cast<size_t>(m_layers[static_cast<size_t>(layer)].current)].name;
}
void Animator::setLayerWeight(int layer, float weight) {
    if (m_layers.size() != m_graph.layers.size()) m_layers.assign(m_graph.layers.size(), {});
    if (layer <= 0 || layer >= static_cast<int>(m_layers.size())) return;
    m_layers[static_cast<size_t>(layer)].weightOverride = weight < 0 ? -1.0f : std::min(weight, 1.0f);
}
float Animator::layerWeight(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(m_graph.layers.size())) return 0;
    if (layer == 0) return 1;
    float o = layer < static_cast<int>(m_layers.size()) ? m_layers[static_cast<size_t>(layer)].weightOverride : -1.0f;
    return o >= 0 ? o : m_graph.layers[static_cast<size_t>(layer)].weight;
}
int Animator::layerIndex(std::string_view name) const {
    for (size_t i = 0; i < m_graph.layers.size(); ++i)
        if (m_graph.layers[i].name == name) return static_cast<int>(i);
    return -1;
}

float Animator::stateTime(int layer) const { return layer < static_cast<int>(m_layers.size()) ? m_layers[static_cast<size_t>(layer)].time : 0; }
bool Animator::inTransition(int layer) const {
    if (layer >= static_cast<int>(m_layers.size())) return false;
    const auto& L = m_layers[static_cast<size_t>(layer)];
    return L.previous >= 0 && L.fade < L.fadeDuration;
}

float Animator::stateDuration(const AnimGraph::State& st, const std::map<std::string, const Clip*>& clips) const {
    auto dur = [&](const std::string& n) {
        auto it = clips.find(n);
        return it != clips.end() && it->second ? it->second->duration : 1.0f;
    };
    if (st.blend == 0) return dur(st.clip);
    // weighted by the same weights evalState uses
    float total = 0, wsum = 0;
    if (st.blend == 1 && !st.children.empty()) {
        float p = param(st.paramX);
        if (p <= st.children.front().pos) return dur(st.children.front().clip);
        if (p >= st.children.back().pos) return dur(st.children.back().clip);
        for (size_t i = 0; i + 1 < st.children.size(); ++i)
            if (p >= st.children[i].pos && p <= st.children[i + 1].pos) {
                float k = (p - st.children[i].pos) / std::max(1e-6f, st.children[i + 1].pos - st.children[i].pos);
                return dur(st.children[i].clip) * (1 - k) + dur(st.children[i + 1].clip) * k;
            }
    }
    for (auto& c : st.children) {
        float dx = c.x - param(st.paramX), dy = c.y - param(st.paramY);
        float w = 1.0f / (dx * dx + dy * dy + 1e-3f);
        total += dur(c.clip) * w;
        wsum += w;
    }
    return wsum > 0 ? total / wsum : 1.0f;
}

void Animator::evalState(const AnimGraph::State& st, float time, const Skeleton& s, const std::map<std::string, const Clip*>& clips, Pose& out,
                         Vec3* motion, float dt) const {
    out = restPose(s);
    float dur = std::max(1e-4f, stateDuration(st, clips));
    float phase = time / dur;  // normalised (sync) time
    auto sampleChild = [&](const std::string& name, Pose& p, float w) {
        auto it = clips.find(name);
        if (it == clips.end() || !it->second) return;
        const Clip& c = *it->second;
        float t = phase * c.duration;
        sampleClip(c, t, p);
        if (motion && w > 0) *motion += rootMotion(c, t - dt * st.speed * c.duration / dur, t) * w;
    };
    if (st.blend == 0) {
        sampleChild(st.clip, out, 1);
        return;
    }
    std::vector<std::pair<std::string, float>> weights;
    if (st.blend == 1 && !st.children.empty()) {
        float p = param(st.paramX);
        if (p <= st.children.front().pos) weights.push_back({st.children.front().clip, 1});
        else if (p >= st.children.back().pos) weights.push_back({st.children.back().clip, 1});
        else
            for (size_t i = 0; i + 1 < st.children.size(); ++i)
                if (p >= st.children[i].pos && p <= st.children[i + 1].pos) {
                    float k = (p - st.children[i].pos) / std::max(1e-6f, st.children[i + 1].pos - st.children[i].pos);
                    weights.push_back({st.children[i].clip, 1 - k});
                    weights.push_back({st.children[i + 1].clip, k});
                    break;
                }
    } else {
        float sum = 0;
        for (auto& c : st.children) {
            float dx = c.x - param(st.paramX), dy = c.y - param(st.paramY);
            float w = 1.0f / (dx * dx + dy * dy + 1e-3f);
            weights.push_back({c.clip, w});
            sum += w;
        }
        for (auto& w : weights) w.second /= sum;
    }
    float acc = 0;
    for (auto& [clip, w] : weights) {
        if (w <= 0) continue;
        Pose p = restPose(s);
        sampleChild(clip, p, w);
        if (acc <= 0) out = p;
        else blendPoses(out, p, w / (acc + w), out);
        acc += w;
    }
}

Vec3 Animator::update(float dt, const Skeleton& s, const std::map<std::string, const Clip*>& clips, Pose& out) {
    m_events.clear();
    if (m_layers.size() != m_graph.layers.size()) m_layers.assign(m_graph.layers.size(), {});
    Vec3 motion{0, 0, 0};
    out = restPose(s);
    for (size_t li = 0; li < m_graph.layers.size(); ++li) {
        const auto& G = m_graph.layers[li];
        LayerState& L = m_layers[li];
        if (G.states.empty()) continue;
        if (L.current < 0) L.current = std::max(0, G.find(G.defaultState));
        // transitions
        const auto& cur = G.states[static_cast<size_t>(L.current)];
        float normalized = L.time / std::max(1e-4f, stateDuration(cur, clips));
        for (const auto& tr : G.transitions) {
            if (tr.from != "*" && tr.from != cur.name) continue;
            if (tr.to == cur.name && tr.from == "*") continue;
            int to = G.find(tr.to);
            if (to < 0) continue;
            bool ok = true;
            std::vector<std::string> consume;
            for (auto& c : tr.conditions) {
                float v = param(c.param);
                bool r = c.op == ">" ? v > c.value : c.op == "<" ? v < c.value : c.op == ">=" ? v >= c.value : c.op == "<=" ? v <= c.value
                         : c.op == "==" ? std::fabs(v - c.value) < 1e-4f : c.op == "!=" ? std::fabs(v - c.value) >= 1e-4f
                         : c.op == "true" ? v > 0.5f : c.op == "false" ? v <= 0.5f : c.op == "trigger" ? v > 0.5f : false;
                if (c.op == "trigger" && r) consume.push_back(c.param);
                ok = ok && r;
            }
            if (tr.exitTime >= 0) ok = ok && normalized >= tr.exitTime;
            if (tr.conditions.empty() && tr.exitTime < 0) ok = false;  // needs at least one condition
            if (!ok) continue;
            for (auto& p : consume) m_params[p] = 0;
            L.previous = L.current;
            L.prevTime = L.time;
            L.current = to;
            L.time = 0;
            L.fade = 0;
            L.fadeDuration = tr.duration;
            L.curve = tr.curve;
            L.playSpeed = 1;  // a play() speed lasts until the layer leaves that state
            break;
        }
        const auto& st = G.states[static_cast<size_t>(L.current)];
        // 1.3.2 layer clock: setLayerSpeed x play() speed (both 1 by default, then ldt == dt exactly)
        const float ldt = dt * (L.speed * L.playSpeed);
        float before = L.time;
        L.time += ldt * st.speed;
        L.prevTime += ldt;
        L.fade += ldt;
        // events from the dominant clip of the current state
        {
            std::string clipName = st.blend == 0 ? st.clip : (st.children.empty() ? "" : st.children.back().clip);
            if (st.blend == 1 && !st.children.empty()) {  // the closest child
                float p = param(st.paramX), best = 1e30f;
                for (auto& c : st.children)
                    if (std::fabs(c.pos - p) < best) {
                        best = std::fabs(c.pos - p);
                        clipName = c.clip;
                    }
            }
            auto it = clips.find(clipName);
            if (it != clips.end() && it->second) {
                float dur = std::max(1e-4f, stateDuration(st, clips));
                float c0 = before / dur * it->second->duration, c1 = L.time / dur * it->second->duration;
                eventsBetween(*it->second, c0, c1, m_events);
                if (auto ex = m_graph.events.find(clipName); ex != m_graph.events.end()) {
                    Clip tmp;  // authored events: same wrap rules as clip events
                    tmp.duration = it->second->duration;
                    tmp.loop = it->second->loop;
                    tmp.events = ex->second;
                    eventsBetween(tmp, c0, c1, m_events);
                }
            }
        }
        Pose layerPose;
        Vec3 lm{0, 0, 0};
        evalState(st, L.time, s, clips, layerPose, &lm, ldt);
        if (L.previous >= 0 && L.fade < L.fadeDuration) {
            Pose prev;
            Vec3 pm{0, 0, 0};
            evalState(G.states[static_cast<size_t>(L.previous)], L.prevTime, s, clips, prev, &pm, ldt);
            float k = std::clamp(L.fade / std::max(1e-4f, L.fadeDuration), 0.0f, 1.0f);
            if (L.curve == "ease") k = k * k * (3 - 2 * k);
            blendPoses(prev, layerPose, k, layerPose);
            lm = pm * (1 - k) + lm * k;
        } else {
            L.previous = -1;
        }
        std::vector<float> mask;
        if (!G.mask.empty()) {
            mask.assign(s.bones.size(), 0.0f);
            for (size_t b = 0; b < s.bones.size(); ++b) {  // a masked bone includes its descendants
                for (int p = static_cast<int>(b); p >= 0; p = s.bones[static_cast<size_t>(p)].parent)
                    if (std::find(G.mask.begin(), G.mask.end(), s.bones[static_cast<size_t>(p)].name) != G.mask.end()) {
                        mask[b] = 1;
                        break;
                    }
            }
        }
        float weight = L.weightOverride >= 0 ? L.weightOverride : G.weight;
        if (li == 0) {
            out = layerPose;
            motion = lm;
        } else if (G.additive) {
            applyAdditive(out, makeAdditive(layerPose, restPose(s)), weight, mask);
        } else {
            blendPoses(out, layerPose, weight, out, mask);
        }
    }
    // root motion is returned to the caller; the pose keeps the root at its rest XZ (no double movement)
    if (!out.empty() && !s.bones.empty()) {
        out[0].position.x = s.bones[0].rest.position.x;
        out[0].position.z = s.bones[0].rest.position.z;
    }
    return motion;
}

// ============================================================================== characters ===
const Clip* Character::clip(std::string_view n) const {
    for (auto& c : clips)
        if (c.name == n) return &c;
    return nullptr;
}

std::map<std::string, const Clip*> Character::clipMap() const {
    std::map<std::string, const Clip*> m;
    for (auto& c : clips) m[c.name] = &c;
    return m;
}

namespace {
template <typename T>
std::string pack(const std::vector<T>& v) {
    return base64Encode(std::span(reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(T)));
}
template <typename T>
bool unpack(const Json& j, std::vector<T>& out) {
    if (!j.is_string()) return false;
    Bytes b = base64Decode(j.get<std::string>());
    if (b.size() % sizeof(T)) return false;
    out.resize(b.size() / sizeof(T));
    if (!b.empty()) std::memcpy(out.data(), b.data(), b.size());
    return true;
}
} // namespace

Json Character::toJson() const {
    Json bones = Json::array();
    for (auto& b : skeleton.bones)
    {
        Json bj = {{"name", b.name}, {"parent", b.parent}, {"t", b.rest.position}, {"r", Json::array({b.rest.rotation.x, b.rest.rotation.y, b.rest.rotation.z, b.rest.rotation.w})},
                   {"s", b.rest.scale}};
        bj["ibm"] = std::vector<float>(b.inverseBind.data(), b.inverseBind.data() + 16);
        bones.push_back(bj);
    }
    Json clipsJ = Json::array();
    for (auto& c : clips) {
        Json tracks = Json::array();
        for (auto& t : c.tracks)
            tracks.push_back({{"bone", t.bone}, {"tt", pack(t.tTimes)}, {"t", pack(t.translations)}, {"rt", pack(t.rTimes)}, {"r", pack(t.rotations)},
                              {"st", pack(t.sTimes)}, {"s", pack(t.scales)}});
        Json ev = Json::array();
        for (auto& e : c.events) ev.push_back({{"time", e.time}, {"name", e.name}});
        clipsJ.push_back({{"name", c.name}, {"duration", c.duration}, {"loop", c.loop}, {"root", c.rootBone}, {"tracks", tracks}, {"events", ev}});
    }
    Json meshJ = {{"vertices", pack(mesh.vertices)}, {"indices", pack(mesh.indices)}, {"joints", pack(skin.joints)}, {"weights", pack(skin.weights)}};
    // .fchar versions (docs/ANIMATION.md ".fchar format"): a 4-influence character is written exactly as 1.3.1 wrote it
    // (version 1), so 1.3.1 and older read it unchanged. Version 2 (1.3.2) adds influences 5..8 ("joints1" / "weights1";
    // an older reader ignores them and skins with the first 4).
    int version = 1;
    if (!skin.joints1.empty()) {
        meshJ["joints1"] = pack(skin.joints1);
        meshJ["weights1"] = pack(skin.weights1);
        version = 2;
    }
    if (!submeshes.empty()) {  // 1.3.2 material slots (an older reader ignores them: one mesh, one material)
        Json slots = Json::array();
        for (const auto& s : submeshes) slots.push_back({{"name", s.name}, {"material", s.material}, {"first", s.firstIndex}, {"count", s.indexCount}});
        meshJ["submeshes"] = slots;
        version = 2;
    }
    Json out = {{"format", "fchar"}, {"version", version}, {"name", name}, {"bones", bones}, {"mesh", meshJ}, {"clips", clipsJ}};
    return out;
}

std::optional<Character> Character::fromJson(const Json& j, std::string* error) {
    try {
        return fromJsonChecked(j, error);
    } catch (const std::exception& e) {  // wrong JSON types (e.g. a string where a number belongs)
        if (error) *error = std::string("damaged .fchar: ") + e.what();
        return std::nullopt;
    }
}

std::optional<Character> Character::fromJsonChecked(const Json& j, std::string* error) {
    auto fail = [&](const std::string& m) -> std::optional<Character> {
        if (error) *error = m;
        return std::nullopt;
    };
    if (!j.is_object() || j.value("format", "") != "fchar") return fail("not a .fchar character");
    Character c;
    c.name = j.value("name", "");
    for (auto& b : j.value("bones", Json::array())) {
        if (!b.is_object()) return fail("bone entry is not an object");
        Bone bone;
        bone.name = b.value("name", "");
        bone.parent = b.value("parent", -1);
        auto v3 = [](const Json& a, Vec3 d) { return a.is_array() && a.size() == 3 ? Vec3{a[0].get<float>(), a[1].get<float>(), a[2].get<float>()} : d; };
        bone.rest.position = v3(b.value("t", Json()), {0, 0, 0});
        bone.rest.scale = v3(b.value("s", Json()), {1, 1, 1});
        auto r = b.value("r", Json::array({0, 0, 0, 1}));
        if (!r.is_array() || r.size() != 4) return fail(std::format("bone {} ('{}'): rotation must have 4 numbers", c.skeleton.bones.size(), bone.name));
        bone.rest.rotation = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()};
        if (bone.parent >= static_cast<int>(c.skeleton.bones.size())) return fail("bone parent after child");
        if (bone.parent < -1) return fail(std::format("bone {} ('{}'): invalid parent {}", c.skeleton.bones.size(), bone.name, bone.parent));
        c.skeleton.bones.push_back(bone);
    }
    if (c.skeleton.bones.empty()) return fail("the character has no bones");
    c.skeleton.computeInverseBind();
    {
        size_t i = 0;
        for (auto& b : j.value("bones", Json::array())) {
            if (b.contains("ibm") && b["ibm"].is_array() && b["ibm"].size() == 16 && i < c.skeleton.bones.size())
                for (int k = 0; k < 16; ++k) (&c.skeleton.bones[i].inverseBind.c[0].x)[k] = b["ibm"][static_cast<size_t>(k)].get<float>();
            ++i;
        }
    }
    const Json& m = j.value("mesh", Json::object());
    if (!unpack(m.value("vertices", Json()), c.mesh.vertices) || !unpack(m.value("indices", Json()), c.mesh.indices) ||
        !unpack(m.value("joints", Json()), c.skin.joints) || !unpack(m.value("weights", Json()), c.skin.weights))
        return fail("damaged mesh data");
    for (uint32_t i : c.mesh.indices)
        if (i >= c.mesh.vertices.size()) return fail("index out of range");
    // skin: one joint/weight entry per vertex (an animation-only character has neither vertices nor skin)
    if (c.skin.joints.size() != c.mesh.vertices.size())
        return fail(std::format("skin has {} joint entries for {} vertices", c.skin.joints.size(), c.mesh.vertices.size()));
    if (c.skin.weights.size() != c.skin.joints.size())
        return fail(std::format("skin has {} weight entries for {} joint entries", c.skin.weights.size(), c.skin.joints.size()));
    for (size_t v = 0; v < c.skin.joints.size(); ++v)
        for (int k = 0; k < 4; ++k)
            if (c.skin.weights[v][k] > 0 && c.skin.joints[v][static_cast<size_t>(k)] >= c.skeleton.bones.size())
                return fail(std::format("vertex {} uses joint {} but the skeleton has {} bones", v, c.skin.joints[v][static_cast<size_t>(k)], c.skeleton.bones.size()));
    // version 2 (1.3.2): influences 5..8, both arrays or neither, one entry per vertex
    if (m.contains("joints1") || m.contains("weights1")) {
        if (!unpack(m.value("joints1", Json()), c.skin.joints1) || !unpack(m.value("weights1", Json()), c.skin.weights1))
            return fail("damaged mesh data (joints1 / weights1)");
        if (c.skin.joints1.size() != c.mesh.vertices.size() || c.skin.weights1.size() != c.skin.joints1.size())
            return fail(std::format("skin influences 5..8 have {} joint / {} weight entries for {} vertices", c.skin.joints1.size(),
                                    c.skin.weights1.size(), c.mesh.vertices.size()));
        for (size_t v = 0; v < c.skin.joints1.size(); ++v)
            for (int k = 0; k < 4; ++k)
                if (c.skin.weights1[v][k] > 0 && c.skin.joints1[v][static_cast<size_t>(k)] >= c.skeleton.bones.size())
                    return fail(std::format("vertex {} uses joint {} (influence {}) but the skeleton has {} bones", v,
                                            c.skin.joints1[v][static_cast<size_t>(k)], k + 5, c.skeleton.bones.size()));
        if (c.skin.joints1.empty()) c.skin.weights1.clear();
    }
    // version 2 (1.3.2): material slots, each a whole-triangle range inside the index buffer
    if (m.contains("submeshes")) {
        const Json& sj = m["submeshes"];
        if (!sj.is_array()) return fail("mesh.submeshes must be an array");
        if (sj.size() > 1024) return fail(std::format("{} material slots (at most 1024)", sj.size()));
        for (const Json& s : sj) {
            if (!s.is_object()) return fail("material slot is not an object");
            Submesh sm;
            sm.name = s.value("name", "");
            sm.material = s.value("material", "");
            int64_t first = s.value("first", int64_t(-1)), count = s.value("count", int64_t(-1));
            if (first < 0 || count < 0 || first % 3 || count % 3 || static_cast<uint64_t>(first) + static_cast<uint64_t>(count) > c.mesh.indices.size())
                return fail(std::format("material slot {} ('{}'): indices {}..{} are not whole triangles inside the {} indices", c.submeshes.size(), sm.name,
                                        first, first + count, c.mesh.indices.size()));
            if (sm.name.size() > 256 || sm.material.size() > 1024) return fail("material slot name / material too long");
            sm.firstIndex = static_cast<uint32_t>(first);
            sm.indexCount = static_cast<uint32_t>(count);
            c.submeshes.push_back(std::move(sm));
        }
    }
    for (auto& cj : j.value("clips", Json::array())) {
        Clip clip;
        clip.name = cj.value("name", "");
        clip.duration = cj.value("duration", 1.0f);
        clip.loop = cj.value("loop", true);
        clip.rootBone = cj.value("root", 0);
        for (auto& tj : cj.value("tracks", Json::array())) {
            Track t;
            t.bone = tj.value("bone", -1);
            unpack(tj.value("tt", Json()), t.tTimes);
            unpack(tj.value("t", Json()), t.translations);
            unpack(tj.value("rt", Json()), t.rTimes);
            unpack(tj.value("r", Json()), t.rotations);
            unpack(tj.value("st", Json()), t.sTimes);
            unpack(tj.value("s", Json()), t.scales);
            if (t.tTimes.size() != t.translations.size() || t.rTimes.size() != t.rotations.size() || t.sTimes.size() != t.scales.size())
                return fail("track key count mismatch");
            if (t.bone < 0 || static_cast<size_t>(t.bone) >= c.skeleton.bones.size())
                return fail(std::format("clip '{}' animates bone {} but the skeleton has {} bones", clip.name, t.bone, c.skeleton.bones.size()));
            clip.tracks.push_back(std::move(t));
        }
        for (auto& e : cj.value("events", Json::array())) clip.events.push_back({e.value("time", 0.0f), e.value("name", "")});
        c.clips.push_back(std::move(clip));
    }
    return c;
}

// ============================================================================== mannequin ===
namespace {
void addBox(MeshData& m, SkinData& skin, Vec3 a, Vec3 b, float w, float d, uint16_t bone) {
    // a box from a to b (bone segment), w x d cross-section, in model space
    Vec3 axis = b - a;
    float len = length(axis);
    if (len < 1e-5f) return;
    Vec3 y = axis / len;
    Vec3 x = std::fabs(y.y) > 0.9f ? normalize(cross({0, 0, 1}, y)) : normalize(cross({0, 1, 0}, y));
    Vec3 z = cross(x, y);
    Vec3 c = (a + b) * 0.5f;
    Vec3 hx = x * (w * 0.5f), hy = y * (len * 0.5f), hz = z * (d * 0.5f);
    const Vec3 n[6] = {x, -x, y, -y, z, -z};
    for (int f = 0; f < 6; ++f) {
        Vec3 nn = n[f];
        Vec3 u = (f < 2) ? hy : (f < 4 ? hx : hx), v = (f < 2) ? hz : (f < 4 ? hz : hy);
        Vec3 o = c + (f == 0 ? hx : f == 1 ? -hx : f == 2 ? hy : f == 3 ? -hy : f == 4 ? hz : -hz);
        uint32_t base = static_cast<uint32_t>(m.vertices.size());
        Vec3 corners[4] = {o - u - v, o + u - v, o + u + v, o - u + v};
        for (int k = 0; k < 4; ++k) {
            m.vertices.push_back({corners[k], nn, {k == 1 || k == 2 ? 1.0f : 0.0f, k >= 2 ? 1.0f : 0.0f}});
            skin.joints.push_back({bone, 0, 0, 0});
            skin.weights.push_back({1, 0, 0, 0});
        }
        // orient so the face normal and winding agree
        Vec3 tri = cross(corners[1] - corners[0], corners[2] - corners[0]);
        if (dot(tri, nn) > 0) m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        else m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
}

Clip makeClip(const char* name, float duration, bool loop, const Skeleton& s, const std::function<void(float, Pose&)>& fn, int fps = 30) {
    Clip c;
    c.name = name;
    c.duration = duration;
    c.loop = loop;
    int frames = std::max(2, static_cast<int>(duration * fps) + 1);
    std::vector<Pose> poses;
    for (int f = 0; f < frames; ++f) {
        Pose p = restPose(s);
        fn(duration * static_cast<float>(f) / static_cast<float>(frames - 1), p);
        poses.push_back(p);
    }
    for (size_t b = 0; b < s.bones.size(); ++b) {
        Track t;
        t.bone = static_cast<int>(b);
        bool movesT = false, movesR = false;
        for (auto& p : poses) {
            movesT |= length(p[b].position - s.bones[b].rest.position) > 1e-5f;
            movesR |= quatAngle(p[b].rotation, s.bones[b].rest.rotation) > 1e-5f;
        }
        for (int f = 0; f < frames; ++f) {
            float tm = duration * static_cast<float>(f) / static_cast<float>(frames - 1);
            if (movesT) {
                t.tTimes.push_back(tm);
                t.translations.push_back(poses[static_cast<size_t>(f)][b].position);
            }
            if (movesR) {
                t.rTimes.push_back(tm);
                t.rotations.push_back(poses[static_cast<size_t>(f)][b].rotation);
            }
        }
        if (movesT || movesR) c.tracks.push_back(std::move(t));
    }
    compressClip(c);
    return c;
}

Character buildMannequin() {
    Character ch;
    ch.name = "Mannequin";
    struct B {
        const char* name;
        int parent;
        Vec3 t;
    };
    const B bones[] = {{"Hips", -1, {0, 0.95f, 0}},          {"Spine", 0, {0, 0.12f, 0}},         {"Chest", 1, {0, 0.22f, 0}},
                       {"Neck", 2, {0, 0.22f, 0}},           {"Head", 3, {0, 0.08f, 0}},          {"UpperArm.L", 2, {0.18f, 0.17f, 0}},
                       {"LowerArm.L", 5, {0.27f, 0, 0}},     {"Hand.L", 6, {0.24f, 0, 0}},        {"UpperArm.R", 2, {-0.18f, 0.17f, 0}},
                       {"LowerArm.R", 8, {-0.27f, 0, 0}},    {"Hand.R", 9, {-0.24f, 0, 0}},       {"UpperLeg.L", 0, {0.1f, -0.06f, 0}},
                       {"LowerLeg.L", 11, {0, -0.42f, 0}},   {"Foot.L", 12, {0, -0.42f, 0}},      {"UpperLeg.R", 0, {-0.1f, -0.06f, 0}},
                       {"LowerLeg.R", 14, {0, -0.42f, 0}},   {"Foot.R", 15, {0, -0.42f, 0}}};
    for (auto& b : bones) {
        Bone bone;
        bone.name = b.name;
        bone.parent = b.parent;
        bone.rest.position = b.t;
        ch.skeleton.bones.push_back(bone);
    }
    ch.skeleton.computeInverseBind();
    auto model = modelSpace(ch.skeleton, restPose(ch.skeleton));
    auto P = [&](int i) { return model[static_cast<size_t>(i)].c[3].xyz(); };
    // body segments
    addBox(ch.mesh, ch.skin, P(0) - Vec3{0, 0.05f, 0}, P(1), 0.32f, 0.2f, 0);
    addBox(ch.mesh, ch.skin, P(1), P(2), 0.34f, 0.2f, 1);
    addBox(ch.mesh, ch.skin, P(2), P(3), 0.4f, 0.22f, 2);
    addBox(ch.mesh, ch.skin, P(3), P(4), 0.1f, 0.1f, 3);
    addBox(ch.mesh, ch.skin, P(4), P(4) + Vec3{0, 0.25f, 0}, 0.22f, 0.24f, 4);
    for (int side : {5, 8}) {
        addBox(ch.mesh, ch.skin, P(side), P(side + 1), 0.09f, 0.09f, static_cast<uint16_t>(side));
        addBox(ch.mesh, ch.skin, P(side + 1), P(side + 2), 0.08f, 0.08f, static_cast<uint16_t>(side + 1));
        addBox(ch.mesh, ch.skin, P(side + 2), P(side + 2) + (P(side + 2) - P(side + 1)) * 0.35f, 0.09f, 0.04f, static_cast<uint16_t>(side + 2));
    }
    for (int side : {11, 14}) {
        addBox(ch.mesh, ch.skin, P(side), P(side + 1), 0.13f, 0.13f, static_cast<uint16_t>(side));
        addBox(ch.mesh, ch.skin, P(side + 1), P(side + 2), 0.11f, 0.11f, static_cast<uint16_t>(side + 1));
        // foot points forward (-Z)
        addBox(ch.mesh, ch.skin, P(side + 2) + Vec3{0, 0.03f, 0.05f}, P(side + 2) + Vec3{0, 0.03f, -0.17f}, 0.1f, 0.07f, static_cast<uint16_t>(side + 2));
    }
    const Skeleton& s = ch.skeleton;
    auto rot = [](Vec3 axis, float deg) { return Quat::fromAxisAngle(axis, radians(deg)); };
    const float PI2 = 6.2831853f;
    // arms down from the T-pose
    auto armsDown = [&](Pose& p, float swingL, float swingR) {
        p[5].rotation = rot({1, 0, 0}, swingL) * rot({0, 0, 1}, -75);
        p[8].rotation = rot({1, 0, 0}, swingR) * rot({0, 0, 1}, 75);
        p[6].rotation = rot({0, 1, 0}, -10);
        p[9].rotation = rot({0, 1, 0}, 10);
    };
    ch.clips.push_back(makeClip("Idle", 2.0f, true, s, [&](float t, Pose& p) {
        float b = std::sin(t / 2.0f * PI2);
        p[1].rotation = rot({1, 0, 0}, 1.5f * b);
        p[4].rotation = rot({0, 1, 0}, 4 * std::sin(t / 2.0f * PI2 * 0.5f));
        armsDown(p, 2 * b, -2 * b);
    }));
    auto gait = [&](float cycle, float speed, float legAmp, float kneeAmp, float armAmp, float bob) {
        return [=, &rot, &armsDown](float t, Pose& p) {
            float ph = t / cycle * PI2;
            float l = std::sin(ph), r = std::sin(ph + 3.14159265f);
            p[11].rotation = rot({1, 0, 0}, legAmp * l);
            p[14].rotation = rot({1, 0, 0}, legAmp * r);
            p[12].rotation = rot({1, 0, 0}, -kneeAmp * std::max(0.0f, std::sin(ph + 1.2f)));
            p[15].rotation = rot({1, 0, 0}, -kneeAmp * std::max(0.0f, std::sin(ph + 1.2f + 3.14159265f)));
            armsDown(p, -armAmp * l, -armAmp * r);
            p[0].position = s.bones[0].rest.position + Vec3{0, bob * std::fabs(std::cos(ph)), -speed * t};  // forward = -Z
            p[1].rotation = rot({1, 0, 0}, speed > 3 ? 8.0f : 3.0f);
        };
    };
    Clip walk = makeClip("Walk", 1.0f, true, s, gait(1.0f, 1.4f, 28, 40, 22, 0.03f));
    walk.events = {{0.0f, "footstep"}, {0.5f, "footstep"}};
    ch.clips.push_back(walk);
    Clip run = makeClip("Run", 0.66f, true, s, gait(0.66f, 4.0f, 45, 80, 45, 0.06f));
    run.events = {{0.0f, "footstep"}, {0.33f, "footstep"}};
    ch.clips.push_back(run);
    Clip jump = makeClip("Jump", 0.8f, false, s, [&](float t, Pose& p) {
        float k = t / 0.8f;
        float crouch = k < 0.2f ? k / 0.2f : (k > 0.8f ? (1 - k) / 0.2f : 0.0f);
        float air = (k > 0.2f && k < 0.8f) ? std::sin((k - 0.2f) / 0.6f * 3.14159265f) : 0.0f;
        p[0].position = s.bones[0].rest.position + Vec3{0, -0.15f * crouch + 0.6f * air, 0};
        p[11].rotation = rot({1, 0, 0}, 30 * crouch + 20 * air);
        p[14].rotation = rot({1, 0, 0}, 30 * crouch + 20 * air);
        p[12].rotation = rot({1, 0, 0}, -60 * crouch - 30 * air);
        p[15].rotation = rot({1, 0, 0}, -60 * crouch - 30 * air);
        armsDown(p, -40 * air, -40 * air);
    });
    jump.events = {{0.2f, "takeoff"}, {0.72f, "land"}};
    ch.clips.push_back(jump);
    Clip wave = makeClip("Wave", 1.5f, false, s, [&](float t, Pose& p) {
        armsDown(p, 0, 0);
        float up = std::min(1.0f, std::min(t / 0.3f, (1.5f - t) / 0.3f));
        p[8].rotation = rot({0, 0, 1}, 75 - 230 * up);
        p[9].rotation = rot({0, 0, 1}, -40 * up * (1 + 0.5f * std::sin(t * 12)));
    });
    ch.clips.push_back(wave);
    for (auto& c : ch.clips) c.rootBone = 0;
    return ch;
}
} // namespace

const Character& mannequin() {
    static const Character c = buildMannequin();
    return c;
}

// ============================================================================== BVH ===
std::optional<Character> parseBvh(std::string_view text, float scale, std::string* error) {
    auto fail = [&](const std::string& m) -> std::optional<Character> {
        if (error) *error = m;
        return std::nullopt;
    };
    std::istringstream in{std::string(text)};
    std::string tok;
    Character ch;
    struct Chan {
        int bone;
        int kind;  // 0..2 pos xyz, 3..5 rot xyz
    };
    std::vector<Chan> chans;
    std::vector<int> stack;
    bool endSite = false;
    in >> tok;
    if (tok != "HIERARCHY") return fail("not a BVH file");
    while (in >> tok) {
        if (tok == "ROOT" || tok == "JOINT") {
            Bone b;
            in >> b.name;
            b.parent = stack.empty() ? -1 : stack.back();
            ch.skeleton.bones.push_back(b);
        } else if (tok == "End") {
            in >> tok;  // Site
            endSite = true;
        } else if (tok == "{") {
            stack.push_back(endSite ? -2 : static_cast<int>(ch.skeleton.bones.size()) - 1);
        } else if (tok == "}") {
            if (stack.empty()) return fail("unbalanced braces");
            if (stack.back() == -2) endSite = false;
            stack.pop_back();
        } else if (tok == "OFFSET") {
            float x, y, z;
            in >> x >> y >> z;
            if (!endSite && !ch.skeleton.bones.empty()) ch.skeleton.bones.back().rest.position = Vec3{x, y, z} * scale;
        } else if (tok == "CHANNELS") {
            int n;
            in >> n;
            for (int i = 0; i < n; ++i) {
                in >> tok;
                int k = tok == "Xposition" ? 0 : tok == "Yposition" ? 1 : tok == "Zposition" ? 2 : tok == "Xrotation" ? 3 : tok == "Yrotation" ? 4 : 5;
                chans.push_back({static_cast<int>(ch.skeleton.bones.size()) - 1, k});
            }
        } else if (tok == "MOTION") {
            break;
        }
    }
    // stack entries for end sites used -2; parents above were pushed correctly since End Site pushes -2
    int frames = 0;
    float frameTime = 1.0f / 30;
    in >> tok >> frames;            // Frames: N
    in >> tok >> tok >> frameTime;  // Frame Time: x
    if (frames <= 0 || ch.skeleton.bones.empty()) return fail("no motion data");
    ch.skeleton.computeInverseBind();
    Clip clip;
    clip.name = "Take";
    clip.duration = frameTime * static_cast<float>(frames - 1);
    clip.loop = true;
    std::vector<Track> tracks(ch.skeleton.bones.size());
    for (size_t b = 0; b < tracks.size(); ++b) tracks[b].bone = static_cast<int>(b);
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> pos(ch.skeleton.bones.size());
        std::vector<Quat> rotq(ch.skeleton.bones.size());
        std::vector<bool> hasPos(ch.skeleton.bones.size(), false);
        for (auto& c : chans) {
            float v;
            if (!(in >> v)) return fail("truncated motion data");
            if (c.kind < 3) {
                pos[static_cast<size_t>(c.bone)][c.kind] = v * scale;
                hasPos[static_cast<size_t>(c.bone)] = true;
            } else {
                Vec3 axis = c.kind == 3 ? Vec3{1, 0, 0} : c.kind == 4 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
                rotq[static_cast<size_t>(c.bone)] = (rotq[static_cast<size_t>(c.bone)] * Quat::fromAxisAngle(axis, radians(v))).normalized();
            }
        }
        float t = frameTime * static_cast<float>(f);
        for (size_t b = 0; b < tracks.size(); ++b) {
            tracks[b].rTimes.push_back(t);
            tracks[b].rotations.push_back(rotq[b]);
            if (hasPos[b]) {
                tracks[b].tTimes.push_back(t);
                tracks[b].translations.push_back(pos[b]);
            }
        }
    }
    clip.tracks = std::move(tracks);
    compressClip(clip);
    ch.clips.push_back(std::move(clip));
    ch.name = ch.skeleton.bones[0].name;
    return ch;
}

// ============================================================ 1.3.3 robustness warnings ===
std::vector<ZeroScaleTrack> zeroScaleTracks(const Character& ch) {
    std::vector<ZeroScaleTrack> out;
    for (size_t c = 0; c < ch.clips.size(); ++c)
        for (const Track& t : ch.clips[c].tracks) {
            ZeroScaleTrack z{c, t.bone, 0, 0};
            for (size_t k = 0; k < t.scales.size(); ++k) {
                const Vec3& s = t.scales[k];
                if (s.x == 0.0f || s.y == 0.0f || s.z == 0.0f) {
                    if (z.keys++ == 0) z.firstTime = k < t.sTimes.size() ? t.sTimes[k] : 0.0f;
                }
            }
            if (z.keys > 0) out.push_back(z);
        }
    return out;
}

std::vector<std::string> robustnessWarnings(Character& ch, const std::string& name, bool fixNormals) {
    std::vector<std::string> out;
    if (!ch.mesh.empty()) {
        std::vector<uint32_t> zero = zeroNormalVertices(ch.mesh);
        if (!zero.empty()) {
            int fixed = fixNormals ? fixZeroNormals(ch.mesh) : -1;
            out.push_back(zeroNormalWarning(std::format("character '{}'", name), ch.mesh.vertices.size(), zero, fixed));
        }
    }
    std::vector<ZeroScaleTrack> zs = zeroScaleTracks(ch);
    if (!zs.empty()) {
        // clips in order, each with its bones (first 3 named with the time of their first zero key)
        std::string list;
        size_t clips = 0, shown = 0;
        for (size_t i = 0; i < zs.size();) {
            size_t c = zs[i].clip, j = i;
            std::string bones;
            size_t nb = 0;
            for (; j < zs.size() && zs[j].clip == c; ++j, ++nb) {
                if (nb >= 3) continue;
                const int b = zs[j].bone;
                std::string bn = b >= 0 && static_cast<size_t>(b) < ch.skeleton.bones.size() ? ch.skeleton.bones[static_cast<size_t>(b)].name
                                                                                          : std::format("#{}", b);
                bones += std::format("{}'{}' from {:.2f} s", nb ? ", " : "", bn, zs[j].firstTime);
            }
            if (nb > 3) bones += std::format(", +{} more", nb - 3);
            ++clips;
            if (shown < 4) {
                list += std::format("{}'{}' ({} bone{}: {})", shown ? "; " : "", ch.clips[c].name, nb, nb == 1 ? "" : "s", bones);
                ++shown;
            }
            i = j;
        }
        if (clips > shown) list += std::format("; +{} more clip{}", clips - shown, clips - shown == 1 ? "" : "s");
        out.push_back(std::format("character '{}': {} clip{} key a bone's scale to exactly 0 - {}. Vertices skinned only to such a bone "
                                  "collapse and get zero-length normals (Fledge 1.3.3 shades them with the face normal; older versions and "
                                  "other engines can show black pixels and a bloom halo). To hide a part, key a tiny scale such as 0.002 "
                                  "instead, or toggle its visibility (docs/ANIMATION.md \"Hiding parts\")",
                                  name, clips, clips == 1 ? "" : "s", list));
    }
    return out;
}

} // namespace fledge::anim
