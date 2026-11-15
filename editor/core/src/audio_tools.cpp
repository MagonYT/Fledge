// MCP tools for audio (docs/AUDIO.md). Mutating tools run in one AI-tagged undo transaction (callWrapper).
#include "fledge/editor/editor_session.h"
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
} // namespace

void registerAudioTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"add_audio_source", "Add / set a sound",
             "Adds (or updates) an AudioSource on an entity. clip: an Audio asset (GUID / path of a WAV, MP3, FLAC or Ogg Vorbis) or builtin:beep | "
             "builtin:click | builtin:noise | builtin:wind. bus: Music | Sfx | Ambient | Voice | UI. spatial (3D, default true), loop, "
             "volume, pitch, minDistance, maxDistance, stream (long music), playOnStart. One undo step.",
             sch({{"entity", {{"description", "entity id or name"}}}, {"clip", {{"type", "string"}}}, {"bus", {{"type", "string"}}},
                  {"spatial", {{"type", "boolean"}}}, {"loop", {{"type", "boolean"}}}, {"volume", {{"type", "number"}}}, {"pitch", {{"type", "number"}}},
                  {"minDistance", {{"type", "number"}}}, {"maxDistance", {{"type", "number"}}}, {"stream", {{"type", "boolean"}}},
                  {"playOnStart", {{"type", "boolean"}}}},
                 {"entity", "clip"}),
             false, false, [&s](const Json& a) {
                 EntityId e = ref(s, a["entity"]);
                 if (!e) return ToolResult::error("no entity " + a["entity"].dump());
                 std::string clip = a["clip"].get<std::string>();
                 if (!s.world().audioClip(clip)) return ToolResult::error("cannot load clip '" + clip + "' (WAV / MP3 / FLAC / Ogg asset or builtin:beep|click|noise|wind)");
                 Json c = Json::object();
                 for (const char* k : {"clip", "bus", "spatial", "loop", "volume", "pitch", "minDistance", "maxDistance", "stream", "playOnStart"})
                     if (a.contains(k)) c[k] = a[k];
                 if (auto st = s.applyComponents(e, {{"AudioSource", c}}, "Add sound"); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e)["components"]["AudioSource"]);
             }});
    reg.add({"set_mixer", "Set mixer",
             "Sets bus volumes (0..2: master, music, sfx, ambient, voice, ui) and ducking (duckMusicUnderVoice, duckDb) on the scene's "
             "AudioMixer, creating it if needed. One undo step.",
             sch({{"master", {{"type", "number"}}}, {"music", {{"type", "number"}}}, {"sfx", {{"type", "number"}}}, {"ambient", {{"type", "number"}}},
                  {"voice", {{"type", "number"}}}, {"ui", {{"type", "number"}}}, {"duckMusicUnderVoice", {{"type", "boolean"}}}, {"duckDb", {{"type", "number"}}}}),
             false, false, [&s](const Json& a) {
                 EntityId m = kNullEntity;
                 for (EntityId e : s.scene().entities())
                     if (s.scene().get<AudioMixerComponent>(e)) m = e;
                 Json c = a.is_object() ? a : Json::object();
                 if (!m) m = s.createEntity("Audio Mixer", kNullEntity, {{"AudioMixer", c}});
                 else if (auto st = s.applyComponents(m, {{"AudioMixer", c}}, "Set mixer"); !st) return ToolResult::error(st.error);
                 if (!m) return ToolResult::error("cannot create the mixer");
                 return ToolResult::json(s.scene().entityToJson(m)["components"]["AudioMixer"]);
             }});
    reg.add({"get_audio_status", "Audio status",
             "Output device, live voices, bus volumes and peak levels, the ducking gain, the zone reverb, and every AudioSource (clip, bus, "
             "playing, occluded 0..1).",
             sch({}), true, false, [&s](const Json&) {
                 auto& mx = s.world().mixer();
                 Json buses = Json::object();
                 for (int b = 0; b < static_cast<int>(audio::Bus::Count); ++b) {
                     auto bus = static_cast<audio::Bus>(b);
                     buses[audio::busName(bus)] = {{"volume", mx.busVolume(bus)}, {"peak", mx.busPeak(bus)}, {"muted", mx.busMuted(bus)}};
                 }
                 Json sources = Json::array();
                 for (EntityId e : s.scene().entities())
                     if (auto* a = s.scene().get<AudioSourceComponent>(e))
                         sources.push_back({{"entity", e}, {"name", s.scene().displayName(e)}, {"clip", a->clip}, {"playing", a->playing},
                                            {"bus", s.scene().entityToJson(e)["components"]["AudioSource"]["bus"]}, {"occluded", a->occluded}});
                 const auto& st = s.world().audioStats();
                 audio::ReverbSettings rv = mx.reverb();
                 return ToolResult::json({{"device", s.world().audioDevice() ? s.world().audioDevice()->name() : "none"},
                                          {"voices", mx.voiceCount()},
                                          {"duckGain", mx.duckGain()},
                                          {"reverb", {{"enabled", rv.enabled}, {"roomSize", rv.roomSize}, {"wet", rv.wet}, {"zone", st.reverbZone}}},
                                          {"buses", buses},
                                          {"sources", sources},
                                          {"engineSounds", st.engines},
                                          {"streams", st.streams}});
             }});
}

} // namespace fledge
