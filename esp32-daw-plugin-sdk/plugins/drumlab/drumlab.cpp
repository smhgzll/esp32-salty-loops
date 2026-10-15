// SPDX-License-Identifier: MIT
// DrumLab: analog-style drum machine. Kick, snare and hi-hat voices are DaisySP's drum models
// (MIT, themselves ports of Mutable Instruments Plaits; third_party/DaisySP, see PROVENANCE.md);
// the clap and the voice management, parameters and key mapping are original.
//
// Modes: "Kit" maps GM drum keys to the eight pieces (36 kick, 38 snare, 42/46 hats, 39 clap,
// 41..50 toms...). "Single <piece>" plays one piece on every key, pitched relative to C5 (key 60),
// which is how a Channel Rack row uses it (one channel per drum sound).
#include "Drums/analogbassdrum.h"
#include "Drums/analogsnaredrum.h"
#include "Drums/hihat.h"
#include "Drums/synthbassdrum.h"
#include "Drums/synthsnaredrum.h"
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

using namespace dawsdk;

namespace {

enum Piece : uint32_t { KICK, KICK2, SNARE, SNARE2, CHAT, OHAT, CLAP, TOM, NPIECE };
const char *const kPieceNames[NPIECE] = {"Kick", "Kick 2", "Snare", "Snare 2", "Closed Hat", "Open Hat", "Clap", "Tom"};

// per-piece parameters: level, tune, decay, tone, character, pan
enum : uint32_t { PP_LEVEL, PP_TUNE, PP_DECAY, PP_TONE, PP_CHAR, PP_PAN, PP_N };
enum : uint32_t { P_MODE = 0, P_MASTER = 1, P_PIECE0 = 2, P_COUNT = P_PIECE0 + (uint32_t)NPIECE * (uint32_t)PP_N };

const char *const kModeLabels[] = {"Kit", "Kick", "Kick 2", "Snare", "Snare 2", "Closed Hat", "Open Hat", "Clap", "Tom"};

#define PIECE_PARAMS(i, grp, tuneDef, decDef, toneDef, charDef, charName, panDef)                                                   \
    {P_PIECE0 + i * PP_N + PP_LEVEL, grp "_level", "Level", grp, "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.8f,             \
     DAW_PARAM_AUTOMATABLE, 0, nullptr},                                                                                          \
    {P_PIECE0 + i * PP_N + PP_TUNE, grp "_tune", "Tune", grp, "st", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -24.0f, 24.0f, tuneDef,  \
     DAW_PARAM_AUTOMATABLE, 0, nullptr},                                                                                          \
    {P_PIECE0 + i * PP_N + PP_DECAY, grp "_decay", "Decay", grp, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, decDef,      \
     DAW_PARAM_AUTOMATABLE, 0, nullptr},                                                                                          \
    {P_PIECE0 + i * PP_N + PP_TONE, grp "_tone", "Tone", grp, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, toneDef,        \
     DAW_PARAM_AUTOMATABLE, 0, nullptr},                                                                                          \
    {P_PIECE0 + i * PP_N + PP_CHAR, grp "_char", charName, grp, "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 1.0f, charDef,      \
     DAW_PARAM_AUTOMATABLE, 0, nullptr},                                                                                          \
    {P_PIECE0 + i * PP_N + PP_PAN, grp "_pan", "Pan", grp, "", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -1.0f, 1.0f, panDef,           \
     DAW_PARAM_AUTOMATABLE, 0, nullptr}

const daw_param_desc_t kParams[P_COUNT] = {
    {P_MODE, "mode", "Mode", "Global", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0.0f, 8.0f, 0.0f, DAW_PARAM_NO_SMOOTH, 9, kModeLabels},
    {P_MASTER, "master", "Master", "Global", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.8f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    PIECE_PARAMS(0, "kick", 0.0f, 0.5f, 0.5f, 0.3f, "Attack FM", 0.0f),
    PIECE_PARAMS(1, "kick2", 0.0f, 0.45f, 0.4f, 0.3f, "Dirt", 0.0f),
    PIECE_PARAMS(2, "snare", 0.0f, 0.4f, 0.5f, 0.6f, "Snappy", 0.0f),
    PIECE_PARAMS(3, "snare2", 0.0f, 0.4f, 0.5f, 0.5f, "Snappy", 0.0f),
    PIECE_PARAMS(4, "chat", 0.0f, 0.25f, 0.6f, 0.3f, "Noise", 0.15f),
    PIECE_PARAMS(5, "ohat", 0.0f, 0.65f, 0.55f, 0.3f, "Noise", 0.15f),
    PIECE_PARAMS(6, "clap", 0.0f, 0.4f, 0.5f, 0.5f, "Spread", -0.1f),
    PIECE_PARAMS(7, "tom", 0.0f, 0.5f, 0.4f, 0.2f, "Attack FM", 0.0f),
};

const char *const kKeyNames[128] = {
    // filled at the GM positions used by Kit mode
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "Kick 2",       // 35
    "Kick",         // 36
    "Rim",          // 37 (snare 2, tight)
    "Snare",        // 38
    "Clap",         // 39
    "Snare 2",      // 40
    "Low Tom",      // 41
    "Closed Hat",   // 42
    "Low Tom 2",    // 43
    "Pedal Hat",    // 44
    "Mid Tom",      // 45
    "Open Hat",     // 46
    "Mid Tom 2",    // 47
    "High Tom",     // 48
    nullptr,        // 49
    "High Tom 2",   // 50
};

struct KeyMap {
    int8_t piece;
    int8_t semis;
};
KeyMap kitMap(uint8_t key)
{
    switch (key) {
    case 35: return {KICK2, 0};
    case 36: return {KICK, 0};
    case 37: return {SNARE2, 7};
    case 38: return {SNARE, 0};
    case 39: return {CLAP, 0};
    case 40: return {SNARE2, 0};
    case 41: return {TOM, -5};
    case 42: return {CHAT, 0};
    case 43: return {TOM, -3};
    case 44: return {CHAT, -2};
    case 45: return {TOM, 0};
    case 46: return {OHAT, 0};
    case 47: return {TOM, 2};
    case 48: return {TOM, 5};
    case 50: return {TOM, 8};
    default: return {-1, 0};
    }
}

// original clap: three noise bursts 11 ms apart, then a band-passed tail
struct Clap {
    dsp::Svf bp;
    dsp::Rng rng;
    float env = 0, decayCoef = 0.999f, tone = 0.5f, spread = 0.5f, sr = 44100;
    uint32_t t = 0xFFFFFFFF;
    float accent = 1;
    void init(float rate)
    {
        sr = rate;
        rng.seed(0xC1A9u);
        bp.reset();
    }
    void set(float freqMul, float decay, float tn, float spr)
    {
        tone = tn;
        spread = spr;
        float dsec = 0.05f + decay * 0.6f;
        decayCoef = expf(-1.0f / (dsec * sr * 0.25f));
        bp.set(800.0f * freqMul * (0.6f + tone * 1.4f), 0.55f, sr);
    }
    void trig(float acc)
    {
        t = 0;
        accent = acc;
    }
    float process()
    {
        if (t == 0xFFFFFFFF)
            return 0;
        uint32_t gap = (uint32_t)(sr * (0.006f + 0.010f * spread));
        float e;
        if (t < 3 * gap) {
            uint32_t ph = t % gap;
            e = expf(-(float)ph / (sr * 0.0025f));  // burst
            env = 0.8f;
        } else {
            env *= decayCoef;
            e = env;
        }
        t++;
        if (t > (uint32_t)(sr * 4))
            t = 0xFFFFFFFF;
        return bp.process(rng.bipolar(), dsp::Svf::Band) * e * accent * 2.5f;
    }
};

class DrumLab : public Plugin<DrumLab, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onStart(const daw_process_t &) { master_.snap(master_.target); }

    int32_t init()
    {
        initPieces();
        return DAW_OK;
    }
    void reset()
    {
        initPieces();
        for (auto &s : sleep_)
            s = true;
    }
    void onParam(uint32_t id, float v)
    {
        if (id == P_MASTER)
            master_.target = v;
        if (id >= P_PIECE0)
            dirty_[(id - P_PIECE0) / PP_N] = true;
    }
    void onEvent(const daw_event_t &e)
    {
        if (e.type == DAW_EV_NOTE_ON) {
            int mode = (int)param(P_MODE);
            int piece;
            float semis;
            if (mode == 0) {
                KeyMap m = kitMap(e.u.note.key);
                if (m.piece < 0)
                    return;
                piece = m.piece;
                semis = m.semis;
            } else {
                piece = mode - 1;
                semis = (float)e.u.note.key - 60.0f;
            }
            semis += e.u.note.pitch;
            float vel = e.u.note.velocity;
            if (e.flags & DAW_NOTE_FLAG_ACCENT)
                vel = vel < 0.8f ? 0.8f + 0.2f * vel : vel;
            trigger(piece, semis, vel, e.u.note.pan);
        } else if (e.type == DAW_EV_ALL_SOUND_OFF) {
            initPieces();
            for (auto &s : sleep_)
                s = true;
        }
        // note-off and ALL_NOTES_OFF: one-shot sounds play out their decay
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        float *L = c.outputs[0] + start, *R = c.outputs[1] + start;
        for (uint32_t i = 0; i < n; i++)
            L[i] = R[i] = 0;
        for (uint32_t p = 0; p < NPIECE; p++) {
            if (sleep_[p])
                continue;
            if (dirty_[p])
                applyPiece(p);
            const float gl = gainL_[p], gr = gainR_[p];
            float follow = follow_[p];
            float x1 = dcX_[p], y1 = dcY_[p];
            for (uint32_t i = 0; i < n; i++) {
                // DC blocker (~7 Hz): some DaisySP models settle on a constant offset
                float x = processPiece(p);
                float y = x - x1 + 0.999f * y1;
                x1 = x;
                y1 = y;
                float s = y * vgain_[p];
                L[i] += s * gl;
                R[i] += s * gr;
                float a = s < 0 ? -s : s;
                follow = a > follow ? a : follow * 0.9995f;
            }
            dcX_[p] = x1;
            dcY_[p] = dsp::sanitize(y1);
            follow_[p] = dsp::sanitize(follow);
            // sleep after ~-110 dB for the rest of the block (keeps silence exact and saves CPU)
            if (follow_[p] < 3e-6f && ++quiet_[p] > 8) {
                sleep_[p] = true;
                follow_[p] = 0;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            float m = master_.next();
            L[i] *= m;
            R[i] *= m;
        }
    }
    uint32_t voices() const
    {
        uint32_t v = 0;
        for (bool s : sleep_)
            v += s ? 0 : 1;
        return v;
    }

private:
    float pp(uint32_t piece, uint32_t k) const { return param(P_PIECE0 + piece * PP_N + k); }

    void initPieces()
    {
        const float sr = sampleRate();
        kick_.Init(sr);
        kick2_.Init(sr);
        snare_.Init(sr);
        snare2_.Init(sr);
        chat_.Init(sr);
        ohat_.Init(sr);
        tom_.Init(sr);
        clap_.init(sr);
        for (uint32_t p = 0; p < NPIECE; p++) {
            dirty_[p] = true;
            sleep_[p] = true;
            semis_[p] = 0;
            follow_[p] = 0;
            quiet_[p] = 0;
            dcX_[p] = dcY_[p] = 0;
        }
        master_.setTime(0.01f, sr);
        master_.snap(param(P_MASTER));
    }
    // pitch in Hz for a piece, base frequencies chosen per model
    float freq(uint32_t p, float base) const { return base * dsp::fastExp2((pp(p, PP_TUNE) + semis_[p]) / 12.0f); }

    void applyPiece(uint32_t p)
    {
        dirty_[p] = false;
        const float dec = pp(p, PP_DECAY), tone = pp(p, PP_TONE), ch = pp(p, PP_CHAR);
        switch (p) {
        case KICK:
            kick_.SetFreq(freq(p, 50.0f));
            kick_.SetDecay(dec);
            kick_.SetTone(tone);
            kick_.SetAttackFmAmount(ch);
            kick_.SetSelfFmAmount(ch * 0.5f);
            break;
        case KICK2:
            kick2_.SetFreq(freq(p, 55.0f));
            kick2_.SetDecay(dec);
            kick2_.SetTone(tone);
            kick2_.SetDirtiness(ch);
            kick2_.SetFmEnvelopeAmount(0.6f);
            kick2_.SetFmEnvelopeDecay(0.3f);
            break;
        case SNARE:
            snare_.SetFreq(freq(p, 180.0f));
            snare_.SetDecay(dec);
            snare_.SetTone(tone);
            snare_.SetSnappy(ch);
            break;
        case SNARE2:
            snare2_.SetFreq(freq(p, 200.0f));
            snare2_.SetDecay(dec);
            snare2_.SetFmAmount(tone);
            snare2_.SetSnappy(ch);
            break;
        case CHAT:
            chat_.SetFreq(freq(p, 3000.0f));
            // DaisySP's HiHat maps SetDecay(x) to Plaits' decay 1.7x - 1.2; values above 1 reach
            // Plaits' long settings. Closed: Plaits -0.43..0.25, open: 0.25..0.99.
            chat_.SetDecay(0.45f + 0.4f * dec);
            chat_.SetTone(tone);
            chat_.SetNoisiness(ch);
            break;
        case OHAT:
            ohat_.SetFreq(freq(p, 3000.0f));
            ohat_.SetDecay(0.85f + 0.44f * dec);
            ohat_.SetTone(tone);
            ohat_.SetNoisiness(ch);
            break;
        case CLAP: clap_.set(fastPitch(p), dec, tone, ch); break;
        case TOM:
            tom_.SetFreq(freq(p, 110.0f));
            tom_.SetDecay(dec);
            tom_.SetTone(tone);
            tom_.SetAttackFmAmount(ch);
            tom_.SetSelfFmAmount(0.1f);
            break;
        }
        float l, r;
        dsp::panGains(pp(p, PP_PAN), l, r);
        gainL_[p] = l * pp(p, PP_LEVEL);
        gainR_[p] = r * pp(p, PP_LEVEL);
    }
    float fastPitch(uint32_t p) const { return dsp::fastExp2((pp(p, PP_TUNE) + semis_[p]) / 12.0f); }

    void trigger(int piece, float semis, float vel, float pan)
    {
        if (piece < 0 || piece >= (int)NPIECE)
            return;
        semis_[piece] = semis;
        dirty_[piece] = true;
        applyPiece((uint32_t)piece);
        if (pan != 0.0f) {  // per-note pan on top of the piece's pan
            float l, r;
            dsp::panGains(dsp::clamp(pp((uint32_t)piece, PP_PAN) + pan, -1.0f, 1.0f), l, r);
            gainL_[piece] = l * pp((uint32_t)piece, PP_LEVEL);
            gainR_[piece] = r * pp((uint32_t)piece, PP_LEVEL);
            dirty_[piece] = true;  // back to the piece's own pan on the next trigger / param change
        }
        vgain_[piece] = 0.35f + 0.65f * vel;
        sleep_[piece] = false;
        quiet_[piece] = 0;
        follow_[piece] = 1.0f;
        switch (piece) {
        case KICK: kick_.SetAccent(vel); kick_.Trig(); break;
        case KICK2: kick2_.SetAccent(vel); kick2_.Trig(); break;
        case SNARE: snare_.SetAccent(vel); snare_.Trig(); break;
        case SNARE2: snare2_.SetAccent(vel); snare2_.Trig(); break;
        case CHAT:
            chat_.SetAccent(vel);
            chat_.Trig();
            choke(OHAT);
            break;
        case OHAT:
            ohat_.SetAccent(vel);
            ohat_.Trig();
            break;
        case CLAP: clap_.trig(vel); break;
        case TOM: tom_.SetAccent(vel); tom_.Trig(); break;
        }
    }
    void choke(uint32_t p)
    {
        if (p == OHAT) {
            ohat_.Init(sampleRate());
            dirty_[OHAT] = true;
            sleep_[OHAT] = true;
        }
    }
    float processPiece(uint32_t p)
    {
        switch (p) {
        case KICK: return kick_.Process();
        case KICK2: return kick2_.Process();
        case SNARE: return snare_.Process();
        case SNARE2: return snare2_.Process();
        case CHAT: return chat_.Process();
        case OHAT: return ohat_.Process();
        case CLAP: return clap_.process();
        default: return tom_.Process();
        }
    }

    daisysp::AnalogBassDrum kick_, tom_;
    daisysp::SyntheticBassDrum kick2_;
    daisysp::AnalogSnareDrum snare_;
    daisysp::SyntheticSnareDrum snare2_;
    daisysp::HiHat<> chat_, ohat_;
    Clap clap_;
    float dcX_[NPIECE] = {}, dcY_[NPIECE] = {};
    float gainL_[NPIECE] = {}, gainR_[NPIECE] = {}, vgain_[NPIECE] = {}, semis_[NPIECE] = {}, follow_[NPIECE] = {};
    uint32_t quiet_[NPIECE] = {};
    bool sleep_[NPIECE] = {}, dirty_[NPIECE] = {};
    dsp::Smoother master_;
};

#define V(...) __VA_ARGS__
// presets: mode, master, then per piece level,tune,decay,tone,char,pan
const float kP808[P_COUNT] = {0, 0.8f,
                              0.9f, -2, 0.7f, 0.4f, 0.2f, 0,     0.8f, 0, 0.5f, 0.4f, 0.3f, 0,
                              0.75f, 0, 0.35f, 0.4f, 0.7f, 0,    0.7f, 0, 0.4f, 0.5f, 0.5f, 0,
                              0.6f, 0, 0.2f, 0.5f, 0.2f, 0.2f,   0.55f, 0, 0.6f, 0.5f, 0.2f, 0.2f,
                              0.7f, 0, 0.45f, 0.45f, 0.5f, -0.1f, 0.7f, 0, 0.55f, 0.3f, 0.2f, 0};
const float kPTight[P_COUNT] = {0, 0.85f,
                                1.0f, 2, 0.35f, 0.6f, 0.5f, 0,    0.9f, 3, 0.3f, 0.6f, 0.6f, 0,
                                0.8f, 3, 0.25f, 0.7f, 0.8f, 0,    0.7f, 2, 0.25f, 0.6f, 0.7f, 0,
                                0.6f, 2, 0.12f, 0.8f, 0.4f, 0.25f, 0.5f, 2, 0.35f, 0.8f, 0.4f, 0.25f,
                                0.75f, 2, 0.3f, 0.6f, 0.3f, -0.15f, 0.6f, 3, 0.35f, 0.5f, 0.4f, 0};
const daw_preset_desc_t kPresets[] = {
    {"Default Kit", "Kit", nullptr},
    {"Boom 808", "Kit", kP808},
    {"Tight", "Kit", kPTight},
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.drumlab",
    .name = "DrumLab",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Analog-style drum machine: kicks, snares, hats, clap and toms (DaisySP drum models)",
    .category = "Drums",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_POLYPHONIC | DAW_CAP_VELOCITY | DAW_CAP_ACCENT | DAW_CAP_DRUM_KIT | DAW_CAP_NOTE_PAN |
                    DAW_CAP_NOTE_PITCH,
    .max_voices = NPIECE,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = kKeyNames,
    .mem_fast_bytes = sizeof(DrumLab),
    .mem_large_bytes = 0,
    .cpu_permille_base = 2,
    .cpu_permille_voice = 25,
    DAWSDK_CALLBACKS(DrumLab),
};

const daw_plugin_descriptor_t *DrumLab::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_drumlab(void) { return &kDescriptor; }
