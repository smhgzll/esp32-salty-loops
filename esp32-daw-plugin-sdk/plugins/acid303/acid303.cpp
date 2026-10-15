// SPDX-License-Identifier: MIT
// Acid303: a monophonic TB-303-style bass line synth built on Open303 by Robin Schmidt
// (https://github.com/RobinSchmidt/Open303, MIT, commit 313bf0d; PROVENANCE.md).
//
// From Open303 (third_party/Open303, converted to single precision): the TB-303 ladder filter
// (TeeBeeFilter, mode TB_303), the decay / amplitude envelopes, the slide slew limiter, the RC
// smoothers driving the filter envelope and accent, the de-clicker and the fixed high-pass /
// all-pass / notch filters around the voice, and the voice logic below (note priority, accent,
// slide, the measured cutoff / env-mod mapping) adapted from rosic_Open303.cpp.
//
// Replaced for the ESP32-S3 (single-precision FPU, no allocation in process()):
//  - the mip-mapped wavetable oscillator (2 x 100 KB tables generated with a heap-allocating FFT)
//    by a PolyBLEP oscillator producing the same two waveforms: Open303's SAW303 and its
//    tanh-shaped SQUARE303, blended;
//  - 4x oversampling with a 12th-order direct-form elliptic decimator (unstable in float) by 2x
//    oversampling with a 23-tap halfband FIR;
//  - the std::list of held notes by a fixed array (last-note priority, slide back on release).
// Sequenced notes with DAW_NOTE_FLAG_SLIDE glide without retriggering, DAW_NOTE_FLAG_ACCENT (or
// velocity >= 100/127) plays the accent, exactly as the 303's slide / accent steps.
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

#include "rosic_AnalogEnvelope.h"
#include "rosic_BiquadFilter.h"
#include "rosic_DecayEnvelope.h"
#include "rosic_LeakyIntegrator.h"
#include "rosic_OnePoleFilter.h"
#include "rosic_TeeBeeFilter.h"

using namespace dawsdk;

namespace {

enum : uint32_t { P_WAVE, P_CUTOFF, P_RESO, P_ENVMOD, P_DECAY, P_ACCENT, P_SLIDE, P_LEVEL, P_TUNE, P_SUSTAIN, P_COUNT };

const daw_param_desc_t kParams[P_COUNT] = {
    {P_WAVE, "wave", "Waveform", "Oscillator", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_CUTOFF, "cutoff", "Cutoff", "Filter", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 314.0f, 2394.0f, 600.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RESO, "resonance", "Resonance", "Filter", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 70.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_ENVMOD, "envmod", "Env mod", "Filter", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 50.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DECAY, "decay", "Decay", "Filter", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 200.0f, 2000.0f, 400.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_ACCENT, "accent", "Accent", "Voice", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 60.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_SLIDE, "slide", "Slide time", "Voice", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 10.0f, 500.0f, 60.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_LEVEL, "level", "Level", "Voice", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.7f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_TUNE, "tune", "Tune", "Voice", "st", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -12.0f, 12.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_SUSTAIN, "sustain", "Sustain", "Voice", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};

//                                 wave cutoff reso envmod decay accent slide level tune sustain
const float kPresetClassic[P_COUNT] = {0.0f, 600.0f, 70.0f, 50.0f, 400.0f, 60.0f, 60.0f, 0.7f, 0.0f, 0.0f};
const float kPresetSquelch[P_COUNT] = {0.0f, 420.0f, 92.0f, 85.0f, 300.0f, 90.0f, 60.0f, 0.65f, 0.0f, 0.0f};
const float kPresetSquare[P_COUNT] = {1.0f, 800.0f, 60.0f, 40.0f, 600.0f, 50.0f, 60.0f, 0.7f, 0.0f, 0.0f};
const float kPresetDeep[P_COUNT] = {0.3f, 340.0f, 40.0f, 25.0f, 1200.0f, 30.0f, 80.0f, 0.75f, -12.0f, 0.3f};
const float kPresetLead[P_COUNT] = {0.0f, 1400.0f, 80.0f, 60.0f, 250.0f, 70.0f, 40.0f, 0.6f, 12.0f, 0.0f};
const daw_preset_desc_t kPresets[] = {
    {"Init", "Basic", nullptr},
    {"Classic Acid", "Bass", kPresetClassic},
    {"Squelch", "Bass", kPresetSquelch},
    {"Square Bass", "Bass", kPresetSquare},
    {"Deep Sub", "Bass", kPresetDeep},
    {"Acid Lead", "Lead", kPresetLead},
};

constexpr int kOver = 2;           // oversampling of oscillator and ladder
constexpr uint32_t kHeld = 16;     // held keys remembered for last-note priority

// 23-tap halfband low-pass for the 2x decimation (Kaiser beta 6): the non-zero taps left of the
// centre (the centre is 0.5, the right half mirrors these). Flat to 16 kHz, -80 dB at 30 kHz.
constexpr int kHbHalf = 6;
const float kHb[kHbHalf] = {-0.000430307f, 0.003720102f, -0.013237673f, 0.035121692f, -0.086237624f, 0.311114362f};

class HalfbandDecimator {
public:
    void reset()
    {
        for (float &x : buf_)
            x = 0;
        pos_ = 0;
    }
    // two input samples at the oversampled rate -> one output sample
    float process(float a, float b)
    {
        push(a);
        push(b);
        // newest sample at pos_-1; taps at odd distances from the centre (index 11 back)
        float acc = 0.5f * at(11);
        #pragma GCC unroll 6
        for (int k = 0; k < kHbHalf; k++) {
            const int d = 2 * (kHbHalf - k) - 1;  // 11, 9, ... 1
            acc += kHb[k] * (at(11 - d) + at(11 + d));
        }
        return acc;
    }

private:
    void push(float x)
    {
        buf_[pos_] = x;
        pos_ = (pos_ + 1) & 31;
    }
    float at(int back) const { return buf_[(pos_ - 1 - back) & 31]; }
    float buf_[32] = {};
    int pos_ = 0;
};

inline float fastTanh(float x)
{
    if (x > 4.97f)
        return 1.0f;
    if (x < -4.97f)
        return -1.0f;
    const float x2 = x * x;  // Pade 7/6, |err| < 2e-4 inside the clamp
    return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) / (135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2)));
}

class Acid303 : public Plugin<Acid303, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();

    int32_t init()
    {
        const float sr = sampleRate();
        mainEnv_.setSampleRate(sr);
        ampEnv_.setSampleRate(sr);
        slew_.setSampleRate(sr);
        declick_.setSampleRate(sr);
        rc1_.setSampleRate(sr);
        rc2_.setSampleRate(sr);
        hp2_.setSampleRate(sr);
        allpass_.setSampleRate(sr);
        notch_.setSampleRate(sr);
        hp1_.setSampleRate(kOver * sr);
        filter_.setSampleRate(kOver * sr);
        // Open303's constructor settings
        mainEnv_.setNormalizeSum(false);
        ampEnv_.setAttack(0.0f);
        ampEnv_.setDecay(1230.0f);
        ampEnv_.setSustainLevel(0.0f);
        ampEnv_.setRelease(0.5f);
        ampEnv_.setTauScale(1.0f);
        declick_.setMode(rosic::BiquadFilter::LOWPASS12);
        declick_.setGain(amp2dB(0.70710678f));
        declick_.setFrequency(200.0f);
        rc1_.setTimeConstant(0.0f);
        rc2_.setTimeConstant(15.0f);
        hp1_.setMode(rosic::OnePoleFilter::HIGHPASS);
        hp2_.setMode(rosic::OnePoleFilter::HIGHPASS);
        allpass_.setMode(rosic::OnePoleFilter::ALLPASS);
        notch_.setMode(rosic::BiquadFilter::BANDREJECT);
        hp1_.setCutoff(44.486f);
        hp2_.setCutoff(24.167f);
        allpass_.setCutoff(14.008f);
        notch_.setFrequency(7.5164f);
        notch_.setBandwidth(4.7f);
        filter_.setMode(rosic::TeeBeeFilter::TB_303);
        filter_.setFeedbackHighpassCutoff(150.0f);
        return DAW_OK;
    }
    void reset()
    {
        nHeld_ = 0;
        idle_ = true;
        ampEnv_.reset();
        resetFilters();
        phase_ = 0;
        bend_ = 1.0f;
        mainEnv_.trigger();  // a defined state; the amp envelope is at rest, so this is silent
    }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_CUTOFF:
        case P_ENVMOD: envModMapping(); break;
        case P_RESO: filter_.setResonance(v); break;
        case P_DECAY: normalDecay_ = v; break;
        case P_ACCENT: accent_ = 0.01f * v; break;
        case P_SLIDE: slew_.setTimeConstant(0.2f * v); break;  // Open303's scaling of the slide time
        case P_LEVEL: level_.target = v; break;
        case P_SUSTAIN: ampEnv_.setSustainLevel(v); break;
        default: break;
        }
    }
    void onStart(const daw_process_t &) { level_.snap(level_.target); }

    void onEvent(const daw_event_t &e)
    {
        switch (e.type) {
        case DAW_EV_NOTE_ON: {
            const int key = e.u.note.key;
            const bool accent = (e.flags & DAW_NOTE_FLAG_ACCENT) || e.u.note.velocity >= 100.0f / 127.0f;
            // a slide (or overlapping keys, like holding a 303 key while pressing the next) glides
            const bool glide = !idle_ && ampEnv_.isNoteOn() && ((e.flags & (DAW_NOTE_FLAG_SLIDE | DAW_NOTE_FLAG_LEGATO)) || nHeld_ > 0);
            if (glide)
                slideTo(key, accent);
            else
                trigger(key, accent);
            hold(key);
            break;
        }
        case DAW_EV_NOTE_OFF: {
            const int key = e.u.note.key;
            unhold(key);
            if (nHeld_ == 0)
                ampEnv_.noteOff();
            else if (key == current_)  // slide back to the most recent key still held
                setFreq(held_[nHeld_ - 1]);
            break;
        }
        case DAW_EV_CHANNEL_PITCH: bend_ = dsp::fastExp2(e.u.midi.value / 12.f); break;
        case DAW_EV_PITCH_BEND: bend_ = dsp::fastExp2(e.u.midi.value * 2.0f / 12.0f); break;  // +-2 semitones
        case DAW_EV_ALL_NOTES_OFF:
            nHeld_ = 0;
            ampEnv_.noteOff();
            break;
        case DAW_EV_ALL_SOUND_OFF:
            nHeld_ = 0;
            ampEnv_.reset();
            resetFilters();
            idle_ = true;
            break;
        default: break;
        }
    }

    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        float *L = c.outputs[0] + start, *R = c.outputs[1] + start;
        if (idle_) {
            for (uint32_t i = 0; i < n; i++) {
                L[i] = R[i] = 0;
                level_.next();
            }
            return;
        }
        const float blend = param(P_WAVE);
        const float tune = dsp::fastExp2(param(P_TUNE) * (1.0f / 12.0f));
        const float osr = 1.0f / (kOver * sampleRate());
        for (uint32_t i = 0; i < n; i++) {
            // Open303::getSample: slewed pitch, filter envelope (RC-smoothed), accent, amplitude
            const float freq = slew_.getSample(oscFreq_) * bend_ * tune;
            const float inc = dsp::clamp(freq * osr, 0.0f, 0.45f);
            const float env = mainEnv_.getSample();
            float t1 = rc1_.getSample(env);
            float t2 = rc2_.getSample(accentGain_ > 0.0f ? env : 0.0f);
            t1 = envScaler_ * (t1 - envOffset_);
            t2 = accentGain_ * t2;
            filter_.setCutoff(param(P_CUTOFF) * dsp::fastExp2(t1 + t2));
            float amp = ampEnv_.getSample();
            if (ampEnv_.isNoteOn())
                amp += 0.45f * env + accentGain_ * 4.0f * env;
            amp = declick_.getSample(amp);

            float os[kOver];
            for (int k = 0; k < kOver; k++) {
                float s = -oscillator(inc, blend);
                s = hp1_.getSample(s);
                os[k] = filter_.getSample(s);
            }
            float y = hb_.process(os[0], os[1]);
            y = allpass_.getSample(y);
            y = hp2_.getSample(y);
            y = notch_.getSample(y);
            y *= amp * level_.next() * kOutGain;
            L[i] = R[i] = y;
        }
        // asleep once the note has faded out (the ABI lets the host skip us then)
        if (!ampEnv_.isNoteOn() && ampEnv_.endIsReached() && quiet(L, n))
            idle_ = true;
    }
    uint32_t voices() const { return idle_ ? 0 : 1; }

private:
    // Open303's two waveforms, band-limited with PolyBLEP: SAW303 (jump at half the period) and
    // SQUARE303 = -tanh(70 * saw + 4.37) shifted by half a period (its jump falls on the wrap)
    float oscillator(float inc, float blend)
    {
        const float p = phase_;
        float saw = p < 0.5f ? 2.0f * p : 2.0f * p - 2.0f;
        float tq = p + 0.5f;
        if (tq >= 1.0f)
            tq -= 1.0f;
        saw -= dsp::polyBlep(tq, inc);  // the saw's -2 jump at p = 0.5 (tq = 0)
        float out = saw;
        if (blend != 0.0f) {
            const float q = tq < 0.5f ? 2.0f * tq : 2.0f * tq - 2.0f;
            float sq = -fastTanh(69.98f * q + 4.37f);
            sq += dsp::polyBlep(p, inc);
            out = saw + blend * (sq - saw);
        }
        phase_ += inc;
        if (phase_ >= 1.0f)
            phase_ -= 1.0f;
        return out;
    }
    void resetFilters()
    {
        filter_.reset();
        hp1_.reset();
        hp2_.reset();
        allpass_.reset();
        notch_.reset();
        declick_.reset();
        hb_.reset();
    }
    void setFreq(int key)
    {
        current_ = key;
        oscFreq_ = dsp::midiToHz((float)key);
    }
    void accentOrNot(bool accent)
    {
        if (accent) {
            accentGain_ = accent_;
            mainEnv_.setDecayTimeConstant(kAccentDecay);
            ampEnv_.setRelease(kAccentRelease);
        } else {
            accentGain_ = 0.0f;
            mainEnv_.setDecayTimeConstant(normalDecay_);
            ampEnv_.setRelease(kNormalRelease);
        }
    }
    void trigger(int key, bool accent)
    {
        if (idle_) {  // only from silence: a running voice keeps its phase (no clicks)
            phase_ = 0;
            resetFilters();
        }
        accentOrNot(accent);
        setFreq(key);
        slew_.setState(oscFreq_);
        mainEnv_.trigger();
        ampEnv_.noteOn(true, key, 64);
        idle_ = false;
    }
    void slideTo(int key, bool accent)
    {
        setFreq(key);
        accentOrNot(accent);
        idle_ = false;
    }
    void hold(int key)
    {
        unhold(key);
        if (nHeld_ == kHeld) {
            for (uint32_t i = 1; i < kHeld; i++)
                held_[i - 1] = held_[i];
            nHeld_--;
        }
        held_[nHeld_++] = (uint8_t)key;
    }
    void unhold(int key)
    {
        uint32_t w = 0;
        for (uint32_t i = 0; i < nHeld_; i++)
            if (held_[i] != key)
                held_[w++] = held_[i];
        nHeld_ = w;
    }
    // Open303::calculateEnvModScalerAndOffset, the measured mapping
    void envModMapping()
    {
        const float c0 = 313.8152786f, c1 = 2394.411987f;
        const float e = param(P_ENVMOD) * 0.01f;
        const float c = expToLin(param(P_CUTOFF), c0, c1, 0.0f, 1.0f);
        const float sLo = 3.773996325f * e + 0.736965594f;
        const float sHi = 4.194548788f * e + 0.864344901f;
        envScaler_ = (1.0f - c) * sLo + c * sHi;
        envOffset_ = 0.048292931f * c + 0.294391201f;
    }
    static bool quiet(const float *x, uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
            if (x[i] > 1e-5f || x[i] < -1e-5f)
                return false;
        return true;
    }

    static constexpr float kAccentDecay = 200.0f, kNormalRelease = 1.0f, kAccentRelease = 50.0f;
    static constexpr float kOutGain = 0.177f;  // -15 dB: Open303 uses -12 dB; 3 dB more headroom for accents with the filter open

    rosic::TeeBeeFilter filter_;
    rosic::AnalogEnvelope ampEnv_;
    rosic::DecayEnvelope mainEnv_;
    rosic::LeakyIntegrator slew_, rc1_, rc2_;
    rosic::BiquadFilter declick_, notch_;
    rosic::OnePoleFilter hp1_, hp2_, allpass_;
    HalfbandDecimator hb_;
    dsp::Smoother level_;
    float phase_ = 0, oscFreq_ = 110.0f, bend_ = 1.0f;
    float accent_ = 0.6f, accentGain_ = 0, normalDecay_ = 400.0f;
    float envScaler_ = 1, envOffset_ = 0;
    uint8_t held_[kHeld] = {};
    uint32_t nHeld_ = 0;
    int current_ = -1;
    bool idle_ = true;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.acid303",
    .name = "Acid303",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "TB-303-style bass line synth: Open303's ladder filter, envelopes, accent and slide",
    .category = "Bass",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_VELOCITY | DAW_CAP_SLIDE | DAW_CAP_ACCENT | DAW_CAP_PITCH_BEND | DAW_CAP_MONO_LEGATO,
    .max_voices = 1,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(Acid303),
    .mem_large_bytes = 0,
    .cpu_permille_base = 120,  // estimate (2x oversampled ladder); to be measured on the board
    .cpu_permille_voice = 0,
    DAWSDK_CALLBACKS(Acid303),
};

const daw_plugin_descriptor_t *Acid303::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_acid303(void) { return &kDescriptor; }
