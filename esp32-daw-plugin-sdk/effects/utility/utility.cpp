// SPDX-License-Identifier: MIT
// Utility: gain, pan (balance), stereo width, mono, channel swap and polarity.
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_GAIN, P_PAN, P_WIDTH, P_MONO, P_SWAP, P_INVL, P_INVR, P_COUNT };

const daw_param_desc_t kParams[P_COUNT] = {
    {P_GAIN, "gain", "Gain", "Level", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, -60.0f, 24.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_PAN, "pan", "Pan", "Level", "", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -1.0f, 1.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_WIDTH, "width", "Width", "Stereo", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 200.0f, 100.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MONO, "mono", "Mono", "Stereo", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_SWAP, "swap", "Swap L/R", "Stereo", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_INVL, "invert_l", "Invert L", "Polarity", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_INVR, "invert_r", "Invert R", "Polarity", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 0, nullptr},
};
const daw_preset_desc_t kPresets[] = {{"Unity", "Basic", nullptr}};

class Utility : public Plugin<Utility, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onStart(const daw_process_t &c)
    {
        gl_.snap(gl_.target);
        gr_.snap(gr_.target);
        width_.snap(width_.target);
    }
    void reset()
    {
        gl_.setTime(0.01f, sampleRate());
        gr_.setTime(0.01f, sampleRate());
        width_.setTime(0.01f, sampleRate());
        update();
        gl_.snap(gl_.target);
        gr_.snap(gr_.target);
        width_.snap(width_.target);
    }
    void onParam(uint32_t, float) { update(); }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *iL = c.inputs[0] + start, *iR = c.inputs[1] + start;
        float *oL = c.outputs[0] + start, *oR = c.outputs[1] + start;
        const bool mono = param(P_MONO) > 0.5f, swap = param(P_SWAP) > 0.5f;
        for (uint32_t i = 0; i < n; i++) {
            float l = iL[i], r = iR[i];
            if (swap) { float t = l; l = r; r = t; }
            float w = width_.next() * 0.01f;
            float m = 0.5f * (l + r), s = 0.5f * (l - r) * (mono ? 0.0f : w);
            l = m + s;
            r = m - s;
            oL[i] = l * gl_.next();
            oR[i] = r * gr_.next();
        }
    }

private:
    void update()
    {
        float g = dsp::dbToGain(param(P_GAIN)), l, r;
        dsp::panGains(param(P_PAN), l, r);
        gl_.target = g * l * (param(P_INVL) > 0.5f ? -1.0f : 1.0f);
        gr_.target = g * r * (param(P_INVR) > 0.5f ? -1.0f : 1.0f);
        width_.target = param(P_WIDTH);
    }
    dsp::Smoother gl_, gr_, width_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.utility", .name = "Utility", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Gain, balance, stereo width, mono, swap and polarity", .category = "Utility",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = 0, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = 1, .presets = kPresets, .key_names = nullptr,
    .mem_fast_bytes = sizeof(Utility), .mem_large_bytes = 0, .cpu_permille_base = 3, .cpu_permille_voice = 0,
    DAWSDK_CALLBACKS(Utility),
};
const daw_plugin_descriptor_t *Utility::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_utility(void) { return &kDescriptor; }
