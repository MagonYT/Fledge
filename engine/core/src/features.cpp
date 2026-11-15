#include "fledge/core/features.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <map>

#include "fledge/core/utility.h"

namespace fledge {

const char* editionName(Edition e) { return e == Edition::Advanced ? "Fledge Advanced" : "Fledge"; }
const char* editionId(Edition e) { return e == Edition::Advanced ? "advanced" : "standard"; }
const char* editionUserDataFolder(Edition e) { return e == Edition::Advanced ? "Fledge Advanced" : "Fledge"; }

std::optional<Edition> parseEdition(std::string_view s) {
    std::string l = toLower(s);
    if (l == "standard" || l == "fledge") return Edition::Standard;
    if (l == "advanced" || l == "fledge advanced" || l == "fledge_advanced") return Edition::Advanced;
    return std::nullopt;
}

namespace {
std::atomic<int> g_edition{static_cast<int>(Edition::Standard)};

FeatureInfo std1(const char* id, const char* title, const char* summary, const char* module) {
    return {id, 1, Edition::Standard, title, summary, module, FeatureStatus::Available, ""};
}
FeatureInfo planned(const char* id, Edition e, const char* title, const char* summary, const char* module, const char* release) {
    return {id, 1, e, title, summary, module, FeatureStatus::Planned, release};
}
// An implemented Advanced feature: available in Fledge Advanced (its module provides it), "needs Fledge Advanced" elsewhere.
FeatureInfo adv1(const char* id, const char* title, const char* summary, const char* module) {
    return {id, 1, Edition::Advanced, title, summary, module, FeatureStatus::Available, ""};
}

// Everything this source tree knows about. Standard features are provided by every executable; Advanced entries are
// provided only when their module is linked AND implemented (weapons, ballistics and movement since 1.3.0; active ragdoll
// since 1.3.5; docs/ROADMAP.md).
std::vector<FeatureInfo> buildCatalog() {
    return {
        // ---- Standard (Fledge) ---------------------------------------------------------------------------------
        std1("core.scene", "Scenes and components", "ECS, .fscene, hierarchy, prefabs-free scene editing", "fledge_scene"),
        std1("render.pbr", "PBR rendering", "PBR, IBL, cascaded + spot shadows, bloom, tonemapping, TAA / upscalers", "fledge_render"),
        std1("render.ssao", "Screen-space ambient occlusion", "SSAO from a depth prepass, applied to ambient light", "fledge_render"),
        std1("render.ssr", "Screen-space reflections", "Reflections traced against the depth prepass, previous-frame colour", "fledge_render"),
        std1("render.point_shadows", "Point-light shadows", "Six-face shadow maps for up to two point lights", "fledge_render"),
        std1("render.wet_surfaces", "Wet surfaces", "Rain-driven darkening, gloss and puddles tied to the weather", "fledge_render"),
        std1("render.volumetric_light", "Volumetric lighting", "Sun light shafts through height fog with cascade shadows", "fledge_render"),
        std1("render.auto_exposure", "Auto-exposure", "Average log-luminance with temporal adaptation", "fledge_render"),
        std1("render.dof", "Depth of field", "Gather bokeh from a circle of confusion", "fledge_render"),
        std1("render.motion_blur", "Motion blur", "Per-pixel blur along the motion vectors", "fledge_render"),
        std1("render.clustered_lights", "Clustered lights", "The 8 shader lights are the sun plus the local lights that overlap this view", "fledge_render"),
        adv1("advanced.decals", "Projected decals", "Box-projected decals on the scene", "fledge_render"),
        std1("render.gi_probes", "GI probes / light volumes", "Baked irradiance volume sampled into PBR ambient", "fledge_render"),
        planned("render.volumetric_clouds", Edition::Standard, "Volumetric clouds", "Ray-marched cloud layer in the sky", "fledge_render", "1.3.0"),
        std1("tools.blueprint_converter", "Blueprint converter", "Unreal Blueprint (T3D text) to Fledge Script C++ with a conversion report", "fledge_blueprint"),
        std1("physics", "Physics", "Jolt rigid bodies, characters, vehicles, ragdolls, joints", "fledge_physics"),
        std1("animation", "Animation", "Skeletal animation, blend trees, state machines, IK, crowds", "fledge_anim"),
        std1("audio", "Audio", "3D sources, buses, streaming, reverb zones, occlusion", "fledge_audio"),
        std1("gameplay.city", "City gameplay", "Player, driving, traffic, police, missions, weapons-lite, day/night, weather", "fledge_gameplay"),
        std1("world.streaming", "World streaming", "Chunks, LOD, HLOD, foliage, culling", "fledge_world"),
        std1("net.basic", "Basic networking", "Client/server replication, prediction, interpolation", "fledge_net"),
        std1("scripting", "Fledge Script", "C++ gameplay scripts with hot reload", "fledge_scripting"),
        std1("ui.game", "Game UI", "HUD toolkit, retained UI screens, fonts, localisation", "fledge_framework"),
        std1("assets.store_import", "Asset store import", "Unity packages, Fab downloads, FBX / glTF import", "fledge_importers"),
        std1("mcp.core", "Core MCP tools", "Scene, assets, play, build, graphics, gameplay authoring tools", "fledge_editor_core"),
        // ---- Advanced (Fledge Advanced): implemented in 1.3.0 (provided only where the module is linked) -------
        adv1("advanced.weapons", "Weapon system", "First-person viewmodel, layered animation, recoil, attachments, reloads, sway", "fledge_adv_weapons"),
        adv1("advanced.ballistics", "Ballistics", "Penetration, material hit effects, hit markers", "fledge_adv_ballistics"),
        adv1("advanced.movement", "Fluid movement", "Slide, mantle, vault", "fledge_adv_movement"),
        adv1("advanced.active_ragdoll", "Active ragdoll", "Euphoria-style balance and protective reactions", "fledge_adv_active_ragdoll"),
        // ---- Advanced, roadmap 1.4.0 and later: modules are scaffolds ------------------------------------------
        planned("advanced.netcode", Edition::Advanced, "Advanced netcode", "Lag compensation, dedicated server tools, snapshot tuning",
                "fledge_adv_netcode", "1.4.0"),
        planned("advanced.spectator", Edition::Advanced, "Killcams and spectator", "Killcams, spectator cameras, bot extras",
                "fledge_adv_spectator", "1.4.0"),
        planned("advanced.vehicle_damage", Edition::Advanced, "Vehicle damage", "Deformation, detachable parts, engine damage",
                "fledge_adv_vehicle_damage", "1.4.0"),
        planned("advanced.pedestrian_life", Edition::Advanced, "Pedestrian life", "Daily routines, schedules, reactions",
                "fledge_adv_pedestrian_life", "1.5.0"),
        planned("advanced.interiors", Edition::Advanced, "Interiors", "Enterable interiors with portals and streaming",
                "fledge_adv_interiors", "1.5.0"),
        planned("advanced.city_services", Edition::Advanced, "Radio, phone and shops", "In-game radio stations, phone, shops",
                "fledge_adv_city_services", "1.5.0"),
        planned("advanced.replay_editor", Edition::Advanced, "Replay editor", "Free camera, cuts, export", "fledge_adv_replay", "1.5.0"),
    };
}
} // namespace

Edition currentEdition() { return static_cast<Edition>(g_edition.load()); }
void setCurrentEdition(Edition e) { g_edition = static_cast<int>(e); }

Json FeatureInfo::toJson() const {
    Json j = {{"id", id}, {"version", version}, {"edition", editionId(edition)}, {"title", title}, {"summary", summary},
              {"module", module}, {"status", status == FeatureStatus::Available ? "available" : "planned"}};
    if (!plannedFor.empty()) j["plannedFor"] = plannedFor;
    return j;
}

Json FeatureRequirement::toJson() const {
    Json j = {{"id", id}, {"version", version}};
    if (!title.empty()) j["title"] = title;
    j["edition"] = editionId(edition);
    return j;
}

std::optional<FeatureRequirement> FeatureRequirement::fromJson(const Json& j) {
    FeatureRequirement r;
    if (j.is_string()) {
        r.id = j.get<std::string>();
    } else if (j.is_object() && j.contains("id") && j["id"].is_string()) {
        r.id = j["id"].get<std::string>();
        if (j.contains("version")) {
            if (!j["version"].is_number_integer()) return std::nullopt;
            r.version = j["version"].get<int>();
        }
        if (j.contains("title") && j["title"].is_string()) r.title = j["title"].get<std::string>();
        if (j.contains("edition") && j["edition"].is_string())
            if (auto e = parseEdition(j["edition"].get<std::string>())) r.edition = *e;
    } else {
        return std::nullopt;
    }
    if (r.id.empty() || r.version < 1) return std::nullopt;
    if (startsWith(r.id, "advanced.")) r.edition = Edition::Advanced;
    return r;
}

Json ModuleInfo::toJson() const {
    return {{"id", id}, {"title", title}, {"edition", editionId(edition)}, {"status", status}, {"features", features}};
}

FeatureRegistry& FeatureRegistry::get() {
    static FeatureRegistry r;
    return r;
}

FeatureRegistry::FeatureRegistry() : m_catalog(buildCatalog()) {
    for (const auto& f : m_catalog)
        if (f.status == FeatureStatus::Available && f.edition == Edition::Standard) m_provided.push_back(f.id);
    // MCP tools of the implemented Advanced features (1.3.0): known everywhere, so Fledge answers them with a structured
    // "requires Fledge Advanced" error instead of "unknown tool" (Fledge Advanced registers the real tools)
    for (const char* t : {"create_weapon", "describe_weapon", "validate_weapon_definition", "simulate_weapon", "weapon_command", "get_weapon_status"})
        m_tools.emplace_back(t, "advanced.weapons");
    for (const char* t : {"ballistic_trace", "get_surface_materials", "get_ballistics_status"}) m_tools.emplace_back(t, "advanced.ballistics");
    for (const char* t : {"movement_probe", "simulate_movement", "get_movement_status"}) m_tools.emplace_back(t, "advanced.movement");
}

void FeatureRegistry::provide(FeatureInfo f) {
    f.status = FeatureStatus::Available;
    auto it = std::find_if(m_catalog.begin(), m_catalog.end(), [&](const FeatureInfo& c) { return c.id == f.id; });
    if (it != m_catalog.end()) *it = f;
    else m_catalog.push_back(f);
    if (std::find(m_provided.begin(), m_provided.end(), f.id) == m_provided.end()) m_provided.push_back(f.id);
}

void FeatureRegistry::addModule(ModuleInfo m) {
    for (auto& x : m_modules)
        if (x.id == m.id) {
            x = std::move(m);
            return;
        }
    m_modules.push_back(std::move(m));
}

void FeatureRegistry::declareTool(const std::string& tool, const std::string& featureId) {
    for (auto& [t, f] : m_tools)
        if (t == tool) {
            f = featureId;
            return;
        }
    m_tools.emplace_back(tool, featureId);
}

const FeatureInfo* FeatureRegistry::known(std::string_view id) const {
    for (const auto& f : m_catalog)
        if (f.id == id) return &f;
    return nullptr;
}

const FeatureInfo* FeatureRegistry::provided(std::string_view id) const {
    if (std::find(m_provided.begin(), m_provided.end(), id) == m_provided.end()) return nullptr;
    return known(id);
}

int FeatureRegistry::providedVersion(std::string_view id) const {
    const FeatureInfo* f = provided(id);
    return f ? f->version : 0;
}

std::vector<const FeatureInfo*> FeatureRegistry::providedFeatures() const {
    std::vector<const FeatureInfo*> out;
    for (const auto& f : m_catalog)
        if (provided(f.id)) out.push_back(&f);
    return out;
}

std::vector<const FeatureInfo*> FeatureRegistry::unavailableFeatures() const {
    std::vector<const FeatureInfo*> out;
    for (const auto& f : m_catalog)
        if (!provided(f.id)) out.push_back(&f);
    return out;
}

std::string FeatureRegistry::toolFeature(const std::string& tool) const {
    for (const auto& [t, f] : m_tools)
        if (t == tool) return f;
    return {};
}

Json FeatureRegistry::describe() const {
    Edition e = currentEdition();
    Json features = Json::array(), unavailable = Json::array(), modules = Json::array();
    for (const FeatureInfo* f : providedFeatures()) features.push_back(f->toJson());
    for (const FeatureInfo* f : unavailableFeatures()) {
        Json j = f->toJson();
        j["why"] = f->status == FeatureStatus::Planned
                       ? std::format("planned for {} ({})", f->plannedFor.empty() ? "a later release" : f->plannedFor,
                                     f->edition == Edition::Advanced ? "Fledge Advanced" : "every edition")
                       : std::string("needs Fledge Advanced");
        unavailable.push_back(j);
    }
    for (const auto& m : m_modules) modules.push_back(m.toJson());
    return {{"edition", editionId(e)}, {"editionName", editionName(e)}, {"features", features}, {"unavailable", unavailable},
            {"modules", modules}};
}

Json MissingFeature::toJson() const {
    return {{"id", required.id}, {"version", required.version}, {"title", title}, {"edition", editionId(edition)},
            {"providedVersion", providedVersion}, {"reason", reason}};
}

static Edition requirementEdition(const FeatureRequirement& r, const FeatureRegistry& reg) {
    if (r.edition == Edition::Advanced || startsWith(r.id, "advanced.")) return Edition::Advanced;
    if (const FeatureInfo* k = reg.known(r.id)) return k->edition;
    return r.edition;
}

std::vector<MissingFeature> missingFeatures(const std::vector<FeatureRequirement>& required, const FeatureRegistry& reg) {
    std::vector<MissingFeature> out;
    for (const auto& r : required) {
        int have = reg.providedVersion(r.id);
        if (have >= r.version) continue;
        MissingFeature m;
        m.required = r;
        m.providedVersion = have;
        const FeatureInfo* k = reg.known(r.id);
        m.title = !r.title.empty() ? r.title : k ? k->title : r.id;
        m.edition = requirementEdition(r, reg);
        if (have > 0) {
            m.reason = std::format("needs a newer version of {} (this one has v{}, the project needs v{})", editionName(m.edition), have, r.version);
        } else if (m.edition == Edition::Advanced && currentEdition() == Edition::Standard) {
            m.reason = "needs Fledge Advanced";
        } else if (k && k->status == FeatureStatus::Planned) {
            m.reason = std::format("not available in this release of {} (planned for {})", editionName(currentEdition()),
                                   k->plannedFor.empty() ? "a later release" : k->plannedFor);
        } else if (k) {  // implemented, but its module is not linked into this executable
            m.reason = std::format("not provided by this executable ({} is not linked)", k->module.empty() ? k->id : k->module);
        } else {
            m.reason = std::format("made with a newer {} (unknown feature '{}')", editionName(m.edition), r.id);
        }
        out.push_back(std::move(m));
    }
    return out;
}

Edition minimumEdition(const std::vector<FeatureRequirement>& required, const FeatureRegistry& reg) {
    for (const auto& r : required)
        if (requirementEdition(r, reg) == Edition::Advanced) return Edition::Advanced;
    return Edition::Standard;
}

std::vector<FeatureRequirement> mergeRequirements(const std::vector<FeatureRequirement>& a, const std::vector<FeatureRequirement>& b) {
    std::map<std::string, FeatureRequirement> m;
    for (const auto* list : {&a, &b})
        for (const auto& r : *list) {
            auto [it, inserted] = m.emplace(r.id, r);
            if (!inserted && r.version > it->second.version) it->second = r;
            if (!inserted && it->second.title.empty()) it->second.title = r.title;
        }
    std::vector<FeatureRequirement> out;
    for (auto& [id, r] : m) out.push_back(r);
    return out;
}

FeatureRequirement requirementFor(const FeatureRegistry& reg, const std::string& id, int version) {
    FeatureRequirement r;
    r.id = id;
    const FeatureInfo* k = reg.known(id);
    r.version = version > 0 ? version : k ? k->version : 1;
    r.title = k ? k->title : id;
    r.edition = k ? k->edition : startsWith(id, "advanced.") ? Edition::Advanced : Edition::Standard;
    return r;
}

} // namespace fledge
