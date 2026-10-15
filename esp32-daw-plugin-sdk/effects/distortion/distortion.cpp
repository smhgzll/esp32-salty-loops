// SPDX-License-Identifier: MIT
// Distortion: four characters after a drive stage, then a tone low-pass, output gain and mix.
//   Soft:  cubic soft clipper, f(u) = 1.5 (u - u^3/3) for |u| <= 1, +-1 beyond
//   Hard:  clip at +-1
//   Fold:  sine wavefolder, sin(pi u / 2)
//   Crush: sample-rate reduction (hold) and bit-depth reduction
// Soft and hard use first-order antiderivative anti-aliasing (ADAA; Parker, Zavalishin & Le Bivic,
// DAFx 2016): y = (F(u) - F(u_prev)) / (u - u_prev) with F the clipper's antiderivative, which
// suppresses most of the aliasing a plain clipper produces, without oversampling.
#include <math.h>

#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_MODE, P_DRIVE, P_TONE, P_BITS, P_DOWNSAMPLE, P_OUTPUT, P_MIX, P_COUNT };
const char *const kModes[] = {"Soft", "Hard", "Fold", "Crush"};
const daw_param_desc_t kParams[P_COUNT] = {
    {P_MODE, "mode", "Mode", "Drive", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 3, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 4, kModes},
    {P_DRIVE, "drive", "Drive", "Drive", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 40.0f, 12.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_TONE, "tone", "Tone", "Tone", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 500.0f, 20000.0f, 9000.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_BITS, "bits", "Bits", "Crush", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 2.0f, 16.0f, 8.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DOWNSAMPLE, "downsample", "Downsample", "Crush", "x", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 1.0f, 32.0f, 1.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_OUTPUT, "output", "Output", "Output", "dB", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -24.0f, 6.0f, -6.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MIX, "mix", "Mix", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 100.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
//                           mode drive tone  bits down out mix
const float kWarm[P_COUNT] = {0, 6, 7000, 8, 1, -3, 100};
const float kFuzz[P_COUNT] = {1, 30, 5000, 8, 1, -12, 100};
const float kFold[P_COUNT] = {2, 14, 12000, 8, 1, -6, 100};
const float kLofi[P_COUNT] = {3, 0, 8000, 6, 6, 0, 100};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}, {"Warm", "Saturation", kWarm}, {"Fuzz", "Hard", kFuzz}, {"Folder", "Synth", kFold}, {"Lo-fi", "Crush", kLofi}};

// the clippers and their antiderivatives
inline float softF(float u) { return u > 1.0f ? 1.0f : (u < -1.0f ? -1.0f : 1.5f * (u - u * u * u * (1.0f / 3.0f))); }
inline float softAD(float u)
{
    const float a = u < 0 ? -u : u;
    return a <= 1.0f ? 1.5f * (0.5f * u * u - u * u * u * u * (1.0f / 12.0f)) : a - 0.375f;  // continuous at |u| = 1: 0.625
}
inline float hardF(float u) { return u > 1.0f ? 1.0f : (u < -1.0f ? -1.0f : u); }
inline float hardAD(float u)
{
    const float a = u < 0 ? -u : u;
    return a <= 1.0f ? 0.5f * u * u : a - 0.5f;
}

class Distortion : public Plugin<Distortion, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void reset()
    {
        for (int ch = 0; ch < 2; ch++)
            prev_[ch] = hold_[ch] = lp_[ch] = 0;
        phase_ = 0;
        dsp::Smoother *const smoothers[] = {&drive_, &out_, &mix_};
        for (auto *s : smoothers)
            s->setTime(0.02f, sampleRate());
        snapAll();
    }
    void onStart(const daw_process_t &) { snapAll(); }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_DRIVE: drive_.target = dsp::dbToGain(v); break;
        case P_OUTPUT: out_.target = dsp::dbToGain(v); break;
        case P_MIX: mix_.target = v * 0.01f; break;
        case P_TONE: lpA_ = 1.0f - expf(-dsp::kTwoPi * v / sampleRate()); break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *in[2] = {c.inputs[0] + start, c.inputs[1] + start};
        float *out[2] = {c.outputs[0] + start, c.outputs[1] + start};
        const int mode = (int)param(P_MODE);
        const float levels = dsp::fastExp2(param(P_BITS) - 1.0f), step = 1.0f / param(P_DOWNSAMPLE);
        for (uint32_t i = 0; i < n; i++) {
            const float d = drive_.next(), g = out_.next(), m = mix_.next();
            bool take = true;
            if (mode == 3) {  // the hold clock is shared by both channels
                phase_ += step;
                take = phase_ >= 1.0f;
                if (take)
                    phase_ -= 1.0f;
            }
            for (int ch = 0; ch < 2; ch++) {
                const float x = in[ch][i], u = x * d;
                float y;
                switch (mode) {
                case 0: y = adaa(u, prev_[ch], softF, softAD); break;
                case 1: y = adaa(u, prev_[ch], hardF, hardAD); break;
                case 2: {
                    float p = u * 0.25f;  // sin(pi u / 2) = sin(2 pi * u / 4)
                    p -= floorf(p);
                    y = dsp::sinTurns(p);
                    break;
                }
                default:
                    if (take)
                        hold_[ch] = floorf(u * levels + 0.5f) / levels;
                    y = hold_[ch] > 1.0f ? 1.0f : (hold_[ch] < -1.0f ? -1.0f : hold_[ch]);
                    break;
                }
                prev_[ch] = u;
                lp_[ch] = dsp::sanitize(lp_[ch] + lpA_ * (y - lp_[ch]));
                out[ch][i] = x + (lp_[ch] * g - x) * m;
            }
        }
    }

private:
    template <class F, class AD>
    static float adaa(float u, float u1, F f, AD ad)
    {
        const float du = u - u1;
        if (du > 1e-3f || du < -1e-3f)
            return dsp::fastDiv(ad(u) - ad(u1), du);  // |du| > 1e-3: no __divsf3 call on the S3
        return f(0.5f * (u + u1));  // ill-conditioned: the clipper at the midpoint
    }
    void snapAll()
    {
        drive_.snap(dsp::dbToGain(param(P_DRIVE)));
        out_.snap(dsp::dbToGain(param(P_OUTPUT)));
        mix_.snap(param(P_MIX) * 0.01f);
        lpA_ = 1.0f - expf(-dsp::kTwoPi * param(P_TONE) / sampleRate());
    }
    float prev_[2] = {}, hold_[2] = {}, lp_[2] = {};
    float phase_ = 0, lpA_ = 1.0f;
    dsp::Smoother drive_, out_, mix_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.distortion", .name = "Distortion", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Anti-aliased soft / hard clipping, wavefolding and bit crushing", .category = "Distortion",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = 0, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = sizeof kPresets / sizeof kPresets[0], .presets = kPresets,
    .key_names = nullptr, .mem_fast_bytes = sizeof(Distortion), .mem_large_bytes = 0,
    .cpu_permille_base = 12, .cpu_permille_voice = 0,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(Distortion),
};
const daw_plugin_descriptor_t *Distortion::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_distortion(void) { return &kDescriptor; }
