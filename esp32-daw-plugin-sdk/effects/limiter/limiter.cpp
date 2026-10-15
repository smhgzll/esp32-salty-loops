// SPDX-License-Identifier: MIT
// Limiter: look-ahead brickwall peak limiter, stereo linked.
//   t[n] = min(1, ceiling / |x[n]|)              per-sample target gain
//   m[n] = min(t[n-L] .. t[n])                    window minimum over the look-ahead
//   g[n] = mean(m[n-L+1] .. m[n])                 a ramp instead of a gain step
//   y[n] = x[n-L] * min(g[n], release follower)   every term of the mean covers x[n-L]: no overs
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_INPUT, P_CEILING, P_RELEASE, P_COUNT };
constexpr uint32_t kLook = 64;  // 1.45 ms at 44.1 kHz

const daw_param_desc_t kParams[P_COUNT] = {
    {P_INPUT, "input", "Input gain", "Limiter", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, -12.0f, 24.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_CEILING, "ceiling", "Ceiling", "Limiter", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, -24.0f, 0.0f, -0.3f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_RELEASE, "release", "Release", "Limiter", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 5.0f, 2000.0f, 120.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
const float kLoud[P_COUNT] = {6, -0.3f, 60};
const daw_preset_desc_t kPresets[] = {{"Safety", "Master", nullptr}, {"Loud", "Master", kLoud}};

class Limiter : public Plugin<Limiter, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onStart(const daw_process_t &) { in_.snap(in_.target); }
    void reset()
    {
        for (uint32_t i = 0; i < kLook; i++) {
            dl_[i] = dr_[i] = 0;
            m_[i] = 1;
        }
        pos_ = 0;
        sequence_ = 0;
        minHead_ = minCount_ = 0;
        sum_ = (float)kLook;
        env_ = 1;
        in_.setTime(0.02f, sampleRate());
        in_.snap(dsp::dbToGain(param(P_INPUT)));
    }
    void onParam(uint32_t id, float v)
    {
        if (id == P_INPUT)
            in_.target = dsp::dbToGain(v);
        else if (id == P_CEILING)
            ceil_ = dsp::dbToGain(v);
        else if (id == P_RELEASE)
            rel_ = 1.0f - expf(-1.0f / (v * 0.001f * sampleRate()));
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *iL = c.inputs[0] + start, *iR = c.inputs[1] + start;
        float *oL = c.outputs[0] + start, *oR = c.outputs[1] + start;
        const float ceil = ceil_;
        for (uint32_t i = 0; i < n; i++) {
            float g = in_.next();
            float l = dsp::sanitize(iL[i] * g), r = dsp::sanitize(iR[i] * g);
            float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            float t = a > ceil ? ceil / a : 1.0f;
            // ring buffers hold x[n-L] .. x[n-1] and their targets; pos_ is the oldest, x[n-L]
            float yl = dl_[pos_], yr = dr_[pos_];
            const float mn = windowMinimum(t);  // m[n] = min(t[n-L] .. t[n])
            dl_[pos_] = l;
            dr_[pos_] = r;
            sum_ += mn - m_[pos_];
            m_[pos_] = mn;
            float box = sum_ * (1.0f / (float)kLook);
            env_ = box < env_ ? box : env_ + (box - env_) * rel_;
            float gr = env_;
            yl *= gr;
            yr *= gr;
            // numerical safety: never above the ceiling
            oL[i] = yl > ceil ? ceil : (yl < -ceil ? -ceil : yl);
            oR[i] = yr > ceil ? ceil : (yr < -ceil ? -ceil : yr);
            pos_ = pos_ + 1 == kLook ? 0 : pos_ + 1;
        }
    }

private:
    // Monotonic queue: each target enters and leaves once, instead of scanning 64 targets
    // for every sample. The inclusive window contains kLook + 1 targets.
    struct Minimum {
        uint32_t sequence;
        float value;
    };
    Minimum minima_[kLook + 1];
    uint32_t sequence_ = 0;
    uint32_t minHead_ = 0, minCount_ = 0;
    float windowMinimum(float value)
    {
        constexpr uint32_t capacity = kLook + 1;
        if (minCount_ && sequence_ - minima_[minHead_].sequence > kLook) {
            if (++minHead_ == capacity)
                minHead_ = 0;
            --minCount_;
        }
        while (minCount_) {
            uint32_t tail = minHead_ + minCount_ - 1;
            if (tail >= capacity)
                tail -= capacity;
            if (minima_[tail].value < value)
                break;
            --minCount_;
        }
        uint32_t next = minHead_ + minCount_;
        if (next >= capacity)
            next -= capacity;
        minima_[next] = {sequence_++, value};
        ++minCount_;
        return minima_[minHead_].value;
    }
    float dl_[kLook], dr_[kLook], m_[kLook];
    uint32_t pos_ = 0;
    float sum_ = kLook, env_ = 1, ceil_ = 1, rel_ = 0.001f;
    dsp::Smoother in_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.limiter", .name = "Limiter", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Look-ahead brickwall peak limiter (1.5 ms), stereo linked", .category = "Dynamics",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = 0, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = 2, .presets = kPresets, .key_names = nullptr,
    .mem_fast_bytes = sizeof(Limiter), .mem_large_bytes = 0, .cpu_permille_base = 40, .cpu_permille_voice = 0,
    DAWSDK_CALLBACKS(Limiter),
};
const daw_plugin_descriptor_t *Limiter::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_limiter(void) { return &kDescriptor; }
