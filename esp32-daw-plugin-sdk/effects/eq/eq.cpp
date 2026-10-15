// SPDX-License-Identifier: MIT
// EQ: six-band stereo equalizer from RBJ-cookbook biquads (dsp::Biquad): low cut (12 dB/oct),
// low shelf, two peaking bands, high shelf, high cut (12 dB/oct), output gain. Frequencies, gains
// and Q are smoothed and the coefficients recomputed every 32 samples while they move; a band at
// 0 dB (or a cut that is off) costs nothing.
#include <math.h>

#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t {
    P_LC_ON, P_LC_FREQ,
    P_LS_FREQ, P_LS_GAIN,
    P_M1_FREQ, P_M1_GAIN, P_M1_Q,
    P_M2_FREQ, P_M2_GAIN, P_M2_Q,
    P_HS_FREQ, P_HS_GAIN,
    P_HC_ON, P_HC_FREQ,
    P_OUT, P_COUNT
};
#define DBP(id, key, name, group) {id, key, name, group, "dB", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -18.0f, 18.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr}
#define HZP(id, key, name, group, lo, hi, def) {id, key, name, group, "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, lo, hi, def, DAW_PARAM_AUTOMATABLE, 0, nullptr}
#define QP(id, key, group) {id, key, "Q", group, "", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.3f, 8.0f, 1.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr}
const daw_param_desc_t kParams[P_COUNT] = {
    {P_LC_ON, "lowcut_on", "Low cut", "Low cut", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    HZP(P_LC_FREQ, "lowcut", "Frequency", "Low cut", 20.0f, 1000.0f, 80.0f),
    HZP(P_LS_FREQ, "low_freq", "Frequency", "Low shelf", 30.0f, 1000.0f, 120.0f),
    DBP(P_LS_GAIN, "low_gain", "Gain", "Low shelf"),
    HZP(P_M1_FREQ, "mid1_freq", "Frequency", "Mid 1", 80.0f, 10000.0f, 500.0f),
    DBP(P_M1_GAIN, "mid1_gain", "Gain", "Mid 1"),
    QP(P_M1_Q, "mid1_q", "Mid 1"),
    HZP(P_M2_FREQ, "mid2_freq", "Frequency", "Mid 2", 200.0f, 16000.0f, 3000.0f),
    DBP(P_M2_GAIN, "mid2_gain", "Gain", "Mid 2"),
    QP(P_M2_Q, "mid2_q", "Mid 2"),
    HZP(P_HS_FREQ, "high_freq", "Frequency", "High shelf", 1000.0f, 16000.0f, 8000.0f),
    DBP(P_HS_GAIN, "high_gain", "Gain", "High shelf"),
    {P_HC_ON, "highcut_on", "High cut", "High cut", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    HZP(P_HC_FREQ, "highcut", "Frequency", "High cut", 1000.0f, 20000.0f, 16000.0f),
    {P_OUT, "output", "Output", "Output", "dB", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -12.0f, 12.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
#undef DBP
#undef HZP
#undef QP
//                         lcOn lc   lsF  lsG  m1F  m1G  m1Q  m2F   m2G  m2Q  hsF   hsG  hcOn hc     out
const float kVocal[P_COUNT] = {1, 100, 200, -2, 400, -3, 1.2f, 3500, 3, 0.8f, 10000, 2.5f, 0, 18000, 0};
const float kKick[P_COUNT] = {1, 30, 70, 4, 350, -5, 1.5f, 3000, 3, 1.0f, 8000, 0, 1, 12000, 0};
const float kWarm[P_COUNT] = {0, 40, 150, 2.5f, 600, 0, 1, 3000, -1.5f, 0.7f, 9000, -3, 0, 18000, 0};
const float kTelephone[P_COUNT] = {1, 400, 120, 0, 1200, 6, 0.7f, 3000, 0, 1, 8000, 0, 1, 3000, 0};
const daw_preset_desc_t kPresets[] = {{"Flat", "Basic", nullptr}, {"Vocal presence", "Voice", kVocal}, {"Kick punch", "Drums", kKick}, {"Warm", "Mix", kWarm}, {"Telephone", "Color", kTelephone}};

enum Band { LC, LS, M1, M2, HS, HC, kBands };

class Eq : public Plugin<Eq, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void reset()
    {
        const float cr = sampleRate() / 32.0f;
        for (int b = 0; b < kBands; b++) {
            f_[b].setTime(0.03f, cr);
            g_[b].setTime(0.03f, cr);
            q_[b].setTime(0.03f, cr);
            for (auto &x : bq_[b])
                x.reset();
        }
        out_.setTime(0.03f, sampleRate());
        snapAll();
        ctl_ = 0;
    }
    void onStart(const daw_process_t &) { snapAll(); }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_LC_FREQ: f_[LC].target = logf(v); break;
        case P_LS_FREQ: f_[LS].target = logf(v); break;
        case P_LS_GAIN: g_[LS].target = v; break;
        case P_M1_FREQ: f_[M1].target = logf(v); break;
        case P_M1_GAIN: g_[M1].target = v; break;
        case P_M1_Q: q_[M1].target = v; break;
        case P_M2_FREQ: f_[M2].target = logf(v); break;
        case P_M2_GAIN: g_[M2].target = v; break;
        case P_M2_Q: q_[M2].target = v; break;
        case P_HS_FREQ: f_[HS].target = logf(v); break;
        case P_HS_GAIN: g_[HS].target = v; break;
        case P_HC_FREQ: f_[HC].target = logf(v); break;
        case P_OUT: out_.target = dsp::dbToGain(v); break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *iL = c.inputs[0] + start, *iR = c.inputs[1] + start;
        float *oL = c.outputs[0] + start, *oR = c.outputs[1] + start;
        const bool lc = param(P_LC_ON) >= 0.5f, hc = param(P_HC_ON) >= 0.5f;
        bool zero = true;
        for (auto &band : bq_)
            for (auto &b : band) zero &= b.z1 == 0 && b.z2 == 0;
        zero = zero && dsp::silent(iL, iR, n);
        for (uint32_t i = 0; i < n; i++) {
            if (ctl_ == 0)
                control();
            ctl_ = (ctl_ + 1) & 31;
            if (zero) { out_.next(); oL[i] = oR[i] = 0; continue; }
            float l = iL[i], r = iR[i];
            if (lc)
                l = bq_[LC][0].process(l), r = bq_[LC][1].process(r);
            for (int b = LS; b <= HS; b++)
                if (active_[b])
                    l = bq_[b][0].process(l), r = bq_[b][1].process(r);
            if (hc)
                l = bq_[HC][0].process(l), r = bq_[HC][1].process(r);
            const float g = out_.next();
            oL[i] = l * g;
            oR[i] = r * g;
        }
    }

private:
    // control rate: advance the smoothers, recompute the bands that moved
    void control()
    {
        for (int b = 0; b < kBands; b++) {
            const bool moving = !f_[b].settled() || !g_[b].settled() || !q_[b].settled();
            const float logF = f_[b].next(), g = g_[b].next(), q = q_[b].next();
            if (!moving && !dirty_[b])
                continue;
            dirty_[b] = false;
            set(b, expf(logF), g, q);
        }
    }
    void set(int b, float f, float g, float q)
    {
        dsp::Biquad &x = bq_[b][0];
        const float z1 = x.z1, z2 = x.z2;  // keep the filter state, change only the coefficients
        switch (b) {
        case LC: x.set(dsp::Biquad::HighPass, f, 0.7071f, 0, sampleRate()); break;
        case LS: x.set(dsp::Biquad::LowShelf, f, 0.7071f, g, sampleRate()); break;
        case HS: x.set(dsp::Biquad::HighShelf, f, 0.7071f, g, sampleRate()); break;
        case HC: x.set(dsp::Biquad::LowPass, f, 0.7071f, 0, sampleRate()); break;
        default: x.set(dsp::Biquad::Peak, f, q, g, sampleRate()); break;
        }
        x.z1 = z1;
        x.z2 = z2;
        dsp::Biquad &y = bq_[b][1];
        y.b0 = x.b0, y.b1 = x.b1, y.b2 = x.b2, y.a1 = x.a1, y.a2 = x.a2;
        active_[b] = g > 0.01f || g < -0.01f;
    }
    void snapAll()
    {
        const uint32_t fid[kBands] = {P_LC_FREQ, P_LS_FREQ, P_M1_FREQ, P_M2_FREQ, P_HS_FREQ, P_HC_FREQ};
        const int32_t gid[kBands] = {-1, P_LS_GAIN, P_M1_GAIN, P_M2_GAIN, P_HS_GAIN, -1};
        const int32_t qid[kBands] = {-1, -1, P_M1_Q, P_M2_Q, -1, -1};
        for (int b = 0; b < kBands; b++) {
            f_[b].snap(logf(param(fid[b])));
            g_[b].snap(gid[b] >= 0 ? param((uint32_t)gid[b]) : 0.0f);
            q_[b].snap(qid[b] >= 0 ? param((uint32_t)qid[b]) : 0.7071f);
            dirty_[b] = true;
        }
        out_.snap(dsp::dbToGain(param(P_OUT)));
    }
    dsp::Biquad bq_[kBands][2];
    dsp::Smoother f_[kBands], g_[kBands], q_[kBands], out_;
    bool active_[kBands] = {}, dirty_[kBands] = {};
    uint32_t ctl_ = 0;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.eq", .name = "EQ", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Six-band equalizer: low / high cut, shelves and two peaking bands", .category = "EQ",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = 0, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = sizeof kPresets / sizeof kPresets[0], .presets = kPresets,
    .key_names = nullptr, .mem_fast_bytes = sizeof(Eq), .mem_large_bytes = 0,
    .cpu_permille_base = 25, .cpu_permille_voice = 0,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(Eq),
};
const daw_plugin_descriptor_t *Eq::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_eq(void) { return &kDescriptor; }
