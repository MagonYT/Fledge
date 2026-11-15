// GameWorld <-> audio glue (docs/AUDIO.md): AudioSource voices, vehicle engine sounds, the listener, the
// AudioMixer component (bus volumes, ducking), ReverbZones, occlusion rays, one-shots and editor previews.
#include <algorithm>
#include <cmath>

#include "fledge/core/log.h"
#include "fledge/framework/game_world.h"

namespace fledge {

namespace {
audio::Bus toBus(AudioBus b) { return static_cast<audio::Bus>(static_cast<int>(b) + 1); }
} // namespace

void GameWorld::openAudio(bool nullDevice) {
    if (m_audioDevice && m_audioDevice->isNull() == nullDevice) return;
    m_audioDevice.reset();
    std::string err;
    m_audioDevice = audio::AudioDevice::open(m_mixer, nullDevice, &err);
    if (!err.empty()) FLEDGE_LOG_WARN("audio", "{}", err);
    else FLEDGE_LOG_INFO("audio", "Audio output: {}", m_audioDevice->name());
}

std::shared_ptr<const audio::Clip> GameWorld::audioClip(const std::string& ref) {
    if (ref.empty()) return nullptr;
    if (auto it = m_clips.find(ref); it != m_clips.end()) return it->second;
    std::shared_ptr<const audio::Clip> clip;
    if (startsWith(ref, "builtin:")) {
        audio::Clip c = audio::builtinClip(ref, m_mixer.sampleRate());
        if (c.frames() > 0) clip = std::make_shared<const audio::Clip>(std::move(c));
    } else {
        std::string path = m_registry.resolve(ref);
        auto bytes = path.empty() ? std::nullopt : m_vfs.read(path);
        std::string err = path.empty() ? "not a registered asset" : "file not found";
        if (bytes) {
            if (auto c = audio::decodeClip(*bytes, &err)) clip = std::make_shared<const audio::Clip>(std::move(*c));
        }
        if (!clip) FLEDGE_LOG_WARN("audio", "Clip '{}': {}", ref, err);
    }
    m_clips[ref] = clip;  // failures cached too (no log spam)
    return clip;
}

bool GameWorld::previewClip(const std::string& ref) {
    if (!m_audioDevice) openAudio(true);
    stopPreview();
    auto clip = audioClip(ref);
    if (!clip) return false;
    audio::VoiceParams p;
    p.bus = audio::Bus::UI;
    m_preview = m_mixer.play(clip, p);
    return m_preview != 0;
}

void GameWorld::stopPreview() {
    if (m_preview) m_mixer.stop(m_preview);
    m_preview = 0;
}

void GameWorld::stopSceneAudio() {
    for (auto& [e, s] : m_sources)
        if (s.voice) m_mixer.stop(s.voice);
    for (auto& [e, s] : m_engines)
        if (s.voice) m_mixer.stop(s.voice);
    m_sources.clear();
    m_engines.clear();
    m_lastListener.reset();
    for (EntityId e : m_scene.entities())
        if (auto* a = m_scene.get<AudioSourceComponent>(e)) a->playing = false;
}

float GameWorld::occlusionBetween(Vec3 ears, Vec3 src, EntityId source, EntityId alsoClear) {
    if (!m_physics) return 0;
    Vec3 d = src - ears;
    float len = length(d);
    if (len < 0.5f) return 0;
    Vec3 dir = d / len;
    auto clear = [&](EntityId hit) {  // the source itself, its parents (a radio in a car) and the player's car do not count
        if (hit == alsoClear && alsoClear) return true;
        for (EntityId e = source; e; e = m_scene.parentOf(e))
            if (hit == e) return true;
        return false;
    };
    auto a = m_physics->raycast(ears, dir, len - 0.25f, source);
    if (!a || clear(a->entity)) return 0;
    auto b = m_physics->raycast(src, dir * -1.0f, len - 0.25f, source);  // from the other side: a second, different obstacle?
    bool second = b && !clear(b->entity) && b->entity != a->entity;
    return second ? 0.9f : 0.6f;
}

void GameWorld::updateAudio(float dt) {
    if (!m_audioDevice) openAudio(true);
    auto worlds = m_scene.computeWorldMatrices();
    float inv = dt > 1e-5f ? 1.0f / dt : 0.0f;
    EntityId playerCar = kNullEntity;  // the camera often sits behind it: never an occluder
    if (EntityId p = player())
        if (auto* pc = m_scene.get<PlayerControllerComponent>(p); pc && !pc->vehicle.empty()) playerCar = m_scene.findByName(pc->vehicle);
    // mixer settings
    for (EntityId e : m_scene.entities())
        if (auto* mx = m_scene.get<AudioMixerComponent>(e)) {
            m_mixer.setBusVolume(audio::Bus::Master, mx->master);
            m_mixer.setBusVolume(audio::Bus::Music, mx->music);
            m_mixer.setBusVolume(audio::Bus::Sfx, mx->sfx);
            m_mixer.setBusVolume(audio::Bus::Ambient, mx->ambient);
            m_mixer.setBusVolume(audio::Bus::Voice, mx->voice);
            m_mixer.setBusVolume(audio::Bus::UI, mx->ui);
            audio::DuckRule d = m_mixer.ducking();
            d.enabled = mx->duckMusicUnderVoice;
            d.amountDb = mx->duckDb;
            m_mixer.setDucking(d);
            break;
        }
    // listener: AudioListener > primary camera > editor viewer
    std::optional<Transform> ears;
    for (EntityId e : m_scene.entities())
        if (auto* l = m_scene.get<AudioListenerComponent>(e); l && l->active) {
            ears = decompose(worlds[e]);
            break;
        }
    if (!ears)
        for (EntityId e : m_scene.entities())
            if (auto* c = m_scene.get<CameraComponent>(e); c && c->primary) {
                ears = decompose(worlds[e]);
                break;
            }
    if (!ears && viewerOverride) ears = Transform{*viewerOverride, Quat{}, {1, 1, 1}};
    if (ears) {
        Vec3 vel = m_lastListener ? (ears->position - *m_lastListener) * inv : Vec3{0, 0, 0};
        if (length(vel) > 200) vel = {0, 0, 0};  // teleports / rebases are not motion
        m_mixer.setListener(ears->position, ears->rotation, vel);
        m_lastListener = ears->position;
    }
    // reverb: the zone the listener is deepest inside (fading in over `fade` m from its walls)
    audio::ReverbSettings reverb;
    float bestZone = 0;
    if (ears)
        for (EntityId e : m_scene.entities()) {
            auto* rz = m_scene.get<ReverbZoneComponent>(e);
            if (!rz) continue;
            Vec3 local = worlds[e].inverse().transformPoint(ears->position);  // entity scale applies to size
            float depth = 1e9f;
            for (int k = 0; k < 3; ++k) depth = std::min(depth, rz->size[k] * 0.5f - std::fabs(local[k]));
            if (depth <= 0) continue;
            float w = rz->fade > 0 ? std::min(1.0f, depth / rz->fade) : 1.0f;
            if (w <= bestZone) continue;
            static const char* kPresets[] = {"Room", "Hall", "Cave", "Tunnel", "Garage"};
            bestZone = w;
            reverb = audio::reverbPreset(kPresets[std::clamp(static_cast<int>(rz->preset), 0, 4)]);
            reverb.wet *= std::clamp(rz->mix, 0.0f, 2.0f) * w;
        }
    if (!(m_mixer.reverb() == reverb)) m_mixer.setReverb(reverb);
    m_audioStats = {};
    m_audioStats.reverbZone = bestZone;
    auto occlusionFor = [&](SourceState& st, Vec3 pos, EntityId e, bool enabled) {
        if (!enabled || !ears) {
            st.occlusion = 0;
            return;
        }
        st.occlusionTimer -= dt;
        if (st.occlusionTimer > 0) return;
        st.occlusionTimer = 0.1f;
        st.occlusion = occlusionBetween(ears->position, pos, e, playerCar);
    };
    if (m_playing) {
        std::vector<EntityId> finished;
        std::vector<EntityId> alive;
        for (EntityId e : m_scene.entities()) {
            auto* a = m_scene.get<AudioSourceComponent>(e);
            if (!a) continue;
            ++m_audioStats.sources;
            alive.push_back(e);
            SourceState& st = m_sources[e];
            Vec3 pos = worlds[e].c[3].xyz();
            Vec3 vel = st.hasLast ? (pos - st.lastPos) * inv : Vec3{0, 0, 0};
            if (length(vel) > 200) vel = {0, 0, 0};
            st.lastPos = pos;
            st.hasLast = true;
            audio::VoiceParams p;
            p.bus = toBus(a->bus);
            p.volume = a->volume;
            p.pitch = a->pitch;
            p.loop = a->loop;
            p.spatial = a->spatial;
            p.position = pos;
            p.velocity = vel;
            p.minDistance = a->minDistance;
            p.maxDistance = a->maxDistance;
            occlusionFor(st, pos, e, a->spatial && a->occlusion);
            p.occlusion = st.occlusion;
            a->occluded = st.occlusion;
            if (st.occlusion > 0) ++m_audioStats.occluded;
            p.reverbSend = a->spatial ? std::clamp(a->reverb, 0.0f, 1.0f) : 0.0f;
            if (a->stop) {
                if (st.voice) m_mixer.stop(st.voice);
                st.voice = 0;
                a->stop = false;
                a->play = false;
                st.started = true;
            }
            if (a->play || (!st.started && a->playOnStart)) {
                if (st.voice) m_mixer.stop(st.voice);
                st.voice = 0;
                if (a->stream && !startsWith(a->clip, "builtin:")) {
                    std::string path = m_registry.resolve(a->clip);
                    if (auto bytes = path.empty() ? std::nullopt : m_vfs.read(path)) {
                        std::string err;
                        if (auto s = audio::openStream(std::move(*bytes), &err)) st.voice = m_mixer.playStream(s, p);
                        else FLEDGE_LOG_WARN("audio", "Stream '{}': {}", a->clip, err);
                    }
                } else if (auto clip = audioClip(a->clip)) {
                    st.voice = m_mixer.play(clip, p);
                }
                st.started = true;
                a->play = false;
            } else if (st.voice) {
                m_mixer.setParams(st.voice, p);
            }
            bool was = a->playing;
            a->playing = st.voice && m_mixer.isPlaying(st.voice);
            if (!a->playing) st.voice = 0;
            if (a->stream && a->playing) ++m_audioStats.streams;
            if (a->oneShot && st.started && !a->playing && (was || !a->playOnStart)) finished.push_back(e);
        }
        for (auto it = m_sources.begin(); it != m_sources.end();) {  // removed entities / components stop their sound
            if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) {
                if (it->second.voice) m_mixer.stop(it->second.voice);
                it = m_sources.erase(it);
            } else {
                ++it;
            }
        }
        for (EntityId e : finished)
            if (m_scene.isValid(e)) m_scene.destroyEntity(e);
        // vehicle engine sounds
        std::vector<EntityId> cars;
        for (EntityId e : m_scene.entities()) {
            auto* vc = m_scene.get<VehicleComponent>(e);
            if (!vc || !vc->engineSound) continue;
            cars.push_back(e);
            SourceState& st = m_engines[e];
            Vec3 pos = worlds[e].c[3].xyz();
            Vec3 vel = st.hasLast ? (pos - st.lastPos) * inv : Vec3{0, 0, 0};
            if (length(vel) > 200) vel = {0, 0, 0};
            st.lastPos = pos;
            st.hasLast = true;
            audio::VoiceParams p;
            p.bus = audio::Bus::Sfx;
            p.loop = true;
            p.spatial = true;
            p.position = pos;
            p.velocity = vel;
            p.minDistance = 4;
            p.maxDistance = 120;
            p.volume = 0.5f + 0.4f * std::clamp(std::fabs(vc->throttle), 0.0f, 1.0f);
            occlusionFor(st, pos, e, e != playerCar);
            p.occlusion = st.occlusion;
            p.reverbSend = 0.6f;  // engines in tunnels / garages
            if (!st.engine) {
                st.engine = std::make_shared<audio::EngineSynth>(m_mixer.sampleRate(), vc->preset == "truck" ? 6 : vc->preset == "bike" ? 2 : 4);
                st.voice = m_mixer.playStream(st.engine, p);
            } else {
                m_mixer.setParams(st.voice, p);
            }
            st.engine->rpm = std::max(vc->rpm, 800.0f);
            st.engine->throttle = std::fabs(vc->throttle);
            ++m_audioStats.engines;
        }
        for (auto it = m_engines.begin(); it != m_engines.end();) {
            if (std::find(cars.begin(), cars.end(), it->first) == cars.end()) {
                if (it->second.voice) m_mixer.stop(it->second.voice);
                it = m_engines.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_audioStats.voices = static_cast<int>(m_mixer.voiceCount());
    if (m_audioDevice) m_audioDevice->pump(dt);
}

} // namespace fledge
