// SPDX-License-Identifier: MIT
// Sampler: plays one audio file (ABI 1.1 resource slot "sample", decoded by the host) chromatically.
// Pitch from the key relative to the root key (the file's 'smpl' chunk or the Root parameter),
// start / end points, loop (off, forward between the loop points, or the file's own loop),
// reverse, one-shot, ADSR, a low-pass filter, velocity, per-note pan / pitch, up to 16 voices.
// Playback position is an integer frame plus a fraction (float alone loses precision after a few
// seconds of audio), read with 4-point cubic (Catmull-Rom) interpolation.
// The sample memory belongs to the host: the plugin only keeps pointers to it (set_resource runs
// on the control thread, before activate).
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"
#include "dawsdk/voices.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t {
    P_ROOT, P_FILE_ROOT, P_TUNE, P_FINE, P_START, P_END, P_LOOP, P_LOOP_START, P_LOOP_END, P_REVERSE, P_ONESHOT,
    P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE, P_CUTOFF, P_RES, P_VEL, P_LEVEL, P_POLY, P_COUNT
};
const char *const kLoopModes[] = {"Off", "Forward", "File loop"};
const daw_param_desc_t kParams[P_COUNT] = {
    {P_ROOT, "root", "Root key", "Pitch", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 127, 60, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_FILE_ROOT, "file_root", "Root from file", "Pitch", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 1, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_TUNE, "tune", "Tune", "Pitch", "st", DAW_PARAM_INT, DAW_SCALE_BIPOLAR, -24, 24, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_FINE, "fine", "Fine", "Pitch", "ct", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -100.0f, 100.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_START, "start", "Start", "Region", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_END, "end", "End", "Region", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 100.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_LOOP, "loop", "Loop", "Region", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 2, 0, DAW_PARAM_NO_SMOOTH, 3, kLoopModes},
    {P_LOOP_START, "loop_start", "Loop start", "Region", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_LOOP_END, "loop_end", "Loop end", "Region", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 100.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_REVERSE, "reverse", "Reverse", "Region", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_ONESHOT, "oneshot", "One-shot", "Region", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_ATTACK, "attack", "Attack", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.5f, 5000.0f, 1.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DECAY, "decay", "Decay", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 5.0f, 10000.0f, 500.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_SUSTAIN, "sustain", "Sustain", "Envelope", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 1.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RELEASE, "release", "Release", "Envelope", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 5.0f, 10000.0f, 150.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_CUTOFF, "cutoff", "Cutoff", "Filter", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 50.0f, 20000.0f, 20000.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RES, "resonance", "Resonance", "Filter", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_VEL, "velocity", "Velocity", "Output", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.7f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_LEVEL, "level", "Level", "Output", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.8f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_POLY, "polyphony", "Polyphony", "Output", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 16, 8, DAW_PARAM_NO_SMOOTH, 0, nullptr},
};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}};
const daw_resource_slot_desc_t kSlots[] = {{"sample", "Sample", DAW_RES_MASK(DAW_RES_AUDIO), "wav"}};

constexpr uint32_t kMaxVoices = 16;

struct Voice : VoiceTag {
    uint32_t pos = 0;   // integer frame
    float frac = 0;     // fraction of a frame
    float step = 1;     // frames per output sample (always > 0; direction separate)
    int dir = 1;        // +1 forward, -1 reverse / ping-pong back
    float key01 = 60, vel = 1, panL = 1, panR = 1;
    bool done = false;  // ran past the end
    dsp::Adsr env;
    dsp::Svf fl, fr;
    bool active() const { return env.active() && !done; }
};

class Sampler : public Plugin<Sampler, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();

    int32_t setResource(uint32_t slot, const daw_resource_t *res)
    {
        if (slot != 0)
            return DAW_ERR_ARG;
        frames_ = 0;
        if (!res)
            return DAW_OK;
        if (res->type != DAW_RES_AUDIO || !res->channels || res->channel_count < 1 || res->channel_count > 2 ||
            res->frames < 2 || !res->sample_rate)
            return DAW_ERR_ARG;
        ch_[0] = res->channels[0];
        ch_[1] = res->channel_count > 1 ? res->channels[1] : res->channels[0];
        frames_ = res->frames;
        fileRate_ = (float)res->sample_rate;
        fileRoot_ = res->root_key;
        fileLoopStart_ = res->loop_start;
        fileLoopEnd_ = res->loop_end;
        return DAW_OK;
    }
    void reset()
    {
        midiBend_ = 0;
        for (auto &v : pool_.voice) {
            v.env.kill();
            v.held = false;
        }
        level_.setTime(0.02f, sampleRate());
        level_.snap(param(P_LEVEL));
    }
    void onStart(const daw_process_t &) { level_.snap(param(P_LEVEL)); }
    void onParam(uint32_t id, float v)
    {
        if (id == P_LEVEL)
            level_.target = v;
        else if (id == P_POLY)
            pool_.setLimit((uint32_t)v);
        else if (id >= P_ATTACK && id <= P_RELEASE)
            for (auto &vc : pool_.voice)
                setEnv(vc);
        else if (id == P_TUNE || id == P_FINE || id == P_ROOT || id == P_FILE_ROOT)
            for (auto &vc : pool_.voice)
                retune(vc);
        else if (id == P_CUTOFF || id == P_RES)
            for (auto &vc : pool_.voice)
                setFilter(vc);
    }
    void onEvent(const daw_event_t &e)
    {
        switch (e.type) {
        case DAW_EV_NOTE_ON: {
            if (!frames_)
                break;
            bool stolen;
            Voice *v = pool_.allocate(stolen);
            if (stolen)
                report(DAW_REPORT_VOICE_STEAL);
            v->noteId = e.u.note.note_id;
            v->key = e.u.note.key;
            v->held = true;
            v->key01 = (float)e.u.note.key + e.u.note.pitch;
            const float sens = param(P_VEL);
            v->vel = 1.0f - sens + sens * e.u.note.velocity;
            dsp::panGains(e.u.note.pan, v->panL, v->panR);
            const bool rev = param(P_REVERSE) >= 0.5f;
            v->dir = rev ? -1 : 1;
            v->pos = rev ? regionEnd() - 1 : regionStart();
            v->frac = 0;
            v->done = false;
            v->fl.reset();
            v->fr.reset();
            setEnv(*v);
            setFilter(*v);
            retune(*v);
            v->env.gate(true);
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
                if (param(P_ONESHOT) < 0.5f)
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
        if (frames_) {
            const uint32_t rs = regionStart(), re = regionEnd();
            uint32_t ls = rs, le = re;
            const int loop = (int)param(P_LOOP);
            if (loop == 1) {
                ls = pct(P_LOOP_START);
                le = pct(P_LOOP_END);
            } else if (loop == 2 && fileLoopEnd_ > fileLoopStart_) {
                ls = fileLoopStart_;
                le = fileLoopEnd_;
            }
            const bool looping = loop != 0 && le > ls + 1;
            const bool filter = param(P_CUTOFF) < 19999.0f;
            for (auto &v : pool_.voice) {
                if (!v.active())
                    continue;
                for (uint32_t i = 0; i < n; i++) {
                    float l, r;
                    read(v.pos, v.frac, l, r);
                    if (filter) {
                        l = v.fl.process(l, dsp::Svf::Low);
                        r = ch_[0] == ch_[1] ? l : v.fr.process(r, dsp::Svf::Low);
                    }
                    const float g = v.env.next() * v.vel;
                    L[i] += l * g * v.panL;
                    R[i] += r * g * v.panR;
                    if (!advance(v, rs, re, ls, le, looping)) {
                        v.done = true;
                        v.env.kill();
                        break;
                    }
                }
                if (filter && ch_[0] == ch_[1]) {
                    v.fr.ic1 = v.fl.ic1;
                    v.fr.ic2 = v.fl.ic2;
                }
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            const float g = level_.next();
            L[i] = dsp::sanitize(L[i] * g);
            R[i] = dsp::sanitize(R[i] * g);
        }
    }
    uint32_t voices() const { return pool_.activeCount(); }

private:
    float midiBend_ = 0;
    uint32_t pct(uint32_t id) const
    {
        const float f = param(id) * 0.01f * (float)(frames_ - 1);
        return f <= 0 ? 0 : (f >= (float)(frames_ - 1) ? frames_ - 1 : (uint32_t)f);
    }
    uint32_t regionStart() const { return pct(P_START); }
    uint32_t regionEnd() const
    {
        const uint32_t e = pct(P_END) + 1, s = regionStart();
        return e > s + 1 ? e : s + 2 < frames_ ? s + 2 : frames_;
    }
    // Catmull-Rom between frames pos and pos + 1, clamped at the file's edges
    void read(uint32_t pos, float t, float &l, float &r) const
    {
        const uint32_t last = frames_ - 1;
        const uint32_t i0 = pos ? pos - 1 : 0, i1 = pos < last ? pos : last, i2 = pos + 1 < last ? pos + 1 : last,
                       i3 = pos + 2 < last ? pos + 2 : last;
        for (int c = 0; c < 2; c++) {
            const float *s = ch_[c];
            const float y0 = s[i0], y1 = s[i1], y2 = s[i2], y3 = s[i3];
            const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            (c ? r : l) = ((c3 * t + c2) * t + c1) * t + y1;
        }
    }
    // false when the voice ran out of sample
    static bool advance(Voice &v, uint32_t rs, uint32_t re, uint32_t ls, uint32_t le, bool looping)
    {
        v.frac += v.step;
        const uint32_t whole = (uint32_t)v.frac;
        v.frac -= (float)whole;
        if (v.dir > 0) {
            v.pos += whole;
            if (looping && v.pos >= le)
                v.pos = ls + (v.pos - le) % (le - ls);
            return v.pos + 1 < re;
        }
        if (v.pos < rs + whole) {
            if (!looping)
                return false;
            v.pos = le - 1;  // reverse loop: wrap to the loop end
            return true;
        }
        v.pos -= whole;
        if (looping && v.pos < ls)
            v.pos = le - 1 - (ls - v.pos) % (le - ls);
        return true;
    }
    void setEnv(Voice &v)
    {
        v.env.set(param(P_ATTACK) * 0.001f, param(P_DECAY) * 0.001f, param(P_SUSTAIN), param(P_RELEASE) * 0.001f, sampleRate());
    }
    void setFilter(Voice &v)
    {
        v.fl.set(param(P_CUTOFF), param(P_RES), sampleRate());
        v.fr.set(param(P_CUTOFF), param(P_RES), sampleRate());
    }
    void retune(Voice &v)
    {
        const float root = (param(P_FILE_ROOT) >= 0.5f && fileRoot_ >= 0 && fileRoot_ <= 127) ? (float)fileRoot_ : param(P_ROOT);
        const float semis = v.key01 + param(P_TUNE) + midiBend_ + param(P_FINE) * 0.01f - root;
        const float s = fileRate_ / sampleRate() * dsp::fastExp2(semis * (1.0f / 12.0f));
        v.step = s < 1e-4f ? 1e-4f : (s > 64.0f ? 64.0f : s);
    }
    const float *ch_[2] = {nullptr, nullptr};
    uint32_t frames_ = 0, fileLoopStart_ = 0, fileLoopEnd_ = 0;
    float fileRate_ = 44100.0f;
    int32_t fileRoot_ = -1;
    VoicePool<Voice, kMaxVoices> pool_;
    dsp::Smoother level_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.sampler",
    .name = "Sampler",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Plays an audio file across the keyboard: loop, reverse, one-shot, envelope, filter",
    .category = "Sampler",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_PITCH_BEND | DAW_CAP_POLYPHONIC | DAW_CAP_VELOCITY | DAW_CAP_NOTE_PAN | DAW_CAP_NOTE_PITCH,
    .max_voices = kMaxVoices,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(Sampler),
    .mem_large_bytes = 0,  // the sample itself is the host's memory
    .cpu_permille_base = 1,
    .cpu_permille_voice = 12,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(Sampler),
    DAWSDK_RESOURCES(Sampler, 1, kSlots),
};

const daw_plugin_descriptor_t *Sampler::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_sampler(void) { return &kDescriptor; }
