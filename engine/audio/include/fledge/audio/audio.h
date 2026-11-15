#pragma once
// Audio (docs/AUDIO.md): a small software mixer (voices, buses, ducking, 3D panning / attenuation / doppler,
// occlusion low-pass, a send reverb, streaming) on top of miniaudio for device output and WAV / MP3 / FLAC / Ogg Vorbis decoding. The mixer is UI-free and
// deterministic: tests render it into buffers through the null device (no sound card needed).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/utility.h"

namespace fledge::audio {

// Fully decoded clip: interleaved float samples.
struct Clip {
    int channels = 1;
    int sampleRate = 48000;
    std::vector<float> samples;
    size_t frames() const { return channels > 0 ? samples.size() / static_cast<size_t>(channels) : 0; }
    float duration() const { return sampleRate > 0 ? static_cast<float>(frames()) / static_cast<float>(sampleRate) : 0; }
};
// Decodes WAV / MP3 / FLAC / Ogg Vorbis from memory (miniaudio + stb_vorbis). nullopt + error on failure.
std::optional<Clip> decodeClip(const Bytes& data, std::string* error = nullptr);
// Header-only check for importers (format, channels, rate, length) without decoding everything.
struct ClipInfo {
    int channels = 0, sampleRate = 0;
    double seconds = 0;
    std::string format;
};
std::optional<ClipInfo> probeClip(const Bytes& data, std::string* error = nullptr);
// Procedural built-in sounds ("builtin:beep", "builtin:click", "builtin:noise", "builtin:wind", "builtin:rain", "builtin:shot");
// empty clip if unknown.
Clip builtinClip(const std::string& name, int sampleRate = 48000);
Clip makeTone(float hz, float seconds, int sampleRate = 48000, float amplitude = 0.5f);

// Sequential source (decoder streams, synthesizers). Produces frames at sampleRate() with channels() channels.
class StreamSource {
public:
    virtual ~StreamSource() = default;
    virtual int channels() const = 0;
    virtual int sampleRate() const = 0;
    virtual size_t read(float* out, size_t frames) = 0;  // returns frames produced (< frames = end)
    virtual bool rewind() = 0;                          // for looping
};
// Streams an encoded file from memory, decoding as it plays (keeps the encoded bytes, not the PCM).
std::shared_ptr<StreamSource> openStream(Bytes data, std::string* error = nullptr);

// Procedural engine sound: a few detuned harmonics of the firing frequency plus noise; rpm and throttle are live.
class EngineSynth : public StreamSource {
public:
    explicit EngineSynth(int sampleRate = 48000, int cylinders = 4) : m_rate(sampleRate), m_cyl(cylinders) {}
    std::atomic<float> rpm{900}, throttle{0};
    int channels() const override { return 1; }
    int sampleRate() const override { return m_rate; }
    size_t read(float* out, size_t frames) override;
    bool rewind() override { return true; }

private:
    int m_rate, m_cyl;
    double m_phase = 0;
    uint32_t m_noise = 0x1234567u;
    float m_lp = 0, m_rpmSmooth = 900;
};

enum class Bus : int { Master = 0, Music = 1, Sfx = 2, Ambient = 3, Voice = 4, UI = 5, Count = 6 };
const char* busName(Bus b);
Bus busFromName(std::string_view s);  // unknown = Sfx

struct VoiceParams {
    Bus bus = Bus::Sfx;
    float volume = 1, pitch = 1;
    bool loop = false;
    bool spatial = false;
    Vec3 position{0, 0, 0}, velocity{0, 0, 0};
    float minDistance = 2, maxDistance = 60;
    float dopplerFactor = 1;
    float occlusion = 0;   // 0..1: walls between source and listener (low-pass 20 kHz -> 800 Hz, up to -8 dB); smoothed
    float reverbSend = 0;  // 0..1 of the voice (after its bus gain) fed into the mixer's reverb
};

// Freeverb-style reverb (8 damped combs + 4 allpasses per side, Jezar's tuning scaled to the mixer rate) on a send:
// voices feed it by reverbSend, the wet signal is added to the master mix. Reverb zones set it per listener position.
struct ReverbSettings {
    bool enabled = false;
    float roomSize = 0.7f;  // 0..1: decay time
    float damping = 0.5f;   // 0..1: high frequencies die faster
    float wet = 0.3f;       // 0..1 return level
    float width = 1.0f;     // 0 mono .. 1 full stereo
    bool operator==(const ReverbSettings&) const = default;
};
// "Room", "Hall", "Cave", "Tunnel", "Garage" (unknown = Room); enabled = true.
ReverbSettings reverbPreset(const std::string& name);
// Occlusion low-pass cutoff (Hz) and gain for an occlusion amount 0..1 (what the mixer applies).
float occlusionCutoffHz(float occlusion);
float occlusionGain(float occlusion);

using VoiceId = uint32_t;

struct DuckRule {
    bool enabled = true;
    Bus trigger = Bus::Voice, target = Bus::Music;
    float amountDb = -12;  // gain applied to the target while the trigger bus is active
    float attack = 0.05f, release = 0.5f;  // seconds
};

class Mixer {
public:
    explicit Mixer(int sampleRate = 48000);
    int sampleRate() const { return m_rate; }

    VoiceId play(std::shared_ptr<const Clip> clip, const VoiceParams& p);
    VoiceId playStream(std::shared_ptr<StreamSource> src, const VoiceParams& p);
    void stop(VoiceId v);
    void stopAll();
    bool isPlaying(VoiceId v) const;
    bool setParams(VoiceId v, const VoiceParams& p);
    size_t voiceCount() const;

    void setListener(Vec3 position, Quat rotation, Vec3 velocity = {0, 0, 0});
    void setBusVolume(Bus b, float v);
    float busVolume(Bus b) const;
    void setBusMuted(Bus b, bool m);
    bool busMuted(Bus b) const;
    void setDucking(const DuckRule& r);
    DuckRule ducking() const;
    float duckGain() const;     // current gain on the duck target (1 = not ducked)
    void setReverb(const ReverbSettings& r);
    ReverbSettings reverb() const;
    float busPeak(Bus b) const; // peak level of the last rendered block (after bus gain)

    // Renders interleaved stereo float frames (called from the device thread or the null device).
    void render(float* outStereo, size_t frames);
    uint64_t framesRendered() const { return m_rendered; }

    // Gains for a spatial voice (testing / debugging): left, right, distance gain, doppler pitch factor.
    struct Spatial {
        float left = 1, right = 1, distanceGain = 1, doppler = 1;
    };
    Spatial spatialize(const VoiceParams& p) const;

private:
    struct Voice {
        VoiceId id = 0;
        VoiceParams p;
        std::shared_ptr<const Clip> clip;
        std::shared_ptr<StreamSource> stream;
        double pos = 0;  // source frames
        std::vector<float> window;  // streams: decoded frames [winBase, winBase + window.size()/ch)
        uint64_t winBase = 0;
        bool ended = false, rewound = false;
        float gainL = -1, gainR = -1;  // smoothed (no zipper noise); -1 = first block
        float occ = -1;                // smoothed occlusion; -1 = first block
        float lpL = 0, lpR = 0;        // occlusion low-pass state
    };
    bool fetch(Voice& v, uint64_t frame, int channel, float& out);
    struct Reverb {
        std::vector<float> comb[2][8], allpass[2][4];
        size_t combPos[2][8] = {}, apPos[2][4] = {};
        float combLp[2][8] = {};
        int rate = 0;
        void init(int sampleRate);
        void process(const float* in, float* outStereo, size_t frames, const ReverbSettings& s);  // adds into out
        void clear();
    };
    ReverbSettings m_reverbSettings;
    Reverb m_reverb;
    std::vector<float> m_sendBuf[static_cast<int>(Bus::Count)];  // mono reverb send per bus (scaled by the bus gain)
    std::vector<float> m_reverbIn, m_reverbOut;
    int m_rate;
    mutable std::mutex m_mutex;
    std::vector<Voice> m_voices;
    VoiceId m_next = 1;
    Vec3 m_listenerPos{0, 0, 0}, m_listenerVel{0, 0, 0};
    Quat m_listenerRot{};
    float m_busVol[static_cast<int>(Bus::Count)] = {1, 1, 1, 1, 1, 1};
    bool m_busMute[static_cast<int>(Bus::Count)] = {};
    float m_peak[static_cast<int>(Bus::Count)] = {};
    DuckRule m_duck;
    float m_duckGain = 1;
    uint64_t m_rendered = 0;
    std::vector<float> m_busBuf[static_cast<int>(Bus::Count)];
};

// Output: a miniaudio playback device pulling from a Mixer, or the null device (nothing is opened; pump()
// renders into a scratch buffer so time still passes, which keeps tests and headless runs deterministic).
class AudioDevice {
public:
    ~AudioDevice();
    static std::unique_ptr<AudioDevice> open(Mixer& mixer, bool nullDevice, std::string* error = nullptr);
    bool isNull() const { return m_null; }
    const std::string& name() const { return m_name; }
    // Null device only: renders dt seconds (no-op for a real device, whose thread pulls on its own).
    void pump(float dt);

private:
    AudioDevice() = default;
    Mixer* m_mixer = nullptr;
    bool m_null = true;
    std::string m_name = "null";
    void* m_device = nullptr;  // ma_device*
    double m_carry = 0;
    std::vector<float> m_scratch;
};

} // namespace fledge::audio
