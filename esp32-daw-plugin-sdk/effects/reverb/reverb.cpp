// SPDX-License-Identifier: MIT
// Reverb: a Schroeder / Moorer room after the public-domain Freeverb design (Jezar at Dreampoint):
// eight damped feedback combs in parallel and four series all-passes per channel, the right
// channel's delays offset for decorrelation. Added: pre-delay, an input low cut, damping set as a
// frequency, and a freeze switch. Original implementation (no Freeverb code is used; its published
// delay lengths are tuning constants, scaled to the sample rate).
// Delay lines live in DAW_MEM_LARGE (PSRAM on the ESP32-S3): 137 KB at 44.1 kHz, 149 KB at 48 kHz.
// Quality Light runs the comb / all-pass network at half the sample rate (lines half as long in
// samples, same times), between 7-tap half-band filters: half the work and half the PSRAM traffic,
// for a tail without content above about a quarter of the sample rate. Full is the default.
#include <math.h>

#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum : uint32_t { P_SIZE, P_DAMP, P_PREDELAY, P_LOWCUT, P_WIDTH, P_MIX, P_FREEZE, P_QUALITY, P_COUNT };
const char *const kQualities[] = {"Full", "Light"};
const daw_param_desc_t kParams[P_COUNT] = {
    {P_SIZE, "size", "Size", "Room", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 70.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_DAMP, "damping", "Damping", "Room", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 1000.0f, 20000.0f, 6000.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_PREDELAY, "predelay", "Pre-delay", "Room", "ms", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 200.0f, 10.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_LOWCUT, "lowcut", "Low cut", "Tone", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 20.0f, 1000.0f, 120.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_WIDTH, "width", "Width", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 100.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_MIX, "mix", "Mix", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 25.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_FREEZE, "freeze", "Freeze", "Room", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_QUALITY, "quality", "Quality", "Room", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 2, kQualities},
};
//                         size damp predelay lowcut width mix freeze quality
const float kRoom[P_COUNT] = {45, 8000, 5, 150, 80, 20, 0, 0};
const float kHall[P_COUNT] = {88, 5000, 25, 100, 100, 30, 0, 0};
const float kPlate[P_COUNT] = {75, 11000, 0, 200, 100, 28, 0, 0};
const float kAmbience[P_COUNT] = {25, 9000, 0, 250, 100, 15, 0, 0};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}, {"Room", "Room", kRoom}, {"Hall", "Hall", kHall}, {"Bright Plate", "Plate", kPlate}, {"Ambience", "Room", kAmbience}};

constexpr int kCombs = 8, kAllpasses = 4;
const int kCombLen[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};  // at 44.1 kHz
const int kAllpassLen[kAllpasses] = {556, 441, 341, 225};
constexpr int kSpread = 23;
constexpr float kInputGain = 0.015f, kWetScale = 3.0f;
constexpr float kMaxPredelayMs = 200.0f;

struct Line {
    float *buf = nullptr;
    int len = 0, pos = 0;
};

class Reverb : public Plugin<Reverb, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();

    int32_t init()
    {
        const float k = sampleRate() / 44100.0f;
        int total = 0;
        for (int ch = 0; ch < 2; ch++) {
            for (int i = 0; i < kCombs; i++) {
                total += comb_[ch][i].len = (int)((kCombLen[i] + ch * kSpread) * k) + 1;
                combHalf_[ch][i] = (int)((kCombLen[i] + ch * kSpread) * k * 0.5f) + 1;
            }
            for (int i = 0; i < kAllpasses; i++) {
                total += ap_[ch][i].len = (int)((kAllpassLen[i] + ch * kSpread) * k) + 1;
                apHalf_[ch][i] = (int)((kAllpassLen[i] + ch * kSpread) * k * 0.5f) + 1;
            }
        }
        pre_.len = (int)(kMaxPredelayMs * 0.001f * sampleRate()) + 2;
        total += pre_.len;
        mem_ = alloc<float>((size_t)total, DAW_MEM_LARGE);
        if (!mem_)
            return DAW_ERR_NOMEM;
        float *p = mem_;
        for (int ch = 0; ch < 2; ch++) {
            for (auto &l : comb_[ch])
                (l.buf = p), p += l.len;
            for (auto &l : ap_[ch])
                (l.buf = p), p += l.len;
        }
        pre_.buf = p;
        total_ = total;
        for (int ch = 0; ch < 2; ch++) {
            for (int i = 0; i < kCombs; i++)
                combFull_[ch][i] = comb_[ch][i].len;
            for (int i = 0; i < kAllpasses; i++)
                apFull_[ch][i] = ap_[ch][i].len;
        }
        return DAW_OK;
    }
    void release()
    {
        dealloc(mem_);
        mem_ = nullptr;
    }
    void reset()
    {
        pristine_ = true;
        for (int i = 0; i < total_; i++)
            mem_[i] = 0;
        for (int ch = 0; ch < 2; ch++) {
            for (auto &l : comb_[ch])
                l.pos = 0;
            for (auto &l : ap_[ch])
                l.pos = 0;
            for (float &f : store_[ch])
                f = 0;
        }
        pre_.pos = 0;
        hpX_ = hpY_ = 0;
        setQuality(param(P_QUALITY) >= 0.5f);
        mix_.setTime(0.02f, sampleRate());
        width_.setTime(0.02f, sampleRate());
        fb_.setTime(0.05f, sampleRate());
        snapAll();
    }
    void onStart(const daw_process_t &) { snapAll(); }
    void onParam(uint32_t id, float v)
    {
        switch (id) {
        case P_SIZE:
        case P_FREEZE: fb_.target = feedback(); break;
        case P_DAMP:
            damp_ = expf(-dsp::kTwoPi * v / sampleRate());
            dampHalf_ = expf(-dsp::kTwoPi * v / (0.5f * sampleRate()));
            break;
        case P_LOWCUT: hpA_ = expf(-dsp::kTwoPi * v / sampleRate()); break;
        case P_WIDTH: width_.target = v * 0.01f; break;
        case P_MIX: mix_.target = v * 0.01f; break;
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        const float *iL = c.inputs[0] + start, *iR = c.inputs[1] + start;
        float *oL = c.outputs[0] + start, *oR = c.outputs[1] + start;
        const int predelay = (int)(param(P_PREDELAY) * 0.001f * sampleRate());
        const bool freeze = param(P_FREEZE) >= 0.5f;
        const float damp = freeze ? 0.0f : damp_, inGain = freeze ? 0.0f : kInputGain;
        if ((param(P_QUALITY) >= 0.5f) != light_)
            setQuality(!light_);  // a quality change starts from an empty room
        if (pristine_) {
            for (uint32_t i = 0; i < n; i++)
                if (iL[i] != 0 || iR[i] != 0) { pristine_ = false; break; }
            if (pristine_) {
                // All delay/filter state is known zero since reset. Preserve positions and
                // smoothing exactly; after the first signal always process every tail/freeze.
                for (int ch = 0; ch < 2; ch++) {
                    for (auto &l : comb_[ch]) l.pos = (l.pos + n) % l.len;
                    for (auto &l : ap_[ch]) l.pos = (l.pos + n) % l.len;
                }
                pre_.pos = (pre_.pos + n) % pre_.len;
                for (uint32_t i = 0; i < n; i++) {
                    fb_.next(); width_.next(); mix_.next();
                    oL[i] = oR[i] = 0;
                }
                return;
            }
        }
        if (light_) {
            renderLight(iL, iR, oL, oR, n, predelay, freeze ? 0.0f : dampHalf_, inGain);
            return;
        }
        renderBlocked(iL, iR, oL, oR, n, predelay, damp, inGain, false);
    }

private:
    // Light quality: the network at half rate. Input: 7-tap half-band low-pass (-1, 0, 9, 16, 9, 0,
    // -1) / 32 and every second sample; output: the same filter interpolates back (even samples
    // the network's value, odd samples (9 (a + b) - (a' + b')) / 16), 2.5 half-rate samples later.
    void renderLight(const float *iL, const float *iR, float *oL, float *oR, uint32_t n, int predelay, float damp, float inGain)
    {
        renderBlocked(iL, iR, oL, oR, n, predelay, damp, inGain, true);
    }

    // Work on each delay line contiguously. Every comb's feedback recurrence and each frame's
    // summation order are unchanged. Small chunks bound stack use on the board's audio task.
    static constexpr uint32_t kChunk = 64;
    struct Scratch {
        float input[kChunk], feedback[kChunk], width[kChunk], mix[kChunk];
        float wet[2][kChunk];
    };
    void networkBlock(int ch, const float *input, const float *feedback, float *acc, uint32_t n, float damp)
    {
        for (uint32_t i = 0; i < n; ++i) acc[i] = 0;
        for (int k = 0; k < kCombs; ++k) {
            Line &l = comb_[ch][k];
            float store = store_[ch][k];
            int pos = l.pos;
            for (uint32_t i = 0; i < n; ++i) {
                const float y = l.buf[pos];
                store = dsp::sanitize(y * (1.0f - damp) + store * damp);
                l.buf[pos] = input[i] + store * feedback[i];
                if (++pos >= l.len) pos = 0;
                acc[i] += y;
            }
            l.pos = pos;
            store_[ch][k] = store;
        }
        for (int k = 0; k < kAllpasses; ++k) {
            Line &l = ap_[ch][k];
            int pos = l.pos;
            for (uint32_t i = 0; i < n; ++i) {
                const float b = l.buf[pos];
                l.buf[pos] = dsp::sanitize(acc[i] + b * 0.5f);
                acc[i] = b - acc[i];
                if (++pos >= l.len) pos = 0;
            }
            l.pos = pos;
        }
    }
    struct NetworkJob { Reverb *owner; Scratch *scratch; uint32_t count; float damp; int channel; };
    static void networkJob(void *arg)
    {
        const auto &j = *static_cast<NetworkJob *>(arg);
        j.owner->networkBlock(j.channel, j.scratch->input, j.scratch->feedback,
                             j.scratch->wet[j.channel], j.count, j.damp);
    }
    void renderBlocked(const float *iL, const float *iR, float *oL, float *oR, uint32_t n,
                       int predelay, float damp, float inGain, bool halfRate)
    {
        Scratch s;
        for (uint32_t base = 0; base < n; base += kChunk) {
            const uint32_t count = n - base < kChunk ? n - base : kChunk;
            uint32_t networkCount = 0;
            bool phase = odd_;
            for (uint32_t i = 0; i < count; ++i) {
                const float x = 0.5f * (iL[base + i] + iR[base + i]);
                hpY_ = hpA_ * (hpY_ + x - hpX_);
                hpX_ = x;
                pre_.buf[pre_.pos] = hpY_;
                int rp = pre_.pos - predelay;
                if (rp < 0) rp += pre_.len;
                const float in = pre_.buf[rp] * inGain;
                if (++pre_.pos >= pre_.len) pre_.pos = 0;
                const float fb = fb_.next();
                s.width[i] = width_.next();
                s.mix[i] = mix_.next();
                if (halfRate) {
                    for (int k = 0; k < 6; ++k) dec_[k] = dec_[k + 1];
                    dec_[6] = in;
                    if (phase) {
                        s.input[networkCount] = (9.0f / 32.0f) * (dec_[2] + dec_[4])
                            - (1.0f / 32.0f) * (dec_[0] + dec_[6]) + 0.5f * dec_[3];
                        s.feedback[networkCount++] = fb;
                    }
                    phase = !phase;
                } else {
                    s.input[networkCount] = in;
                    s.feedback[networkCount++] = fb;
                }
            }
            NetworkJob jobs[] = {{this, &s, networkCount, damp, 0}, {this, &s, networkCount, damp, 1}};
            const bool parallel = networkCount >= 16 && host_->struct_size >= offsetof(daw_host_api_t, parallel) + sizeof(host_->parallel)
                && host_->parallel && host_->parallel(host_->host_ctx, networkJob, &jobs[0], networkJob, &jobs[1]);
            if (!parallel) { networkJob(&jobs[0]); networkJob(&jobs[1]); }
            uint32_t networkPos = 0;
            for (uint32_t i = 0; i < count; ++i) {
                float wet[2];
                if (halfRate) {
                    if (odd_) {
                        wet[0] = pend_[0][1]; wet[1] = pend_[1][1];
                        for (int ch = 0; ch < 2; ++ch) {
                            float *h = hist_[ch];
                            h[0] = h[1]; h[1] = h[2]; h[2] = h[3];
                            h[3] = s.wet[ch][networkPos] * kWetScale;
                            pend_[ch][0] = h[1];
                            pend_[ch][1] = (9.0f / 16.0f) * (h[1] + h[2]) - (1.0f / 16.0f) * (h[0] + h[3]);
                        }
                        ++networkPos;
                    } else {
                        wet[0] = pend_[0][0]; wet[1] = pend_[1][0];
                    }
                    odd_ = !odd_;
                } else {
                    wet[0] = s.wet[0][i] * kWetScale;
                    wet[1] = s.wet[1][i] * kWetScale;
                }
                const float w = s.width[i], m = s.mix[i];
                const float w1 = 0.5f + 0.5f * w, w2 = 0.5f - 0.5f * w;
                const float wl = wet[0] * w1 + wet[1] * w2, wr = wet[1] * w1 + wet[0] * w2;
                const float dl = iL[base + i], dr = iR[base + i];
                oL[base + i] = dl * (1.0f - m) + wl * m;
                oR[base + i] = dr * (1.0f - m) + wr * m;
            }
        }
    }
    // line lengths for a quality, and an empty room (a change clears the tail)
    void setQuality(bool light)
    {
        light_ = light;
        for (int ch = 0; ch < 2; ch++) {
            for (int i = 0; i < kCombs; i++)
                comb_[ch][i] = Line{comb_[ch][i].buf, light ? combHalf_[ch][i] : combFull_[ch][i], 0};
            for (int i = 0; i < kAllpasses; i++)
                ap_[ch][i] = Line{ap_[ch][i].buf, light ? apHalf_[ch][i] : apFull_[ch][i], 0};
            for (float &f : store_[ch])
                f = 0;
            for (float &f : hist_[ch])
                f = 0;
            pend_[ch][0] = pend_[ch][1] = 0;
        }
        for (float &f : dec_)
            f = 0;
        odd_ = false;
        for (int i = 0; i < total_; i++)
            mem_[i] = 0;
        pre_.pos = 0;
        pristine_ = true;
    }
    int combFull_[2][kCombs] = {}, combHalf_[2][kCombs] = {}, apFull_[2][kAllpasses] = {}, apHalf_[2][kAllpasses] = {};
    bool light_ = false, odd_ = false;
    float dec_[7] = {}, hist_[2][4] = {}, pend_[2][2] = {};
    float dampHalf_ = 0.6f;
    bool pristine_ = true;
    float feedback() const { return param(P_FREEZE) >= 0.5f ? 1.0f : 0.7f + 0.28f * param(P_SIZE) * 0.01f; }
    void snapAll()
    {
        fb_.snap(feedback());
        mix_.snap(param(P_MIX) * 0.01f);
        width_.snap(param(P_WIDTH) * 0.01f);
    }
    Line comb_[2][kCombs], ap_[2][kAllpasses], pre_;
    float store_[2][kCombs] = {};
    float *mem_ = nullptr;
    int total_ = 0;
    float damp_ = 0.4f, hpA_ = 0.98f, hpX_ = 0, hpY_ = 0;
    dsp::Smoother mix_, width_, fb_;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t), .api_major = DAW_PLUGIN_API_MAJOR, .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.fx.reverb", .name = "Reverb", .vendor = "smhgzll", .version = "0.1.0", .license = "MIT",
    .description = "Stereo room / hall reverb: damped comb network with pre-delay, low cut and freeze", .category = "Reverb",
    .kind = DAW_PLUGIN_EFFECT, .state_version = 1, .capabilities = DAW_CAP_TAIL, .max_voices = 0,
    .param_count = P_COUNT, .params = kParams, .preset_count = sizeof kPresets / sizeof kPresets[0], .presets = kPresets,
    .key_names = nullptr, .mem_fast_bytes = sizeof(Reverb), .mem_large_bytes = 150000,  // 149 KB of lines at 48 kHz
    .cpu_permille_base = 60, .cpu_permille_voice = 0,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(Reverb),
};
const daw_plugin_descriptor_t *Reverb::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_fx_reverb(void) { return &kDescriptor; }
