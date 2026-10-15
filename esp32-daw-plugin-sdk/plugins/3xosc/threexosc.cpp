// SPDX-License-Identifier: MIT
// 3xOSC: three-oscillator subtractive synth (original implementation; the concept follows the
// classic "three oscillators into one filter" design). Per voice: 3 band-limited oscillators
// (sine / triangle / saw / pulse / noise) with octave, semitone and fine tuning, a state-variable
// filter with its own ADSR and key tracking, an amp ADSR, independent pan and pitch.
// Global: an LFO (pitch / cutoff / pulse width / amplitude), portamento and slide, polyphony limit.
//
// Slide (DAW_NOTE_FLAG_SLIDE): the newest sounding voice glides to the new key without
// retriggering its envelopes; the glide time is the "Glide" parameter (or the event's slide_ms).
// Portamento mode "Always" glides every new voice from the previous key.
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"
#include "dawsdk/voices.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t {
    // oscillators (3 x 6)
    P_O1_WAVE, P_O1_OCT, P_O1_SEMI, P_O1_FINE, P_O1_LEVEL, P_O1_PW,
    P_O2_WAVE, P_O2_OCT, P_O2_SEMI, P_O2_FINE, P_O2_LEVEL, P_O2_PW,
    P_O3_WAVE, P_O3_OCT, P_O3_SEMI, P_O3_FINE, P_O3_LEVEL, P_O3_PW,
    // amp envelope
    P_A_ATT, P_A_DEC, P_A_SUS, P_A_REL,
    // filter
    P_F_TYPE, P_F_CUT, P_F_RES, P_F_ENV, P_F_KEY, P_F_ATT, P_F_DEC, P_F_SUS, P_F_REL,
    // lfo
    P_L_WAVE, P_L_RATE, P_L_DEST, P_L_AMT,
    // voice
    P_GLIDE, P_PORTA, P_POLY, P_VELSENS, P_VOLUME,
    P_COUNT
};

const char *const kWaves[] = {"Sine", "Triangle", "Saw", "Square", "Noise"};
const char *const kFilterTypes[] = {"Low pass", "Band pass", "High pass", "Notch"};
const char *const kLfoWaves[] = {"Sine", "Triangle", "Square", "Saw"};
const char *const kLfoDest[] = {"Off", "Pitch", "Cutoff", "Pulse width", "Amplitude"};
const char *const kPorta[] = {"Off", "Slide notes", "Always"};

#define OSC(n, grp, wave, oct, lvl)                                                                                                      \
    {P_O##n##_WAVE, "osc" #n "_wave", "Wave", grp, "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 4, wave, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 5, kWaves}, \
    {P_O##n##_OCT, "osc" #n "_oct", "Octave", grp, "oct", DAW_PARAM_INT, DAW_SCALE_BIPOLAR, -3, 3, oct, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr}, \
    {P_O##n##_SEMI, "osc" #n "_semi", "Coarse", grp, "st", DAW_PARAM_INT, DAW_SCALE_BIPOLAR, -12, 12, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr}, \
    {P_O##n##_FINE, "osc" #n "_fine", "Fine", grp, "ct", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -100, 100, 0, DAW_PARAM_AUTOMATABLE, 0, nullptr}, \
    {P_O##n##_LEVEL, "osc" #n "_level", "Level", grp, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0, 1, lvl, DAW_PARAM_AUTOMATABLE, 0, nullptr}, \
    {P_O##n##_PW, "osc" #n "_pw", "Pulse width", grp, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.05f, 0.95f, 0.5f, DAW_PARAM_AUTOMATABLE, 0, nullptr}

#define FP(id, key, name, grp, unit, scale, mn, mx, def) \
    {id, key, name, grp, unit, DAW_PARAM_FLOAT, scale, mn, mx, def, DAW_PARAM_AUTOMATABLE, 0, nullptr}

const daw_param_desc_t kParams[P_COUNT] = {
    OSC(1, "Osc 1", 2, 0, 0.7f),
    OSC(2, "Osc 2", 2, 0, 0.5f),
    OSC(3, "Osc 3", 3, -1, 0.4f),
    FP(P_A_ATT, "amp_attack", "Attack", "Amp envelope", "ms", DAW_SCALE_LOG, 0.5f, 5000, 2),
    FP(P_A_DEC, "amp_decay", "Decay", "Amp envelope", "ms", DAW_SCALE_LOG, 1, 10000, 300),
    FP(P_A_SUS, "amp_sustain", "Sustain", "Amp envelope", "", DAW_SCALE_LINEAR, 0, 1, 0.7f),
    FP(P_A_REL, "amp_release", "Release", "Amp envelope", "ms", DAW_SCALE_LOG, 1, 10000, 150),
    {P_F_TYPE, "flt_type", "Type", "Filter", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 3, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 4, kFilterTypes},
    FP(P_F_CUT, "cutoff", "Cutoff", "Filter", "Hz", DAW_SCALE_LOG, 20, 20000, 4000),
    FP(P_F_RES, "resonance", "Resonance", "Filter", "", DAW_SCALE_LINEAR, 0, 1, 0.2f),
    FP(P_F_ENV, "flt_env", "Env amount", "Filter", "oct", DAW_SCALE_BIPOLAR, -8, 8, 2),
    FP(P_F_KEY, "flt_key", "Key track", "Filter", "", DAW_SCALE_LINEAR, 0, 1, 0.5f),
    FP(P_F_ATT, "flt_attack", "Attack", "Filter envelope", "ms", DAW_SCALE_LOG, 0.5f, 5000, 2),
    FP(P_F_DEC, "flt_decay", "Decay", "Filter envelope", "ms", DAW_SCALE_LOG, 1, 10000, 250),
    FP(P_F_SUS, "flt_sustain", "Sustain", "Filter envelope", "", DAW_SCALE_LINEAR, 0, 1, 0.2f),
    FP(P_F_REL, "flt_release", "Release", "Filter envelope", "ms", DAW_SCALE_LOG, 1, 10000, 200),
    {P_L_WAVE, "lfo_wave", "Wave", "LFO", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 3, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 4, kLfoWaves},
    FP(P_L_RATE, "lfo_rate", "Rate", "LFO", "Hz", DAW_SCALE_LOG, 0.05f, 30, 5),
    {P_L_DEST, "lfo_dest", "Destination", "LFO", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 4, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 5, kLfoDest},
    FP(P_L_AMT, "lfo_amount", "Amount", "LFO", "", DAW_SCALE_LINEAR, 0, 1, 0.2f),
    FP(P_GLIDE, "glide", "Glide", "Voice", "ms", DAW_SCALE_LOG, 1, 2000, 80),
    {P_PORTA, "porta", "Portamento", "Voice", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 2, 1, DAW_PARAM_NO_SMOOTH, 3, kPorta},
    {P_POLY, "polyphony", "Polyphony", "Voice", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 8, 6, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    FP(P_VELSENS, "vel_sens", "Velocity", "Voice", "", DAW_SCALE_LINEAR, 0, 1, 0.7f),
    FP(P_VOLUME, "volume", "Volume", "Voice", "", DAW_SCALE_DB, 0, 1, 0.5f),
};

constexpr uint32_t kMaxVoices = 8;
constexpr uint32_t kCtl = 16;  // control-rate period (samples) for filter / pitch modulation

// Cache the frequency the filter actually uses. An envelope above the Nyquist
// clamp otherwise repeats exactly the same tanf/coefficient work every period.
struct VoiceFilter : dsp::Svf {
    void set(float hz, float res, float sr)
    {
        dsp::Svf::set(dsp::clamp(hz, 10.0f, sr * 0.45f), res, sr);
    }
};

struct Voice : VoiceTag {
    dsp::Osc osc[3];
    dsp::Adsr amp, flt;
    VoiceFilter svf;
    float pitch = 60;       // current (gliding) pitch, semitones
    float target = 60;      // target pitch
    float glideStep = 0;    // semitones per control period
    float exprPitch = 0;    // per-note pitch expression
    float oscillatorPitch = -10000;
    uint32_t oscillatorRevision = 0;
    float vel = 1;
    float panL = 1, panR = 1;
    float fade = 1;         // stolen voice fast fade (1 = none)
    bool active() const { return amp.active(); }
};

class ThreeOsc : public Plugin<ThreeOsc, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onStart(const daw_process_t &) { volume_.snap(volume_.target); }

    void reset()
    {
        for (auto &v : pool_.voice) {
            v.amp.kill();
            v.flt.kill();
            v.svf.reset();
        }
        volume_.setTime(0.01f, sampleRate());
        volume_.snap(param(P_VOLUME));
        lfoPhase_ = 0;
        lastKey_ = -1;
        ctl_ = 0;
        midiBend_ = 0;
    }
    void onParam(uint32_t id, float v)
    {
        if (id < P_A_ATT) ++oscillatorRevision_;
        if (id == P_VOLUME)
            volume_.target = v;
        else if (id == P_POLY)
            pool_.setLimit((uint32_t)v);
        else if (id >= P_A_ATT && id <= P_F_REL)
            envDirty_ = true;
    }
    void onEvent(const daw_event_t &e)
    {
        switch (e.type) {
        case DAW_EV_NOTE_ON: noteOn(e); break;
        case DAW_EV_CHANNEL_PITCH: midiBend_ = e.u.midi.value; break;
        case DAW_EV_PITCH_BEND: midiBend_ = e.u.midi.value * 2.f; break;
        case DAW_EV_NOTE_OFF: {
            Voice *v = pool_.find(e.u.note.note_id, e.u.note.key);
            if (v) {
                v->held = false;
                v->amp.gate(false);
                v->flt.gate(false);
            }
            break;
        }
        case DAW_EV_NOTE_EXPR: {
            Voice *v = pool_.find(e.u.expr.note_id, e.u.expr.key);
            if (!v)
                break;
            if (e.u.expr.expr == DAW_EXPR_PAN)
                dsp::panGains(e.u.expr.value, v->panL, v->panR);
            else if (e.u.expr.expr == DAW_EXPR_PITCH)
                v->exprPitch = e.u.expr.value;
            break;
        }
        case DAW_EV_ALL_NOTES_OFF:
            for (auto &v : pool_.voice) {
                v.held = false;
                v.amp.gate(false);
                v.flt.gate(false);
            }
            break;
        case DAW_EV_ALL_SOUND_OFF:
            for (auto &v : pool_.voice) {
                v.held = false;
                v.amp.kill();
                v.flt.kill();
                v.svf.reset();
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
        if (envDirty_) {
            envDirty_ = false;
            for (auto &v : pool_.voice)
                setEnvelopes(v);
        }
        const float sr = sampleRate();
        const int lfoDest = (int)param(P_L_DEST);
        const float lfoAmt = param(P_L_AMT);
        const float lfoInc = param(P_L_RATE) / sr;
        const auto ftype = (dsp::Svf::Mode)(int)param(P_F_TYPE);
        const float lvl[3] = {param(P_O1_LEVEL), param(P_O2_LEVEL), param(P_O3_LEVEL)};
        const float pw[3] = {param(P_O1_PW), param(P_O2_PW), param(P_O3_PW)};
        const float velSens = param(P_VELSENS);
        for (int k = 0; k < 3; k++) {
            const uint32_t base = P_O1_WAVE + 6 * k;
            oscOffset_[k] = 12.0f * param(base + 1) + param(base + 2) + param(base + 3) * 0.01f;
        }

        uint32_t i = 0;
        while (i < n) {
            // control-rate segment: up to the next kCtl boundary
            uint32_t seg = kCtl - ctl_;
            if (seg > n - i)
                seg = n - i;
            float lfo = lfoValue();
            lfoPhase_ += lfoInc * (float)seg;
            lfoPhase_ -= (float)(int32_t)lfoPhase_;
            float lfoPitch = lfoDest == 1 ? lfo * lfoAmt * 2.0f : 0.0f;          // +-2 semitones
            float lfoCut = lfoDest == 2 ? lfo * lfoAmt * 3.0f : 0.0f;            // +-3 octaves
            float lfoPw = lfoDest == 3 ? lfo * lfoAmt * 0.4f : 0.0f;
            float lfoAmp = lfoDest == 4 ? 1.0f - lfoAmt * 0.5f * (1.0f + lfo) : 1.0f;

            for (auto &v : pool_.voice) {
                if (!v.active())
                    continue;
                // control rate: glide, pitch, filter
                // glide
                if (v.pitch != v.target) {
                    float d = v.target - v.pitch;
                    float step = v.glideStep * (float)seg / (float)kCtl;
                    if (d > 0 ? d <= step : -d <= step)
                        v.pitch = v.target;
                    else
                        v.pitch += d > 0 ? step : -step;
                }
                float p = v.pitch + v.exprPitch + lfoPitch;
                if (midiBend_ != 0) p += midiBend_;
                const bool retune = p != v.oscillatorPitch || v.oscillatorRevision != oscillatorRevision_;
                for (int k = 0; k < 3; k++) {
                    if (retune) v.osc[k].setFreq(dsp::midiToHz(p + oscOffset_[k]), sr);
                    v.osc[k].pw = dsp::clamp(pw[k] + lfoPw, 0.05f, 0.95f);
                }
                v.oscillatorPitch = p;
                v.oscillatorRevision = oscillatorRevision_;
                float fenv = v.flt.level;
                float oct = param(P_F_ENV) * fenv + param(P_F_KEY) * (p - 60.0f) / 12.0f + lfoCut;
                v.svf.set(param(P_F_CUT) * dsp::fastExp2(oct), param(P_F_RES), sr);
                float gain = (1.0f - velSens + velSens * v.vel) * lfoAmp * 0.35f;
                float *l = L + i, *r = R + i;
                renderVoice(v, l, r, seg, lvl, gain, ftype);
            }
            for (uint32_t s = 0; s < seg; s++) {
                float g = volume_.next();
                L[i + s] *= g;
                R[i + s] *= g;
            }
            i += seg;
            ctl_ = (ctl_ + seg) % kCtl;
        }
    }
    uint32_t voices() const { return pool_.activeCount(); }

private:
    static float polyBlep(float t, float dt)
    {
        if (t < dt) {
            t = dsp::fastDiv(t, dt);
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt) {
            t = dsp::fastDiv(t - 1.0f, dt);
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    template <dsp::Wave W>
    static float oscillatorNext(dsp::Osc &o, dsp::Rng &rng)
    {
        const float t = o.phase;
        float out;
        if constexpr (W == dsp::Wave::Sine)
            out = dsp::sinTurnsUnit(t);
        else if constexpr (W == dsp::Wave::Triangle)
            out = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
        else if constexpr (W == dsp::Wave::Saw)
            out = 2.0f * t - 1.0f - polyBlep(t, o.inc);
        else if constexpr (W == dsp::Wave::Square) {
            out = t < o.pw ? 1.0f : -1.0f;
            out += polyBlep(t, o.inc);
            float t2 = t - o.pw + 1.0f;
            t2 -= (float)(int32_t)t2;
            out -= polyBlep(t2, o.inc);
        } else
            out = rng.bipolar();
        o.phase += o.inc;
        if (o.phase >= 1.0f) o.phase -= 1.0f;
        return out;
    }

    template <dsp::Wave W0, dsp::Wave W1, dsp::Wave W2>
    void mixedTile(Voice &v, float *out, uint32_t n, const float *lvl)
    {
        dsp::Osc o0 = v.osc[0], o1 = v.osc[1], o2 = v.osc[2];
        o0.wave = W0;
        o1.wave = W1;
        o2.wave = W2;
        for (uint32_t s = 0; s < n; s++)
            out[s] = oscillatorNext<W0>(o0, rng_) * lvl[0] + oscillatorNext<W1>(o1, rng_) * lvl[1] + oscillatorNext<W2>(o2, rng_) * lvl[2];
        v.osc[0].phase = o0.phase;
        v.osc[1].phase = o1.phase;
        v.osc[2].phase = o2.phase;
    }

    // One waveform decision per oscillator tile. Keeping the oscillator's state in
    // registers avoids spilling it around the other oscillators, envelopes and SVF.
    template <dsp::Wave W>
    static void oscillatorTile(dsp::Osc &osc, float *out, uint32_t n)
    {
        dsp::Osc o;
        o.phase = osc.phase;
        o.inc = osc.inc;
        o.pw = osc.pw;
        o.wave = W;
        dsp::Rng unused;
        for (uint32_t s = 0; s < n; s++)
            out[s] = oscillatorNext<W>(o, unused);
        osc.phase = o.phase;
    }

    // Permit the S3's multiply/add instructions only in the sample kernels.
    // Pitch and coefficient setup retain their original arithmetic/rounding.
#if defined(__XTENSA__)
    __attribute__((section(".iram1.daw_threeosc"), noinline, optimize("fp-contract=fast")))
#endif
    static void renderOscillator(dsp::Osc &osc, float *out, uint32_t n)
    {
        switch (osc.wave) {
        case dsp::Wave::Sine: oscillatorTile<dsp::Wave::Sine>(osc, out, n); break;
        case dsp::Wave::Triangle: oscillatorTile<dsp::Wave::Triangle>(osc, out, n); break;
        case dsp::Wave::Saw: oscillatorTile<dsp::Wave::Saw>(osc, out, n); break;
        case dsp::Wave::Square: oscillatorTile<dsp::Wave::Square>(osc, out, n); break;
        default:
            // Noise was filled in sample/oscillator order before rendering the
            // independent waveforms. It still advances phase, just like Osc::next.
            for (uint32_t s = 0; s < n; s++) {
                osc.phase += osc.inc;
                if (osc.phase >= 1.0f) osc.phase -= 1.0f;
            }
            break;
        }
    }

    // Stage changes are rare inside a control period. Dispatch once per stage,
    // retain the exact threshold checks, and keep the recurrence in registers.
    template <bool Write>
#if defined(__XTENSA__)
    __attribute__((section(".iram1.daw_threeosc"), noinline, optimize("fp-contract=fast")))
#endif
    static void renderEnvelope(dsp::Adsr &env, float *out, uint32_t n)
    {
        float level = env.level;
        auto stage = env.stage;
        const float aInc = env.aInc, dCoef = env.dCoef, rCoef = env.rCoef, sustain = env.sustain;
        uint32_t s = 0;
        while (s < n) {
            switch (stage) {
            case dsp::Adsr::Attack:
                while (s < n && stage == dsp::Adsr::Attack) {
                    level += aInc;
                    if (level >= 1.0f) { level = 1.0f; stage = dsp::Adsr::Decay; }
                    if constexpr (Write) out[s] = level;
                    s++;
                }
                break;
            case dsp::Adsr::Decay:
                while (s < n && stage == dsp::Adsr::Decay) {
                    level += (sustain - level) * dCoef;
                    if (level - sustain < 1e-4f) { level = sustain; stage = dsp::Adsr::Sustain; }
                    if constexpr (Write) out[s] = level;
                    s++;
                }
                break;
            case dsp::Adsr::Release:
                while (s < n && stage == dsp::Adsr::Release) {
                    level -= level * rCoef;
                    if (level < 1e-4f) { level = 0; stage = dsp::Adsr::Idle; }
                    if constexpr (Write) out[s] = level;
                    s++;
                }
                break;
            case dsp::Adsr::Sustain:
                level = sustain;
                [[fallthrough]];
            default:
                if constexpr (Write)
                    while (s < n) out[s++] = level;
                else
                    s = n;
                break;
            }
        }
        env.level = level;
        env.stage = stage;
    }

    // Only the per-sample voice kernel needs IRAM; control-rate pitch/filter setup
    // stays in flash to leave internal RAM for multiple instrument/effect instances.
#if defined(__XTENSA__)
    __attribute__((section(".iram1.daw_threeosc"), noinline, optimize("fp-contract=fast")))
#endif
    void renderVoice(Voice &v, float *l, float *r, uint32_t seg, const float *lvl,
                     float gain, dsp::Svf::Mode ftype)
    {
        float osc[3][kCtl];
        float mixed[kCtl];
        float amp[kCtl];
        const bool saws = v.osc[0].wave == dsp::Wave::Saw && v.osc[1].wave == dsp::Wave::Saw && v.osc[2].wave == dsp::Wave::Saw;
        const bool pluck = v.osc[0].wave == dsp::Wave::Square && v.osc[1].wave == dsp::Wave::Saw && v.osc[2].wave == dsp::Wave::Sine;
        if (saws)
            mixedTile<dsp::Wave::Saw, dsp::Wave::Saw, dsp::Wave::Saw>(v, mixed, seg, lvl);
        else if (pluck)
            mixedTile<dsp::Wave::Square, dsp::Wave::Saw, dsp::Wave::Sine>(v, mixed, seg, lvl);
        else {
            const bool noise[3] = {v.osc[0].wave == dsp::Wave::Noise, v.osc[1].wave == dsp::Wave::Noise,
                                   v.osc[2].wave == dsp::Wave::Noise};
            if (noise[0] || noise[1] || noise[2])
                for (uint32_t s = 0; s < seg; s++)
                    for (uint32_t k = 0; k < 3; k++)
                        if (noise[k]) osc[k][s] = rng_.bipolar();
            for (uint32_t k = 0; k < 3; k++)
                renderOscillator(v.osc[k], osc[k], seg);
            for (uint32_t s = 0; s < seg; s++)
                mixed[s] = osc[0][s] * lvl[0] + osc[1][s] * lvl[1] + osc[2][s] * lvl[2];
        }
        renderEnvelope<false>(v.flt, nullptr, seg);
        renderEnvelope<true>(v.amp, amp, seg);
        // Output pointers cannot alias this local filter: coefficients and states
        // can stay in registers until the end of the tile.
        auto filter = v.svf;
        const float panL = v.panL, panR = v.panR, fadeInc = fadeInc_;
        float fade = v.fade;
        for (uint32_t s = 0; s < seg; s++) {
            float x = mixed[s];
            float y = filter.process(x, ftype) * amp[s] * gain;
            if (fade < 1.0f) {  // a stolen voice's attack: 1.5 ms ramp
                y *= fade;
                fade += fadeInc;
            }
            l[s] += y * panL;
            r[s] += y * panR;
        }
        v.svf.ic1 = filter.ic1;
        v.svf.ic2 = filter.ic2;
        v.fade = fade;
    }
    float lfoValue() const
    {
        float ph = lfoPhase_;
        switch ((int)param(P_L_WAVE)) {
        case 1: return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
        case 2: return ph < 0.5f ? 1.0f : -1.0f;
        case 3: return 2 * ph - 1;
        default: return dsp::sinTurns(ph);
        }
    }
    void setEnvelopes(Voice &v)
    {
        const float sr = sampleRate();
        v.amp.set(param(P_A_ATT) * 1e-3f, param(P_A_DEC) * 1e-3f, param(P_A_SUS), param(P_A_REL) * 1e-3f, sr);
        v.flt.set(param(P_F_ATT) * 1e-3f, param(P_F_DEC) * 1e-3f, param(P_F_SUS), param(P_F_REL) * 1e-3f, sr);
    }
    void noteOn(const daw_event_t &e)
    {
        for (int k = 0; k < 3; k++) {
            uint32_t base = P_O1_WAVE + 6 * k;
            oscOffset_[k] = 12.0f * param(base + 1) + param(base + 2) + param(base + 3) * 0.01f;
        }
        const float key = (float)e.u.note.key;
        const int porta = (int)param(P_PORTA);
        const float glideMs = e.u.note.slide_ms ? (float)e.u.note.slide_ms : param(P_GLIDE);
        const bool slide = (e.flags & DAW_NOTE_FLAG_SLIDE) && porta != 0;
        Voice *v = nullptr;
        if (slide || (e.flags & DAW_NOTE_FLAG_LEGATO))
            v = pool_.newest();
        if (v) {
            // legato / slide: keep envelopes running, glide the pitch to the new key
            v->noteId = e.u.note.note_id;
            v->key = e.u.note.key;
            v->held = true;
            v->target = key;
            setGlide(*v, glideMs);
            lastKey_ = (int)key;
            return;
        }
        bool stolen;
        v = pool_.allocate(stolen);
        if (stolen) {
            report(DAW_REPORT_VOICE_STEAL);
            v->fade = 0.0f;
        } else {
            v->fade = 1.0f;
            v->svf.reset();
            for (auto &o : v->osc)
                o.phase = 0;
        }
        fadeInc_ = 1.0f / (0.0015f * sampleRate());
        v->noteId = e.u.note.note_id;
        v->key = e.u.note.key;
        v->held = true;
        v->vel = e.u.note.velocity;
        v->exprPitch = e.u.note.pitch;
        dsp::panGains(e.u.note.pan, v->panL, v->panR);
        for (int k = 0; k < 3; k++)
            v->osc[k].wave = (dsp::Wave)(int)param(P_O1_WAVE + 6 * k);
        v->target = key;
        // portamento "Always", or a slide with no sounding note to take over: glide from the last key
        if ((porta == 2 || slide) && lastKey_ >= 0) {
            v->pitch = (float)lastKey_;
            setGlide(*v, glideMs);
        } else {
            v->pitch = key;
        }
        setEnvelopes(*v);
        v->amp.gate(true);
        v->flt.gate(true);
        lastKey_ = (int)key;
    }
    void setGlide(Voice &v, float ms)
    {
        float periods = ms * 1e-3f * sampleRate() / (float)kCtl;
        float d = v.target - v.pitch;
        if (d < 0)
            d = -d;
        v.glideStep = periods < 1.0f ? 1000.0f : d / periods;
        if (v.glideStep <= 0)
            v.glideStep = 1000.0f;
    }

    VoicePool<Voice, kMaxVoices> pool_;
    dsp::Smoother volume_;
    dsp::Rng rng_;
    float oscOffset_[3] = {};
    float midiBend_ = 0;
    float lfoPhase_ = 0;
    float fadeInc_ = 0.015f;
    int lastKey_ = -1;
    uint32_t ctl_ = 0;
    uint32_t oscillatorRevision_ = 1;
    bool envDirty_ = true;
};

// preset values in parameter order
const float kSuperSaw[P_COUNT] = {2, 0, 0, -12, 0.7f, 0.5f, 2, 0, 0, 12, 0.7f, 0.5f, 2, -1, 0, 0, 0.5f, 0.5f,
                                  15, 600, 0.8f, 400, 0, 5000, 0.15f, 1.5f, 0.5f, 5, 400, 0.4f, 400, 0, 5, 0, 0.2f, 80, 1, 6, 0.6f, 0.45f};
const float kBass[P_COUNT] = {2, -1, 0, 0, 0.8f, 0.5f, 3, -1, 0, 7, 0.6f, 0.35f, 0, -2, 0, 0, 0.6f, 0.5f,
                              1, 250, 0.5f, 80, 0, 600, 0.45f, 3.5f, 0.3f, 1, 180, 0.0f, 120, 0, 5, 0, 0.2f, 60, 1, 1, 0.8f, 0.55f};
const float kPluck[P_COUNT] = {3, 0, 0, 0, 0.7f, 0.3f, 2, 1, 0, 5, 0.4f, 0.5f, 0, 0, 7, 0, 0.3f, 0.5f,
                               1, 350, 0.0f, 300, 0, 900, 0.3f, 4.0f, 0.6f, 1, 220, 0.0f, 250, 0, 5, 0, 0.2f, 80, 0, 8, 0.9f, 0.5f};
const float kPad[P_COUNT] = {2, 0, 0, -8, 0.6f, 0.5f, 1, 0, 0, 8, 0.6f, 0.5f, 0, 1, 0, 0, 0.3f, 0.5f,
                             900, 1500, 0.8f, 1800, 0, 2200, 0.25f, 1.0f, 0.3f, 1200, 2000, 0.5f, 1800, 0, 0.3f, 2, 0.35f, 80, 0, 8, 0.4f, 0.4f};
const float kLead[P_COUNT] = {3, 0, 0, 0, 0.7f, 0.25f, 2, 0, 0, 6, 0.6f, 0.5f, 0, -1, 0, 0, 0.4f, 0.5f,
                              4, 400, 0.85f, 250, 0, 3000, 0.35f, 1.5f, 0.5f, 4, 300, 0.5f, 300, 0, 5.5f, 1, 0.15f, 60, 2, 1, 0.6f, 0.45f};
const daw_preset_desc_t kPresets[] = {
    {"Init", "Basic", nullptr},  {"Super Saw", "Lead", kSuperSaw}, {"Fat Bass", "Bass", kBass},
    {"Pluck", "Keys", kPluck},   {"Warm Pad", "Pad", kPad},        {"Mono Lead", "Lead", kLead},
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.3xosc",
    .name = "3xOSC",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Three-oscillator polyphonic subtractive synth with filter, envelopes, LFO and glide",
    .category = "Synth",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_PITCH_BEND | DAW_CAP_POLYPHONIC | DAW_CAP_VELOCITY | DAW_CAP_SLIDE | DAW_CAP_NOTE_PAN | DAW_CAP_NOTE_PITCH,
    .max_voices = kMaxVoices,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(ThreeOsc),
    .mem_large_bytes = 0,
    .cpu_permille_base = 2,
    .cpu_permille_voice = 30,
    DAWSDK_CALLBACKS(ThreeOsc),
};

const daw_plugin_descriptor_t *ThreeOsc::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_3xosc(void) { return &kDescriptor; }
