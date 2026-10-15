// SPDX-License-Identifier: MIT
// Flanger: a short delay swept by a sine LFO, with positive or negative feedback (through a soft
// limiter, so 95 % stays stable) and a stereo phase offset. Delay lines in DAW_MEM_LARGE.
#include <math.h>

#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_RATE, P_DEPTH, P_DELAY, P_FEEDBACK, P_SPREAD, P_MIX, P_COUNT };
const daw_param_desc_t kParams[P_COUNT] = {
    {P_RATE, "rate", "Rate", "Modulation", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.02f, 5.0f, 0.25f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DEPTH, "depth", "Depth", "Modulation", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 8.0f, 3.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DELAY, "delay", "Delay", "Modulation", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.1f, 5.0f, 0.5f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_FEEDBACK, "feedback", "Feedback", "Tone", "%", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -95.0f, 95.0f, 60.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_SPREAD, "spread", "Stereo spread", "Output", "°", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 180.0f, 90.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MIX, "mix", "Mix", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 50.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
//                          rate depth delay fb spread mix
const float kJet[P_COUNT] = {0.12f, 5.0f, 0.3f, 85, 60, 50};
const float kGentle[P_COUNT] = {0.3f, 2.0f, 1.0f, 30, 90, 40};
const float kMetal[P_COUNT] = {0.8f, 1.0f, 0.2f, -90, 120, 50};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}, {"Jet", "Classic", kJet}, {"Gentle", "Basic", kGentle}, {"Metallic", "Color", kMetal}};

constexpr float kMaxMs = 14.0f;

class Flanger : public Plugin<Flanger, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    int32_t init()
    {
        size_ = 1;
        while (size_ < (uint32_t)(kMaxMs * 0.001f * sampleRate()) + 8)
            size_ <<= 1;
        buf_[0] = alloc<float>(size_, DAW_MEM_LARGE);
        buf_[1] = alloc<float>(size_, DAW_MEM_LARGE);
        return buf_[0] && buf_[1] ? DAW_OK : DAW_ERR_NOMEM;
    }
    void release()
    {
        dealloc(buf_[0]);
        dealloc(buf_[1]);
        buf_[0] = buf_[1] = nullptr;
    }
    void reset()
    {
        pristine_ = true;
        for (uint32_t i = 0; i < size_; i++)
            buf_[0][i] = buf_[1][i] = 0;
        w_ = 0;
        phase_ = 0;
        last_[0] = last_[1] = 0;
        dsp::Smoother *const smoothers[] = {&depth_, &delay_, &fb_, &mix_, &spread_};
        for (auto *s : smoothers)
            s->setTime(0.05f, sampleRate());
        snapAll();
    }
    void onStart(const daw_process_t &) { snapAll(); }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_DEPTH: depth_.target = v * 0.001f * sampleRate(); break;
        case P_DELAY: delay_.target = v * 0.001f * sampleRate(); break;
        case P_FEEDBACK: fb_.target = v * 0.01f; break;
        case P_SPREAD: spread_.target = v / 360.0f; break;
        case P_MIX: mix_.target = v * 0.01f; break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *in[2] = {c.inputs[0] + start, c.inputs[1] + start};
        float *out[2] = {c.outputs[0] + start, c.outputs[1] + start};
        const float inc = param(P_RATE) / sampleRate();
        const uint32_t mask = size_ - 1;
        if (pristine_) {
            pristine_ = dsp::silent(in[0], in[1], n);
            if (pristine_) {
                w_ = (w_ + n) & mask;
                for (uint32_t i = 0; i < n; i++) {
                    depth_.next(); delay_.next(); spread_.next(); mix_.next(); fb_.next();
                    phase_ += inc;
                    if (phase_ >= 1.0f) phase_ -= 1.0f;
                    out[0][i] = out[1][i] = 0;
                }
                return;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            const float depth = depth_.next(), base = delay_.next(), fb = fb_.next(), spread = spread_.next(), m = mix_.next();
            for (int ch = 0; ch < 2; ch++) {
                // the feedback path is soft-limited: high feedback rings but cannot run away
                buf_[ch][w_] = dsp::sanitize(in[ch][i] + dsp::softClip(last_[ch] * fb));
                float ph = phase_ + (ch ? spread : 0.0f);
                ph -= (float)(int)ph;
                float d = base + depth * 0.5f * (1.0f + dsp::sinTurnsUnit(ph));
                if (d < 1.0f)
                    d = 1.0f;
                const float pos = (float)w_ - d, fl = (float)dsp::floorInt(pos), t = pos - fl;
                const uint32_t k = (uint32_t)(int32_t)fl;
                const float y = buf_[ch][k & mask] + (buf_[ch][(k + 1) & mask] - buf_[ch][k & mask]) * t;
                last_[ch] = y;
                out[ch][i] = in[ch][i] + (y - in[ch][i]) * m;  // 50 %: the deepest comb notches
            }
            w_ = (w_ + 1) & mask;
            phase_ += inc;
            if (phase_ >= 1.0f)
                phase_ -= 1.0f;
        }
    }

private:
    bool pristine_ = true;
    void snapAll()
    {
        depth_.snap(param(P_DEPTH) * 0.001f * sampleRate());
        delay_.snap(param(P_DELAY) * 0.001f * sampleRate());
        fb_.snap(param(P_FEEDBACK) * 0.01f);
        spread_.snap(param(P_SPREAD) / 360.0f);
        mix_.snap(param(P_MIX) * 0.01f);
    }
    float *buf_[2] = {nullptr, nullptr};
    uint32_t size_ = 0, w_ = 0;
    float phase_ = 0, last_[2] = {0, 0};
    dsp::Smoother depth_, delay_, fb_, mix_, spread_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.flanger", .name = "Flanger", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Stereo flanger with positive / negative feedback", .category = "Modulation",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = DAW_CAP_TAIL, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = sizeof kPresets / sizeof kPresets[0], .presets = kPresets,
    .key_names = nullptr, .mem_fast_bytes = sizeof(Flanger), .mem_large_bytes = 2 * 1024 * 4,
    .cpu_permille_base = 15, .cpu_permille_voice = 0,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(Flanger),
};
const daw_plugin_descriptor_t *Flanger::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_flanger(void) { return &kDescriptor; }
