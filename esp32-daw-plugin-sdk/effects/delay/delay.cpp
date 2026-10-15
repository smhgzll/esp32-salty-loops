// SPDX-License-Identifier: MIT
// Delay: stereo / ping-pong delay with tempo sync, feedback, a damping low-pass in the loop and
// dry/wet. Delay lines (up to 2 s per channel) live in DAW_MEM_LARGE (PSRAM on the ESP32-S3).
// Time changes are smoothed (a short pitch glide, no clicks).
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_TIME, P_SYNC, P_FEEDBACK, P_DAMP, P_PINGPONG, P_MIX, P_COUNT };
const char *const kSync[] = {"Off (ms)", "1/32", "1/16", "1/16 dotted", "1/8 triplet", "1/8", "1/8 dotted", "1/4 triplet", "1/4", "1/4 dotted", "1/2", "1 bar"};
const float kSyncBeats[] = {0, 0.125f, 0.25f, 0.375f, 1.0f / 3.0f, 0.5f, 0.75f, 2.0f / 3.0f, 1.0f, 1.5f, 2.0f, 4.0f};
constexpr float kMaxSeconds = 2.0f;

const daw_param_desc_t kParams[P_COUNT] = {
    {P_TIME, "time", "Time", "Delay", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 1.0f, 2000.0f, 375.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_SYNC, "sync", "Sync", "Delay", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 11, 6, DAW_PARAM_NO_SMOOTH, 12, kSync},
    {P_FEEDBACK, "feedback", "Feedback", "Delay", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 95.0f, 40.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DAMP, "damping", "Damping", "Tone", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 500.0f, 20000.0f, 6000.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_PINGPONG, "pingpong", "Ping-pong", "Delay", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 1, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_MIX, "mix", "Mix", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 30.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
};
const float kSlap[P_COUNT] = {90, 0, 15, 8000, 0, 25};
const float kDub[P_COUNT] = {375, 6, 70, 2500, 1, 35};
const float kWide[P_COUNT] = {250, 5, 45, 9000, 1, 30};
const daw_preset_desc_t kPresets[] = {{"Dotted 8th", "Sync", nullptr}, {"Slapback", "Short", kSlap}, {"Dub", "Long", kDub}, {"Wide 8th", "Sync", kWide}};

class Delay : public Plugin<Delay, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onStart(const daw_process_t &c)
    {
        time_.snap(delaySamples(c.tempo_bpm));
        fb_.snap(fb_.target);
        mix_.snap(mix_.target);
    }
    int32_t init()
    {
        len_ = (uint32_t)(kMaxSeconds * sampleRate()) + 4;
        bufL_ = alloc<float>(len_, DAW_MEM_LARGE);
        bufR_ = alloc<float>(len_, DAW_MEM_LARGE);
        return bufL_ && bufR_ ? DAW_OK : DAW_ERR_NOMEM;
    }
    void release()
    {
        dealloc(bufL_);
        dealloc(bufR_);
        bufL_ = bufR_ = nullptr;
    }
    void reset()
    {
        for (uint32_t i = 0; i < len_; i++)
            bufL_[i] = bufR_[i] = 0;
        w_ = 0;
        quietWrites_ = len_;  // all samples in both lines are known to be exactly zero
        lpL_ = lpR_ = 0;
        time_.setTime(0.08f, sampleRate());
        fb_.setTime(0.02f, sampleRate());
        mix_.setTime(0.02f, sampleRate());
        time_.snap(delaySamples(120.0));
        fb_.snap(param(P_FEEDBACK) * 0.01f);
        mix_.snap(param(P_MIX) * 0.01f);
    }
    void onParam(uint32_t id, float v)
    {
        if (id == P_FEEDBACK)
            fb_.target = v * 0.01f;
        else if (id == P_MIX)
            mix_.target = v * 0.01f;
        else if (id == P_DAMP)
            dampCoef_ = 1.0f - expf(-dsp::kTwoPi * v / sampleRate());
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *iL = c.inputs[0] + start, *iR = c.inputs[1] + start;
        float *oL = c.outputs[0] + start, *oR = c.outputs[1] + start;
        time_.target = delaySamples(c.tempo_bpm);
        const bool pp = param(P_PINGPONG) > 0.5f;
        const float damp = dampCoef_;
        if (quietWrites_ == len_) {
            bool inputSilent = true;
            for (uint32_t i = 0; i < n; i++)
                if (iL[i] != 0.0f || iR[i] != 0.0f) {
                    inputSilent = false;
                    break;
                }
            if (inputSilent) {
                // Advance time and smoothing exactly as normal rendering does, without reading
                // or writing the already empty PSRAM lines. Future notes keep the same timing.
                for (uint32_t i = 0; i < n; i++) {
                    time_.next();
                    fb_.next();
                    mix_.next();
                    oL[i] = oR[i] = 0.0f;
                    if (++w_ == len_)
                        w_ = 0;
                }
                return;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            float d = time_.next();
            float rp = (float)w_ - d;
            if (rp < 0)
                rp += (float)len_;
            uint32_t i0 = (uint32_t)rp;
            float fr = rp - (float)i0;
            uint32_t i1 = i0 + 1 == len_ ? 0 : i0 + 1;
            float dl = bufL_[i0] + (bufL_[i1] - bufL_[i0]) * fr;
            float dr = bufR_[i0] + (bufR_[i1] - bufR_[i0]) * fr;
            lpL_ = dsp::sanitize(lpL_ + (dl - lpL_) * damp);
            lpR_ = dsp::sanitize(lpR_ + (dr - lpR_) * damp);
            float fb = fb_.next();
            float l = iL[i], r = iR[i];
            float writeL, writeR;
            if (pp) {  // mono input into the left line, the lines feed each other
                writeL = dsp::softClip(0.5f * (l + r) + lpR_ * fb);
                writeR = dsp::softClip(lpL_ * fb);
            } else {
                writeL = dsp::softClip(l + lpL_ * fb);
                writeR = dsp::softClip(r + lpR_ * fb);
            }
            bufL_[w_] = writeL;
            bufR_[w_] = writeR;
            if (writeL == 0.0f && writeR == 0.0f && lpL_ == 0.0f && lpR_ == 0.0f) {
                if (quietWrites_ < len_)
                    ++quietWrites_;
            } else {
                quietWrites_ = 0;
            }
            if (++w_ == len_)
                w_ = 0;
            float m = mix_.next();
            oL[i] = l * (1.0f - m) + dl * m;
            oR[i] = r * (1.0f - m) + dr * m;
        }
    }

private:
    float delaySamples(double bpm) const
    {
        int s = (int)param(P_SYNC);
        float sec = param(P_TIME) * 0.001f;
        if (s > 0 && bpm > 1.0)
            sec = kSyncBeats[s] * 60.0f / (float)bpm;  // single precision (the S3 has no double FPU)
        float d = sec * sampleRate();
        float maxd = (float)(len_ - 3);
        return d < 1.0f ? 1.0f : (d > maxd ? maxd : d);
    }
    float *bufL_ = nullptr, *bufR_ = nullptr;
    uint32_t len_ = 0, w_ = 0, quietWrites_ = 0;
    float lpL_ = 0, lpR_ = 0, dampCoef_ = 0.5f;
    dsp::Smoother time_, fb_, mix_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.delay", .name = "Delay", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Stereo / ping-pong delay with tempo sync, feedback and damping", .category = "Delay",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = DAW_CAP_TAIL, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = 4, .presets = kPresets, .key_names = nullptr,
    .mem_fast_bytes = sizeof(Delay), .mem_large_bytes = 2 * ((uint32_t)(kMaxSeconds * 48000) + 4) * 4,
    .cpu_permille_base = 15, .cpu_permille_voice = 0,
    DAWSDK_CALLBACKS(Delay),
};
const daw_plugin_descriptor_t *Delay::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_delay(void) { return &kDescriptor; }
