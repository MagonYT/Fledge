#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include <miniaudio.h>

#include "fledge/audio/audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace fledge::audio {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kSpeedOfSound = 343.0f;

std::string formatOf(const Bytes& d) {
    if (d.size() >= 4 && std::memcmp(d.data(), "RIFF", 4) == 0) return "wav";
    if (d.size() >= 4 && std::memcmp(d.data(), "fLaC", 4) == 0) return "flac";
    if (d.size() >= 4 && std::memcmp(d.data(), "OggS", 4) == 0) return "ogg";
    if (d.size() >= 3 && std::memcmp(d.data(), "ID3", 3) == 0) return "mp3";
    if (d.size() >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return "mp3";
    return "unknown";
}

struct Decoder {
    ma_decoder dec{};
    bool ok = false;
    ~Decoder() {
        if (ok) ma_decoder_uninit(&dec);
    }
};

bool initDecoder(Decoder& d, const Bytes& data, std::string* error) {
    std::string fmt = formatOf(data);
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (fmt == "ogg") cfg.encodingFormat = ma_encoding_format_vorbis;  // Ogg Vorbis via stb_vorbis (miniaudio_impl.cpp)
    if (ma_decoder_init_memory(data.data(), data.size(), &cfg, &d.dec) != MA_SUCCESS) {
        if (error) *error = fmt == "ogg" ? "not an Ogg Vorbis stream (Ogg Opus / FLAC-in-Ogg are not supported)" : "not a WAV, MP3, FLAC or Ogg Vorbis file";
        return false;
    }
    d.ok = true;
    return true;
}

class DecoderStream : public StreamSource {
public:
    explicit DecoderStream(Bytes data) : m_data(std::move(data)) {}
    bool init(std::string* error) {
        if (!initDecoder(m_dec, m_data, error)) return false;
        ma_format f;
        ma_uint32 ch = 0, rate = 0;
        ma_decoder_get_data_format(&m_dec.dec, &f, &ch, &rate, nullptr, 0);
        m_ch = static_cast<int>(ch);
        m_rate = static_cast<int>(rate);
        return m_ch > 0 && m_rate > 0;
    }
    int channels() const override { return m_ch; }
    int sampleRate() const override { return m_rate; }
    size_t read(float* out, size_t frames) override {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&m_dec.dec, out, frames, &got);
        return static_cast<size_t>(got);
    }
    bool rewind() override { return ma_decoder_seek_to_pcm_frame(&m_dec.dec, 0) == MA_SUCCESS; }

private:
    Bytes m_data;  // the decoder reads from this memory
    Decoder m_dec;
    int m_ch = 0, m_rate = 0;
};

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
} // namespace

const char* busName(Bus b) {
    switch (b) {
        case Bus::Master: return "Master";
        case Bus::Music: return "Music";
        case Bus::Sfx: return "Sfx";
        case Bus::Ambient: return "Ambient";
        case Bus::Voice: return "Voice";
        case Bus::UI: return "UI";
        default: return "Sfx";
    }
}
Bus busFromName(std::string_view s) {
    for (int i = 0; i < static_cast<int>(Bus::Count); ++i)
        if (s == busName(static_cast<Bus>(i))) return static_cast<Bus>(i);
    return Bus::Sfx;
}

std::optional<Clip> decodeClip(const Bytes& data, std::string* error) {
    Decoder d;
    if (!initDecoder(d, data, error)) return std::nullopt;
    ma_format f;
    ma_uint32 ch = 0, rate = 0;
    ma_decoder_get_data_format(&d.dec, &f, &ch, &rate, nullptr, 0);
    if (ch == 0 || rate == 0) {
        if (error) *error = "audio file has no channels";
        return std::nullopt;
    }
    Clip c;
    c.channels = static_cast<int>(ch);
    c.sampleRate = static_cast<int>(rate);
    std::vector<float> buf(4096 * ch);
    for (;;) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&d.dec, buf.data(), 4096, &got);
        if (got == 0) break;
        c.samples.insert(c.samples.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got * ch));
        if (c.samples.size() > 48000ull * 60 * 20 * ch) break;  // 20 min cap: use streaming for long music
    }
    return c;
}

std::optional<ClipInfo> probeClip(const Bytes& data, std::string* error) {
    Decoder d;
    if (!initDecoder(d, data, error)) return std::nullopt;
    ma_format f;
    ma_uint32 ch = 0, rate = 0;
    ma_decoder_get_data_format(&d.dec, &f, &ch, &rate, nullptr, 0);
    ma_uint64 len = 0;
    ma_decoder_get_length_in_pcm_frames(&d.dec, &len);
    ClipInfo i;
    i.channels = static_cast<int>(ch);
    i.sampleRate = static_cast<int>(rate);
    i.seconds = rate ? static_cast<double>(len) / rate : 0;
    i.format = formatOf(data);
    return i;
}

std::shared_ptr<StreamSource> openStream(Bytes data, std::string* error) {
    auto s = std::make_shared<DecoderStream>(std::move(data));
    if (!s->init(error)) return nullptr;
    return s;
}

Clip makeTone(float hz, float seconds, int sampleRate, float amplitude) {
    Clip c;
    c.channels = 1;
    c.sampleRate = sampleRate;
    size_t n = static_cast<size_t>(seconds * static_cast<float>(sampleRate));
    c.samples.resize(n);
    for (size_t i = 0; i < n; ++i) c.samples[i] = amplitude * std::sin(2 * kPi * hz * static_cast<float>(i) / static_cast<float>(sampleRate));
    return c;
}

Clip builtinClip(const std::string& name, int rate) {
    Clip c;
    c.sampleRate = rate;
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed) / 4294967295.0f * 2 - 1;
    };
    if (name == "builtin:beep") {
        c = makeTone(880, 0.25f, rate, 0.4f);
        size_t fade = static_cast<size_t>(rate * 0.01f);
        for (size_t i = 0; i < fade && i < c.samples.size(); ++i) {
            float g = static_cast<float>(i) / static_cast<float>(fade);
            c.samples[i] *= g;
            c.samples[c.samples.size() - 1 - i] *= g;
        }
    } else if (name == "builtin:shot") {  // gunshot: noise burst with a fast decay and a low thump
        size_t n = static_cast<size_t>(rate * 0.35f);
        c.samples.resize(n);
        for (size_t i = 0; i < n; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(rate);
            c.samples[i] = (rnd() * 0.8f * std::exp(-t / 0.04f) + 0.6f * std::sin(2 * kPi * 70 * t) * std::exp(-t / 0.08f)) * 0.9f;
        }
    } else if (name == "builtin:rain") {  // loopable rain: bright filtered noise with droplets
        size_t n = static_cast<size_t>(rate * 4.0f);
        c.samples.resize(n);
        float lp = 0;
        for (size_t i = 0; i < n; ++i) {
            float x = rnd();
            lp += (x - lp) * 0.35f;
            float drop = (seed % 5000u == 0) ? 0.6f : 0.0f;
            c.samples[i] = (x - lp) * 0.25f + drop;
        }
    } else if (name == "builtin:click") {
        size_t n = static_cast<size_t>(rate * 0.03f);
        c.samples.resize(n);
        for (size_t i = 0; i < n; ++i) c.samples[i] = rnd() * 0.6f * std::exp(-static_cast<float>(i) / (rate * 0.004f));
    } else if (name == "builtin:noise" || name == "builtin:wind") {
        size_t n = static_cast<size_t>(rate * 4.0f);  // loops seamlessly enough for ambience
        c.samples.resize(n);
        float lp = 0, lp2 = 0;
        bool wind = name == "builtin:wind";
        for (size_t i = 0; i < n; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(rate);
            float x = rnd();
            lp += (x - lp) * (wind ? 0.02f : 0.5f);
            lp2 += (lp - lp2) * (wind ? 0.05f : 1.0f);
            float gust = wind ? 0.6f + 0.4f * std::sin(2 * kPi * t / 4.0f) : 1.0f;  // one gust per loop
            c.samples[i] = (wind ? lp2 * 3.0f : lp2 * 0.3f) * gust;
        }
    }
    return c;
}

size_t EngineSynth::read(float* out, size_t frames) {
    float target = std::clamp(rpm.load(), 300.0f, 12000.0f), thr = std::clamp(throttle.load(), 0.0f, 1.0f);
    for (size_t i = 0; i < frames; ++i) {
        m_rpmSmooth += (target - m_rpmSmooth) * 0.0005f;
        double fire = m_rpmSmooth / 60.0 * m_cyl / 2.0;  // firing frequency (4-stroke)
        m_phase += fire / m_rate;
        if (m_phase > 1e6) m_phase -= 1e6;
        double p = 2.0 * 3.14159265358979 * m_phase;
        float tone = static_cast<float>(0.55 * std::sin(p) + 0.3 * std::sin(2 * p + 0.3) + 0.18 * std::sin(3 * p + 1.1) + 0.1 * std::sin(0.5 * p));
        m_noise ^= m_noise << 13;
        m_noise ^= m_noise >> 17;
        m_noise ^= m_noise << 5;
        float n = static_cast<float>(m_noise) / 4294967295.0f * 2 - 1;
        m_lp += (n - m_lp) * 0.15f;
        float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(p));  // combustion pulses modulate the noise
        out[i] = (tone * (0.35f + 0.25f * thr) + m_lp * pulse * (0.15f + 0.35f * thr)) * 0.6f;
    }
    return frames;
}

// ----------------------------------------------------- reverb + occlusion --
ReverbSettings reverbPreset(const std::string& name) {
    ReverbSettings r;
    r.enabled = true;
    if (name == "Hall") r.roomSize = 0.85f, r.damping = 0.35f, r.wet = 0.35f;
    else if (name == "Cave") r.roomSize = 0.95f, r.damping = 0.2f, r.wet = 0.45f;
    else if (name == "Tunnel") r.roomSize = 0.8f, r.damping = 0.45f, r.wet = 0.4f, r.width = 0.6f;
    else if (name == "Garage") r.roomSize = 0.6f, r.damping = 0.3f, r.wet = 0.3f;
    else r.roomSize = 0.45f, r.damping = 0.6f, r.wet = 0.25f;  // Room
    return r;
}

float occlusionCutoffHz(float occlusion) { return 20000.0f * std::pow(800.0f / 20000.0f, std::clamp(occlusion, 0.0f, 1.0f)); }
float occlusionGain(float occlusion) { return 1.0f - 0.6f * std::clamp(occlusion, 0.0f, 1.0f); }

namespace {
constexpr int kCombTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};  // Freeverb (Jezar), samples at 44.1 kHz
constexpr int kAllpassTuning[4] = {556, 441, 341, 225};
constexpr int kStereoSpread = 23;
} // namespace

void Mixer::Reverb::init(int sampleRate) {
    rate = sampleRate;
    double scale = static_cast<double>(sampleRate) / 44100.0;
    for (int s = 0; s < 2; ++s) {
        for (int k = 0; k < 8; ++k) comb[s][k].assign(static_cast<size_t>(std::max(1.0, (kCombTuning[k] + s * kStereoSpread) * scale)), 0.0f);
        for (int k = 0; k < 4; ++k) allpass[s][k].assign(static_cast<size_t>(std::max(1.0, (kAllpassTuning[k] + s * kStereoSpread) * scale)), 0.0f);
    }
    clear();
}

void Mixer::Reverb::clear() {
    for (int s = 0; s < 2; ++s) {
        for (int k = 0; k < 8; ++k) {
            std::fill(comb[s][k].begin(), comb[s][k].end(), 0.0f);
            combPos[s][k] = 0;
            combLp[s][k] = 0;
        }
        for (int k = 0; k < 4; ++k) {
            std::fill(allpass[s][k].begin(), allpass[s][k].end(), 0.0f);
            apPos[s][k] = 0;
        }
    }
}

void Mixer::Reverb::process(const float* in, float* out, size_t frames, const ReverbSettings& st) {
    const float feedback = std::clamp(st.roomSize, 0.0f, 1.0f) * 0.28f + 0.7f, damp = std::clamp(st.damping, 0.0f, 1.0f) * 0.4f;
    const float width = std::clamp(st.width, 0.0f, 1.0f), wet = std::clamp(st.wet, 0.0f, 1.0f) * 3.0f;
    const float wet1 = wet * (width * 0.5f + 0.5f), wet2 = wet * ((1.0f - width) * 0.5f);
    for (size_t i = 0; i < frames; ++i) {
        float x = in[i] * 0.015f, side[2];
        for (int s = 0; s < 2; ++s) {
            float acc = 0;
            for (int k = 0; k < 8; ++k) {  // damped feedback combs in parallel
                auto& buf = comb[s][k];
                size_t& p = combPos[s][k];
                float y = buf[p];
                combLp[s][k] = y * (1.0f - damp) + combLp[s][k] * damp;
                buf[p] = x + combLp[s][k] * feedback;
                if (++p >= buf.size()) p = 0;
                acc += y;
            }
            for (int k = 0; k < 4; ++k) {  // allpasses in series (diffusion)
                auto& buf = allpass[s][k];
                size_t& p = apPos[s][k];
                float b = buf[p];
                buf[p] = acc + b * 0.5f;
                acc = b - acc;
                if (++p >= buf.size()) p = 0;
            }
            side[s] = acc;
        }
        out[i * 2] += side[0] * wet1 + side[1] * wet2;
        out[i * 2 + 1] += side[1] * wet1 + side[0] * wet2;
    }
}

// ------------------------------------------------------------------ mixer --
Mixer::Mixer(int sampleRate) : m_rate(sampleRate) {}

void Mixer::setReverb(const ReverbSettings& r) {
    std::lock_guard l(m_mutex);
    if (m_reverbSettings.enabled && !r.enabled) m_reverb.clear();  // no stale tail when it comes back
    m_reverbSettings = r;
}
ReverbSettings Mixer::reverb() const {
    std::lock_guard l(m_mutex);
    return m_reverbSettings;
}

VoiceId Mixer::play(std::shared_ptr<const Clip> clip, const VoiceParams& p) {
    if (!clip || clip->frames() == 0) return 0;
    std::lock_guard l(m_mutex);
    Voice v;
    v.id = m_next++;
    v.p = p;
    v.clip = std::move(clip);
    m_voices.push_back(std::move(v));
    return m_voices.back().id;
}

VoiceId Mixer::playStream(std::shared_ptr<StreamSource> src, const VoiceParams& p) {
    if (!src || src->channels() <= 0) return 0;
    std::lock_guard l(m_mutex);
    Voice v;
    v.id = m_next++;
    v.p = p;
    v.stream = std::move(src);
    m_voices.push_back(std::move(v));
    return m_voices.back().id;
}

void Mixer::stop(VoiceId id) {
    std::lock_guard l(m_mutex);
    std::erase_if(m_voices, [id](const Voice& v) { return v.id == id; });
}
void Mixer::stopAll() {
    std::lock_guard l(m_mutex);
    m_voices.clear();
}
bool Mixer::isPlaying(VoiceId id) const {
    std::lock_guard l(m_mutex);
    for (auto& v : m_voices)
        if (v.id == id) return !v.ended;
    return false;
}
bool Mixer::setParams(VoiceId id, const VoiceParams& p) {
    std::lock_guard l(m_mutex);
    for (auto& v : m_voices)
        if (v.id == id) {
            v.p = p;
            return true;
        }
    return false;
}
size_t Mixer::voiceCount() const {
    std::lock_guard l(m_mutex);
    return m_voices.size();
}
void Mixer::setListener(Vec3 position, Quat rotation, Vec3 velocity) {
    std::lock_guard l(m_mutex);
    m_listenerPos = position;
    m_listenerRot = rotation;
    m_listenerVel = velocity;
}
void Mixer::setBusVolume(Bus b, float v) {
    std::lock_guard l(m_mutex);
    m_busVol[static_cast<int>(b)] = std::clamp(v, 0.0f, 4.0f);
}
float Mixer::busVolume(Bus b) const {
    std::lock_guard l(m_mutex);
    return m_busVol[static_cast<int>(b)];
}
void Mixer::setBusMuted(Bus b, bool m) {
    std::lock_guard l(m_mutex);
    m_busMute[static_cast<int>(b)] = m;
}
bool Mixer::busMuted(Bus b) const {
    std::lock_guard l(m_mutex);
    return m_busMute[static_cast<int>(b)];
}
void Mixer::setDucking(const DuckRule& r) {
    std::lock_guard l(m_mutex);
    m_duck = r;
}
DuckRule Mixer::ducking() const {
    std::lock_guard l(m_mutex);
    return m_duck;
}
float Mixer::duckGain() const {
    std::lock_guard l(m_mutex);
    return m_duckGain;
}
float Mixer::busPeak(Bus b) const {
    std::lock_guard l(m_mutex);
    return m_peak[static_cast<int>(b)];
}

Mixer::Spatial Mixer::spatialize(const VoiceParams& p) const {
    Spatial s;
    if (!p.spatial) return s;
    Vec3 rel = p.position - m_listenerPos;
    float d = length(rel);
    float minD = std::max(0.01f, p.minDistance), maxD = std::max(minD + 0.01f, p.maxDistance);
    if (d >= maxD) s.distanceGain = 0;
    else if (d > minD) {
        float fadeStart = maxD * 0.8f;
        float fade = d > fadeStart ? 1.0f - (d - fadeStart) / (maxD - fadeStart) : 1.0f;
        s.distanceGain = minD / d * fade;
    }
    if (d > 1e-4f) {
        Vec3 local = m_listenerRot.conjugate().rotate(rel / d);
        float pan = std::clamp(local.x, -1.0f, 1.0f);  // +1 = right
        float a = (pan + 1) * kPi / 4;
        s.left = std::min(1.0f, std::sqrt(2.0f) * std::cos(a));
        s.right = std::min(1.0f, std::sqrt(2.0f) * std::sin(a));
        Vec3 u = (m_listenerPos - p.position) / d;  // source -> listener
        float vs = dot(p.velocity, u);              // source approaching
        float vl = dot(m_listenerVel, -u);          // listener approaching
        float f = (kSpeedOfSound + vl) / std::max(1.0f, kSpeedOfSound - vs);
        f = std::clamp(f, 0.5f, 2.0f);
        s.doppler = 1 + (f - 1) * p.dopplerFactor;
    }
    return s;
}

bool Mixer::fetch(Voice& v, uint64_t frame, int ch, float& out) {
    if (v.clip) {
        const Clip& c = *v.clip;
        size_t n = c.frames();
        if (frame >= n) {
            if (!v.p.loop) return false;
            frame %= n;
        }
        out = c.samples[frame * static_cast<size_t>(c.channels) + static_cast<size_t>(std::min(ch, c.channels - 1))];
        return true;
    }
    StreamSource& s = *v.stream;
    int sc = s.channels();
    size_t have = v.window.size() / static_cast<size_t>(sc);
    while (frame >= v.winBase + have) {
        size_t old = v.window.size();
        v.window.resize(old + 1024 * static_cast<size_t>(sc));
        size_t got = s.read(v.window.data() + old, 1024);
        v.window.resize(old + got * static_cast<size_t>(sc));
        if (got == 0) {
            if (v.p.loop && !v.rewound && s.rewind()) {  // seamless loop: keep appending from the start
                v.rewound = true;
                continue;
            }
            return false;
        }
        v.rewound = false;
        have = v.window.size() / static_cast<size_t>(sc);
    }
    if (frame < v.winBase) return false;  // never happens: the mixer only moves forward
    out = v.window[(frame - v.winBase) * static_cast<size_t>(sc) + static_cast<size_t>(std::min(ch, sc - 1))];
    // drop consumed frames now and then
    if (frame > v.winBase + 8192) {
        uint64_t drop = frame - v.winBase - 2;
        v.window.erase(v.window.begin(), v.window.begin() + static_cast<std::ptrdiff_t>(drop * static_cast<uint64_t>(sc)));
        v.winBase += drop;
    }
    return true;
}

void Mixer::render(float* out, size_t frames) {
    std::lock_guard l(m_mutex);
    constexpr int kBuses = static_cast<int>(Bus::Count);
    for (int b = 0; b < kBuses; ++b) m_busBuf[b].assign(frames * 2, 0.0f);
    const bool reverbOn = m_reverbSettings.enabled && m_reverbSettings.wet > 0;
    if (reverbOn) {
        if (m_reverb.rate != m_rate) m_reverb.init(m_rate);
        for (int b = 0; b < kBuses; ++b) m_sendBuf[b].assign(frames, 0.0f);
    }
    // ducking envelope (per block)
    bool triggerActive = false;
    if (m_duck.enabled)
        for (auto& v : m_voices) triggerActive |= v.p.bus == m_duck.trigger && !v.ended && v.p.volume > 0.001f;
    float target = triggerActive ? dbToGain(m_duck.amountDb) : 1.0f;
    float blockSec = static_cast<float>(frames) / static_cast<float>(m_rate);
    float tau = std::max(1e-3f, triggerActive ? m_duck.attack : m_duck.release);
    m_duckGain += (target - m_duckGain) * (1.0f - std::exp(-blockSec / tau));
    for (auto& v : m_voices) {
        if (v.ended) continue;
        Spatial sp = spatialize(v.p);
        float gl = v.p.volume * sp.distanceGain * sp.left, gr = v.p.volume * sp.distanceGain * sp.right;
        if (v.gainL < 0) {
            v.gainL = gl;
            v.gainR = gr;
        }
        int srcRate = v.clip ? v.clip->sampleRate : v.stream->sampleRate();
        int srcCh = v.clip ? v.clip->channels : v.stream->channels();
        double step = static_cast<double>(srcRate) / m_rate * std::max(0.05f, v.p.pitch * sp.doppler);
        float* bus = m_busBuf[static_cast<int>(v.p.bus)].data();
        // occlusion: smoothed per block (~80 ms), one-pole low-pass + gain
        float occTarget = std::clamp(v.p.occlusion, 0.0f, 1.0f);
        if (v.occ < 0) v.occ = occTarget;
        v.occ += (occTarget - v.occ) * (1.0f - std::exp(-blockSec / 0.08f));
        const bool occluded = v.occ > 0.001f;
        const float lpA = 1.0f - std::exp(-2.0f * kPi * occlusionCutoffHz(v.occ) / static_cast<float>(m_rate)), occGain = occlusionGain(v.occ);
        float* send = reverbOn && v.p.reverbSend > 0 ? m_sendBuf[static_cast<int>(v.p.bus)].data() : nullptr;
        const float sendAmt = std::clamp(v.p.reverbSend, 0.0f, 1.0f) * 0.5f;
        for (size_t i = 0; i < frames; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(frames);
            float al = v.gainL + (gl - v.gainL) * t, ar = v.gainR + (gr - v.gainR) * t;
            uint64_t f0 = static_cast<uint64_t>(v.pos);
            float frac = static_cast<float>(v.pos - static_cast<double>(f0));
            float a0, a1, b0 = 0, b1 = 0;
            if (!fetch(v, f0, 0, a0)) {
                v.ended = true;
                break;
            }
            if (!fetch(v, f0 + 1, 0, a1)) a1 = a0;
            float left = a0 + (a1 - a0) * frac, right = left;
            if (srcCh > 1) {
                (void)fetch(v, f0, 1, b0);
                if (!fetch(v, f0 + 1, 1, b1)) b1 = b0;
                right = b0 + (b1 - b0) * frac;
            }
            if (v.p.spatial && srcCh > 1) left = right = (left + right) * 0.5f;  // spatial sources are mono-folded
            if (occluded) {
                v.lpL += lpA * (left - v.lpL);
                v.lpR += lpA * (right - v.lpR);
                left = v.lpL * occGain;
                right = v.lpR * occGain;
            }
            bus[i * 2] += left * al;
            bus[i * 2 + 1] += right * ar;
            if (send) send[i] += (left * al + right * ar) * sendAmt;
            v.pos += step;
            if (v.clip && !v.p.loop && v.pos >= static_cast<double>(v.clip->frames())) {
                v.ended = true;
                break;
            }
            if (v.clip && v.p.loop && v.pos >= static_cast<double>(v.clip->frames())) v.pos -= static_cast<double>(v.clip->frames());
        }
        v.gainL = gl;
        v.gainR = gr;
    }
    std::erase_if(m_voices, [](const Voice& v) { return v.ended; });
    // buses -> master
    float master = m_busMute[0] ? 0.0f : m_busVol[0];
    float masterPeak = 0;
    if (reverbOn) {
        m_reverbIn.assign(frames, 0.0f);
        m_reverbOut.assign(frames * 2, 0.0f);
    }
    for (int b = 1; b < kBuses; ++b) {
        float g = m_busMute[b] ? 0.0f : m_busVol[b];
        if (m_duck.enabled && b == static_cast<int>(m_duck.target)) g *= m_duckGain;
        float peak = 0;
        auto& buf = m_busBuf[b];
        for (float& x : buf) {
            x *= g;
            peak = std::max(peak, std::fabs(x));
        }
        m_peak[b] = peak;
        if (reverbOn && g > 0)  // sends follow their bus volume / mute / ducking
            for (size_t i = 0; i < frames; ++i) m_reverbIn[i] += m_sendBuf[b][i] * g;
    }
    if (reverbOn) m_reverb.process(m_reverbIn.data(), m_reverbOut.data(), frames, m_reverbSettings);
    for (size_t i = 0; i < frames * 2; ++i) {
        float s = reverbOn ? m_reverbOut[i] : 0.0f;
        for (int b = 1; b < kBuses; ++b) s += m_busBuf[b][i];
        s *= master;
        s = std::clamp(s, -1.0f, 1.0f);  // hard limit (never wraps)
        masterPeak = std::max(masterPeak, std::fabs(s));
        out[i] = s;
    }
    m_peak[0] = masterPeak;
    m_rendered += frames;
}

// ------------------------------------------------------------------ device --
namespace {
void dataCallback(ma_device* dev, void* output, const void*, ma_uint32 frames) {
    static_cast<Mixer*>(dev->pUserData)->render(static_cast<float*>(output), frames);
}
} // namespace

AudioDevice::~AudioDevice() {
    if (m_device) {
        auto* d = static_cast<ma_device*>(m_device);
        ma_device_uninit(d);
        delete d;
    }
}

std::unique_ptr<AudioDevice> AudioDevice::open(Mixer& mixer, bool nullDevice, std::string* error) {
    std::unique_ptr<AudioDevice> out(new AudioDevice());
    out->m_mixer = &mixer;
    if (nullDevice) return out;
    auto* d = new ma_device();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = static_cast<ma_uint32>(mixer.sampleRate());
    cfg.dataCallback = dataCallback;
    cfg.pUserData = &mixer;
    if (ma_device_init(nullptr, &cfg, d) != MA_SUCCESS) {
        delete d;
        if (error) *error = "no audio output device (using the null device)";
        return out;  // falls back to null
    }
    if (ma_device_start(d) != MA_SUCCESS) {
        ma_device_uninit(d);
        delete d;
        if (error) *error = "audio device failed to start (using the null device)";
        return out;
    }
    out->m_device = d;
    out->m_null = false;
    out->m_name = d->playback.name;
    return out;
}

void AudioDevice::pump(float dt) {
    if (!m_null || !m_mixer || dt <= 0) return;
    m_carry += static_cast<double>(dt) * m_mixer->sampleRate();
    auto frames = static_cast<size_t>(m_carry);
    m_carry -= static_cast<double>(frames);
    while (frames > 0) {
        size_t n = std::min<size_t>(frames, 1024);
        m_scratch.resize(n * 2);
        m_mixer->render(m_scratch.data(), n);
        frames -= n;
    }
}

} // namespace fledge::audio
