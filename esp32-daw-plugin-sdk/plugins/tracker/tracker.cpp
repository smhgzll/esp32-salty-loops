// SPDX-License-Identifier: MIT
// Tracker: plays a tracker module (.mod / .xm / .s3m / .it) from the sample library with libxmp
// (Claudio Matsuoka and Hipolito Carraro Jr, MIT; the "lite" player of 4.7.4 with a scan hook, exact
// tick lengths and mixing in parts patched in, see PROVENANCE.md).
//
// A note plays the song: NOTE_ON starts it, its NOTE_OFF ends it. The plugin asks for note chase
// (ABI 1.2): when the transport starts, jumps or loops inside the note, the host sends the note
// again with how much of it has passed, and the song continues from there. Positions map onto the
// song through the Song tempo parameter: one quarter note at that tempo is 60 / tempo seconds of
// the song at its own speed. At any other project tempo the song plays faster or slower (libxmp's
// tempo factor), so it stays on the project's grid when the tempo changes or is automated.
//
// Seeking: on loading (control thread) libxmp's scan of the song runs once more and reports where
// every row starts, with the tempo state there (a hook patched into scan.c: no mixing, no channel
// processing, a few milliseconds on the ESP32-S3). A chase lands on the next row start
// (silent until then) and continues there with that row's speed, tempo and global volume. Notes
// sounding before that row are not restarted; effect memory and channel volumes set earlier are
// not restored (as when a tracker starts playing from a row).
//
// libxmp allocates through the shim (xmp_shim.h): LARGE memory except the small FAST mixer
// voice array, only on the control
// thread: loading, the row map, starting and ending the player. Rendering a frame (one tracker
// tick, 2.5 / BPM s: 882 samples at 125 BPM) runs libxmp's player at the tick's start and its
// mixer in parts of 256 frames as the blocks need them (deferred mixing, patched into mixer.c), so
// a tick's mixing is spread over the blocks it spans; nothing allocates. Frame lengths carry their
// fraction of a sample (also patched in), so a song at any tempo stays on the project's beat grid.
// With the host's parallel service (ABI 1.3) a part's voices are mixed on two cores, grouped by
// sample (patched in: libxmp_mixer_part_split); the output is the same as on one core.
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "dawsdk/plugin.hpp"
#include "xmp_glue.h"
#define DAWXMP_SHIM_IMPL  // declarations only: this file keeps the C library's names
#include "xmp_shim.h"

using namespace dawsdk;

namespace {

enum : uint32_t {
    P_TEMPO, P_LEVEL, P_STEREO, P_INTERP, P_FIRST, P_LAST, P_LOOP,
    // read-only, set when a module loads: what the DAW needs to arrange it
    P_LENGTH, P_SONG_TEMPO, P_CHANNELS, P_FORMAT, P_ORDERS, P_INSTRUMENTS, P_MEMORY,
    P_COUNT
};
constexpr uint32_t kInfo0 = P_LENGTH, kInfoCount = P_COUNT - P_LENGTH;
const char *const kInterp[] = {"Nearest", "Linear", "Spline"};
const char *const kFormats[] = {"None", "MOD", "XM", "S3M", "IT", "Other"};
constexpr uint32_t kRO = DAW_PARAM_READONLY | DAW_PARAM_HIDDEN | DAW_PARAM_NO_SMOOTH;
const daw_param_desc_t kParams[P_COUNT] = {
    {P_TEMPO, "tempo", "Song tempo", "Song", "BPM", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 10.0f, 2000.0f, 125.0f, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_LEVEL, "level", "Level", "Output", "", DAW_PARAM_FLOAT, DAW_SCALE_DB, 0.0f, 2.0f, 1.0f, DAW_PARAM_AUTOMATABLE, 0, nullptr},
    {P_STEREO, "stereo", "Stereo separation", "Output", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 70.0f, DAW_PARAM_AUTOMATABLE | DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_INTERP, "interp", "Interpolation", "Output", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 2, 1, DAW_PARAM_NO_SMOOTH, 3, kInterp},
    {P_FIRST, "first", "First channel", "Channels", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 64, 1, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_LAST, "last", "Last channel", "Channels", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 1, 64, 64, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_LOOP, "loop", "Loop song", "Song", "", DAW_PARAM_BOOL, DAW_SCALE_LINEAR, 0, 1, 0, DAW_PARAM_NO_SMOOTH, 0, nullptr},
    {P_LENGTH, "length", "Length", "Module", "s", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 86400.0f, 0.0f, kRO, 0, nullptr},
    {P_SONG_TEMPO, "song_tempo", "Suggested tempo", "Module", "BPM", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 2000.0f, 0.0f, kRO, 0, nullptr},
    {P_CHANNELS, "channels", "Channels", "Module", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 64, 0, kRO, 0, nullptr},
    {P_FORMAT, "format", "Format", "Module", "", DAW_PARAM_ENUM, DAW_SCALE_LINEAR, 0, 5, 0, kRO, 6, kFormats},
    {P_ORDERS, "orders", "Orders", "Module", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 256, 0, kRO, 0, nullptr},
    {P_INSTRUMENTS, "instruments", "Instruments", "Module", "", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 255, 0, kRO, 0, nullptr},
    {P_MEMORY, "memory", "Player memory", "Module", "KB", DAW_PARAM_INT, DAW_SCALE_LINEAR, 0, 1048576, 0, kRO, 0, nullptr},
};
const daw_preset_desc_t kPresets[] = {{"Default", "Basic", nullptr}};
const daw_resource_slot_desc_t kSlots[] = {{"module", "Module", DAW_RES_MASK(DAW_RES_FILE), "mod,xm,s3m,it"}};

// the mix's full scale at libxmp's default amplification, see dawxmp_buffer
constexpr float kScale = 1.0f / 67108864.0f;
// a tick is mixed in parts of this many frames as the blocks need them (882 frames at 125 BPM
// would otherwise all be mixed in the one block a tick starts in)
constexpr uint32_t kMixUnit = 256;

uint32_t sniffFormat(const uint8_t *d, uint32_t n)
{
    if (n >= 4 && !memcmp(d, "IMPM", 4))
        return 4;
    if (n >= 17 && !memcmp(d, "Extended Module: ", 17))
        return 2;
    if (n >= 48 && !memcmp(d + 44, "SCRM", 4))
        return 3;
    return n >= 1084 ? 1 : 5;  // libxmp's MOD loader checks the signature itself
}

class Tracker : public Plugin<Tracker, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();

    void release() { close(); }

    void reset()
    {
        mode_ = Idle;
        fading_ = false;
        gain_ = param(P_LEVEL);
    }

    void onParam(uint32_t id, float v)
    {
        if (id >= kInfo0) {
            params_[id] = info_[id - kInfo0];  // read-only: what the module says
            return;
        }
        (void)v;
        if (xmp_)
            applyPlayer();
    }

    int32_t setResource(uint32_t slot, const daw_resource_t *res)
    {
        if (slot != 0)
            return DAW_ERR_ARG;
        close();
        if (!res)
            return DAW_OK;
        if (res->type != DAW_RES_FILE || !res->data || res->size < 64)
            return DAW_ERR_ARG;
        dawxmp_set_host(host_, DAW_MEM_LARGE);
        const size_t before = dawxmp_allocated();
        const int32_t r = load(static_cast<const uint8_t *>(res->data), res->size);
        if (r == DAW_OK) {
            info_[P_MEMORY - kInfo0] = (float)((dawxmp_allocated() - before + rowCount_ * sizeof(dawxmp_row)) / 1024);
            for (uint32_t i = 0; i < kInfoCount; i++)
                params_[kInfo0 + i] = info_[i];
        }
        dawxmp_set_host(nullptr, 0);
        if (r != DAW_OK)
            close();
        return r;
    }

    void onEvent(const daw_event_t &e)
    {
        switch (e.type) {
        case DAW_EV_NOTE_ON: start(e); break;
        case DAW_EV_NOTE_OFF:
            if (mode_ != Idle && (e.u.note.note_id >= 0 ? e.u.note.note_id == noteId_ : e.u.note.key == noteKey_))
                fadeOut();
            break;
        case DAW_EV_ALL_NOTES_OFF: fadeOut(); break;
        case DAW_EV_ALL_SOUND_OFF: mode_ = Idle; break;
        default: break;
        }
    }

    // the project tempo is needed before the block's first event (a chase at offset 0)
    static void abiProcess(daw_plugin_instance_t *inst, const daw_process_t *c)
    {
        Tracker *t = reinterpret_cast<Tracker *>(inst);
        if (c->tempo_bpm > 0)
            t->hostTempo_ = c->tempo_bpm;
        t->ticked_ = false;
        Plugin::abiProcess(inst, c);
        // A host block without player work can prepare this tick's last mixer part before the
        // next block crosses the tick boundary. Keep the original 256-frame part boundaries:
        // only the time of the computation changes, never its arithmetic or sample positions.
        if (t->mode_ == Play && !t->ticked_ && t->frameLen_ > t->framePos_ &&
            t->frameLen_ - t->framePos_ < c->frames && t->mixed_ < t->frameLen_) {
            while (t->mixed_ < t->frameLen_) {
                const uint32_t unit = t->frameLen_ - t->mixed_ < kMixUnit ? t->frameLen_ - t->mixed_ : kMixUnit;
                const int got = t->mix((int)unit);
                if (got <= 0) break;
                t->mixed_ += (uint32_t)got;
            }
        }
    }

    void render(const daw_process_t &c, uint32_t start, uint32_t n)
    {
        float *L = c.outputs[0] + start, *R = c.outputs[1] + start;
        if (xmp_)
            followTempo();
        const float target = param(P_LEVEL), smooth = 1.0f - expf(-1.0f / (0.01f * sampleRate()));
        const float fadeStep = 1.0f / (0.005f * sampleRate());
        uint32_t i = 0;
        while (i < n) {
            if (mode_ == Idle) {
                memset(L + i, 0, (n - i) * sizeof(float));
                memset(R + i, 0, (n - i) * sizeof(float));
                break;
            }
            if (mode_ == Wait) {
                const uint32_t k = wait_ < n - i ? wait_ : n - i;
                memset(L + i, 0, k * sizeof(float));
                memset(R + i, 0, k * sizeof(float));
                i += k;
                wait_ -= k;
                if (wait_ == 0) {
                    ticked_ = true;
                    if (dawxmp_seek(xmp_, &rows_[startRow_]) < 0) {
                        mode_ = Idle;
                        continue;
                    }
                    frame_ = dawxmp_buffer(xmp_, &frameLen_);
                    framePos_ = mixed_ = 0;
                    voices_ = (uint32_t)dawxmp_voices(xmp_);
                    mode_ = Play;
                }
                continue;
            }
            if (framePos_ >= frameLen_) {
                ticked_ = true;
                if (xmp_play_frame(xmp_) < 0 || (dawxmp_loops(xmp_) > 0 && param(P_LOOP) < 0.5f)) {
                    mode_ = Idle;  // the song ended
                    continue;
                }
                frame_ = dawxmp_buffer(xmp_, &frameLen_);
                framePos_ = mixed_ = 0;
                voices_ = (uint32_t)dawxmp_voices(xmp_);
                if (frameLen_ == 0) {
                    mode_ = Idle;
                    continue;
                }
            }
            const uint32_t k = frameLen_ - framePos_ < n - i ? frameLen_ - framePos_ : n - i;
            // mix what these samples need, in units counted from the tick's start: the same
            // parts whichever block boundaries the tick falls on (reproducible output)
            while (mixed_ < framePos_ + k) {
                const uint32_t unit = frameLen_ - mixed_ < kMixUnit ? frameLen_ - mixed_ : kMixUnit;
                const int got = mix((int)unit);
                if (got <= 0)
                    break;
                mixed_ += (uint32_t)got;
            }
            const int32_t *src = frame_ + 2 * framePos_;
            for (uint32_t j = 0; j < k; j++) {
                gain_ += (target - gain_) * smooth;
                float g = gain_ * kScale;
                if (fading_) {
                    fade_ -= fadeStep;
                    if (fade_ <= 0.0f) {
                        fade_ = 0.0f;
                        mode_ = Idle;  // the rest of the block is zeroed above
                    }
                    g *= fade_;
                }
                L[i + j] = (float)src[2 * j] * g;
                R[i + j] = (float)src[2 * j + 1] * g;
            }
            framePos_ += k;  // a fade that ended inside this run wrote zeros for its rest
            i += k;
        }
    }

    uint32_t voices() const { return mode_ == Idle ? 0 : 1; }

private:
    bool ticked_ = false;
    enum Mode : uint8_t { Idle, Wait, Play };

    // the next part of the tick: its voices split between this core and the host's other one when
    // the host offers that (ABI 1.3), on this core otherwise; the same output either way
    int mix(int frames)
    {
        return split_ ? dawxmp_mix_split(xmp_, split_, frames, host_->parallel, host_->host_ctx)
                      : dawxmp_mix(xmp_, frames);
    }

    int32_t load(const uint8_t *data, uint32_t size)
    {
        xmp_ = xmp_create_context();
        if (!xmp_)
            return DAW_ERR_NOMEM;
        if (xmp_load_module_from_memory(xmp_, data, (long)size) != 0) {
            xmp_free_context(xmp_);
            xmp_ = nullptr;
            return DAW_ERR_ARG;
        }
        loaded_ = true;
        xmp_module_info mi;
        xmp_get_module_info(xmp_, &mi);
        const int n = dawxmp_rows(xmp_, &rows_, &lengthMs_);
        if (n <= 0)
            return DAW_ERR_ARG;  // no playable order
        rowCount_ = (uint32_t)n;
        if (xmp_start_player(xmp_, (int)sampleRate(), 0) != 0)
            return DAW_ERR_NOMEM;
        playerOn_ = true;
        dawxmp_set_deferred(xmp_, 1);  // ticks are mixed in parts as their samples are needed
        if (host_->struct_size >= offsetof(daw_host_api_t, parallel) + sizeof(host_->parallel) && host_->parallel)
            split_ = dawxmp_split_new(xmp_);  // nullptr: one core
        tempoFactor_ = 1.0;
        const xmp_module *m = mi.mod;
        chn_ = m->chn;  // before applyPlayer: the channel range mutes chn_ channels
        applyPlayer();
        info_[P_LENGTH - kInfo0] = (float)(lengthMs_ / 1000.0);
        // one quarter note = four rows at the song's first speed (speed 6 at 125 BPM: 125 BPM)
        float song = m->spd > 0 ? (float)m->bpm * 6.0f / (float)m->spd : (float)m->bpm;
        if (song < 20.0f || song > 999.0f)
            song = (float)m->bpm;
        info_[P_SONG_TEMPO - kInfo0] = song;
        info_[P_CHANNELS - kInfo0] = (float)m->chn;
        info_[P_FORMAT - kInfo0] = (float)sniffFormat(data, size);
        info_[P_ORDERS - kInfo0] = (float)m->len;
        info_[P_INSTRUMENTS - kInfo0] = (float)m->ins;
        return DAW_OK;
    }

    void close()
    {
        mode_ = Idle;
        if (xmp_) {
            if (playerOn_)
                xmp_end_player(xmp_);  // frees the second core's tick buffer too
            if (loaded_)
                xmp_release_module(xmp_);
            xmp_free_context(xmp_);
        }
        dawxmp_free(rows_);
        dawxmp_free(split_);
        split_ = nullptr;
        xmp_ = nullptr;
        rows_ = nullptr;
        rowCount_ = 0;
        lengthMs_ = 0;
        playerOn_ = loaded_ = false;
        frame_ = nullptr;
        frameLen_ = framePos_ = mixed_ = 0;
        for (auto &v : info_)
            v = 0;
        for (uint32_t i = 0; i < kInfoCount; i++)
            params_[kInfo0 + i] = 0;
    }

    // stereo separation, interpolation and the channel range: plain player settings (no allocation)
    void applyPlayer()
    {
        xmp_set_player(xmp_, XMP_PLAYER_MIX, (int)lroundf(param(P_STEREO)));
        xmp_set_player(xmp_, XMP_PLAYER_INTERP, (int)param(P_INTERP));
        const int first = (int)param(P_FIRST) - 1, last = (int)param(P_LAST) - 1;
        for (int c = 0; c < chn_; c++)
            xmp_channel_mute(xmp_, c, c < first || c > last ? 1 : 0);
    }

    // the project tempo against the song tempo: libxmp's frame length follows the ratio
    void followTempo()
    {
        const double want = hostTempo_ > 0 ? param(P_TEMPO) / hostTempo_ : 1.0;
        if (fabs(want - tempoFactor_) < 1e-6)
            return;
        if (xmp_set_tempo_factor_relative(xmp_, want) == 0)
            tempoFactor_ = want;
    }

    void start(const daw_event_t &e)
    {
        if (!xmp_ || !rowCount_)
            return;
        noteId_ = e.u.note.note_id;
        noteKey_ = e.u.note.key;
        fading_ = false;
        fade_ = 1.0f;
        // song time of this point, at the song's own speed
        double t = (e.flags & DAW_NOTE_FLAG_CHASE) ? e.u.note.chase_mbeats * 60.0 / param(P_TEMPO) : 0.0;
        if (t >= lengthMs_) {
            if (param(P_LOOP) < 0.5f || lengthMs_ <= 0) {
                mode_ = Idle;
                return;
            }
            t = fmod(t, lengthMs_);
        }
        uint32_t lo = 0, hi = rowCount_;  // last row starting at or before t
        while (hi - lo > 1) {
            const uint32_t mid = (lo + hi) / 2;
            (rows_[mid].ms <= t ? lo : hi) = mid;
        }
        double wait = 0;
        if (t - rows_[lo].ms > 0.5) {  // inside a row: continue at the next one
            if (++lo >= rowCount_) {
                mode_ = Idle;
                return;
            }
            wait = rows_[lo].ms - t;
        }
        // song milliseconds to output samples at the current speed
        const double speed = hostTempo_ > 0 ? hostTempo_ / param(P_TEMPO) : 1.0;
        wait_ = (uint32_t)(wait / speed * sampleRate() / 1000.0 + 0.5);
        startRow_ = lo;
        mode_ = Wait;  // wait_ == 0: the seek happens at this event's sample, in the next render call
    }

    void fadeOut()
    {
        if (mode_ == Play)
            fading_ = true;
        else
            mode_ = Idle;
    }

    xmp_context xmp_ = nullptr;
    void *split_ = nullptr;  // two-core mixing (dawxmp_split_new)
    bool loaded_ = false, playerOn_ = false;  // module loaded, player started
    dawxmp_row *rows_ = nullptr;
    uint32_t rowCount_ = 0;
    double lengthMs_ = 0;
    int chn_ = 0;
    float info_[kInfoCount] = {};

    Mode mode_ = Idle;
    bool fading_ = false;
    float fade_ = 1.0f, gain_ = 1.0f;
    uint32_t wait_ = 0, startRow_ = 0;
    int32_t noteId_ = -1;
    uint8_t noteKey_ = 0;
    const int32_t *frame_ = nullptr;
    uint32_t frameLen_ = 0, framePos_ = 0, mixed_ = 0;  // tick length, frames handed out, frames mixed
    uint32_t voices_ = 0;
    double hostTempo_ = 0, tempoFactor_ = 1.0;
};

const daw_plugin_descriptor_t kDescriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.tracker",
    .name = "Tracker",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Plays tracker modules (.mod, .xm, .s3m, .it) from the sample library (libxmp)",
    .category = "Sampler",
    .kind = DAW_PLUGIN_INSTRUMENT,
    .state_version = 1,
    .capabilities = DAW_CAP_MIDI_STREAM | DAW_CAP_NOTE_CHASE,
    .max_voices = 1,
    .param_count = P_COUNT,
    .params = kParams,
    .preset_count = sizeof kPresets / sizeof kPresets[0],
    .presets = kPresets,
    .key_names = nullptr,
    .mem_fast_bytes = sizeof(Tracker) + DAWXMP_VOICE_FAST_BUDGET,
    .mem_large_bytes = 256 * 1024,  // libxmp's context and mix buffers (two cores: a second tick buffer); the module's samples come with it
    .cpu_permille_base = 20,
    .cpu_permille_voice = 20,  // estimate (one song); to be measured on the board
    .create = Tracker::abiCreate,
    .destroy = Tracker::abiDestroy,
    .activate = Tracker::abiActivate,
    .deactivate = Tracker::abiDeactivate,
    .process = Tracker::abiProcess,
    .set_param = Tracker::abiSetParam,
    .get_param = Tracker::abiGetParam,
    .save_state = nullptr,
    .load_state = nullptr,
    .active_voices = Tracker::abiVoices,
    DAWSDK_RESOURCES(Tracker, 1, kSlots),
};

const daw_plugin_descriptor_t *Tracker::descriptor() { return &kDescriptor; }

}  // namespace

extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_tracker(void) { return &kDescriptor; }
