// SPDX-License-Identifier: MIT
// Compressor: feed-forward, stereo-linked. The level detector takes the louder channel's peak in
// dB (dsp::gainToDb, no libm per sample); a soft-knee gain computer gives the gain reduction,
// smoothed with separate attack and release time constants in the dB domain; makeup gain and a
// dry / wet mix for parallel compression.
//   below the knee: no change; above: out = thr + (in - thr) / ratio; within the knee
//   (|in - thr| < knee / 2) a quadratic blend (Giannoulis, Massberg & Reiss, JAES 2012).
#include <math.h>

#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_THRESH, P_RATIO, P_ATTACK, P_RELEASE, P_KNEE, P_MAKEUP, P_MIX, P_COUNT };
const daw_param_desc_t kParams[P_COUNT] = {
    {P_THRESH, "threshold", "Threshold", "Dynamics", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, -60.0f, 0.0f, -18.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RATIO, "ratio", "Ratio", "Dynamics", "", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 1.0f, 20.0f, 4.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_ATTACK, "attack", "Attack", "Timing", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.1f, 100.0f, 10.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RELEASE, "release", "Release", "Timing", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 10.0f, 1500.0f, 150.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_KNEE, "knee", "Knee", "Dynamics", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 12.0f, 6.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MAKEUP, "makeup", "Makeup", "Output", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 24.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MIX, "mix", "Mix", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 100.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
//                          thr ratio att  rel  knee makeup mix
const float kGlue[P_COUNT] = {-20, 2.0f, 30, 200, 6, 3, 100};
const float kPunch[P_COUNT] = {-24, 4.0f, 25, 120, 3, 6, 100};
const float kSquash[P_COUNT] = {-40, 10.0f, 1, 80, 2, 18, 100};
const float kParallel[P_COUNT] = {-35, 8.0f, 2, 150, 4, 14, 40};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}, {"Bus glue", "Mix", kGlue}, {"Drum punch", "Drums", kPunch}, {"Squash", "Color", kSquash}, {"Parallel", "Drums", kParallel}};

class Compressor : public Plugin<Compressor, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void reset()
    {
        gr_ = 0;
        makeup_.setTime(0.03f, sampleRate());
        mix_.setTime(0.03f, sampleRate());
        snapAll();
    }
    void onStart(const daw_process_t &) { snapAll(); }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_ATTACK: att_ = coef(v); break;
        case P_RELEASE: rel_ = coef(v); break;
        case P_MAKEUP: makeup_.target = dsp::dbToGain(v); break;
        case P_MIX: mix_.target = v * 0.01f; break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *iL = c.inputs[0] + start, *iR = c.inputs[1] + start;
        float *oL = c.outputs[0] + start, *oR = c.outputs[1] + start;
        const float thr = param(P_THRESH), slope = 1.0f - 1.0f / param(P_RATIO), knee = param(P_KNEE);
        for (uint32_t i = 0; i < n; i++) {
            const float l = iL[i], r = iR[i];
            const float al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            const float lvl = dsp::gainToDb(al > ar ? al : ar);
            // static curve: the gain reduction (dB, >= 0) wanted at this level
            const float over = lvl - thr;
            float want;
            if (2.0f * over <= -knee)
                want = 0;
            else if (2.0f * over >= knee)
                want = over * slope;
            else {
                const float x = over + 0.5f * knee;
                want = slope * x * x / (2.0f * knee);
            }
            // ballistics: attack when more reduction is needed, release otherwise
            gr_ += (want - gr_) * (want > gr_ ? att_ : rel_);
            gr_ = dsp::sanitize(gr_);
            const float g = dsp::dbToGain(-gr_) * makeup_.next(), m = mix_.next();
            oL[i] = l + (l * g - l) * m;
            oR[i] = r + (r * g - r) * m;
        }
    }

private:
    float coef(float ms) const { return 1.0f - expf(-1.0f / (ms * 0.001f * sampleRate())); }
    void snapAll()
    {
        att_ = coef(param(P_ATTACK));
        rel_ = coef(param(P_RELEASE));
        makeup_.snap(dsp::dbToGain(param(P_MAKEUP)));
        mix_.snap(param(P_MIX) * 0.01f);
    }
    float gr_ = 0, att_ = 0.01f, rel_ = 0.001f;
    dsp::Smoother makeup_, mix_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.compressor", .name = "Compressor", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Stereo-linked soft-knee compressor with makeup gain and parallel mix", .category = "Dynamics",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = 0, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = sizeof kPresets / sizeof kPresets[0], .presets = kPresets,
    .key_names = nullptr, .mem_fast_bytes = sizeof(Compressor), .mem_large_bytes = 0,
    .cpu_permille_base = 15, .cpu_permille_voice = 0,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(Compressor),
};
const daw_plugin_descriptor_t *Compressor::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_compressor(void) { return &kDescriptor; }
