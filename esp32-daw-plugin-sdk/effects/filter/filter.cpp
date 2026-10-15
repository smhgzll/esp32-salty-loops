// SPDX-License-Identifier: MIT
// Filter: stereo state-variable filter (low / band / high pass, notch) with drive and dry/wet.
// Cutoff and resonance are smoothed and the coefficients updated every 16 samples.
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_TYPE, P_CUTOFF, P_RES, P_DRIVE, P_MIX, P_COUNT };
const char *const kTypes[] = {"Low pass", "Band pass", "High pass", "Notch"};
const daw_param_desc_t kParams[P_COUNT] = {
    {P_TYPE, "type", "Type", "Filter", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 3, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 4, kTypes},
    {P_CUTOFF, "cutoff", "Cutoff", "Filter", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 20.0f, 20000.0f, 2000.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RES, "resonance", "Resonance", "Filter", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, 0.3f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DRIVE, "drive", "Drive", "Filter", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 24.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MIX, "mix", "Mix", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 100.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
const float kSweep[P_COUNT] = {0, 800, 0.7f, 6, 100};
const float kHp[P_COUNT] = {2, 300, 0.2f, 0, 100};
const float kTelephone[P_COUNT] = {1, 1500, 0.5f, 9, 100};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}, {"Resonant LP", "Sweep", kSweep}, {"Clean HP", "Basic", kHp}, {"Telephone", "Color", kTelephone}};

class Filter : public Plugin<Filter, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onStart(const daw_process_t &c)
    {
        cut_.snap(cut_.target);
        res_.snap(res_.target);
        mix_.snap(mix_.target);
        drive_.snap(drive_.target);
    }
    void reset()
    {
        fl_.reset();
        fr_.reset();
        cut_.setTime(0.02f, sampleRate() / 16.0f);
        res_.setTime(0.02f, sampleRate() / 16.0f);
        mix_.setTime(0.02f, sampleRate());
        drive_.setTime(0.02f, sampleRate());
        cut_.snap(logf(param(P_CUTOFF)));
        res_.snap(param(P_RES));
        mix_.snap(param(P_MIX) * 0.01f);
        drive_.snap(dsp::dbToGain(param(P_DRIVE)));
        ctl_ = 0;
    }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_CUTOFF: cut_.target = logf(v); break;
        case P_RES: res_.target = v; break;
        case P_MIX: mix_.target = v * 0.01f; break;
        case P_DRIVE: drive_.target = dsp::dbToGain(v); break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *iL = c.inputs[0] + start, *iR = c.inputs[1] + start;
        float *oL = c.outputs[0] + start, *oR = c.outputs[1] + start;
        const auto mode = (dsp::Svf::Mode)(int)param(P_TYPE);
        const bool zero = fl_.ic1 == 0 && fl_.ic2 == 0 && fr_.ic1 == 0 && fr_.ic2 == 0 && dsp::silent(iL, iR, n);
        for (uint32_t i = 0; i < n; i++) {
            if (ctl_ == 0) {
                float logHz = cut_.next();
                float r = res_.next();
                if (logHz != lastCut_ || r != lastRes_) {
                    lastCut_ = logHz;
                    lastRes_ = r;
                    float hz = expf(logHz);
                    fl_.set(hz, r, sampleRate());
                    fr_.set(hz, r, sampleRate());
                }
            }
            ctl_ = (ctl_ + 1) & 15;
            float d = drive_.next(), m = mix_.next();
            if (zero) { oL[i] = oR[i] = 0; continue; }
            float l = iL[i], r = iR[i];
            float xl = d > 1.001f ? dsp::softClip(l * d) / (0.5f + 0.5f * d) * 2.0f : l;
            float xr = d > 1.001f ? dsp::softClip(r * d) / (0.5f + 0.5f * d) * 2.0f : r;
            float yl = fl_.process(xl, mode), yr = fr_.process(xr, mode);
            oL[i] = l + (yl - l) * m;
            oR[i] = r + (yr - r) * m;
        }
    }

private:
    dsp::Svf fl_, fr_;
    dsp::Smoother cut_, res_, mix_, drive_;
    uint32_t ctl_ = 0;
    float lastCut_ = -100, lastRes_ = -1;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.filter", .name = "Filter", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Stereo state-variable filter: low / band / high pass and notch, with drive", .category = "Filter",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = 0, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = 4, .presets = kPresets, .key_names = nullptr,
    .mem_fast_bytes = sizeof(Filter), .mem_large_bytes = 0, .cpu_permille_base = 12, .cpu_permille_voice = 0,
    DAWSDK_CALLBACKS(Filter),
};
const daw_plugin_descriptor_t *Filter::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_filter(void) { return &kDescriptor; }
