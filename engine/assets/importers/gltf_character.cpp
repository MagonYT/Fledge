// glTF 2.0 skin + animation import -> anim::Character (.fchar). See docs/ANIMATION.md.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>

#include <cgltf.h>

#include "fledge/importers/importers.h"

namespace fledge {

std::string characterBoneWarning(const anim::Character& ch, const std::string& name, Edition edition) {
    // same values as kMaxGpuSkinBones / kMaxGpuSkinBonesAdvanced (renderer.h; tests/test_skinning_1024.cpp checks they agree)
    const size_t limit = edition == Edition::Advanced ? 1024 : 256;
    const size_t bones = ch.skeleton.bones.size();
    if (bones <= limit || ch.mesh.empty()) return {};
    return std::format("character '{}' has {} bones: more than the {} GPU skinning limit of {} bones, so it is skinned on the CPU (slower; "
                       "Fledge skins up to 256 bones on the GPU, Fledge Advanced up to 1024)",
                       name, bones, editionName(edition), limit);
}

namespace {
Transform nodeLocal(const cgltf_node* n) {
    Transform t;
    if (n->has_matrix) {
        Mat4 m;
        std::memcpy(&m.c[0].x, n->matrix, sizeof(float) * 16);
        return decompose(m);
    }
    if (n->has_translation) t.position = {n->translation[0], n->translation[1], n->translation[2]};
    if (n->has_rotation) t.rotation = Quat{n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3]}.normalized();
    if (n->has_scale) t.scale = {n->scale[0], n->scale[1], n->scale[2]};
    return t;
}
} // namespace

std::vector<std::string> skinWeightWarnings(anim::Character& ch, const std::string& name, bool fix) {
    std::vector<std::string> out;
    std::vector<uint32_t> zero = anim::zeroWeightVertices(ch.skin);
    if (zero.empty()) return out;
    std::string ex;
    for (size_t i = 0; i < zero.size() && i < 8; ++i) ex += (i ? ", " : "") + std::to_string(zero[i]);
    if (zero.size() > 8) ex += ", ...";
    const size_t total = zero.size();
    if (fix) {
        int fixed = anim::fixZeroWeights(ch.mesh, ch.skin);
        out.push_back(std::format("character '{}': {} of {} vertices had no skin weight (all {} influences zero; e.g. vertex {}); fixZeroWeights gave "
                                  "{} of them the joints / weights of their nearest weighted vertex",
                                  name, total, ch.mesh.vertices.size(), ch.skin.influences(), ex, fixed));
    } else {
        out.push_back(std::format("character '{}': {} of {} vertices have no skin weight (all {} influences zero; e.g. vertex {}): they skin to the "
                                  "model origin. Fix the weights in the source tool, or re-import with fixZeroWeights (each such vertex takes the "
                                  "weights of its nearest weighted vertex; docs/ANIMATION.md \"Skin weights\")",
                                  name, total, ch.mesh.vertices.size(), ch.skin.influences(), ex));
    }
    return out;
}

Result<anim::Character> importGltfCharacter(std::span<const uint8_t> bytes, const fs::path& baseDir, std::vector<std::string>* warnings,
                                            bool fixZeroWeights, bool fixZeroNormals) {
    using R = Result<anim::Character>;
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&opt, bytes.data(), bytes.size(), &data) != cgltf_result_success) return R::failure("not a valid glTF 2.0 file");
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> guard(data, &cgltf_free);
    std::string base = narrow((baseDir / "x").wstring());
    if (cgltf_load_buffers(&opt, data, base.c_str()) != cgltf_result_success) return R::failure("glTF buffers could not be loaded");
    // hostile input: accessors must lie inside their buffers before anything reads them (cgltf_accessor_read_* trusts them)
    if (cgltf_validate(data) != cgltf_result_success) return R::failure("glTF validation failed (accessor / buffer ranges)");
    if (data->skins_count == 0) return R::failure("the glTF file has no skin");
    const cgltf_skin& skin = data->skins[0];
    anim::Character ch;
    ch.name = skin.name ? skin.name : "Character";
    // joints in parent-before-child order
    std::set<const cgltf_node*> jointSet(skin.joints, skin.joints + skin.joints_count);
    std::vector<const cgltf_node*> order;
    std::set<const cgltf_node*> placed;
    while (order.size() < skin.joints_count) {
        size_t before = order.size();
        for (cgltf_size i = 0; i < skin.joints_count; ++i) {
            const cgltf_node* n = skin.joints[i];
            if (placed.count(n)) continue;
            const cgltf_node* p = n->parent;
            if (p && jointSet.count(p) && !placed.count(p)) continue;
            order.push_back(n);
            placed.insert(n);
        }
        if (order.size() == before) return R::failure("cyclic joint hierarchy");
    }
    std::map<const cgltf_node*, int> boneOf;
    for (size_t i = 0; i < order.size(); ++i) boneOf[order[i]] = static_cast<int>(i);
    std::vector<int> jointToBone(skin.joints_count);
    for (cgltf_size i = 0; i < skin.joints_count; ++i) jointToBone[i] = boneOf[skin.joints[i]];
    for (const cgltf_node* n : order) {
        anim::Bone b;
        b.name = n->name ? n->name : std::format("bone{}", ch.skeleton.bones.size());
        b.parent = n->parent && boneOf.count(n->parent) ? boneOf[n->parent] : -1;
        b.rest = nodeLocal(n);
        ch.skeleton.bones.push_back(b);
    }
    ch.skeleton.computeInverseBind();
    if (skin.inverse_bind_matrices) {
        for (cgltf_size i = 0; i < skin.joints_count; ++i) {
            Mat4 m;
            cgltf_accessor_read_float(skin.inverse_bind_matrices, i, &m.c[0].x, 16);
            ch.skeleton.bones[static_cast<size_t>(jointToBone[i])].inverseBind = m;
        }
    }
    // mesh primitives skinned by this skin. 1.3.2: JOINTS_1 / WEIGHTS_1 (influences 5..8; UE5 exports use them), 8- or 16-bit
    // joints and float / normalized byte / short weights (cgltf converts), weights renormalised over all 8 influences.
    size_t badJoints = 0, badPrims = 0;
    bool eight = false;  // some primitive has influences 5..8: every vertex then carries them (zeros where it has none)
    std::string badPrimWhy;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (node.skin != &skin || !node.mesh) continue;
        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *joints = nullptr, *weights = nullptr, *joints1 = nullptr,
                                 *weights1 = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index == 0) joints = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index == 0) weights = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index == 1) joints1 = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index == 1) weights1 = at.data;
            }
            if (!pos || !joints || !weights) continue;
            if (!joints1 != !weights1) {  // half a second set: ignore it (and say so)
                ++badPrims;
                badPrimWhy = "JOINTS_1 without WEIGHTS_1 (or the reverse): the second influence set was ignored";
                joints1 = weights1 = nullptr;
            }
            // every attribute must cover every vertex (cgltf_validate checks this too; a hostile file must not read past an accessor)
            bool counts = joints->count >= pos->count && weights->count >= pos->count && (!nrm || nrm->count >= pos->count) &&
                          (!uv || uv->count >= pos->count) && (!joints1 || (joints1->count >= pos->count && weights1->count >= pos->count));
            bool indicesOk = true;
            if (prim.indices)
                for (cgltf_size i = 0; i < prim.indices->count && indicesOk; ++i) indicesOk = cgltf_accessor_read_index(prim.indices, i) < pos->count;
            if (!counts || !indicesOk || (prim.indices && prim.indices->count % 3) || (!prim.indices && pos->count % 3)) {
                ++badPrims;
                badPrimWhy = !counts ? "an attribute has fewer entries than POSITION" : !indicesOk ? "an index points past the last vertex"
                                                                                                  : "the index count is not a multiple of 3";
                continue;
            }
            if (joints1 && !eight) {  // first 8-influence primitive: earlier vertices get empty influences 5..8
                eight = true;
                ch.skin.joints1.assign(ch.skin.joints.size(), {0, 0, 0, 0});
                ch.skin.weights1.assign(ch.skin.weights.size(), Vec4{0, 0, 0, 0});
            }
            uint32_t baseV = static_cast<uint32_t>(ch.mesh.vertices.size());
            auto readSet = [&](const cgltf_accessor* ja, const cgltf_accessor* wa, cgltf_size v, std::array<uint16_t, 4>& jb, Vec4& w) {
                cgltf_uint j[4] = {0, 0, 0, 0};
                cgltf_accessor_read_uint(ja, v, j, 4);
                w = {0, 0, 0, 0};
                cgltf_accessor_read_float(wa, v, &w.x, 4);
                for (int k = 0; k < 4; ++k) {
                    if (!std::isfinite(w[k]) || w[k] < 0) w[k] = 0;  // NaN / negative weights from a broken exporter
                    bool known = j[k] < jointToBone.size();
                    if (!known && w[k] > 0) ++badJoints;
                    jb[static_cast<size_t>(k)] = static_cast<uint16_t>(known ? jointToBone[j[k]] : 0);
                    if (!known) w[k] = 0;  // a joint outside the skin carries no weight (was: silently bone 0)
                }
            };
            for (cgltf_size v = 0; v < pos->count; ++v) {
                Vertex vx;
                cgltf_accessor_read_float(pos, v, &vx.position.x, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, &vx.normal.x, 3);
                else vx.normal = {0, 1, 0};
                if (uv) cgltf_accessor_read_float(uv, v, &vx.uv.x, 2);
                ch.mesh.vertices.push_back(vx);
                std::array<uint16_t, 4> jb{}, jb1{};
                Vec4 w, w1{0, 0, 0, 0};
                readSet(joints, weights, v, jb, w);
                if (joints1) readSet(joints1, weights1, v, jb1, w1);
                // normalise over every influence (4-influence vertices: exactly the 1.3.1 arithmetic, the added zeros change nothing)
                float sum = w.x + w.y + w.z + w.w + (w1.x + w1.y + w1.z + w1.w);
                if (sum > 0) {
                    w = w * (1.0f / sum);
                    w1 = w1 * (1.0f / sum);
                }
                ch.skin.joints.push_back(jb);
                ch.skin.weights.push_back(w);
                if (eight) {
                    ch.skin.joints1.push_back(jb1);
                    ch.skin.weights1.push_back(w1);
                }
            }
            const uint32_t firstIndex = static_cast<uint32_t>(ch.mesh.indices.size());
            if (prim.indices)
                for (cgltf_size i = 0; i < prim.indices->count; ++i) ch.mesh.indices.push_back(baseV + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
            else
                for (cgltf_size i = 0; i < pos->count; ++i) ch.mesh.indices.push_back(baseV + static_cast<uint32_t>(i));
            // 1.3.2 material slot per primitive (consecutive primitives with the same material share one)
            const int mat = prim.material ? static_cast<int>(prim.material - data->materials) : -1;
            const uint32_t count = static_cast<uint32_t>(ch.mesh.indices.size()) - firstIndex;
            if (!ch.submeshes.empty() && ch.submeshes.back().sourceMaterial == mat &&
                ch.submeshes.back().firstIndex + ch.submeshes.back().indexCount == firstIndex) {
                ch.submeshes.back().indexCount += count;
            } else {
                anim::Submesh sm;
                sm.name = prim.material && prim.material->name ? prim.material->name : std::format("slot{}", ch.submeshes.size());
                sm.firstIndex = firstIndex;
                sm.indexCount = count;
                sm.sourceMaterial = mat;
                ch.submeshes.push_back(sm);
            }
        }
    }
    if (ch.submeshes.size() < 2) ch.submeshes.clear();  // one material: the whole mesh, written exactly as before 1.3.2
    if (warnings) {
        if (badPrims) warnings->push_back(std::format("{} skinned primitive(s) skipped or reduced: {}", badPrims, badPrimWhy));
        if (badJoints) warnings->push_back(std::format("{} vertex influences named a joint outside the skin (index >= {}); their weight was dropped",
                                                       badJoints, jointToBone.size()));
        for (auto& w : skinWeightWarnings(ch, ch.name, fixZeroWeights)) warnings->push_back(w);
    } else if (fixZeroWeights) {
        anim::fixZeroWeights(ch.mesh, ch.skin);
    }
    // animations (a skin without a mesh is an animation-only character: its clips still play, nothing is drawn)
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& an = data->animations[ai];
        anim::Clip clip;
        clip.name = an.name ? an.name : std::format("Take{}", ai);
        std::map<int, anim::Track> tracks;
        for (cgltf_size ci = 0; ci < an.channels_count; ++ci) {
            const cgltf_animation_channel& c = an.channels[ci];
            if (!c.target_node || !boneOf.count(c.target_node) || !c.sampler) continue;
            int bone = boneOf[c.target_node];
            anim::Track& t = tracks[bone];
            t.bone = bone;
            const cgltf_accessor* in = c.sampler->input;
            const cgltf_accessor* out = c.sampler->output;
            bool cubic = c.sampler->interpolation == cgltf_interpolation_type_cubic_spline;
            for (cgltf_size k = 0; k < in->count; ++k) {
                float time = 0;
                cgltf_accessor_read_float(in, k, &time, 1);
                clip.duration = std::max(clip.duration, time);
                cgltf_size idx = cubic ? k * 3 + 1 : k;  // cubic spline: value is the middle element
                if (c.target_path == cgltf_animation_path_type_translation) {
                    Vec3 v;
                    cgltf_accessor_read_float(out, idx, &v.x, 3);
                    t.tTimes.push_back(time);
                    t.translations.push_back(v);
                } else if (c.target_path == cgltf_animation_path_type_rotation) {
                    Quat q;
                    cgltf_accessor_read_float(out, idx, &q.x, 4);
                    t.rTimes.push_back(time);
                    t.rotations.push_back(q.normalized());
                } else if (c.target_path == cgltf_animation_path_type_scale) {
                    Vec3 v;
                    cgltf_accessor_read_float(out, idx, &v.x, 3);
                    t.sTimes.push_back(time);
                    t.scales.push_back(v);
                }
            }
        }
        for (auto& [b, t] : tracks) clip.tracks.push_back(std::move(t));
        if (clip.duration <= 0) clip.duration = 1;
        anim::compressClip(clip);
        ch.clips.push_back(std::move(clip));
    }
    if (ch.mesh.empty() && ch.clips.empty()) return R::failure("no skinned triangle mesh or animation found");
    // 1.3.3: zero-length bind-mesh normals and clips keying a bone's scale to 0 (one warning each; the normal fix is opt-in)
    std::vector<std::string> robust = anim::robustnessWarnings(ch, ch.name, fixZeroNormals);
    if (warnings) warnings->insert(warnings->end(), robust.begin(), robust.end());
    return ch;
}

} // namespace fledge
