// SPDX-License-Identifier: MIT
// SoundFont: plays a SoundFont 2 (.sf2) from the sample library with TinySoundFont (Bernhard
// Schelling, MIT, commit 853a0a1; voice loop patched to single precision, PROVENANCE.md).
//
// The font arrives as raw file bytes (ABI 1.1 resource slot "soundfont", DAW_RES_FILE) on the
// control thread; TinySoundFont parses it there and borrows the aligned PCM16 sample pool.
// Metadata and voices live in LARGE memory. Unaligned sources copy PCM16 instead
// of expanding to float. TSF_MALLOC / TSF_REALLOC / TSF_FREE go to the
// host allocator through `g_host`, which is set only while the control thread loads, re-voices or
// closes the font. Nothing on the audio path allocates: all voices are created at load time
// (tsf_set_max_voices) and the one MIDI channel the plugin uses exists before activation.
#include <math.h>
#include <string.h>

#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"

namespace {

// ---- TinySoundFont on the host allocator ---------------------------------------------------------
const daw_host_api_t *g_host = nullptr;  // the instance loading / closing a font (control thread)

struct AllocHeader {
    size_t size;
    size_t pad;  // keeps the payload 16-byte aligned on 64-bit hosts, 8 on the S3
};
void *tsfMalloc(size_t n)
{
    if (!g_host)
        return nullptr;  // never called on the audio path: refuse instead of allocating there
    auto *h = static_cast<AllocHeader *>(g_host->alloc(g_host->host_ctx, sizeof(AllocHeader) + n, 16, DAW_MEM_LARGE));
    if (!h)
        return nullptr;
    h->size = n;
    return h + 1;
}
void tsfFree(void *p)
{
    if (p && g_host)
        g_host->free(g_host->host_ctx, static_cast<AllocHeader *>(p) - 1);
}
void *tsfRealloc(void *p, size_t n)
{
    void *q = tsfMalloc(n);
    if (q && p) {
        const size_t old = (static_cast<AllocHeader *>(p) - 1)->size;
        memcpy(q, p, old < n ? old : n);
        tsfFree(p);
    }
    return q;
}

}  // namespace

#define TSF_SHORT_SAMPLES
#define TSF_NO_STDIO
#define TSF_MALLOC tsfMalloc
#define TSF_REALLOC tsfRealloc
#define TSF_FREE tsfFree
#define TSF_IMPLEMENTATION
#define TSF_STATIC
#include "tsf.h"

using namespace dawsdk;

namespace {

enum : uint32_t { P_BANK, P_PROGRAM, P_LEVEL, P_TRANSPOSE, P_FINE, P_BEND, P_POLYPHONY, P_COUNT };
const daw_param_desc_t kParams[P_COUNT] = {
    {P_BANK, "bank", "Bank", "Preset", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 16383, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_PROGRAM, "program", "Program", "Preset", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 127, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_LEVEL, "level", "Level", "Output", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 1.0f, 0.8f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_TRANSPOSE, "transpose", "Transpose", "Pitch", "st", DAW_PARAM_INT, DAW_SCALE_BIPOLAR, -24, 24, 0, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_FINE, "fine", "Fine", "Pitch", "ct", DAW_PARAM_FLOAT, DAW_SCALE_BIPOLAR, -100.0f, 100.0f, 0.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_BEND, "bend_range", "Bend range", "Pitch", "st", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 24, 2, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    // sample voices (a note can play several layered samples; releases ring on): fewer cost less
    {P_POLYPHONY, "polyphony", "Voices", "Voices", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 24, 24, DAW_PARAM_NO_SMOOTH, 0, nullptr},
};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}};
const daw_resource_slot_desc_t kSlots[] = {{"soundfont", "SoundFont", DAW_RES_MASK(DAW_RES_FILE), "sf2"}};
constexpr int kMaxVoices = 24;

class SoundFont : public Plugin<SoundFont, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();

    void release() { close(); }
    int32_t setResource(uint32_t slot, const daw_resource_t *res)
    {
        if (slot != 0)
            return DAW_ERR_ARG;
        close();
        if (!res)
            return DAW_OK;
        if (res->type != DAW_RES_FILE || !res->data || res->size < 12 || memcmp(res->data, "RIFF", 4) != 0 ||
            memcmp(static_cast<const char *>(res->data) + 8, "sfbk", 4) != 0)
            return DAW_ERR_ARG;
        g_host = host_;
        tsf *f = tsf_load_memory(res->data, (int)res->size);
        bool ok = f && tsf_get_presetcount(f) > 0;
        if (ok) {
            tsf_set_output(f, TSF_STEREO_UNWEAVED, (int)sampleRate(), 0.0f);
            // voices in LARGE memory too: the sample loop keeps their phase, fraction and filter
            // state in locals, so a voice is read and written once per 64-sample block. 24 voices
            // of internal RAM per channel (5.4 KB) left a song of eight SoundFont channels
            // without the internal RAM the device keeps for networking.
            ok = tsf_set_max_voices(f, kMaxVoices);
            ok = ok && tsf_channel_set_presetindex(f, 0, 0);
        }
        if (ok) {
            midiState_ = alloc<MidiState>(1, DAW_MEM_LARGE);
            ok = midiState_ != nullptr;
            if (ok) new (midiState_) MidiState{};
        }
        if (!ok && f)
            tsf_close(f);
        g_host = nullptr;
        if (!ok)
            return DAW_ERR_ARG;
        tsf_ = f;
        applyPreset();
        applyChannel();
        applyVoices();
        return DAW_OK;
    }
    bool nativeMidi() const { return true; }
    void reset() { killAll(); midiMode_ = false; }
    void onParam(uint32_t id, float)
    {
        if (!tsf_)
            return;
        if (midiMode_ && id == P_BEND) midiState_->midi_.range = param(P_BEND);
        if (midiMode_ && id == P_BANK) { midiState_->midiBankMsb_ = int(param(P_BANK)) >> 7; midiState_->midiBankLsb_ = int(param(P_BANK)) & 127; }
        if (id == P_BANK || id == P_PROGRAM)
            applyPreset();
        else if (id == P_POLYPHONY)
            applyVoices();
        else
            applyChannel();
    }
    void onEvent(const daw_event_t &e)
    {
        if (!tsf_)
            return;
        if ((e.flags & DAW_NOTE_FLAG_MIDI) || e.type == DAW_EV_PROGRAM || e.type == DAW_EV_MIDI_BYTES ||
            e.type == DAW_EV_POLY_PRESSURE || e.type == DAW_EV_CHANNEL_PRESSURE) {
            if (!midiMode_) resetMidi(false);
            midiEvent(e); return;
        }
        if (midiMode_) { midiEvent(e); return; }
        switch (e.type) {
        case DAW_EV_NOTE_ON:
            tsf_channel_note_on(tsf_, 0, e.u.note.key, e.u.note.velocity > 0.001f ? e.u.note.velocity : 0.001f);
            break;
        case DAW_EV_NOTE_OFF: tsf_channel_note_off(tsf_, 0, e.u.note.key); break;
        case DAW_EV_PITCH_BEND: tsf_channel_set_pitchwheel(tsf_, 0, (int)lrintf(8192.0f + dsp::clamp(e.u.midi.value, -1.0f, 1.0f) * 8191.0f)); break;
        case DAW_EV_SUSTAIN: tsf_channel_set_sustain(tsf_, 0, e.u.midi.value >= 0.5f); break;
        case DAW_EV_CC: tsf_channel_midi_control(tsf_, 0, e.u.midi.number, (int)lrintf(e.u.midi.value * 127.0f)); break;
        case DAW_EV_ALL_NOTES_OFF: tsf_channel_note_off_all(tsf_, 0); break;
        case DAW_EV_ALL_SOUND_OFF: killAll(); break;  // now, not after TSF's 10 ms fast release
        default: break;
        }
    }
    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        float *L = c.outputs[0] + start, *R = c.outputs[1] + start;
        if (!tsf_ || tsf_active_voice_count(tsf_) == 0) {
            memset(L, 0, n * sizeof(float));
            memset(R, 0, n * sizeof(float));
            return;
        }
        tsf_render_float_split(tsf_, L, R, (int)n);
        for (uint32_t i = 0; i < n; i++) {
            L[i] = dsp::sanitize(L[i]);
            R[i] = dsp::sanitize(R[i]);
            if (midiMode_ && midiState_->midi_.balance != 8192) {
                const float b = (int(midiState_->midi_.balance) - 8192) / 8192.f;
                if (b > 0) L[i] *= 1 - b; else R[i] *= 1 + b;
            }
        }
    }
    uint32_t voices() const { return tsf_ ? (uint32_t)tsf_active_voice_count(tsf_) : 0; }

private:
    // silence every voice at once (tsf_reset would also free the channels: not on the audio path)
    void killAll()
    {
        if (!tsf_)
            return;
        for (int i = 0; i < tsf_->voiceNum; i++)
            if (tsf_->voices[i].playingPreset != -1)
                tsf_voice_kill(&tsf_->voices[i]);
    }
    void close()
    {
        if (tsf_) {
            g_host = host_;
            tsf_close(tsf_);
            g_host = nullptr;
            tsf_ = nullptr;
        }
        dealloc(midiState_); midiState_ = nullptr; midiMode_ = false;
    }
    // the bank / program pair if the font has it, else its first preset (fonts are not always GM)
    void applyPreset()
    {
        if (!tsf_channel_set_bank_preset(tsf_, 0, (int)param(P_BANK), (int)param(P_PROGRAM)))
            tsf_channel_set_presetindex(tsf_, 0, 0);
    }
    // TinySoundFont plays the first voiceNum of its preallocated voices: the others are silenced
    // and left out (no allocation: safe on the audio thread). A note finding no free voice takes
    // the one furthest into its release, else the oldest one (PROVENANCE.md).
    void applyVoices()
    {
        const int n = (int)param(P_POLYPHONY);
        for (int i = n; i < tsf_->maxVoiceNum; i++)
            if (tsf_->voices[i].playingPreset != -1)
                tsf_voice_kill(&tsf_->voices[i]);
        tsf_->voiceNum = n < tsf_->maxVoiceNum ? n : tsf_->maxVoiceNum;
    }
    void applyChannel()
    {
        if (midiMode_) { applyMidiChannel(); return; }
        tsf_channel_set_volume(tsf_, 0, param(P_LEVEL));
        tsf_channel_set_tuning(tsf_, 0, param(P_TRANSPOSE) + param(P_FINE) * 0.01f);
        tsf_channel_set_pitchrange(tsf_, 0, param(P_BEND));
    }
    struct MidiState {
        tsf_region regions_[kMaxVoices]{};
        const tsf_region *original_[kMaxVoices] = {};
        uint32_t regionPlay_[kMaxVoices] = {}, sostenutoPlay_[kMaxVoices] = {};
        int32_t noteIds_[kMaxVoices] = {};
        uint8_t keyPressure_[128] = {};
        MidiControls midi_;
        int midiBankMsb_ = 0, midiBankLsb_ = 0;
        bool pedal_ = false, sostenuto_ = false, drums_ = false;
        uint8_t sysex_[256] = {};
        uint32_t sysexSize_ = 0;
        bool sysexOverflow_ = false;
    };
    MidiState *midiState_ = nullptr;
    bool midiMode_ = false;

    void resetMidi(bool hard = true) {
        midiMode_ = true; if (hard) killAll(); midiState_->midi_ = MidiControls{};
        midiState_->midiBankMsb_ = int(param(P_BANK)) >> 7; midiState_->midiBankLsb_ = int(param(P_BANK)) & 127;
        midiState_->drums_ = int(param(P_BANK)) == 128;
        midiState_->pedal_ = midiState_->sostenuto_ = false; midiState_->sysexSize_ = 0; midiState_->sysexOverflow_ = false;
        memset(midiState_->keyPressure_, 0, sizeof midiState_->keyPressure_);
        memset(midiState_->sostenutoPlay_, 0, sizeof midiState_->sostenutoPlay_);
        tsf_channel_set_sustain(tsf_, 0, 0);
        tsf_channel_set_pitchwheel(tsf_, 0, 8192);
        applyPreset(); applyMidiChannel();
    }
    void applyMidiChannel() {
        tsf_channel_set_volume(tsf_, 0, param(P_LEVEL) * midiState_->midi_.gain());
        // MIDI pan is channel state; balance is an extra post-render stereo balance below.
        tsf_channel_set_pan(tsf_, 0, midiState_->midi_.pan <= 8192 ? midiState_->midi_.pan / 16384.f : .5f + (midiState_->midi_.pan - 8192) / 16382.f);
        tsf_channel_set_pitchrange(tsf_, 0, midiState_->midi_.range);
        tsf_channel_set_tuning(tsf_, 0, param(P_TRANSPOSE) + param(P_FINE) * .01f + midiState_->midi_.tuning());
        tsf_channel_set_pitchwheel(tsf_, 0, (int)dsp::clamp(lrintf(8192.f + midiState_->midi_.wheel * 8192.f), 0.f, 16383.f));
    }
    bool captured(int i) const { return midiState_->sostenuto_ && midiState_->sostenutoPlay_[i] == tsf_->voices[i].playIndex + 1u; }
    void releasePedals() {
        for (int i = 0; i < tsf_->voiceNum; ++i) {
            auto &v = tsf_->voices[i];
            if (v.playingPreset != -1 && v.heldSustain && !midiState_->pedal_ && !captured(i)) tsf_voice_end(tsf_, &v);
        }
    }
    void midiNoteOff(int key, bool all = false, int32_t id = -1) {
        uint32_t first = UINT32_MAX;
        if (!all) for (int i = 0; i < tsf_->voiceNum; ++i) {
            const auto &v = tsf_->voices[i];
            if (v.playingPreset != -1 && v.playingKey == key && v.ampenv.segment < TSF_SEGMENT_RELEASE && !v.heldSustain && (id < 0 || midiState_->noteIds_[i] == id) && v.playIndex < first) first = v.playIndex;
        }
        for (int i = 0; i < tsf_->voiceNum; ++i) {
            auto &v = tsf_->voices[i];
            if (v.playingPreset == -1 || v.ampenv.segment >= TSF_SEGMENT_RELEASE || v.heldSustain ||
                (!all && (v.playingKey != key || v.playIndex != first || (id >= 0 && midiState_->noteIds_[i] != id)))) continue;
            if (midiState_->pedal_ || captured(i)) v.heldSustain = 1; else tsf_voice_end(tsf_, &v);
        }
    }
    void applyModulation() {
        for (int i = 0; i < tsf_->voiceNum; ++i) {
            auto &v = tsf_->voices[i];
            if (v.playingPreset == -1) continue;
            if (v.region != &midiState_->regions_[i]) {
                midiState_->original_[i] = v.region; midiState_->regions_[i] = *v.region;
                v.region = &midiState_->regions_[i]; midiState_->regionPlay_[i] = v.playIndex;
            } else if (midiState_->regionPlay_[i] != v.playIndex) {
                // A reused voice was set up from its new preset by tsf_note_on.
                midiState_->regionPlay_[i] = v.playIndex;
            }
            const int pressure = midiState_->keyPressure_[v.playingKey] > midiState_->midi_.pressure ? midiState_->keyPressure_[v.playingKey] : midiState_->midi_.pressure;
            // SF2's default modulation-wheel / pressure vibrato depth is 50 cents.
            midiState_->regions_[i].vibLfoToPitch = midiState_->original_[i]->vibLfoToPitch + (50 * (int(midiState_->midi_.modulation) + pressure)) / 127;
        }
    }
    void midiPacket(const uint8_t *d, uint32_t n) {
        bool reset;
        if (midiDeviceControl(d, n, midiState_->midi_, reset)) {
            if (reset) {
                resetMidi(); midiState_->midiBankMsb_ = midiState_->midiBankLsb_ = 0;
                tsf_channel_set_bank(tsf_, 0, 0);
                if (!tsf_channel_set_presetnumber(tsf_, 0, 0, midiState_->drums_)) tsf_channel_set_presetindex(tsf_, 0, 0);
            } else applyMidiChannel();
        }
    }
    void sysex(const uint8_t *d, uint32_t n) {
        if (!n) return;
        // Vendor dumps are retained by the host. Parsing an arbitrarily large opaque dump
        // must never consume a whole audio deadline; supported globals are at most 11 bytes.
        if (n > sizeof midiState_->sysex_) {
            midiState_->sysexSize_ = 0;
            midiState_->sysexOverflow_ = d[n - 1] != 0xf7;
            return;
        }
        // SMF F7 is a continuation/escape marker, not a second wire byte in the packet.
        uint32_t pos = d[0] == 0xf7 ? 1 : 0;
        if (d[0] == 0xf0) { midiState_->sysexSize_ = 0; midiState_->sysexOverflow_ = false; }
        for (; pos < n; ++pos) {
            const uint8_t b = d[pos];
            if (b >= 0xf8) { if (b == 0xff) resetMidi(); continue; }
            if (midiState_->sysexSize_ < sizeof midiState_->sysex_) midiState_->sysex_[midiState_->sysexSize_++] = b;
            else midiState_->sysexOverflow_ = true;
            if (b == 0xf7) {
                if (!midiState_->sysexOverflow_) midiPacket(midiState_->sysex_, midiState_->sysexSize_);
                midiState_->sysexSize_ = 0; midiState_->sysexOverflow_ = false;
            }
        }
    }
    void midiEvent(const daw_event_t &e) {
        switch (e.type) {
        case DAW_EV_MIDI_RESET: resetMidi(); break;
        case DAW_EV_NOTE_ON: {
            const uint32_t play = tsf_->voicePlayIndex;
            tsf_channel_note_on(tsf_, 0, e.u.note.key, dsp::clamp(e.u.note.velocity * (midiState_->midi_.soft >= 64 ? .7f : 1.f), .001f, 1.f));
            for (int i = 0; i < tsf_->voiceNum; ++i) if (tsf_->voices[i].playingPreset != -1 && tsf_->voices[i].playIndex == play) midiState_->noteIds_[i] = e.u.note.note_id;
            applyModulation(); break;
        }
        case DAW_EV_NOTE_OFF: midiNoteOff(e.u.note.key, false, e.u.note.note_id); break;
        case DAW_EV_ALL_NOTES_OFF:
            if (e.flags & DAW_NOTE_FLAG_MIDI) midiNoteOff(0, true);
            else { midiState_->pedal_ = midiState_->sostenuto_ = false; tsf_channel_note_off_all(tsf_, 0); }
            break;
        case DAW_EV_ALL_SOUND_OFF: killAll(); break;
        case DAW_EV_PITCH_BEND: midiState_->midi_.wheel = e.u.midi.value; applyMidiChannel(); break;
        case DAW_EV_CHANNEL_PRESSURE: midiState_->midi_.pressure = (uint8_t)lrintf(e.u.midi.value * 127); applyModulation(); break;
        case DAW_EV_POLY_PRESSURE: midiState_->keyPressure_[e.u.midi.number & 127] = (uint8_t)lrintf(e.u.midi.value * 127); applyModulation(); break;
        case DAW_EV_SUSTAIN: midiState_->pedal_ = e.u.midi.value >= .5f; releasePedals(); break;
        case DAW_EV_PROGRAM:
            midiState_->drums_ = e.u.midi.channel == 9 || int(param(P_BANK)) == 128;
            tsf_channel_set_bank(tsf_, 0, midiState_->midiBankMsb_ * 128 + midiState_->midiBankLsb_);
            if (!tsf_channel_set_presetnumber(tsf_, 0, e.u.midi.index, midiState_->drums_)) tsf_channel_set_presetindex(tsf_, 0, 0);
            break;
        case DAW_EV_CC: {
            const uint8_t cc = e.u.midi.number, v = (uint8_t)lrintf(dsp::clamp(e.u.midi.value, 0.f, 1.f) * 127);
            if (cc == 0) midiState_->midiBankMsb_ = v;
            else if (cc == 32) midiState_->midiBankLsb_ = v;
            else if (cc == 64) { midiState_->pedal_ = v >= 64; releasePedals(); }
            else if (cc == 66) {
                const bool down = v >= 64;
                if (down && !midiState_->sostenuto_) for (int i = 0; i < tsf_->voiceNum; ++i) {
                    const auto &voice = tsf_->voices[i];
                    midiState_->sostenutoPlay_[i] = voice.playingPreset != -1 && voice.ampenv.segment < TSF_SEGMENT_RELEASE && !voice.heldSustain ? voice.playIndex + 1u : 0;
                }
                midiState_->sostenuto_ = down; releasePedals();
            } else if (cc == 120) killAll();
            else if (cc == 123 || cc >= 124) midiNoteOff(0, true);
            else {
                midiState_->midi_.cc(cc, v);
                if (cc == 121) { midiState_->pedal_ = midiState_->sostenuto_ = false; memset(midiState_->keyPressure_, 0, sizeof midiState_->keyPressure_); releasePedals(); }
                applyMidiChannel(); applyModulation();
            }
            break;
        }
        case DAW_EV_MIDI_BYTES:
            if (e.u.bytes.size == 1 && e.u.bytes.data[0] == 0xff) resetMidi();
            else sysex(e.u.bytes.data, e.u.bytes.size);
            break;
        default: break;
        }
    }
    tsf *tsf_ = nullptr;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.soundfont",
    .name = "SoundFont",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Plays SoundFont 2 instruments (.sf2) from the sample library (TinySoundFont)",
    .category = "Sampler",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_POLYPHONIC | DAW_CAP_VELOCITY | DAW_CAP_PITCH_BEND | DAW_CAP_SUSTAIN,
    .max_voices = kMaxVoices,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(SoundFont),
    .mem_large_bytes = 128 * 1024 + kMaxVoices * sizeof(tsf_voice),  // regions/metadata, voices; aligned PCM16 borrows the host resource
    .cpu_permille_base = 2,
    .cpu_permille_voice = 10,  // estimate; to be measured on the board
    DAWSDK_CALLBACKS(SoundFont),
    DAWSDK_RESOURCES(SoundFont, 1, kSlots),
};

const daw_plugin_descriptor_t *SoundFont::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_soundfont(void) { return &kDescriptor; }
