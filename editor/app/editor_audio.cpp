// Audio Mixer panel (docs/AUDIO.md): bus faders + mute + live meters, music ducking under voice, the output
// device, and the selected AudioSource (preview / stop / one-click "Add sound").
#include <algorithm>
#include <cmath>
#include <format>

#include <imgui.h>

#include "editor_app.h"
#include "editor_icons.h"

namespace fledge::editor {

namespace {
EntityId mixerEntity(Scene& s) {
    for (EntityId e : s.entities())
        if (s.get<AudioMixerComponent>(e)) return e;
    return kNullEntity;
}
void meter(float peak, float width, float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + width, p.y + height}, IM_COL32(30, 30, 34, 255), 2);
    float db = peak > 1e-5f ? 20 * std::log10(peak) : -60;
    float t = std::clamp((db + 60) / 60, 0.0f, 1.0f);
    ImU32 col = db > -3 ? IM_COL32(230, 80, 60, 255) : db > -12 ? IM_COL32(230, 190, 60, 255) : IM_COL32(80, 200, 120, 255);
    dl->AddRectFilled({p.x, p.y + height * (1 - t)}, {p.x + width, p.y + height}, col, 2);
    ImGui::Dummy({width, height});
}
} // namespace

void EditorApp::drawAudioPanel() {
    PanelDef* pd = panel("Audio Mixer");
    if (!ImGui::Begin("Audio Mixer", pd ? &pd->open : nullptr)) {
        ImGui::End();
        return;
    }
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("Open a project.");
        ImGui::End();
        return;
    }
    float s = effectiveScale();
    GameWorld& w = m_session.world();
    audio::Mixer& mx = w.mixer();
    Scene& scene = m_session.scene();
    ImGui::TextDisabled("Output: %s  |  voices %zu", w.audioDevice() ? w.audioDevice()->name().c_str() : "none", mx.voiceCount());
    EntityId me = mixerEntity(scene);
    if (!me) {
        ImGui::TextWrapped("The scene uses default bus volumes. Add a mixer to set volumes and ducking (saved with the scene).");
        if (ImGui::Button(ICON_PLUS " Add mixer to scene") && !m_session.playing()) {
            EntityId e = m_session.createEntity("Audio Mixer", kNullEntity, {{"AudioMixer", Json::object()}});
            if (e) m_status = "Added an AudioMixer";
        }
        trackRect("audio.addmixer");
    }
    // faders: master + 5 buses
    struct Row {
        const char* label;
        const char* field;
        audio::Bus bus;
    };
    const Row rows[] = {{"Master", "master", audio::Bus::Master}, {"Music", "music", audio::Bus::Music}, {"SFX", "sfx", audio::Bus::Sfx},
                        {"Ambient", "ambient", audio::Bus::Ambient}, {"Voice", "voice", audio::Bus::Voice}, {"UI", "ui", audio::Bus::UI}};
    auto* mc = me ? scene.get<AudioMixerComponent>(me) : nullptr;
    ImGui::Separator();
    for (auto& r : rows) {
        ImGui::PushID(r.label);
        ImGui::BeginGroup();
        float vol = mx.busVolume(r.bus);
        ImGui::BeginDisabled(!mc);
        if (ImGui::VSliderFloat("##v", {28 * s, 120 * s}, &vol, 0, 2, "%.2f") && mc)
            (void)m_session.setProperty(me, "AudioMixer", r.field, vol, std::string("mix.") + r.field);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2 * s);
        meter(mx.busPeak(r.bus), 8 * s, 120 * s);
        bool muted = mx.busMuted(r.bus);
        if (ImGui::Checkbox("M", &muted)) mx.setBusMuted(r.bus, muted);  // session-only (monitoring)
        ImGui::TextUnformatted(r.label);
        ImGui::EndGroup();
        trackRect(std::string("audio.bus.") + r.field);
        ImGui::PopID();
        ImGui::SameLine(0, 14 * s);
    }
    ImGui::NewLine();
    ImGui::Separator();
    if (mc) {
        bool duck = mc->duckMusicUnderVoice;
        if (ImGui::Checkbox("Duck music under voice", &duck)) (void)m_session.setProperty(me, "AudioMixer", "duckMusicUnderVoice", duck);
        ImGui::SameLine();
        float db = mc->duckDb;
        ImGui::SetNextItemWidth(120 * s);
        if (ImGui::SliderFloat("##duck", &db, -40, 0, "%.0f dB")) (void)m_session.setProperty(me, "AudioMixer", "duckDb", db, "mix.duck");
        ImGui::SameLine();
        ImGui::TextDisabled("now %.0f%%", mx.duckGain() * 100);
    }
    ImGui::Separator();
    // selected entity: its AudioSource
    EntityId sel = m_session.primarySelection();
    auto* src = sel ? scene.get<AudioSourceComponent>(sel) : nullptr;
    if (src) {
        ImGui::Text("%s: %s", scene.displayName(sel).c_str(), src->clip.empty() ? "(no clip)" : src->clip.c_str());
        if (ImGui::Button(ICON_PLAY " Preview")) m_status = w.previewClip(src->clip) ? "Previewing " + src->clip : "cannot play " + src->clip;
        trackRect("audio.preview");
        ImGui::SameLine();
        if (ImGui::Button(ICON_STOP " Stop")) w.stopPreview();
    } else if (sel && !m_session.playing()) {
        ImGui::TextDisabled("%s has no sound.", scene.displayName(sel).c_str());
        static int pick = 0;
        const char* builtins[] = {"builtin:beep", "builtin:click", "builtin:noise", "builtin:wind"};
        ImGui::SetNextItemWidth(150 * s);
        ImGui::Combo("##clip", &pick, builtins, 4);
        ImGui::SameLine();
        if (ImGui::Button(ICON_PLUS " Add sound")) {
            bool ambience = pick >= 2;
            (void)m_session.applyComponents(
                sel, {{"AudioSource", {{"clip", builtins[pick]}, {"loop", ambience}, {"bus", ambience ? "Ambient" : "Sfx"}}}}, "Add sound");
        }
        trackRect("audio.addsound");
        ImGui::TextDisabled("Or drop a WAV / MP3 / FLAC / Ogg from the Content Browser onto the entity.");
    } else {
        ImGui::TextDisabled("Select an entity to preview or add a sound.");
    }
    ImGui::End();
}

} // namespace fledge::editor
