#pragma once
// Runs Animator components (docs/ANIMATION.md): parameters / play requests from the component (Fledge Script files,
// MCP, editor), state machine evaluation, root motion, foot IK + look-at, animation LOD (lower update rates
// with distance) and crowd pose sharing (agents in the same state and phase bucket share one skinned mesh,
// which the renderer then draws instanced). Poses are always evaluated on the CPU; skinning runs in the vertex shader when
// the renderer supports it (up to kMaxGpuSkinBones bones), otherwise on the CPU.
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "fledge/anim/anim.h"
#include "fledge/core/vfs.h"
#include "fledge/scene/scene.h"

namespace fledge {

struct AnimatorComponent;

class AnimationSystem {
public:
    explicit AnimationSystem(Vfs& vfs) : m_vfs(vfs) {}
    // ground(x, z) -> world height, for foot IK. viewer: for animation LOD.
    void update(Scene& scene, float dt, bool playing, Vec3 viewer, const std::function<std::optional<float>(float, float)>& ground);
    // Evaluates, without advancing time, every Animator that animation LOD skipped in the last update(s) (its pending time
    // is applied now), so state / events / pose / root motion are those of the current frame. MCP play + simulateFrames
    // calls it before returning (GameWorld::catchUpAnimation): headless sessions have no camera, so far characters are LOD'd.
    void catchUp(Scene& scene, Vec3 viewer, const std::function<std::optional<float>(float, float)>& ground) {
        m_catchUp = true;
        update(scene, 0, true, viewer, ground);
        m_catchUp = false;
    }
    struct Skinned {
        std::string key;  // unique per mesh content this frame ("anim:<entity>" or "crowd:<char>/<state>/<bucket>")
        uint64_t version = 0;
        const MeshData* mesh = nullptr;            // CPU-skinned mesh (or nullptr on the GPU path)
        const std::vector<Mat4>* bones = nullptr;  // GPU path: skin matrices; draw the character's bind mesh
        const anim::Character* character = nullptr;
    };
    // Set by GameWorld from the renderer (OpenGL / D3D11 / D3D10 / Vulkan; crowds stay on the CPU path). Characters with more
    // than gpuSkinBones bones are skinned on the CPU: 256 (kMaxGpuSkinBones) in Fledge, in Fledge Advanced (1.3.1)
    // ForwardRenderer::gpuSkinBoneLimit() = 1024 on the backends with the 4-block shader variant.
    bool gpuSkinning = false;
    int gpuSkinBones = 256;
    // 1.3.2: most skin influences per vertex the GPU path handles (ForwardRenderer::gpuSkinInfluences(): 8 where the 8-influence
    // shader variants exist, else 4); characters with more are skinned on the CPU.
    int gpuSkinInfluences = 8;
    // The GPU path is used for a character with this many bones (gpuSkinning and bones <= gpuSkinBones).
    bool gpuSkins(size_t bones) const { return gpuSkinning && bones <= static_cast<size_t>(gpuSkinBones); }
    bool gpuSkins(const anim::Character& ch) const { return gpuSkins(ch.skeleton.bones.size()) && ch.skin.influences() <= gpuSkinInfluences; }
    // Bumps whenever a cached character is dropped (forgetCharacter): GameWorld rebuilds its GPU bind meshes / skin textures.
    uint64_t characterGeneration() const { return m_charGeneration; }
    // Skinned mesh for an entity (empty key if the entity has no animated character).
    Skinned skinned(EntityId e) const;
    // Loads (and caches) a character: "builtin:mannequin" or a .fchar under content:/. nullptr if missing or
    // malformed (see characterError). A character with no mesh (animation only) is valid: it animates, draws nothing.
    const anim::Character* character(const std::string& ref);
    // As character(), but a previous failed load is retried (the file may have been imported / fixed since).
    const anim::Character* reloadCharacter(const std::string& ref);
    // 1.3.0: drops a cached character even when it loaded (the .fchar changed on disk), so every Animator using it reloads
    // clips and skeleton on its next update. ref may be content-relative or "content:/..."; returns the cache entries dropped.
    int forgetCharacter(const std::string& ref);
    void forgetAllCharacters();  // 1.3.2: every cached character (GameWorld::invalidateAll)
    // Why character(ref) returned nullptr ("not found", a parse/validation error), or "" if it loaded.
    std::string characterError(const std::string& ref) const {
        auto it = m_charErrors.find(ref);
        return it == m_charErrors.end() ? std::string() : it->second;
    }
    const Vfs& vfs() const { return m_vfs; }
    void clear() { m_entities.clear(); }
    struct Stats {
        int animators = 0, updated = 0, skipped = 0, crowdAgents = 0, crowdMeshes = 0, skinnedMeshes = 0, gpuSkinned = 0;
        int followers = 0;  // 1.3.2: Animators that used a leader's pose (no state machine evaluation of their own)
        double ms = 0;
    };
    const Stats& stats() const { return m_stats; }
    // The live animator of an entity (editor preview / tests).
    anim::Animator* animator(EntityId e);
    int lodInterval(float distance) const { return distance < 30 ? 1 : distance < 60 ? 2 : 4; }
    // Editor clip preview: the entity shows `clip` at `time` instead of its state machine (empty clip = off).
    void setClipPreview(EntityId e, const std::string& clip, float time) { m_preview = {e, clip, time}; }
    struct ClipPreview {
        EntityId entity = kNullEntity;
        std::string clip;
        float time = 0;
    };
    // Ragdolls: model-space bone matrices that replace the state machine pose while playing (nullptr = none).
    std::function<const std::vector<Mat4>*(EntityId)> poseOverride;
    // 1.3.0 extension point (Fledge Advanced: runtime two-bone IK, docs/ADVANCED_GAMEPLAY.md): called while playing for every
    // evaluated Animator after the state machine, foot IK and look-at, before the model-space pose is built. May edit the
    // local pose; `entityWorld` is the entity's world matrix. Empty in Fledge (nothing calls it there).
    std::function<void(EntityId, const anim::Character&, anim::Pose&, const Mat4& entityWorld)> posePostProcess;
    // Model-space bone matrices of an entity's current pose (editor skeleton view, ragdoll start, bone sockets). Filled on
    // every pose update on all paths (CPU skinning, GPU skinning, crowds, ragdolls, clip preview, edit-mode idle frame).
    std::vector<Mat4> boneMatrices(EntityId e) const;
    // Model-space matrices of the animated pose, before a ragdoll override replaces what is drawn. Null until the first
    // evaluation. Active ragdolls (1.3.5) track this; boneMatrices() is the pose on screen.
    const std::vector<Mat4>* animatedPose(EntityId e) const;
    // Bone sockets (Transform.attachBone) and the Fledge Script bone queries: model-space matrix of `bone` of the Animator on
    // `owner`. Before the entity's first update the character's rest pose is used. nullopt (warned once per entity + bone)
    // when the entity has no Animator or the skeleton has no such bone; nullopt (silent) while the character is missing.
    std::optional<Mat4> socketBone(Scene& scene, EntityId owner, const std::string& bone);
    // Bone names of the character on an entity's Animator (empty when none / not loaded).
    std::vector<std::string> boneNames(Scene& scene, EntityId e);
    // 1.3.2 leader pose report for tools (get_animation_info): {active, leader, mode "same rig" | "by name", mapped, bones,
    // unmapped (first 32 names), unmappedCount, note}.
    Json leaderInfo(Scene& scene, EntityId e) const;

private:
    struct Entry {
        anim::Animator animator;
        std::string charRef;
        uint64_t graphHash = 0;
        anim::Pose pose;
        std::vector<Mat4> model;      // model-space bone matrices of the pose on screen
        std::vector<Mat4> animated;   // model-space pose from the clips, before a ragdoll override
        MeshData skinnedMesh;
        std::vector<Mat4> skinMats;  // GPU path
        const anim::Character* character = nullptr;
        uint64_t version = 0;
        float pendingDt = 0;
        int frame = 0;
        bool played = false;  // the state machine has run since edit mode (play / preview)
        std::string crowdKey;
        uint64_t layerWeightMask = 0;  // layers whose weight AnimatorComponent.layerWeights set (reset to the graph when removed)
        // 1.3.2 leader pose (Fledge Advanced): follower bone -> leader bone (-1 = not in the leader: rest pose under its parent)
        std::vector<int> leaderMap;
        const anim::Character* mapLeader = nullptr;
        const anim::Character* mapSelf = nullptr;
        int mapped = 0;
        bool sameRig = false;
        std::vector<std::string> unmapped;  // follower bones the leader does not have (reported)
        uint64_t followFrame = 0;           // the update (m_frame) this follower last took its leader's pose in
    };
    // Copies the leader's evaluated model-space pose onto a follower and skins it; false while the leader is not ready yet.
    bool followLeader(Scene& scene, EntityId e, Entry& en, AnimatorComponent& ac, bool final);
    std::set<std::string> m_leaderWarned;  // leaderPose ignored here (Fledge) / mismatches, once each
    void applyLayerControls(Entry& en, AnimatorComponent& ac);  // layerPlay / layerSpeeds / layerWeights (1.3.2)
    void writeLayerState(const Entry& en, AnimatorComponent& ac) const;
    uint64_t m_charGeneration = 0;
    Vfs& m_vfs;
    std::unordered_map<std::string, std::unique_ptr<anim::Character>> m_chars;
    std::unordered_map<std::string, std::string> m_charErrors;
    std::unordered_map<EntityId, Entry> m_entities;
    std::map<std::string, std::pair<MeshData, uint64_t>> m_crowd;
    std::unordered_map<const anim::Character*, std::map<std::string, const anim::Clip*>> m_clipMaps;
    Stats m_stats;
    uint64_t m_frame = 0;
    ClipPreview m_preview;
    std::set<std::pair<EntityId, std::string>> m_socketWarned;  // unresolved sockets, warned once each
    bool m_catchUp = false;
    std::set<std::string> m_skinWarned;  // characters warned about (too many bones for the GPU path), once each
    void warnCpuSkinning(const anim::Character& ch, const std::string& ref);
};

} // namespace fledge
