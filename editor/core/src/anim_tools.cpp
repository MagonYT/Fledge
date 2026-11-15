// MCP tools for animation (docs/ANIMATION.md). Mutating tools run in one AI-tagged undo transaction.
#include <algorithm>

#include "fledge/anim/anim.h"
#include "fledge/editor/editor_session.h"
#include "fledge/render/renderer.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge {
using mcp::ToolResult;

namespace {
Json sch(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
EntityId ref(EditorSession& s, const Json& r) {
    if (r.is_number_integer()) return s.scene().isValid(r.get<EntityId>()) ? r.get<EntityId>() : kNullEntity;
    if (r.is_string()) return s.scene().findByName(r.get<std::string>());
    return kNullEntity;
}
const Json kAny = {{"description", "entity id or name"}};
} // namespace

void registerAnimTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"create_character", "Create animated character",
             "Creates an entity with an Animator (default: the built-in mannequin with Idle / Walk / Run / Jump / Wave and a locomotion state "
             "machine driven by the 'speed' param and 'jump' / 'wave' triggers). Options: position, character (.fchar asset), crowd (shared "
             "instanced poses), params, color, footIK, rootMotion.",
             sch({{"name", {{"type", "string"}}}, {"position", {{"type", "array"}}}, {"character", {{"type", "string"}}}, {"crowd", {{"type", "boolean"}}},
                  {"params", {{"type", "object"}}}, {"color", {{"type", "array"}}}, {"footIK", {{"type", "boolean"}}}, {"rootMotion", {{"type", "boolean"}}}}),
             false, false, [&s](const Json& a) {
                 if (a.contains("character")) {  // load + validate first: a bad asset is an error, not an entity that never draws
                     if (!a["character"].is_string() || a["character"].get<std::string>().empty())
                         return ToolResult::error("create_character: 'character' must be a .fchar asset path or builtin:mannequin");
                     std::string ref = a["character"].get<std::string>();
                     if (!s.world().animation().reloadCharacter(ref))
                         return ToolResult::error("create_character: character '" + ref + "' could not be loaded: " + s.world().animation().characterError(ref));
                 }
                 Json an = Json::object();
                 for (const char* k : {"character", "crowd", "params", "color", "footIK", "rootMotion"})
                     if (a.contains(k)) an[k] = a[k];
                 Json comps = {{"Animator", an}};
                 if (a.contains("position")) comps["Transform"] = {{"position", a["position"]}};
                 EntityId e = s.createEntity(a.value("name", std::string("Character")), kNullEntity, comps);
                 if (!e) return ToolResult::error("create_character failed (check the character asset path)");
                 return ToolResult::json({{"id", e}});
             }});
    reg.add({"set_animator", "Drive an Animator",
             "Sets Animator params ({\"speed\": 4}), triggers ({\"jump\": true}), a state request (play + crossFade; 1.3.2: on any layer with "
             "layer = name or index, playSpeed = that layer's clock while it stays in the state), lookAt, speed, and the runtime layer values "
             "layerSpeeds / layerWeights ({\"UpperBody\": 1.5}; merged into the Animator, null removes a key: speed 1 / the graph's weight). "
             "Unknown layers / states are an error. Undoable.",
             sch({{"entity", kAny}, {"params", {{"type", "object"}}}, {"play", {{"type", "string"}}}, {"crossFade", {{"type", "number"}}},
                  {"lookAt", {{"type", "string"}}}, {"speed", {{"type", "number"}}},
                  {"layer", {{"description", "layer name or index for play (default 0, the base layer)"}}}, {"playSpeed", {{"type", "number"}}},
                  {"layerSpeeds", {{"type", "object"}}}, {"layerWeights", {{"type", "object"}}}},
                 {"entity"}),
             false, false, [&s](const Json& a) {
                 EntityId e = ref(s, a["entity"]);
                 auto* ac = e ? s.scene().get<AnimatorComponent>(e) : nullptr;
                 if (!ac) return ToolResult::error("no Animator on " + a["entity"].dump());
                 // the graph the Animator runs (its own or the default locomotion) names the layers / states to check against
                 anim::AnimGraph g = anim::AnimGraph::fromJson(ac->graph.is_object() && !ac->graph.empty() ? ac->graph : anim::AnimGraph::defaultLocomotion());
                 auto layerOf = [&](const Json& k) -> int {
                     if (k.is_number_integer()) return k.get<int>() >= 0 && k.get<int>() < static_cast<int>(g.layers.size()) ? k.get<int>() : -1;
                     if (!k.is_string()) return -1;
                     for (size_t i = 0; i < g.layers.size(); ++i)
                         if (g.layers[i].name == k.get<std::string>()) return static_cast<int>(i);
                     const std::string& t = k.get_ref<const std::string&>();
                     if (!t.empty() && t.size() <= 3 && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
                         return std::stoi(t) < static_cast<int>(g.layers.size()) ? std::stoi(t) : -1;
                     return -1;
                 };
                 auto layerNames = [&] {
                     std::string n;
                     for (auto& l : g.layers) n += (n.empty() ? "" : ", ") + l.name;
                     return n.empty() ? std::string("none") : n;
                 };
                 Json c = Json::object();
                 for (const char* k : {"params", "crossFade", "lookAt", "speed"})
                     if (a.contains(k)) c[k] = a[k];
                 if (a.contains("play")) {
                     Json layer = a.value("layer", Json(0));
                     int li = layerOf(layer);
                     if (li < 0) return ToolResult::error("set_animator: unknown layer " + layer.dump() + " (layers: " + layerNames() + ")");
                     if (g.layers[static_cast<size_t>(li)].find(a["play"].get<std::string>()) < 0)
                         return ToolResult::error("set_animator: layer '" + g.layers[static_cast<size_t>(li)].name + "' has no state '" + a["play"].get<std::string>() + "'");
                     if (li == 0 && !a.contains("playSpeed")) {
                         c["play"] = a["play"];  // the 1.3.0 request field (base layer)
                     } else {
                         Json req = {{"layer", li}, {"state", a["play"]}, {"fade", a.value("crossFade", ac->crossFade)}};
                         if (a.contains("playSpeed")) req["speed"] = a["playSpeed"];
                         Json list = ac->layerPlay.is_array() ? ac->layerPlay : Json::array();
                         list.push_back(req);
                         c["layerPlay"] = list;
                     }
                 }
                 for (const char* field : {"layerSpeeds", "layerWeights"}) {
                     if (!a.contains(field)) continue;
                     Json merged = field == std::string("layerSpeeds") ? ac->layerSpeeds : ac->layerWeights;
                     if (!merged.is_object()) merged = Json::object();
                     for (auto& [k, v] : a[field].items()) {
                         int li = layerOf(Json(k));
                         if (li < 0) return ToolResult::error(std::string("set_animator: ") + field + ": unknown layer '" + k + "' (layers: " + layerNames() + ")");
                         if (field == std::string("layerWeights") && li == 0) return ToolResult::error("set_animator: the base layer (0) always has weight 1");
                         if (v.is_null()) merged.erase(k);
                         else if (!v.is_number()) return ToolResult::error(std::string("set_animator: ") + field + "." + k + " must be a number");
                         else merged[k] = v;
                     }
                     c[field] = merged;
                 }
                 if (auto st = s.applyComponents(e, {{"Animator", c}}, "Animator"); !st) return ToolResult::error(st.error);
                 Json out = s.scene().entityToJson(e)["components"]["Animator"];
                 Json names = Json::array();
                 for (auto& l : g.layers) names.push_back(l.name);
                 out["layerNames"] = names;
                 return ToolResult::json(out);
             }});
    reg.add({"get_animation_info", "Animation info",
             "Character (bones, clips with durations and events), the state machine (params, states, transitions), the live state of an Animator "
             "and skinning {path gpu|cpu|none, reason, bones, gpuLimit, editionLimit, edition, backend, note}: where it is skinned while playing.",
             sch({{"entity", kAny}}, {"entity"}), true, false, [&s](const Json& a) {
                 EntityId e = ref(s, a["entity"]);
                 auto* ac = e ? s.scene().get<AnimatorComponent>(e) : nullptr;
                 if (!ac) return ToolResult::error("no Animator on " + a["entity"].dump());
                 const anim::Character* ch = s.world().animation().character(ac->character);
                 Json j = {{"character", ac->character}, {"state", ac->state}};
                 if (ch) {
                     Json bones = Json::array(), clips = Json::array();
                     for (auto& b : ch->skeleton.bones) bones.push_back(b.name);
                     for (auto& c : ch->clips) {
                         Json ev = Json::array();
                         for (auto& x : c.events) ev.push_back({{"time", x.time}, {"name", x.name}});
                         clips.push_back({{"name", c.name}, {"duration", c.duration}, {"loop", c.loop}, {"events", ev}});
                     }
                     j["bones"] = bones;
                     j["clips"] = clips;
                     j["vertices"] = ch->mesh.vertices.size();  // 0 = animation-only character (nothing is drawn)
                 } else {
                     j["error"] = s.world().animation().characterError(ac->character);
                 }
                 j["graph"] = ac->graph.empty() ? anim::AnimGraph::defaultLocomotion() : ac->graph;
                 j["skinning"] = s.world().skinningInfo(e);  // 1.3.1: GPU or CPU skinning while playing, and why
                 // 1.3.2: per-layer live state (index, name, state, time, weight, speed, inTransition; written by each update)
                 // and the runtime layer values the Animator carries
                 j["layers"] = ac->layers;
                 j["layerSpeeds"] = ac->layerSpeeds;
                 j["layerWeights"] = ac->layerWeights;
                 if (!ac->layerError.empty()) j["layerError"] = ac->layerError;
                 if (ch) j["influences"] = ch->skin.influences();
                 // 1.3.2 Fledge Advanced: material slots (effective material per slot) and the leader pose
                 if (ch) {
                     Json slots = Json::array();
                     for (size_t k = 0; k < ch->submeshes.size(); ++k) {
                         const auto& sm = ch->submeshes[k];
                         std::string over = ac->materials.is_array() && k < ac->materials.size() && ac->materials[k].is_string() ? ac->materials[k].get<std::string>() : "";
                         slots.push_back({{"index", k}, {"name", sm.name}, {"fileMaterial", sm.material}, {"override", over},
                                          {"material", !over.empty() ? over : !sm.material.empty() ? sm.material : ac->material},
                                          {"triangles", sm.indexCount / 3}});
                     }
                     j["materialSlots"] = slots;
                     j["materialSlotsDrawn"] = ch->submeshes.size() >= 2 && advancedRenderFeatures();
                     if (ch->submeshes.size() >= 2 && !advancedRenderFeatures()) j["materialSlotsNote"] = advancedRenderNote("Multi-material characters");
                 }
                 j["leaderPose"] = s.world().animation().leaderInfo(s.scene(), e);
                 return ToolResult::json(j);
             }});
    reg.add({"set_anim_graph", "Set state machine",
             "Replaces an Animator's state machine. Format: {params:[{name,type:float|bool|trigger}], layers:[{name, default, states:[{name, clip} | "
             "{name, blend1d:{param, children:[{clip,pos}]}} | {name, blend2d:{x,y,children:[{clip,x,y}]}}], transitions:[{from ('*' = any), to, "
             "duration, exitTime, curve, conditions:[{param, op: > < >= <= == != true false trigger, value}]}]}], events:{clip:[{time,name}]}}",
             sch({{"entity", kAny}, {"graph", {{"type", "object"}}}}, {"entity", "graph"}), false, false, [&s](const Json& a) {
                 EntityId e = ref(s, a["entity"]);
                 if (!e || !s.scene().get<AnimatorComponent>(e)) return ToolResult::error("no Animator on " + a["entity"].dump());
                 anim::AnimGraph g = anim::AnimGraph::fromJson(a["graph"]);
                 if (g.layers.empty() || g.layers[0].states.empty()) return ToolResult::error("graph has no states");
                 if (auto st = s.setProperty(e, "Animator", "graph", a["graph"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"states", g.layers[0].states.size()}, {"transitions", g.layers[0].transitions.size()}});
             }});
}

} // namespace fledge
