// SPDX-License-Identifier: MIT
// MacroSynth: the 48 macro-oscillator models of Mutable Instruments Braids (Émilie Gillet, MIT;
// third_party/braids, PROVENANCE.md) as a polyphonic instrument: each voice is one Braids macro
// oscillator with Timbre and Color, followed by an ADSR amplifier.
//
// Braids' tables are computed for 96 kHz. The voices run at twice the host rate (88.2 kHz at
// 44.1 kHz, exactly 96 kHz at 48 kHz) with the pitch corrected for the difference, then a 23-tap
// halfband FIR takes them down to the host rate. Braids renders in blocks of at most 24 samples,
// as on the module. Its oscillator state (17 KB per voice) lives in DAW_MEM_LARGE (PSRAM).
// Braids' noise generator state belongs to the instance (shared by its voices, reseeded at every
// activate() so a render is reproducible): instances share nothing and may render on both cores.
#include <math.h>
#include <string.h>

#include <new>

#include "braids/macro_oscillator.h"
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"
#include "dawsdk/voices.hpp"
#include "stmlib/utils/random.h"

using namespace dawsdk;

namespace {

const char *const kShapes[] = {
    "CSaw",           "Morph",         "Saw / Square",   "Sine / Triangle", "Buzz",           "Square sub",      "Saw sub",
    "Square sync",    "Saw sync",      "Triple saw",     "Triple square",   "Triple triangle", "Triple sine",     "Triple ring mod",
    "Saw swarm",      "Saw comb",      "Toy",            "Filter LP",       "Filter peak",    "Filter BP",       "Filter HP",
    "VOSIM",          "Vowel",         "Vowel FOF",      "Harmonics",       "FM",             "Feedback FM",     "Chaotic FM",
    "Plucked",        "Bowed",         "Blown",          "Fluted",          "Struck bell",    "Struck drum",     "Kick",
    "Cymbal",         "Snare",         "Wavetables",     "Wave map",        "Wave line",      "Wave paraphonic", "Filtered noise",
    "Twin peaks noise", "Clocked noise", "Granular cloud", "Particle noise", "Digital mod",    "Question mark"};
constexpr int kShapeCount = sizeof kShapes / sizeof kShapes[0];
static_assert(kShapeCount == braids::MACRO_OSC_SHAPE_LAST, "one label per Braids model");

enum : uint32_t { P_SHAPE, P_TIMBRE, P_COLOR, P_COARSE, P_FINE, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE, P_VEL, P_LEVEL, P_POLY, P_COUNT };
const daw_param_desc_t kParams[P_COUNT] = {
    {P_SHAPE, "model", "Model", "Oscillator", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, kShapeCount - 1, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, kShapeCount, kShapes},
    {P_TIMBRE, "timbre", "Timbre", "Oscillator", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.5f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_COLOR, "color", "Color", "Oscillator", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.5f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_COARSE, "coarse", "Coarse", "Pitch", "st", DAW_PARAM_INT, DAW_SCALE_BIPOLAR, -24, 24, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_FINE, "fine", "Fine", "Pitch", "ct", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -100.0f, 100.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_ATTACK, "attack", "Attack", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.5f, 5000.0f, 2.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DECAY, "decay", "Decay", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 5.0f, 10000.0f, 400.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_SUSTAIN, "sustain", "Sustain", "Envelope", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.7f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RELEASE, "release", "Release", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 5.0f, 10000.0f, 250.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_VEL, "velocity", "Velocity", "Output", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.6f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_LEVEL, "level", "Level", "Output", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.7f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_POLY, "polyphony", "Polyphony", "Output", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 4, 2, DAW_PARAM_NO_SMOOTH, 0, nullptr},
};
//                                 model timbre color coarse fine att  dec   sus  rel  vel  level poly
const float kPresetVowel[P_COUNT] = {22, 0.3f, 0.6f, 0, 0, 30, 800, 0.8f, 600, 0.5f, 0.7f, 2};
const float kPresetPluck[P_COUNT] = {28, 0.5f, 0.6f, 0, 0, 0.5f, 2000, 0.0f, 800, 0.7f, 0.7f, 4};
const float kPresetBell[P_COUNT] = {32, 0.6f, 0.4f, 0, 0, 0.5f, 4000, 0.0f, 3000, 0.6f, 0.7f, 4};
const float kPresetFmBass[P_COUNT] = {25, 0.4f, 0.3f, -12, 0, 0.5f, 300, 0.6f, 120, 0.6f, 0.75f, 1};
const float kPresetSwarm[P_COUNT] = {14, 0.5f, 0.5f, 0, 0, 300, 1500, 0.9f, 1500, 0.3f, 0.6f, 2};
const float kPresetKick[P_COUNT] = {34, 0.5f, 0.5f, -24, 0, 0.5f, 800, 0.0f, 200, 0.8f, 0.8f, 1};
const daw_preset_desc_t kPresets[] = {
    {"Init", "Basic", nullptr},     {"Vowel Pad", "Pad", kPresetVowel},   {"Plucked String", "Pluck", kPresetPluck},
    {"Struck Bell", "Bell", kPresetBell}, {"FM Bass", "Bass", kPresetFmBass}, {"Saw Swarm", "Pad", kPresetSwarm},
    {"Kick", "Drums", kPresetKick},
};

constexpr uint32_t kMaxVoices = 4;
constexpr int kBraidsBlock = 24;  // Braids' render block (oversampled samples)

// 23-tap halfband low-pass for the 2x decimation (Kaiser beta 6): the non-zero taps left of the centre
constexpr int kHbHalf = 6;
const float kHb[kHbHalf] = {-0.000430307f, 0.003720102f, -0.013237673f, 0.035121692f, -0.086237624f, 0.311114362f};
struct Halfband {
    float buf[32] = {};
    int pos = 0;
    void reset()
    {
        memset(buf, 0, sizeof buf);
        pos = 0;
    }
    float at(int back) const { return buf[(pos - 1 - back) & 31]; }
    float process(float a, float b)
    {
        buf[pos] = a;
        pos = (pos + 1) & 31;
        buf[pos] = b;
        pos = (pos + 1) & 31;
        float acc = 0.5f * at(11);
        #pragma GCC unroll 6
        for (int k = 0; k < kHbHalf; k++) {
            const int d = 2 * (kHbHalf - k) - 1;
            acc += kHb[k] * (at(11 - d) + at(11 + d));
        }
        return acc;
    }
};

struct Voice : VoiceTag {
    braids::MacroOscillator *osc = nullptr;  // LARGE memory
    dsp::Adsr env;
    Halfband hb;
    float vel = 1, note = 60;
    bool active() const { return env.active(); }
};

class MacroSynth : public Plugin<MacroSynth, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();

    int32_t init()
    {
        oscs_ = alloc<braids::MacroOscillator>(kMaxVoices, DAW_MEM_LARGE);
        if (!oscs_)
            return DAW_ERR_NOMEM;
        for (uint32_t i = 0; i < kMaxVoices; i++) {
            new (&oscs_[i]) braids::MacroOscillator();
            oscs_[i].set_random_state(&rng_);  // this instance's noise generator
            oscs_[i].Init();
            pool_.voice[i].osc = &oscs_[i];
        }
        // Braids' tables are for 96 kHz; we run at 2 x the host rate
        pitchOffset_ = 128.0f * 12.0f * log2f(96000.0f / (2.0f * sampleRate()));
        return DAW_OK;
    }
    void release()
    {
        if (oscs_) {
            for (uint32_t i = 0; i < kMaxVoices; i++)
                oscs_[i].~MacroOscillator();
            dealloc(oscs_);
            oscs_ = nullptr;
        }
    }
    void reset()
    {
        rng_ = 0x2c9e1f35u;  // reproducible renders (this instance's own generator state)
        for (auto &v : pool_.voice) {
            v.env.kill();
            v.held = false;
            v.osc->Init();
            v.hb.reset();
        }
        level_.setTime(0.02f, sampleRate());
        level_.snap(param(P_LEVEL));
        timbre_.setTime(0.01f, sampleRate() / kBraidsBlock);
        color_.setTime(0.01f, sampleRate() / kBraidsBlock);
        timbre_.snap(param(P_TIMBRE));
        color_.snap(param(P_COLOR));
    }
    void onStart(const daw_process_t &)
    {
        level_.snap(param(P_LEVEL));
        timbre_.snap(param(P_TIMBRE));
        color_.snap(param(P_COLOR));
    }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_LEVEL: level_.target = v; break;
        case P_TIMBRE: timbre_.target = v; break;
        case P_COLOR: color_.target = v; break;
        case P_POLY: pool_.setLimit((uint32_t)v); break;
        case P_ATTACK:
        case P_DECAY:
        case P_SUSTAIN:
        case P_RELEASE:
            for (auto &vc : pool_.voice)
                setEnv(vc);
            break;
        default: break;
        }
    }
    void onEvent(const daw_event_t &e)
    {
        switch (e.type) {
        case DAW_EV_NOTE_ON: {
            bool stolen;
            Voice *v = pool_.allocate(stolen);
            if (stolen)
                report(DAW_REPORT_VOICE_STEAL);
            v->noteId = e.u.note.note_id;
            v->key = e.u.note.key;
            v->held = true;
            v->note = (float)e.u.note.key + e.u.note.pitch;
            const float sens = param(P_VEL);
            v->vel = 1.0f - sens + sens * e.u.note.velocity;
            if (!v->env.active() || stolen)
                v->hb.reset();
            v->osc->Strike();  // excites the physical / percussive models
            setEnv(*v);
            v->env.gate(true);
            break;
        }
        case DAW_EV_NOTE_OFF: {
            Voice *v = pool_.find(e.u.note.note_id, e.u.note.key);
            if (v) {
                v->held = false;
                v->env.gate(false);
            }
            break;
        }
        case DAW_EV_NOTE_EXPR: {
            Voice *v = pool_.find(e.u.expr.note_id, e.u.expr.key);
            if (v && e.u.expr.expr == DAW_EXPR_PITCH)
                v->note = (float)v->key + e.u.expr.value;
            break;
        }
        case DAW_EV_CHANNEL_PITCH: bend_ = e.u.midi.value; break;
        case DAW_EV_PITCH_BEND: bend_ = e.u.midi.value * 2.0f; break;  // +-2 semitones
        case DAW_EV_ALL_NOTES_OFF:
            for (auto &v : pool_.voice) {
                v.held = false;
                v.env.gate(false);
            }
            break;
        case DAW_EV_ALL_SOUND_OFF:
            for (auto &v : pool_.voice) {
                v.held = false;
                v.env.kill();
            }
            break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        float *L = c.outputs[0] + start, *R = c.outputs[1] + start;
        memset(L, 0, n * sizeof(float));
        const auto shape = (braids::MacroOscillatorShape)(int)param(P_SHAPE);
        const float tune = param(P_COARSE) + param(P_FINE) * 0.01f + bend_;
        // control-rate values for this call (Braids interpolates its parameters inside a block)
        const int16_t timbre = (int16_t)dsp::clamp(timbre_.next() * 32767.0f, 0.0f, 32767.0f);
        const int16_t color = (int16_t)dsp::clamp(color_.next() * 32767.0f, 0.0f, 32767.0f);
        const uint8_t sync[kBraidsBlock] = {};
        int16_t os[kBraidsBlock];
        for (auto &v : pool_.voice) {
            if (!v.active())
                continue;
            braids::MacroOscillator &o = *v.osc;
            o.set_shape(shape);
            o.set_parameters(timbre, color);
            const float p = (v.note + tune) * 128.0f + pitchOffset_;
            o.set_pitch((int16_t)dsp::clamp(p, 0.0f, 16383.0f));
            for (uint32_t done = 0; done < n;) {
                const uint32_t outN = n - done < kBraidsBlock / 2 ? n - done : kBraidsBlock / 2;
                o.Render(sync, os, outN * 2);
                for (uint32_t i = 0; i < outN; i++) {
                    const float y = v.hb.process(os[2 * i] * (1.0f / 32768.0f), os[2 * i + 1] * (1.0f / 32768.0f));
                    L[done + i] += y * v.env.next() * v.vel;
                }
                done += outN;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            const float y = dsp::sanitize(L[i] * level_.next());
            L[i] = R[i] = y;
        }
    }
    uint32_t voices() const { return pool_.activeCount(); }

private:
    void setEnv(Voice &v)
    {
        v.env.set(param(P_ATTACK) * 0.001f, param(P_DECAY) * 0.001f, param(P_SUSTAIN), param(P_RELEASE) * 0.001f, sampleRate());
    }
    braids::MacroOscillator *oscs_ = nullptr;
    VoicePool<Voice, kMaxVoices> pool_;
    uint32_t rng_ = 0x2c9e1f35u;  // Braids' generator state for this instance
    dsp::Smoother level_, timbre_, color_;
    float pitchOffset_ = 0, bend_ = 0;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.macrosynth",
    .name = "MacroSynth",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "The 48 macro-oscillator models of Mutable Instruments Braids, polyphonic, with an envelope",
    .category = "Synth",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_POLYPHONIC | DAW_CAP_VELOCITY | DAW_CAP_PITCH_BEND | DAW_CAP_NOTE_PITCH,
    .max_voices = kMaxVoices,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(MacroSynth),
    .mem_large_bytes = kMaxVoices * sizeof(braids::MacroOscillator) + 64,
    .cpu_permille_base = 2,
    .cpu_permille_voice = 150,  // conservative estimate (heaviest Braids models, 2x oversampled); to be measured on the board
    DAWSDK_CALLBACKS(MacroSynth),
};

const daw_plugin_descriptor_t *MacroSynth::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_macrosynth(void) { return &kDescriptor; }
