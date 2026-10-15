// SPDX-License-Identifier: MIT
// FM6: a six-operator phase-modulation synth in the style of the Yamaha DX7 (original code).
// The 32 algorithms follow the DX7's routing diagrams (which operators modulate which, which are
// carriers, where the feedback loop is). Each operator: frequency ratio (coarse, fine, detune),
// output level 0..99 (about 0.75 dB per step, as on the DX7), an ADSR envelope and velocity
// sensitivity. Global: algorithm, feedback 0..7, vibrato, transpose, polyphony (up to 8).
//
// Per sample and voice the operators run from 6 down to 1: in every algorithm a modulator has a
// higher number than what it modulates, so each operator's modulation input is ready when it runs.
// Feedback uses the average of the source operator's last two outputs (the DX7's trick against
// feedback noise). Phase modulation depth: a modulator at full level shifts the phase by up to
// 2 turns (4 pi radians).
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"
#include "dawsdk/voices.hpp"
#include "sine.hpp"

using namespace dawsdk;

namespace {

constexpr int kOps = 6;
// per operator: coarse, fine, detune, level, attack, decay, sustain, release, velocity
enum OpParam : uint32_t { OP_COARSE, OP_FINE, OP_DETUNE, OP_LEVEL, OP_ATTACK, OP_DECAY, OP_SUSTAIN, OP_RELEASE, OP_VEL, kOpParams };
enum : uint32_t { P_ALGO, P_FEEDBACK, P_LEVEL, P_TRANSPOSE, P_POLY, P_VIB_RATE, P_VIB_DEPTH, P_OP0, P_COUNT = P_OP0 + kOps * kOpParams };

constexpr uint32_t op(int o, OpParam p) { return P_OP0 + (uint32_t)o * kOpParams + p; }

constexpr const char *kOpGroup[kOps] = {"Operator 1", "Operator 2", "Operator 3", "Operator 4", "Operator 5", "Operator 6"};
constexpr const char *kOpKeys[kOps][kOpParams] = {
#define K(n) {"op" #n "_coarse", "op" #n "_fine", "op" #n "_detune", "op" #n "_level", "op" #n "_attack", "op" #n "_decay", "op" #n "_sustain", "op" #n "_release", "op" #n "_vel"}
    K(1), K(2), K(3), K(4), K(5), K(6)
#undef K
};

// the parameter table, built at compile time (constexpr: no static constructor, lives in flash)
struct Params {
    daw_param_desc_t p[P_COUNT];
    constexpr Params() : p{}
    {
        p[P_ALGO] = {P_ALGO, "algorithm", "Algorithm", "Global", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 32, 5, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr};
        p[P_FEEDBACK] = {P_FEEDBACK, "feedback", "Feedback", "Global", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 7, 3, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr};
        p[P_LEVEL] = {P_LEVEL, "level", "Level", "Global", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.6f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
        p[P_TRANSPOSE] = {P_TRANSPOSE, "transpose", "Transpose", "Global", "st", DAW_PARAM_INT, DAW_SCALE_BIPOLAR, -24, 24, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr};
        p[P_POLY] = {P_POLY, "polyphony", "Polyphony", "Global", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 8, 4, DAW_PARAM_NO_SMOOTH, 0, nullptr};  // 4 by default until the S3 cost is measured
        p[P_VIB_RATE] = {P_VIB_RATE, "vib_rate", "Vibrato rate", "Global", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.1f, 12.0f, 5.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
        p[P_VIB_DEPTH] = {P_VIB_DEPTH, "vib_depth", "Vibrato depth", "Global", "ct", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
        for (int o = 0; o < kOps; o++) {
            const char *g = kOpGroup[o];
            const char *const *k = kOpKeys[o];
            const float lvl = o == 0 ? 99.0f : (o == 1 ? 70.0f : 0.0f);  // init: op 1 carrier, op 2 a gentle modulator
            p[op(o, OP_COARSE)] = {op(o, OP_COARSE), k[0], "Coarse", g, "x", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 31, 1, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr};
            p[op(o, OP_FINE)] = {op(o, OP_FINE), k[1], "Fine", g, "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 99.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
            p[op(o, OP_DETUNE)] = {op(o, OP_DETUNE), k[2], "Detune", g, "", DAW_PARAM_INT, DAW_SCALE_BIPOLAR, -7, 7, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr};
            p[op(o, OP_LEVEL)] = {op(o, OP_LEVEL), k[3], "Level", g, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 99.0f, lvl, DAW_PARAM_AUTOMATABLE, 0, nullptr};
            p[op(o, OP_ATTACK)] = {op(o, OP_ATTACK), k[4], "Attack", g, "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.5f, 5000.0f, 2.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
            p[op(o, OP_DECAY)] = {op(o, OP_DECAY), k[5], "Decay", g, "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 5.0f, 10000.0f, 800.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
            p[op(o, OP_SUSTAIN)] = {op(o, OP_SUSTAIN), k[6], "Sustain", g, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.6f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
            p[op(o, OP_RELEASE)] = {op(o, OP_RELEASE), k[7], "Release", g, "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 5.0f, 10000.0f, 300.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
            p[op(o, OP_VEL)] = {op(o, OP_VEL), k[8], "Velocity", g, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.5f, DAW_PARAM_AUTOMATABLE, 0, nullptr};
        }
    }
};
constexpr Params kParams;

// ---- algorithms --------------------------------------------------------------------------------
// mod[o]: bit mask of the operators modulating operator o (0-based); carriers: bit mask;
// fbSrc -> fbDst: the feedback path (fbSrc == fbDst: self-feedback)
struct Algo {
    uint8_t mod[kOps];
    uint8_t carriers;
    int8_t fbSrc, fbDst;
};
constexpr uint8_t B(int opNumber) { return (uint8_t)(1u << (opNumber - 1)); }  // DX7 numbering, 1..6
// operator numbers below are the DX7's (1..6); mod[] is indexed by op - 1
const Algo kAlgos[32] = {
    // 1: 2>1  6>5>4>3                 carriers 1,3   fb 6
    {{B(2), 0, B(4), B(5), B(6), 0}, B(1) | B(3), 5, 5},
    // 2: 2>1  6>5>4>3                 carriers 1,3   fb 2
    {{B(2), 0, B(4), B(5), B(6), 0}, B(1) | B(3), 1, 1},
    // 3: 3>2>1  6>5>4                 carriers 1,4   fb 6
    {{B(2), B(3), 0, B(5), B(6), 0}, B(1) | B(4), 5, 5},
    // 4: 3>2>1  6>5>4, 4 feeds back to 6   carriers 1,4
    {{B(2), B(3), 0, B(5), B(6), 0}, B(1) | B(4), 3, 5},
    // 5: 2>1  4>3  6>5                carriers 1,3,5 fb 6
    {{B(2), 0, B(4), 0, B(6), 0}, B(1) | B(3) | B(5), 5, 5},
    // 6: 2>1  4>3  6>5, 5 feeds back to 6   carriers 1,3,5
    {{B(2), 0, B(4), 0, B(6), 0}, B(1) | B(3) | B(5), 4, 5},
    // 7: 2>1  4>3  6>5>3              carriers 1,3   fb 6
    {{B(2), 0, B(4) | B(5), 0, B(6), 0}, B(1) | B(3), 5, 5},
    // 8: as 7, fb 4
    {{B(2), 0, B(4) | B(5), 0, B(6), 0}, B(1) | B(3), 3, 3},
    // 9: as 7, fb 2
    {{B(2), 0, B(4) | B(5), 0, B(6), 0}, B(1) | B(3), 1, 1},
    // 10: 3>2>1  5>4  6>4             carriers 1,4   fb 3
    {{B(2), B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(4), 2, 2},
    // 11: as 10, fb 6
    {{B(2), B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(4), 5, 5},
    // 12: 2>1  4,5,6>3                carriers 1,3   fb 2
    {{B(2), 0, B(4) | B(5) | B(6), 0, 0, 0}, B(1) | B(3), 1, 1},
    // 13: as 12, fb 6
    {{B(2), 0, B(4) | B(5) | B(6), 0, 0, 0}, B(1) | B(3), 5, 5},
    // 14: 2>1  5,6>4>3                carriers 1,3   fb 6
    {{B(2), 0, B(4), B(5) | B(6), 0, 0}, B(1) | B(3), 5, 5},
    // 15: as 14, fb 2
    {{B(2), 0, B(4), B(5) | B(6), 0, 0}, B(1) | B(3), 1, 1},
    // 16: 2>1  4>3>1  6>5>1           carrier 1      fb 6
    {{B(2) | B(3) | B(5), 0, B(4), 0, B(6), 0}, B(1), 5, 5},
    // 17: as 16, fb 2
    {{B(2) | B(3) | B(5), 0, B(4), 0, B(6), 0}, B(1), 1, 1},
    // 18: 2>1  3>1  6>5>4>1           carrier 1      fb 3
    {{B(2) | B(3) | B(4), 0, 0, B(5), B(6), 0}, B(1), 2, 2},
    // 19: 3>2>1  6>4  6>5             carriers 1,4,5 fb 6
    {{B(2), B(3), 0, B(6), B(6), 0}, B(1) | B(4) | B(5), 5, 5},
    // 20: 3>1  3>2  5>4  6>4          carriers 1,2,4 fb 3
    {{B(3), B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(2) | B(4), 2, 2},
    // 21: 3>1  3>2  6>4  6>5          carriers 1,2,4,5 fb 3
    {{B(3), B(3), 0, B(6), B(6), 0}, B(1) | B(2) | B(4) | B(5), 2, 2},
    // 22: 2>1  6>3  6>4  6>5          carriers 1,3,4,5 fb 6
    {{B(2), 0, B(6), B(6), B(6), 0}, B(1) | B(3) | B(4) | B(5), 5, 5},
    // 23: 1  3>2  6>4  6>5            carriers 1,2,4,5 fb 6
    {{0, B(3), 0, B(6), B(6), 0}, B(1) | B(2) | B(4) | B(5), 5, 5},
    // 24: 1  2  6>3  6>4  6>5         carriers 1-5   fb 6
    {{0, 0, B(6), B(6), B(6), 0}, B(1) | B(2) | B(3) | B(4) | B(5), 5, 5},
    // 25: 1  2  3  6>4  6>5           carriers 1-5   fb 6
    {{0, 0, 0, B(6), B(6), 0}, B(1) | B(2) | B(3) | B(4) | B(5), 5, 5},
    // 26: 1  3>2  5>4  6>4            carriers 1,2,4 fb 6
    {{0, B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(2) | B(4), 5, 5},
    // 27: as 26, fb 3
    {{0, B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(2) | B(4), 2, 2},
    // 28: 2>1  5>4>3  6               carriers 1,3,6 fb 5
    {{B(2), 0, B(4), B(5), 0, 0}, B(1) | B(3) | B(6), 4, 4},
    // 29: 1  2  4>3  6>5              carriers 1,2,3,5 fb 6
    {{0, 0, B(4), 0, B(6), 0}, B(1) | B(2) | B(3) | B(5), 5, 5},
    // 30: 1  2  5>4>3  6              carriers 1,2,3,6 fb 5
    {{0, 0, B(4), B(5), 0, 0}, B(1) | B(2) | B(3) | B(6), 4, 4},
    // 31: 1  2  3  4  6>5             carriers 1-5   fb 6
    {{0, 0, 0, 0, B(6), 0}, B(1) | B(2) | B(3) | B(4) | B(5), 5, 5},
    // 32: all carriers                fb 6
    {{0, 0, 0, 0, 0, 0}, 0x3F, 5, 5},
};

// coarse 0 is a ratio of 0.5, as on the DX7
inline float coarseRatio(int c) { return c <= 0 ? 0.5f : (float)c; }
// DX7-like output level: 99 = full, about 0.75 dB per step, 0 = silent
inline float levelGain(float l) { return l < 0.5f ? 0.0f : dsp::fastExp2((l - 99.0f) * 0.125f); }

//                               algo fb  level transpose poly vibR vibD
#define GLOBAL(a, fb, lvl) a, fb, lvl, 0, 4, 5.0f, 0.0f
// op: coarse fine detune level attack decay sustain release vel
#define OP(c, f, dt, l, a, d, s, r, v) c, f, dt, l, a, d, s, r, v
const float kEPiano[P_COUNT] = {
    GLOBAL(5, 6, 0.6f),
    OP(1, 0, 3, 99, 1.5f, 3500, 0.0f, 600, 0.6f), OP(14, 0, 0, 58, 1.0f, 400, 0.0f, 400, 0.8f),
    OP(1, 0, -3, 99, 1.5f, 3000, 0.0f, 600, 0.6f), OP(1, 0, 0, 82, 1.0f, 1800, 0.0f, 500, 0.7f),
    OP(1, 0, -2, 90, 1.0f, 2500, 0.0f, 600, 0.3f), OP(1, 0, 2, 60, 1.0f, 1200, 0.0f, 400, 0.6f)};
const float kBass[P_COUNT] = {
    GLOBAL(1, 5, 0.65f),
    OP(1, 0, 0, 99, 0.5f, 900, 0.5f, 120, 0.3f), OP(1, 0, 0, 84, 0.5f, 250, 0.2f, 100, 0.6f),
    OP(1, 0, 0, 92, 0.5f, 900, 0.5f, 120, 0.3f), OP(2, 0, 0, 78, 0.5f, 350, 0.2f, 120, 0.5f),
    OP(1, 0, 0, 70, 0.5f, 400, 0.1f, 120, 0.5f), OP(3, 0, 0, 60, 0.5f, 300, 0.0f, 100, 0.5f)};
const float kBell[P_COUNT] = {
    GLOBAL(5, 2, 0.55f),
    OP(1, 0, 0, 99, 1.0f, 6000, 0.0f, 2500, 0.3f), OP(3, 50, 0, 80, 1.0f, 4000, 0.0f, 2000, 0.5f),
    OP(1, 0, 3, 95, 1.0f, 5000, 0.0f, 2500, 0.3f), OP(7, 0, 0, 72, 1.0f, 2500, 0.0f, 1500, 0.6f),
    OP(2, 0, -3, 90, 1.0f, 4000, 0.0f, 2000, 0.3f), OP(5, 41, 0, 68, 1.0f, 1500, 0.0f, 1000, 0.6f)};
const float kBrass[P_COUNT] = {
    GLOBAL(22, 7, 0.55f),
    OP(1, 0, 0, 99, 60.0f, 400, 0.8f, 250, 0.4f), OP(1, 0, 0, 74, 40.0f, 600, 0.6f, 250, 0.6f),
    OP(1, 0, 2, 96, 70.0f, 400, 0.8f, 250, 0.4f), OP(1, 0, -2, 96, 70.0f, 400, 0.8f, 250, 0.4f),
    OP(1, 0, 0, 94, 80.0f, 400, 0.8f, 250, 0.4f), OP(1, 0, 0, 80, 30.0f, 500, 0.6f, 250, 0.5f)};
const float kOrgan[P_COUNT] = {
    GLOBAL(32, 0, 0.45f),
    OP(1, 0, 0, 92, 2.0f, 50, 1.0f, 60, 0.0f), OP(2, 0, 0, 86, 2.0f, 50, 1.0f, 60, 0.0f),
    OP(0, 0, 0, 90, 2.0f, 50, 1.0f, 60, 0.0f), OP(3, 0, 0, 80, 2.0f, 50, 1.0f, 60, 0.0f),
    OP(4, 0, 0, 76, 2.0f, 50, 1.0f, 60, 0.0f), OP(8, 0, 0, 70, 2.0f, 50, 1.0f, 60, 0.0f)};
#undef GLOBAL
#undef OP
const daw_preset_desc_t kPresets[] = {
    {"Init", "Basic", nullptr}, {"Electric Piano", "Keys", kEPiano}, {"FM Bass", "Bass", kBass},
    {"Tubular Bell", "Bell", kBell}, {"Brass", "Brass", kBrass}, {"Drawbar Organ", "Organ", kOrgan},
};

constexpr uint32_t kMaxVoices = 8;

struct Voice : VoiceTag {
    float phase[kOps] = {}, inc[kOps] = {}, gain[kOps] = {}, out[kOps] = {};
    float fb1 = 0, fb2 = 0;
    float note = 0;  // key + pitch expression, semitones
    float vel01 = 0.8f;
    dsp::Adsr env[kOps];
    bool active() const
    {
        for (int o = 0; o < kOps; o++)
            if (env[o].active())
                return true;
        return false;
    }
};

class Fm6 : public Plugin<Fm6, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();

    void reset()
    {
        for (auto &v : pool_.voice) {
            for (auto &e : v.env)
                e.kill();
            v.held = false;
        }
        level_.setTime(0.02f, sampleRate());
        level_.snap(param(P_LEVEL));
        vib_ = 0;
        bend_ = 1.0f;
    }
    void onStart(const daw_process_t &) { level_.snap(param(P_LEVEL)); }
    void onParam(uint32_t id, float v)
    {
        if (id == P_LEVEL)
            level_.target = v;
        else if (id == P_POLY)
            pool_.setLimit((uint32_t)v);
        else if (id >= P_OP0) {
            const int o = (int)((id - P_OP0) / kOpParams);
            const uint32_t k = (id - P_OP0) % kOpParams;
            for (auto &vc : pool_.voice) {
                if (k >= OP_ATTACK && k <= OP_RELEASE)
                    setEnv(vc, o);
                else if (k == OP_LEVEL || k == OP_VEL)
                    vc.gain[o] = opGain(o, vc.vel01);
                else
                    retune(vc);
            }
        } else if (id == P_TRANSPOSE)
            for (auto &vc : pool_.voice)
                retune(vc);
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
            v->vel01 = e.u.note.velocity;
            v->note = (float)e.u.note.key + e.u.note.pitch;
            for (int o = 0; o < kOps; o++) {
                v->phase[o] = 0;
                v->out[o] = 0;
                setEnv(*v, o);
                v->env[o].gate(true);
                v->gain[o] = opGain(o, v->vel01);
            }
            v->fb1 = v->fb2 = 0;
            retune(*v);
            break;
        }
        case DAW_EV_NOTE_OFF: {
            Voice *v = pool_.find(e.u.note.note_id, e.u.note.key);
            if (v) {
                v->held = false;
                for (auto &env : v->env)
                    env.gate(false);
            }
            break;
        }
        case DAW_EV_CHANNEL_PITCH:
        case DAW_EV_PITCH_BEND:
            bend_ = dsp::fastExp2(e.u.midi.value * (e.type == DAW_EV_CHANNEL_PITCH ? 1.f : 2.f) / 12.0f);
            for (auto &vc : pool_.voice)
                retune(vc);
            break;
        case DAW_EV_ALL_NOTES_OFF:
            for (auto &v : pool_.voice) {
                v.held = false;
                for (auto &env : v.env)
                    env.gate(false);
            }
            break;
        case DAW_EV_ALL_SOUND_OFF:
            for (auto &v : pool_.voice) {
                v.held = false;
                for (auto &env : v.env)
                    env.kill();
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
        const Algo al = kAlgos[(int)param(P_ALGO) - 1];
        const float fbAmt = param(P_FEEDBACK) > 0.5f ? dsp::fastExp2(param(P_FEEDBACK) - 7.0f) * 0.25f : 0.0f;  // turns
        int nCar = 0;
        for (int o = 0; o < kOps; o++)
            nCar += (al.carriers >> o) & 1;
        const float carNorm = 1.0f / (float)(nCar ? nCar : 1);
        // vibrato: one LFO for all voices, applied per block (its rate is far below the block rate)
        const float vibDepth = param(P_VIB_DEPTH) * 0.01f / 12.0f;  // cents -> octaves
        vib_ += param(P_VIB_RATE) * (float)n / sampleRate();
        vib_ -= (float)(int)vib_;
        const float vibFactor = vibDepth > 0 ? dsp::fastExp2(vibDepth * dsp::sinTurns(vib_)) : 1.0f;
        for (auto &v : pool_.voice) {
            if (!v.active())
                continue;
            float inc[kOps];
            for (int o = 0; o < kOps; o++)
                inc[o] = v.inc[o] * vibFactor;
            renderVoice(v, al, inc, fbAmt, carNorm, L, n);
        }
        for (uint32_t i = 0; i < n; i++) {
            const float g = level_.next();
            L[i] = dsp::sanitize(L[i] * g);
            R[i] = L[i];
        }
    }
    uint32_t voices() const { return pool_.activeCount(); }

private:
    // Process small operator tiles: coefficients, routing and carrier decisions are
    // constant across the inner loop. Scratch stays on the stack (512 B), never in
    // a per-instance allocation. The two multi-operator feedback paths (4 and 6)
    // stay sample-ordered inside their feedback group; independent operators tile.
#ifdef __XTENSA__
    __attribute__((section(".iram1.daw_fm6"), noinline))
#endif
    static void renderVoice(Voice &v, const Algo &al, const float *inc, float fbAmt,
                            float carNorm, float *L, uint32_t n)
    {
        constexpr uint32_t kTile = 16;
        float out[kOps][kTile], sum[kTile], modulation[kTile];
        for (uint32_t start = 0; start < n; start += kTile) {
            const uint32_t count = n - start < kTile ? n - start : kTile;
            for (uint32_t i = 0; i < count; ++i) sum[i] = 0;
            for (int o = kOps - 1; o >= 0; --o) {
                if (o == al.fbDst && al.fbSrc != al.fbDst) {
                    for (uint32_t i = 0; i < count; ++i) {
                        for (int g = o; g >= al.fbSrc; --g) {
                            float m = 0;
                            for (int s = g + 1; s < kOps; ++s)
                                if (al.mod[g] & (1u << s)) m += out[s][i];
                            m *= 2.0f;
                            if (g == al.fbDst)
                                m += 0.5f * (v.fb1 + v.fb2) * fbAmt * 4.0f;
                            const float env = v.env[g].next();
                            float y = 0;
                            if (v.gain[g] > 0.0f) {
                                y = fm6dsp::sineTurns(v.phase[g] + m) * env * v.gain[g];
                            }
                            out[g][i] = v.out[g] = y;
                            v.phase[g] += inc[g];
                            if (v.phase[g] >= 1.0f) v.phase[g] -= 1.0f;
                            if (al.carriers & (1u << g)) sum[i] += y;
                        }
                        v.fb2 = v.fb1;
                        v.fb1 = v.out[al.fbSrc];
                    }
                    o = al.fbSrc;
                    continue;
                }
                for (uint32_t i = 0; i < count; ++i) modulation[i] = 0;
                for (int s = o + 1; s < kOps; ++s)
                    if (al.mod[o] & (1u << s))
                        for (uint32_t i = 0; i < count; ++i) modulation[i] += out[s][i];
                dsp::Adsr env = v.env[o];
                float phase = v.phase[o], fb1 = v.fb1, fb2 = v.fb2;
                const float step = inc[o], gain = v.gain[o];
                const bool feedback = o == al.fbDst, carrier = al.carriers & (1u << o);
                for (uint32_t i = 0; i < count; ++i) {
                    float m = modulation[i] * 2.0f;
                    if (feedback) m += 0.5f * (fb1 + fb2) * fbAmt * 4.0f;
                    const float e = env.next();
                    float y = 0;
                    if (gain > 0.0f) {
                        y = fm6dsp::sineTurns(phase + m) * e * gain;
                    }
                    out[o][i] = y;
                    phase += step;
                    if (phase >= 1.0f) phase -= 1.0f;
                    if (carrier) sum[i] += y;
                    if (feedback) { fb2 = fb1; fb1 = y; }
                }
                v.phase[o] = phase;
                v.env[o] = env;
                v.out[o] = out[o][count - 1];
                if (feedback) { v.fb1 = fb1; v.fb2 = fb2; }
            }
            for (uint32_t i = 0; i < count; ++i) L[start + i] += sum[i] * carNorm;
        }
    }
    float opGain(int o, float vel) const
    {
        const float sens = param(op(o, OP_VEL));
        return levelGain(param(op(o, OP_LEVEL))) * (1.0f - sens + sens * vel);
    }
    void setEnv(Voice &v, int o)
    {
        v.env[o].set(param(op(o, OP_ATTACK)) * 0.001f, param(op(o, OP_DECAY)) * 0.001f, param(op(o, OP_SUSTAIN)),
                     param(op(o, OP_RELEASE)) * 0.001f, sampleRate());
    }
    void retune(Voice &v)
    {
        const float base = dsp::midiToHz(v.note + param(P_TRANSPOSE)) * bend_ / sampleRate();
        for (int o = 0; o < kOps; o++) {
            const float ratio = coarseRatio((int)param(op(o, OP_COARSE))) * (1.0f + param(op(o, OP_FINE)) * 0.01f);
            const float detune = dsp::fastExp2(param(op(o, OP_DETUNE)) * (1.5f / 1200.0f));  // 1.5 cents per step
            float inc = base * ratio * detune;
            v.inc[o] = inc < 0.49f ? inc : 0.49f;
        }
    }
    VoicePool<Voice, kMaxVoices> pool_;
    dsp::Smoother level_;
    float vib_ = 0, bend_ = 1.0f;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fm6",
    .name = "FM6",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Six-operator FM synth with the DX7's 32 algorithms, feedback and per-operator envelopes",
    .category = "Synth",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_POLYPHONIC | DAW_CAP_VELOCITY | DAW_CAP_PITCH_BEND | DAW_CAP_NOTE_PITCH,
    .max_voices = kMaxVoices,
    .param_count = P_COUNT,
    .params = kParams.p,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(Fm6),
    .mem_large_bytes = 0,
    .cpu_permille_base = 2,
    .cpu_permille_voice = 50,  // estimate (six sines per voice and sample); to be measured on the board
    DAWSDK_CALLBACKS(Fm6),
};

const daw_plugin_descriptor_t *Fm6::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fm6(void) { return &kDescriptor; }
