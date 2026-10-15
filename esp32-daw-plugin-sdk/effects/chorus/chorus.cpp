// SPDX-License-Identifier: MIT
// Chorus: up to three modulated delay voices per channel (sine LFOs spread in phase across the
// voices and between left and right), read with cubic interpolation. Delay lines in
// DAW_MEM_LARGE (PSRAM on the ESP32-S3), 64 ms per channel.
#include <math.h>

#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_RATE, P_DEPTH, P_DELAY, P_VOICES, P_SPREAD, P_TONE, P_MIX, P_COUNT };
const daw_param_desc_t kParams[P_COUNT] = {
    {P_RATE, "rate", "Rate", "Modulation", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.05f, 5.0f, 0.6f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DEPTH, "depth", "Depth", "Modulation", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 10.0f, 3.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DELAY, "delay", "Delay", "Modulation", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 2.0f, 40.0f, 14.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_VOICES, "voices", "Voices", "Modulation", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1.0f, 3.0f, 2.0f, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_SPREAD, "spread", "Stereo spread", "Output", "°", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 180.0f, 90.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_TONE, "tone", "Tone", "Output", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 1000.0f, 20000.0f, 12000.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MIX, "mix", "Mix", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 50.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
//                           rate depth delay voices spread tone mix
const float kSubtle[P_COUNT] = {0.3f, 1.5f, 12, 2, 90, 14000, 30};
const float kLush[P_COUNT] = {0.5f, 5.0f, 20, 3, 120, 10000, 50};
const float kVibrato[P_COUNT] = {5.0f, 1.2f, 4, 1, 0, 20000, 100};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}, {"Subtle", "Basic", kSubtle}, {"Lush", "Wide", kLush}, {"Vibrato", "Pitch", kVibrato}};

constexpr float kMaxMs = 64.0f;

class Chorus : public Plugin<Chorus, P_COUNT> {
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
        lp_[0] = lp_[1] = 0;
        dsp::Smoother *const smoothers[] = {&depth_, &delay_, &mix_, &spread_};
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
        case P_SPREAD: spread_.target = v / 360.0f; break;
        case P_MIX: mix_.target = v * 0.01f; break;
        case P_TONE: lpA_ = 1.0f - expf(-dsp::kTwoPi * v / sampleRate()); break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *in[2] = {c.inputs[0] + start, c.inputs[1] + start};
        float *out[2] = {c.outputs[0] + start, c.outputs[1] + start};
        const int voices = (int)param(P_VOICES);
        const float inc = param(P_RATE) / sampleRate(), norm = 1.0f / (float)voices;
        float offset[3];
        for (int v = 0; v < voices; v++) offset[v] = (float)v / (float)voices;
        const uint32_t mask = size_ - 1;
        if (pristine_) {
            pristine_ = dsp::silent(in[0], in[1], n);
            if (pristine_) {
                w_ = (w_ + n) & mask;
                for (uint32_t i = 0; i < n; i++) {
                    depth_.next(); delay_.next(); spread_.next(); mix_.next();
                    phase_ += inc;
                    if (phase_ >= 1.0f) phase_ -= 1.0f;
                    out[0][i] = out[1][i] = 0;
                }
                return;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            const float depth = depth_.next(), base = delay_.next(), spread = spread_.next(), m = mix_.next();
            for (int ch = 0; ch < 2; ch++) {
                buf_[ch][w_] = in[ch][i];
                float wet = 0;
                for (int v = 0; v < voices; v++) {
                    float ph = phase_ + offset[v] + (ch ? spread : 0.0f);
                    ph -= (float)(int)ph;
                    // delay from base to base + depth (never below 1 sample)
                    const float d = base + depth * 0.5f * (1.0f + dsp::sinTurnsUnit(ph));
                    wet += readCubic(buf_[ch], mask, (float)w_ - (d < 1.0f ? 1.0f : d));
                }
                wet *= norm;
                lp_[ch] += lpA_ * (wet - lp_[ch]);
                out[ch][i] = in[ch][i] + (dsp::sanitize(lp_[ch]) - in[ch][i]) * m;
            }
            w_ = (w_ + 1) & mask;
            phase_ += inc;
            if (phase_ >= 1.0f)
                phase_ -= 1.0f;
        }
    }

private:
    bool pristine_ = true;
    static float readCubic(const float *b, uint32_t mask, float pos)
    {
        const float fl = (float)dsp::floorInt(pos);
        const float t = pos - fl;
        const uint32_t i = (uint32_t)(int32_t)fl;
        const float y0 = b[(i - 1) & mask], y1 = b[i & mask], y2 = b[(i + 1) & mask], y3 = b[(i + 2) & mask];
        // Catmull-Rom
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }
    void snapAll()
    {
        depth_.snap(param(P_DEPTH) * 0.001f * sampleRate());
        delay_.snap(param(P_DELAY) * 0.001f * sampleRate());
        spread_.snap(param(P_SPREAD) / 360.0f);
        mix_.snap(param(P_MIX) * 0.01f);
        lpA_ = 1.0f - expf(-dsp::kTwoPi * param(P_TONE) / sampleRate());
    }
    float *buf_[2] = {nullptr, nullptr};
    uint32_t size_ = 0, w_ = 0;
    float phase_ = 0, lpA_ = 1.0f, lp_[2] = {0, 0};
    dsp::Smoother depth_, delay_, mix_, spread_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.chorus", .name = "Chorus", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Stereo chorus with up to three voices, tone control and vibrato setting", .category = "Modulation",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = DAW_CAP_TAIL, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = sizeof kPresets / sizeof kPresets[0], .presets = kPresets,
    .key_names = nullptr, .mem_fast_bytes = sizeof(Chorus), .mem_large_bytes = 2 * 4096 * 4,
    .cpu_permille_base = 30, .cpu_permille_voice = 0,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(Chorus),
};
const daw_plugin_descriptor_t *Chorus::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_chorus(void) { return &kDescriptor; }
