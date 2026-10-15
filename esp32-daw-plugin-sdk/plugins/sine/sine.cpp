// SPDX-License-Identifier: MIT
// Sine: the minimal reference instrument. Polyphonic sine voices with an attack / release
// envelope, velocity, and independent per-note pan and pitch (DAW_EXPR_PAN / DAW_EXPR_PITCH).
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"
#include "dawsdk/voices.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_LEVEL, P_ATTACK, P_RELEASE, P_TUNE, P_POLY, P_COUNT };

const daw_param_desc_t kParams[P_COUNT] = {
    {P_LEVEL, "level", "Level", "Output", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.5f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_ATTACK, "attack", "Attack", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.5f, 2000.0f, 5.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RELEASE, "release", "Release", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 1.0f, 5000.0f, 120.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_TUNE, "tune", "Tune", "Pitch", "st", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -24.0f, 24.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_POLY, "polyphony", "Polyphony", "Voices", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1.0f, 16.0f, 8.0f, DAW_PARAM_NO_SMOOTH, 0, nullptr},
};

const float kPresetSoft[P_COUNT] = {0.5f, 40.0f, 400.0f, 0.0f, 8.0f};
const float kPresetPluck[P_COUNT] = {0.6f, 1.0f, 80.0f, 0.0f, 8.0f};
const float kPresetSub[P_COUNT] = {0.7f, 3.0f, 60.0f, -12.0f, 1.0f};
const daw_preset_desc_t kPresets[] = {
    {"Init", "Basic", nullptr},
    {"Soft Pad", "Pad", kPresetSoft},
    {"Pluck", "Lead", kPresetPluck},
    {"Sub", "Bass", kPresetSub},
};

constexpr uint32_t kMaxVoices = 16;

struct Voice : VoiceTag {
    float phase = 0, inc = 0;
    float key01 = 0;      // key + pitch expression, semitones
    float vel = 0;
    float panL = 1, panR = 1;
    dsp::Adsr env;
    bool active() const { return env.active(); }
};

class Sine : public Plugin<Sine, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onStart(const daw_process_t &) { level_.snap(level_.target); }

    void reset()
    {
        midiBend_ = 0;
        for (auto &v : pool_.voice)
            v.env.kill();
        level_.snap(param(P_LEVEL));
    }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_LEVEL: level_.target = v; break;
        case P_POLY: pool_.setLimit((uint32_t)v); break;
        default: break;
        }
        if (id == P_ATTACK || id == P_RELEASE)
            for (auto &vc : pool_.voice)
                vc.env.set(param(P_ATTACK) * 0.001f, 0.0f, 1.0f, param(P_RELEASE) * 0.001f, sampleRate());
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
            v->vel = e.u.note.velocity;
            v->key01 = (float)e.u.note.key + e.u.note.pitch;
            dsp::panGains(e.u.note.pan, v->panL, v->panR);
            if (stolen || !v->env.active())
                v->phase = 0;
            v->env.set(param(P_ATTACK) * 0.001f, 0.0f, 1.0f, param(P_RELEASE) * 0.001f, sampleRate());
            v->env.gate(true);
            retune(*v);
            break;
        }
        case DAW_EV_CHANNEL_PITCH:
        case DAW_EV_PITCH_BEND:
            midiBend_ = e.u.midi.value * (e.type == DAW_EV_CHANNEL_PITCH ? 1.f : 2.f);
            for (auto &v : pool_.voice) retune(v);
            break;
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
            if (!v)
                break;
            if (e.u.expr.expr == DAW_EXPR_PAN)
                dsp::panGains(e.u.expr.value, v->panL, v->panR);
            else if (e.u.expr.expr == DAW_EXPR_PITCH) {
                v->key01 = (float)v->key + e.u.expr.value;
                retune(*v);
            }
            break;
        }
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
        for (uint32_t i = 0; i < n; i++)
            L[i] = R[i] = 0;
        Voice *active[kMaxVoices];
        uint32_t count = 0;
        for (auto &v : pool_.voice)
            if (v.active()) active[count++] = &v;
        for (uint32_t i = 0; i < n; i++) {
            const float level = level_.next();  // shared, once per sample at any polyphony
            for (uint32_t j = 0; j < count; j++) {
                Voice &v = *active[j];
                float s = dsp::sinTurnsUnit(v.phase) * v.env.next() * v.vel * level;
                v.phase += v.inc;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                L[i] += s * v.panL;
                R[i] += s * v.panR;
            }
        }
    }
    uint32_t voices() const { return pool_.activeCount(); }

private:
    float midiBend_ = 0;
    void retune(Voice &v)
    {
        float hz = dsp::midiToHz(v.key01 + param(P_TUNE) + midiBend_);
        v.inc = hz / sampleRate();
        if (v.inc > 0.45f)
            v.inc = 0.45f;
    }
    VoicePool<Voice, kMaxVoices> pool_;
    dsp::Smoother level_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.sine",
    .name = "Sine",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Minimal polyphonic sine instrument with per-note pan and pitch",
    .category = "Synth",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_PITCH_BEND | DAW_CAP_POLYPHONIC | DAW_CAP_VELOCITY | DAW_CAP_NOTE_PAN | DAW_CAP_NOTE_PITCH,
    .max_voices = kMaxVoices,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(Sine),
    .mem_large_bytes = 0,
    .cpu_permille_base = 1,
    .cpu_permille_voice = 4,
    DAWSDK_CALLBACKS(Sine),
};

const daw_plugin_descriptor_t *Sine::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_sine(void) { return &kDescriptor; }
