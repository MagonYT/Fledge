#include "fledge/framework/anim_system.h"

#include <chrono>
#include <cmath>
#include <format>
#include <functional>

#include "fledge/assets/lod.h"
#include "fledge/core/log.h"
#include "fledge/render/renderer.h"  // kMaxGpuSkinBones
#include "fledge/scene/components.h"

namespace fledge {

namespace {
uint64_t hashAnimGraph(const Json& g, const std::string& character) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix(std::hash<std::string>{}(character));
    if (g.is_null() || g.empty()) {
        mix(0x9e3779b97f4a7c15ull);
        return h;
    }
    std::function<void(const Json&)> walk = [&](const Json& j) {
        mix(static_cast<uint64_t>(j.type()));
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                mix(std::hash<std::string>{}(it.key()));
                walk(it.value());
            }
        } else if (j.is_array()) {
            for (const auto& v : j) walk(v);
        } else if (j.is_string()) {
            mix(std::hash<std::string>{}(j.get_ref<const std::string&>()));
        } else if (j.is_number_integer()) {
            mix(static_cast<uint64_t>(j.get<int64_t>()));
        } else if (j.is_number()) {
            mix(std::hash<double>{}(j.get<double>()));
        } else if (j.is_boolean()) {
            mix(j.get<bool>() ? 1ull : 2ull);
        }
    };
    walk(g);
    return h;
}
} // namespace

const anim::Character* AnimationSystem::character(const std::string& ref) {
    if (ref.empty()) return nullptr;
    if (ref == "builtin:mannequin") return &anim::mannequin();
    auto it = m_chars.find(ref);
    if (it != m_chars.end()) return it->second.get();
    std::unique_ptr<anim::Character> c;
    std::string path = startsWith(ref, "content:/") ? ref : "content:/" + ref;
    std::string err;
    if (auto text = m_vfs.readText(path)) {
        if (auto ch = anim::Character::fromJson(Json::parse(*text, nullptr, false), &err)) c = std::make_unique<anim::Character>(std::move(*ch));
    } else {
        err = "not found";
    }
    if (!c) {
        FLEDGE_LOG_WARN("anim", "Character '{}': {}", ref, err);
        m_charErrors[ref] = err;
    } else {
        m_charErrors.erase(ref);
        // 1.3.3: zero-length bind-mesh normals / clips keying a bone's scale to 0, once per character (also for .fchar files
        // imported before 1.3.3 or written by hand); rendering copes either way (docs/GRAPHICS.md "Robustness rules")
        for (const auto& w : anim::robustnessWarnings(*c, ref, false)) FLEDGE_LOG_WARN("anim", "{}", w);
        if (c->mesh.lods.empty()) generateLods(c->mesh);
    }
    auto* raw = c.get();
    m_chars[ref] = std::move(c);
    return raw;
}

#if FLEDGE_GPU_SKIN_EXTENDED
void AnimationSystem::warnCpuSkinning(const anim::Character& ch, const std::string& ref) {
    if (!m_skinWarned.insert(ref).second) return;
    const int bones = static_cast<int>(ch.skeleton.bones.size());
    FLEDGE_LOG_WARN("anim", "Character '{}' has {} bones: skinned on the CPU ({} GPU skinning limit here: {} bones; Fledge {}, Fledge Advanced {} "
                    "on OpenGL / D3D11 / D3D10 / Vulkan)",
                    ref, bones, editionName(currentEdition()), gpuSkinBones, kMaxGpuSkinBones, kMaxGpuSkinBonesAdvanced);
}
#endif

const anim::Character* AnimationSystem::reloadCharacter(const std::string& ref) {
    if (auto it = m_chars.find(ref); it != m_chars.end() && !it->second) m_chars.erase(it);  // a failed load is retried
    return character(ref);
}

int AnimationSystem::forgetCharacter(const std::string& ref) {
    auto rel = [](const std::string& r) { return startsWith(r, "content:/") ? r.substr(9) : r; };
    std::string want = rel(ref);
    int n = 0;
    for (auto it = m_chars.begin(); it != m_chars.end();) {
        if (rel(it->first) != want) {
            ++it;
            continue;
        }
        const anim::Character* old = it->second.get();
        m_clipMaps.erase(old);
        for (auto& [e, en] : m_entities)  // live Animators re-read the character (clips, skeleton) on their next update
            if (rel(en.charRef) == want) {
                en.graphHash = 0;
                en.character = nullptr;
                en.model.clear();
                en.skinnedMesh = {};
                en.skinMats.clear();
            }
        m_charErrors.erase(it->first);
        it = m_chars.erase(it);
        ++n;
    }
    if (n) ++m_charGeneration;
    return n;
}

void AnimationSystem::forgetAllCharacters() {
    std::vector<std::string> refs;
    for (auto& [k, c] : m_chars) refs.push_back(k);
    for (auto& r : refs) forgetCharacter(r);
}

namespace {
// A layer named in layerPlay / layerSpeeds / layerWeights: its name, or its index as a number or a numeric string.
int resolveLayer(const anim::Animator& a, const Json& key) {
    if (key.is_number_integer()) {
        int i = key.get<int>();
        return i >= 0 && i < a.layerCount() ? i : -1;
    }
    if (!key.is_string()) return -1;
    const std::string& s = key.get_ref<const std::string&>();
    int i = a.layerIndex(s);
    if (i >= 0 || s.empty() || s.size() > 3 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return i;
    i = std::stoi(s);
    return i < a.layerCount() ? i : -1;
}
} // namespace

void AnimationSystem::applyLayerControls(Entry& en, AnimatorComponent& ac) {
    anim::Animator& a = en.animator;
    // persistent speeds: every layer gets its value each update (a removed key = 1)
    if (ac.layerSpeeds.is_object() && !ac.layerSpeeds.empty()) {
        std::vector<float> speeds(static_cast<size_t>(a.layerCount()), 1.0f);
        for (auto& [k, v] : ac.layerSpeeds.items()) {
            int li = resolveLayer(a, Json(k));
            if (li < 0 || !v.is_number()) {
                ac.layerError = std::format("layerSpeeds: unknown layer '{}' (the graph has {} layers)", k, a.layerCount());
                continue;
            }
            speeds[static_cast<size_t>(li)] = v.get<float>();
        }
        for (int li = 0; li < a.layerCount(); ++li) a.setLayerSpeed(li, speeds[static_cast<size_t>(li)]);
    } else if (a.layerCount() > 0) {
        for (int li = 0; li < a.layerCount(); ++li) a.setLayerSpeed(li, 1.0f);
    }
    // weights: set the listed layers; a layer set before and no longer listed returns to the graph weight
    uint64_t mask = 0;
    if (ac.layerWeights.is_object())
        for (auto& [k, v] : ac.layerWeights.items()) {
            int li = resolveLayer(a, Json(k));
            if (li <= 0 || !v.is_number()) {
                ac.layerError = li == 0 ? std::string("layerWeights: the base layer (0) always has weight 1")
                                        : std::format("layerWeights: unknown layer '{}' (the graph has {} layers)", k, a.layerCount());
                continue;
            }
            a.setLayerWeight(li, v.get<float>());
            if (li < 64) mask |= 1ull << li;
        }
    for (int li = 1; li < a.layerCount() && li < 64; ++li)
        if ((en.layerWeightMask & (1ull << li)) && !(mask & (1ull << li))) a.setLayerWeight(li, -1);
    en.layerWeightMask = mask;
    // play requests (consumed)
    if (ac.layerPlay.is_array() && !ac.layerPlay.empty()) {
        for (const Json& r : ac.layerPlay) {
            if (!r.is_object()) continue;
            int li = resolveLayer(a, r.value("layer", Json(0)));
            std::string st = r.contains("state") && r["state"].is_string() ? r["state"].get<std::string>() : std::string();
            float fade = r.contains("fade") && r["fade"].is_number() ? r["fade"].get<float>() : ac.crossFade;
            float speed = r.contains("speed") && r["speed"].is_number() ? r["speed"].get<float>() : 1.0f;
            if (li < 0) ac.layerError = std::format("layerPlay: unknown layer {} (the graph has {} layers)", r.value("layer", Json(0)).dump(), a.layerCount());
            else if (!a.play(st, std::clamp(fade, 0.0f, 10.0f), li, speed))
                ac.layerError = std::format("layerPlay: layer '{}' has no state '{}'", a.graph().layers[static_cast<size_t>(li)].name, st);
            else ac.layerError.clear();
        }
        ac.layerPlay = Json::array();
    }
}

bool AnimationSystem::followLeader(Scene& scene, EntityId e, Entry& en, AnimatorComponent& ac, bool final) {
    const anim::Character* ch = en.character;
    if (!ch) return true;
    EntityId le = scene.findByName(ac.leaderPose);
    auto lit = le && le != e ? m_entities.find(le) : m_entities.end();
    const AnimatorComponent* lac = le ? scene.get<AnimatorComponent>(le) : nullptr;
    const bool leaderFollows = lac && !lac->leaderPose.empty();
    if (lit != m_entities.end() && leaderFollows && lit->second.followFrame != m_frame && !final) return false;  // chain: later
    const Entry* L = lit != m_entities.end() && lit->second.character && lit->second.model.size() == lit->second.character->skeleton.bones.size()
                         ? &lit->second
                         : nullptr;
    if (L && leaderFollows && L->followFrame != m_frame) L = nullptr;  // a cycle: never resolved this update
    const size_t n = ch->skeleton.bones.size();
    std::vector<Mat4>& model = en.model;
    model.resize(n);
    if (!L) {
        ac.leaderStatus = !le ? "leader '" + ac.leaderPose + "' not found (an entity name)"
                        : le == e ? std::string("an Animator cannot follow itself")
                        : !lac ? "leader '" + ac.leaderPose + "' has no Animator"
                        : leaderFollows ? "leader '" + ac.leaderPose + "' follows in a cycle" : "leader '" + ac.leaderPose + "' has no pose yet";
        if (m_leaderWarned.insert(scene.displayName(e) + "|" + ac.leaderStatus).second)
            FLEDGE_LOG_WARN("anim", "Animator '{}' leaderPose: {}; using the rest pose", scene.displayName(e), ac.leaderStatus);
        model = anim::modelSpace(ch->skeleton, anim::restPose(ch->skeleton));
    } else {
        const anim::Character& lc = *L->character;
        if (en.mapLeader != &lc || en.mapSelf != ch || en.leaderMap.size() != n) {  // bone map (by name), rebuilt when a character changes
            en.mapLeader = &lc;
            en.mapSelf = ch;
            en.leaderMap.assign(n, -1);
            en.unmapped.clear();
            en.mapped = 0;
            en.sameRig = lc.skeleton.bones.size() == n;
            for (size_t i = 0; i < n; ++i) {
                int li = en.sameRig && lc.skeleton.bones[i].name == ch->skeleton.bones[i].name ? static_cast<int>(i) : lc.skeleton.find(ch->skeleton.bones[i].name);
                if (li != static_cast<int>(i)) en.sameRig = false;
                en.leaderMap[i] = li;
                if (li >= 0) ++en.mapped;
                else en.unmapped.push_back(ch->skeleton.bones[i].name);
            }
            if (!en.unmapped.empty() && m_leaderWarned.insert(scene.displayName(e) + "|map|" + lc.name).second)
                FLEDGE_LOG_WARN("anim", "Animator '{}' follows '{}': {} of {} bones are not in the leader (e.g. '{}'); they keep their rest pose under their parent",
                                scene.displayName(e), ac.leaderPose, en.unmapped.size(), n, en.unmapped.front());
        }
        if (en.sameRig) {
            model = L->model;
        } else {
            for (size_t i = 0; i < n; ++i) {
                int li = en.leaderMap[i];
                if (li >= 0) {
                    model[i] = L->model[static_cast<size_t>(li)];
                } else {
                    int p = ch->skeleton.bones[i].parent;
                    model[i] = (p >= 0 ? model[static_cast<size_t>(p)] : Mat4{}) * ch->skeleton.bones[i].rest.matrix();
                }
            }
        }
        ac.leaderStatus = std::format("following '{}': {} of {} bones{}", ac.leaderPose, en.mapped, n, en.sameRig ? " (same rig)" : " (by name)");
        if (lac) ac.state = lac->state;
    }
    ac.events = Json::array();
    en.followFrame = m_frame;
    en.crowdKey.clear();
    en.played = true;
    ++m_stats.followers;
    if (gpuSkins(*ch)) {  // the vertex shader skins the follower's own bind mesh with the leader's pose
        en.skinMats = anim::skinMatrices(ch->skeleton, model);
        en.skinnedMesh = {};
        ++m_stats.gpuSkinned;
    } else {
        en.skinMats.clear();
        if (!ch->mesh.empty()) anim::skinMesh(ch->mesh, ch->skin, anim::skinMatrices(ch->skeleton, model), en.skinnedMesh);
        ++m_stats.skinnedMeshes;
    }
    ++en.version;
    return true;
}

Json AnimationSystem::leaderInfo(Scene& scene, EntityId e) const {
    Json j = Json::object();
    auto* ac = scene.get<AnimatorComponent>(e);
    if (!ac || ac->leaderPose.empty()) {
        j["active"] = false;
        return j;
    }
    j["leader"] = ac->leaderPose;
    j["status"] = ac->leaderStatus;
    j["active"] = advancedRenderFeatures();
    if (!advancedRenderFeatures()) j["note"] = advancedRenderNote("Leader pose (Animator.leaderPose)");
    auto it = m_entities.find(e);
    if (it != m_entities.end() && it->second.mapLeader) {
        const Entry& en = it->second;
        j["mode"] = en.sameRig ? "same rig" : "by name";
        j["mapped"] = en.mapped;
        j["bones"] = en.leaderMap.size();
        Json um = Json::array();
        for (size_t i = 0; i < en.unmapped.size() && i < 32; ++i) um.push_back(en.unmapped[i]);
        j["unmapped"] = um;
        j["unmappedCount"] = en.unmapped.size();
    }
    return j;
}

void AnimationSystem::writeLayerState(const Entry& en, AnimatorComponent& ac) const {
    const anim::Animator& a = en.animator;
    const int n = a.layerCount();
    if (!ac.layers.is_array() || static_cast<int>(ac.layers.size()) != n) {
        ac.layers = Json::array();
        for (int li = 0; li < n; ++li) ac.layers.push_back(Json::object());
    }
    for (int li = 0; li < n; ++li) {
        Json& o = ac.layers[static_cast<size_t>(li)];
        o["index"] = li;
        o["name"] = a.graph().layers[static_cast<size_t>(li)].name;
        o["state"] = a.currentState(li);
        o["time"] = a.stateTime(li);
        o["weight"] = a.layerWeight(li);
        o["speed"] = a.layerSpeed(li);
        o["inTransition"] = a.inTransition(li);
    }
}

std::vector<Mat4> AnimationSystem::boneMatrices(EntityId e) const {
    auto it = m_entities.find(e);
    if (it == m_entities.end()) return {};
    auto ch = m_chars.find(it->second.charRef);
    const anim::Character* c = it->second.charRef == "builtin:mannequin" ? &anim::mannequin() : (ch != m_chars.end() ? ch->second.get() : nullptr);
    if (!c) return {};
    if (it->second.model.size() == c->skeleton.bones.size()) return it->second.model;
    if (it->second.pose.size() != c->skeleton.bones.size()) return {};
    return anim::modelSpace(c->skeleton, it->second.pose);
}

const std::vector<Mat4>* AnimationSystem::animatedPose(EntityId e) const {
    auto it = m_entities.find(e);
    if (it == m_entities.end() || it->second.animated.empty()) return nullptr;
    return &it->second.animated;
}

std::optional<Mat4> AnimationSystem::socketBone(Scene& scene, EntityId owner, const std::string& bone) {
    auto warnOnce = [&](const std::string& why) {
        if (m_socketWarned.insert({owner, bone}).second)
            FLEDGE_LOG_WARN("anim", "Bone socket '{}' on '{}': {}; following the entity itself", bone, scene.displayName(owner), why);
    };
    auto* ac = scene.get<AnimatorComponent>(owner);
    if (!ac) {
        warnOnce("it has no Animator");
        return std::nullopt;
    }
    const anim::Character* ch = character(ac->character);
    if (!ch) return std::nullopt;  // missing / malformed character: logged by character()
    int idx = ch->skeleton.find(bone);
    if (idx < 0) {
        warnOnce(std::format("character '{}' has no such bone ({} bones)", ac->character, ch->skeleton.bones.size()));
        return std::nullopt;
    }
    auto it = m_entities.find(owner);
    if (it != m_entities.end() && it->second.charRef == ac->character && it->second.model.size() == ch->skeleton.bones.size())
        return it->second.model[static_cast<size_t>(idx)];
    // not evaluated yet (scene just loaded, no animate() so far): the rest pose
    Mat4 m{};
    std::vector<int> chain;
    for (int b = idx; b >= 0; b = ch->skeleton.bones[static_cast<size_t>(b)].parent) chain.push_back(b);
    for (auto r = chain.rbegin(); r != chain.rend(); ++r) m = m * ch->skeleton.bones[static_cast<size_t>(*r)].rest.matrix();
    return m;
}

std::vector<std::string> AnimationSystem::boneNames(Scene& scene, EntityId e) {
    auto* ac = scene.get<AnimatorComponent>(e);
    const anim::Character* ch = ac ? character(ac->character) : nullptr;
    std::vector<std::string> out;
    if (ch)
        for (auto& b : ch->skeleton.bones) out.push_back(b.name);
    return out;
}

anim::Animator* AnimationSystem::animator(EntityId e) {
    auto it = m_entities.find(e);
    return it == m_entities.end() ? nullptr : &it->second.animator;
}

AnimationSystem::Skinned AnimationSystem::skinned(EntityId e) const {
    auto it = m_entities.find(e);
    if (it != m_entities.end() && !it->second.skinMats.empty() && it->second.skinnedMesh.empty() && it->second.crowdKey.empty())
        return {std::format("anim:{}", e), it->second.version, nullptr, &it->second.skinMats, it->second.character};
    if (it == m_entities.end() || it->second.skinnedMesh.empty()) {
        if (it != m_entities.end() && !it->second.crowdKey.empty()) {
            auto c = m_crowd.find(it->second.crowdKey);
            if (c != m_crowd.end()) return {it->second.crowdKey, c->second.second, &c->second.first};
        }
        return {};
    }
    return {std::format("anim:{}", e), it->second.version, &it->second.skinnedMesh, nullptr, it->second.character};
}

void AnimationSystem::update(Scene& scene, float dt, bool playing, Vec3 viewer, const std::function<std::optional<float>(float, float)>& ground) {
    auto t0 = std::chrono::steady_clock::now();
    m_stats = {};
    if (!playing && !m_catchUp) {
        bool work = false;
        scene.each<AnimatorComponent, TransformComponent>([&](EntityId e, AnimatorComponent& ac, TransformComponent&) {
            if (work) return;
            if (ac.preview) {
                work = true;
                return;
            }
            auto it = m_entities.find(e);
            if (it == m_entities.end() || it->second.charRef != ac.character || it->second.model.empty()) work = true;
        });
        if (!work) {
            m_stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return;
        }
    }
    ++m_frame;
    std::vector<EntityId> alive;
    std::unordered_map<EntityId, Mat4> worlds;
    if (playing) worlds = scene.computeWorldMatrices();
    std::map<std::string, int> crowdUsers;
    std::vector<EntityId> followers;  // 1.3.2 leader pose: resolved after every leader has its pose
    scene.each<AnimatorComponent, TransformComponent>([&](EntityId e, AnimatorComponent& ac, TransformComponent& tc) {
        ++m_stats.animators;
        alive.push_back(e);
        const anim::Character* ch = character(ac.character);
        if (!ch) return;
        Entry& en = m_entities[e];
        // (re)configure the state machine when the graph changes
        bool defaultGraph = ac.graph.is_null() || ac.graph.empty();
        uint64_t gh = hashAnimGraph(ac.graph, ac.character);
        if (gh != en.graphHash) {
            static const anim::AnimGraph kDefault = anim::AnimGraph::fromJson(anim::AnimGraph::defaultLocomotion());
            en.animator = {};
            en.animator.setGraph(defaultGraph ? kDefault : anim::AnimGraph::fromJson(ac.graph));
            en.graphHash = gh;
            en.layerWeightMask = 0;
            if (en.charRef != ac.character || en.character != ch) {
                // 1.3.2 fix: a new character (Animator.character changed, or the .fchar was reloaded) rebinds everything derived
                // from the old one. Before, a rig with the same bone count kept the old skinned mesh in edit mode and the GPU
                // path cached the new character's bind mesh from the old one (until set_anim_graph was re-applied).
                en.model.clear();
                en.skinnedMesh = {};
                en.skinMats.clear();
                en.crowdKey.clear();
                en.character = ch;
                en.pendingDt = 0;
                en.played = false;
                ++en.version;
            }
            en.charRef = ac.character;
            en.pose = anim::restPose(ch->skeleton);
        }
        // 1.3.2 leader pose (Fledge Advanced): no state machine here, the pose comes from the leader after this loop
        if (!ac.leaderPose.empty()) {
            if (advancedRenderFeatures()) {
                en.character = ch;
                followers.push_back(e);
                return;
            }
            ac.leaderStatus = "ignored: " + advancedRenderNote("Leader pose (Animator.leaderPose)");
            if (m_leaderWarned.insert("std:" + scene.displayName(e)).second)
                FLEDGE_LOG_WARN("anim", "Animator '{}': {}; it animates on its own", scene.displayName(e), ac.leaderStatus);
        } else if (!ac.leaderStatus.empty()) {
            ac.leaderStatus.clear();
        }
        if (m_preview.entity == e && !m_preview.clip.empty()) {  // editor clip scrubbing
            anim::Pose p = anim::restPose(ch->skeleton);
            if (const anim::Clip* clip = ch->clip(m_preview.clip)) anim::sampleClip(*clip, m_preview.time, p);
            en.pose = p;
            en.model = anim::modelSpace(ch->skeleton, p);
            anim::skinMesh(ch->mesh, ch->skin, anim::skinMatrices(ch->skeleton, en.model), en.skinnedMesh);
            ++en.version;
            en.crowdKey.clear();
            en.skinMats.clear();
            return;
        }
        bool physicalPose = false;
        if (playing && poseOverride)
            if (const std::vector<Mat4>* ms = poseOverride(e); ms && ms->size() == ch->skeleton.bones.size()) physicalPose = true;
        bool animate = playing || ac.preview;
        if (!animate) {  // edit mode without preview: idle / rest pose, GPU-skinned when the renderer can
            en.played = false;
            const bool gpu = gpuSkins(*ch);
            const bool havePose = en.model.size() == ch->skeleton.bones.size();
            const bool haveSkin = gpu ? !en.skinMats.empty() || ch->mesh.empty() : !en.skinnedMesh.empty() || ch->mesh.empty();
            if (!havePose || !haveSkin) {
                anim::Pose p = anim::restPose(ch->skeleton);
                if (auto* idle = ch->clip("Idle")) anim::sampleClip(*idle, 0, p);
                en.pose = p;
                en.model = anim::modelSpace(ch->skeleton, p);
                if (gpu) {
                    en.skinMats = anim::skinMatrices(ch->skeleton, en.model);
                    en.skinnedMesh = {};
                    ++m_stats.gpuSkinned;
                } else {
                    en.skinMats.clear();
                    if (!ch->mesh.empty()) anim::skinMesh(ch->mesh, ch->skin, anim::skinMatrices(ch->skeleton, en.model), en.skinnedMesh);
                    ++m_stats.skinnedMeshes;
                }
                ++en.version;
            }
            ac.state.clear();
            return;
        }
        // parameters / requests from the component (scripts, MCP, editor)
        const anim::AnimGraph& g = en.animator.graph();
        if (ac.params.is_object()) {
            Json consumed = ac.params;
            for (auto& [k, v] : ac.params.items()) {
                bool isTrigger = false;
                for (auto& p : g.params)
                    if (p.name == k && p.type == "trigger") isTrigger = true;
                float f = v.is_boolean() ? (v.get<bool>() ? 1.0f : 0.0f) : v.is_number() ? v.get<float>() : 0.0f;
                if (isTrigger) {
                    if (f > 0.5f) en.animator.trigger(k);
                    consumed[k] = false;
                } else {
                    en.animator.setFloat(k, f);
                }
            }
            ac.params = consumed;
        }
        if (!ac.play.empty()) {
            en.animator.play(ac.play, ac.crossFade);
            ac.play.clear();
        }
        applyLayerControls(en, ac);  // 1.3.2: play on any layer, per-layer speed / weight
        // animation LOD: far characters update less often (their time still advances in full)
        Vec3 pos = worlds[e].c[3].xyz();
        int interval = lodInterval(length(pos - viewer));
        en.pendingDt += dt * ac.speed;
        if (m_catchUp) {  // catchUp(): only the Animators that LOD skipped (time still pending, or never evaluated in play)
            if (en.pendingDt <= 0 && en.played) return;
        } else if (++en.frame % interval != 0 && !en.skinnedMesh.empty() && en.crowdKey.empty()) {
            ++m_stats.skipped;
            return;
        }
        float step = en.pendingDt;
        en.pendingDt = 0;
        auto cm = m_clipMaps.find(ch);
        if (cm == m_clipMaps.end()) cm = m_clipMaps.emplace(ch, ch->clipMap()).first;
        Vec3 motion = en.animator.update(step, ch->skeleton, cm->second, en.pose);
        en.played = true;
        ++m_stats.updated;
        ac.state = en.animator.currentState();
        ac.events = Json(en.animator.firedEvents());
        writeLayerState(en, ac);
        if (playing && ac.rootMotion && !physicalPose && (motion.x != 0 || motion.z != 0)) {
            Quat r = decompose(worlds[e]).rotation;
            Vec3 w = r.rotate(motion);
            tc.position += Vec3{w.x, 0, w.z};
        }
        // IK
        if (ac.footIK && ground) {
            const anim::Skeleton& s = ch->skeleton;
            const Mat4& m = worlds[e];
            Mat4 inv = m.inverse();
            int hl = s.find("UpperLeg.L"), kl = s.find("LowerLeg.L"), al = s.find("Foot.L");
            int hr = s.find("UpperLeg.R"), kr = s.find("LowerLeg.R"), ar = s.find("Foot.R");
            if (hl >= 0 && kl >= 0 && al >= 0 && hr >= 0 && kr >= 0 && ar >= 0)
                anim::footPlacement(s, en.pose, {{hl, kl, al}, {hr, kr, ar}}, [&](float x, float z) -> std::optional<float> {
                    Vec3 wp = m.transformPoint({x, 0, z});
                    auto h = ground(wp.x, wp.z);
                    if (!h) return std::nullopt;
                    return inv.transformPoint({wp.x, *h, wp.z}).y;
                });
        }
        if (!ac.lookAt.empty()) {
            EntityId target = scene.findByName(ac.lookAt);
            int head = ch->skeleton.find("Head");
            if (target && head >= 0) {
                Vec3 tl = worlds[e].inverse().transformPoint(worlds[target].c[3].xyz());
                anim::lookAt(ch->skeleton, en.pose, head, tl, {0, 0, -1}, 0.7f);
            }
        }
        if (playing && posePostProcess) posePostProcess(e, *ch, en.pose, worlds[e]);  // module IK (Fledge Advanced)
        // model-space pose: bone sockets / script bone queries read it on every path (GPU skinning included)
        en.model = anim::modelSpace(ch->skeleton, en.pose);
        en.animated = en.model;
        if (physicalPose) {  // draw the simulated bones; animated keeps the clip the motors track
            en.model = *poseOverride(e);
            ac.state = "Ragdoll";
            en.crowdKey.clear();
        }
        // crowd: share one skinned mesh per (character, state, phase bucket)
        if (!physicalPose && ac.crowd && !ac.footIK && ac.lookAt.empty()) {
            float phase = en.animator.stateTime() * 8.0f;
            int bucket = static_cast<int>(std::floor(phase)) % 8;
            en.crowdKey = std::format("crowd:{}/{}/{}", ac.character, ac.state, bucket);
            en.skinnedMesh = {};
            ++m_stats.crowdAgents;
            if (!crowdUsers.count(en.crowdKey)) {
                auto& slot = m_crowd[en.crowdKey];
                anim::skinMesh(ch->mesh, ch->skin, anim::skinMatrices(ch->skeleton, en.model), slot.first);
                ++slot.second;
                ++m_stats.crowdMeshes;
            }
            ++crowdUsers[en.crowdKey];
            return;
        }
        en.crowdKey.clear();
        en.character = ch;
        if (gpuSkins(*ch)) {  // the vertex shader skins the bind mesh
            en.skinMats = anim::skinMatrices(ch->skeleton, en.model);
            en.skinnedMesh = {};
            ++en.version;
            ++m_stats.gpuSkinned;
            return;
        }
#if FLEDGE_GPU_SKIN_EXTENDED
        // over the GPU bone limit (once per character); only when running as Fledge Advanced, so the core runtime of an
        // Advanced build logs exactly what Fledge's does
        if (gpuSkinning && !ch->mesh.empty() && maxGpuSkinBones() > kMaxGpuSkinBones) warnCpuSkinning(*ch, ac.character);
#endif
        en.skinMats.clear();
        anim::skinMesh(ch->mesh, ch->skin, anim::skinMatrices(ch->skeleton, en.model), en.skinnedMesh);
        ++en.version;
        ++m_stats.skinnedMeshes;
    });
    // 1.3.2 leader pose: followers copy their leader's pose of this update (leaders that follow another leader first; a
    // cycle or a missing leader leaves the rest pose and says why in leaderStatus)
    for (int pass = 0; pass < 8 && !followers.empty(); ++pass) {
        std::vector<EntityId> waiting;
        for (EntityId f : followers)
            if (auto* ac = scene.get<AnimatorComponent>(f); ac && !followLeader(scene, f, m_entities[f], *ac, pass == 7)) waiting.push_back(f);
        if (waiting.size() == followers.size() && pass < 7) {  // no progress: resolve what is left as final (cycle / missing leader)
            for (EntityId f : waiting)
                if (auto* ac = scene.get<AnimatorComponent>(f)) followLeader(scene, f, m_entities[f], *ac, true);
            break;
        }
        followers.swap(waiting);
    }
    // forget removed entities
    if (m_entities.size() != alive.size())
        for (auto it = m_entities.begin(); it != m_entities.end();)
            it = std::find(alive.begin(), alive.end(), it->first) == alive.end() ? m_entities.erase(it) : std::next(it);
    m_stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace fledge
