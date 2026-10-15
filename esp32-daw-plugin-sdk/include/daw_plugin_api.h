/* SPDX-License-Identifier: MIT
 * esp32-daw-plugin-sdk: the plugin ABI, version 1.
 *
 * A plugin is a static library exporting one C entry point:
 *
 *     const daw_plugin_descriptor_t *<entry>(void);
 *
 * whose name is given in the package manifest ("entry"). The descriptor is immutable static data.
 * Only C types cross the boundary: no C++ classes, STL types, exceptions or RTTI.
 *
 * Threads:
 *   - control thread: create, destroy, activate, deactivate, set_param, get_param, save_state,
 *     load_state, preset loading. May allocate (through the host API only) and may take time.
 *   - audio thread [RT]: process, active_voices. Must not allocate, block, log, or do I/O.
 *   The host never calls two functions of one instance at the same time. activate() happens
 *   before the first process(); deactivate() after the last one.
 *
 * Ownership:
 *   - descriptor and all data it points to: plugin, static lifetime.
 *   - daw_host_api_t passed to create(): host, valid until destroy() returns.
 *   - instance: plugin, valid from create() until destroy().
 *   - process ctx, event array, audio buffers: host, valid only during that process() call.
 *   - state buffers: caller-owned, valid during the call.
 */
#ifndef DAW_PLUGIN_API_H
#define DAW_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#include "daw_plugin_events.h"
#include "daw_plugin_host_api.h"
#include "daw_plugin_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. Major: incompatible layout or semantics change. Minor: fields appended at the end of
 * structs (struct_size tells which exist). A host loads plugins with the same major and a minor
 * <= its own. */
#define DAW_PLUGIN_API_MAJOR 1
#define DAW_PLUGIN_API_MINOR 4  /* 1.1: resources (samples, sound fonts), see daw_resource_t;
                                 * 1.2: note chase (DAW_CAP_NOTE_CHASE, DAW_NOTE_FLAG_CHASE);
                                 * 1.3: optional host-owned parallel DSP jobs;
                                 * 1.4: timed MIDI messages, pressure, controller reset */

typedef enum daw_plugin_kind {
    DAW_PLUGIN_INSTRUMENT = 1, /* events in, stereo audio out */
    DAW_PLUGIN_EFFECT = 2      /* stereo audio in, stereo audio out (events: params only) */
} daw_plugin_kind_t;

/* capabilities: what the host may rely on and what the UI may offer */
#define DAW_CAP_POLYPHONIC      (1ull << 0)  /* more than one voice */
#define DAW_CAP_VELOCITY        (1ull << 1)  /* note velocity changes the sound */
#define DAW_CAP_SLIDE           (1ull << 2)  /* DAW_NOTE_FLAG_SLIDE glides pitch */
#define DAW_CAP_ACCENT          (1ull << 3)  /* DAW_NOTE_FLAG_ACCENT has its own sound */
#define DAW_CAP_NOTE_PAN        (1ull << 4)  /* independent per-voice pan (DAW_EXPR_PAN) */
#define DAW_CAP_NOTE_PITCH      (1ull << 5)  /* independent per-voice pitch (DAW_EXPR_PITCH) */
#define DAW_CAP_NOTE_VOLUME     (1ull << 6)
#define DAW_CAP_NOTE_CUTOFF     (1ull << 7)
#define DAW_CAP_NOTE_RESONANCE  (1ull << 8)
#define DAW_CAP_RELEASE_VEL     (1ull << 9)  /* release velocity changes the release */
#define DAW_CAP_PITCH_BEND      (1ull << 10) /* channel-wide pitch bend */
#define DAW_CAP_SUSTAIN         (1ull << 11)
#define DAW_CAP_DRUM_KIT        (1ull << 12) /* keys select different sounds (see key_names) */
#define DAW_CAP_TAIL            (1ull << 13) /* effect: output continues after input stops */
#define DAW_CAP_MIDI_OUT        (1ull << 14) /* reserved for ABI 1.x */
#define DAW_CAP_STATE_BLOB      (1ull << 15) /* save_state/load_state carry data beyond params */
#define DAW_CAP_MONO_LEGATO     (1ull << 16) /* monophonic with legato / last-note priority */
#define DAW_CAP_NOTE_CHASE      (1ull << 17) /* ABI 1.2: wants notes the transport starts inside of
                                              * (DAW_NOTE_FLAG_CHASE), e.g. a song or long-sample player */

/* ABI 1.4: opts into timed MIDI / raw-byte events and DAW_NOTE_FLAG_MIDI.
 * Unmapped controllers / device commands may be ignored; the host retains their data. */
#define DAW_CAP_MIDI_STREAM     (1ull << 18)

/* return codes */
#define DAW_OK            0
#define DAW_ERR_NOMEM    -1
#define DAW_ERR_ARG      -2
#define DAW_ERR_STATE    -3 /* state blob of unknown version / corrupt */
#define DAW_ERR_UNSUPPORTED -4

typedef struct daw_plugin_instance daw_plugin_instance_t; /* opaque */

/* transport flags in daw_process_t */
#define DAW_TRANSPORT_PLAYING 0x1u
#define DAW_TRANSPORT_LOOPING 0x2u

typedef struct daw_process {
    uint32_t struct_size;
    uint32_t frames;               /* 1 .. max_block given to create() */
    const float *const *inputs;    /* effects: 2 channels; instruments: NULL */
    float *const *outputs;         /* 2 channels, `frames` samples each; may alias inputs (in place) */
    const daw_event_t *events;     /* sorted by offset */
    uint32_t event_count;
    uint32_t transport_flags;      /* DAW_TRANSPORT_* */
    uint64_t sample_pos;           /* host sample clock at the first frame (monotonic) */
    double tempo_bpm;
    double song_pos_beats;         /* musical position at the first frame, quarter notes */
} daw_process_t;

/* ---- ABI 1.1: resources ------------------------------------------------------------------------
 * Samples and sound banks are not plugin code: the host stores them, decodes audio files and
 * hands the plugin read-only memory it owns. A plugin declares resource slots; the host calls
 * set_resource() on the control thread, between create() and activate() (or after deactivate()),
 * never while process() may run. The plugin may allocate and pre-compute there. The resource and
 * all memory it points to stay valid until the slot is set again or destroy() returns. */
typedef enum daw_resource_type {
    DAW_RES_AUDIO = 1, /* decoded PCM: channels / frames / sample_rate (WAV, AIFF, ... decoded by the host) */
    DAW_RES_FILE = 2   /* the raw file bytes (e.g. a SoundFont .sf2): data / size */
} daw_resource_type_t;
#define DAW_RES_MASK(t) (1u << (t))

typedef struct daw_resource {
    uint32_t struct_size;
    uint32_t type;                  /* daw_resource_type_t */
    const char *name;               /* display name (file name), for logs only */
    /* DAW_RES_AUDIO */
    const float *const *channels;   /* channel_count pointers to frames samples, -1..1 */
    uint32_t channel_count;         /* 1 or 2 */
    uint32_t frames;
    uint32_t sample_rate;
    int32_t root_key;               /* MIDI key of the recording (WAV 'smpl' chunk), -1 if unknown */
    uint32_t loop_start, loop_end;  /* frames; loop_end == 0: no loop in the file */
    /* DAW_RES_FILE */
    const void *data;
    uint32_t size;
} daw_resource_t;

typedef struct daw_resource_slot_desc {
    const char *key;                /* stable id stored in projects: "sample", "soundfont" */
    const char *name;               /* display name */
    uint32_t types;                 /* accepted: DAW_RES_MASK(DAW_RES_AUDIO) | ... */
    const char *extensions;         /* file name filter for the UI: "wav", "sf2" */
} daw_resource_slot_desc_t;

typedef struct daw_plugin_descriptor {
    uint32_t struct_size;          /* sizeof(daw_plugin_descriptor_t) as compiled by the plugin */
    uint16_t api_major;            /* DAW_PLUGIN_API_MAJOR the plugin was built against */
    uint16_t api_minor;

    const char *id;                /* reverse-DNS, stable forever: "org.smhgzll.3xosc" */
    const char *name;              /* display name */
    const char *vendor;
    const char *version;           /* semantic version of the plugin "0.1.0" */
    const char *license;           /* SPDX expression */
    const char *description;
    const char *category;          /* "Synth", "Drums", "Bass", "Filter", "Delay", "Dynamics", ... */
    uint32_t kind;                 /* daw_plugin_kind_t */
    uint32_t state_version;        /* version of the save_state() blob / param set */
    uint64_t capabilities;         /* DAW_CAP_* */
    uint32_t max_voices;           /* 1 for monophonic; effects 0 */

    uint32_t param_count;
    const daw_param_desc_t *params;
    uint32_t preset_count;
    const daw_preset_desc_t *presets;
    const char *const *key_names;  /* DAW_CAP_DRUM_KIT: 128 names (NULL entries allowed) or NULL */

    /* resource estimates (measured on the ESP32-S3 where possible; see docs/PERFORMANCE.md) */
    uint32_t mem_fast_bytes;       /* DAW_MEM_FAST allocated by create() */
    uint32_t mem_large_bytes;      /* DAW_MEM_LARGE allocated by create() */
    uint32_t cpu_permille_base;    /* estimated share of one 240 MHz core at 44.1 kHz, x1000 */
    uint32_t cpu_permille_voice;   /* additional share per active voice */

    /* control thread. Returns NULL on failure (nothing left allocated). */
    daw_plugin_instance_t *(*create)(const daw_host_api_t *host, uint32_t sample_rate,
                                     uint32_t max_block);
    void (*destroy)(daw_plugin_instance_t *inst);
    int32_t (*activate)(daw_plugin_instance_t *inst);   /* reset DSP state, ready to process */
    void (*deactivate)(daw_plugin_instance_t *inst);

    /* [RT] render `frames` frames, handling events at their offsets */
    void (*process)(daw_plugin_instance_t *inst, const daw_process_t *ctx);

    /* control thread (when not processing) or as DAW_EV_PARAM events in process() */
    void (*set_param)(daw_plugin_instance_t *inst, uint32_t param_id, float value);
    float (*get_param)(const daw_plugin_instance_t *inst, uint32_t param_id);

    /* optional (NULL if !DAW_CAP_STATE_BLOB): opaque data beyond the parameters. save_state with
     * buf == NULL returns the size needed; otherwise bytes written or a negative DAW_ERR_* */
    int32_t (*save_state)(const daw_plugin_instance_t *inst, void *buf, uint32_t cap);
    int32_t (*load_state)(daw_plugin_instance_t *inst, const void *buf, uint32_t len,
                          uint32_t state_version);

    /* [RT] voices currently sounding (diagnostics, CPU accounting); may be NULL */
    uint32_t (*active_voices)(const daw_plugin_instance_t *inst);

    /* ABI 1.1 (present when struct_size covers them, see daw_plugin_has_resources) */
    uint32_t resource_slot_count;
    const daw_resource_slot_desc_t *resource_slots;
    /* control thread, see "resources" above; res == NULL empties the slot. DAW_OK or DAW_ERR_* */
    int32_t (*set_resource)(daw_plugin_instance_t *inst, uint32_t slot, const daw_resource_t *res);
} daw_plugin_descriptor_t;

typedef const daw_plugin_descriptor_t *(*daw_plugin_entry_fn)(void);

/* host-side check of a descriptor's ABI compatibility */
static inline int daw_plugin_abi_compatible(const daw_plugin_descriptor_t *d)
{
    return d && d->api_major == DAW_PLUGIN_API_MAJOR && d->api_minor <= DAW_PLUGIN_API_MINOR &&
           d->struct_size >= offsetof(daw_plugin_descriptor_t, active_voices) + sizeof(void *);
}

/* ABI 1.1: whether the descriptor has (and uses) resource slots */
static inline int daw_plugin_has_resources(const daw_plugin_descriptor_t *d)
{
    return d->api_minor >= 1 && d->struct_size >= offsetof(daw_plugin_descriptor_t, set_resource) + sizeof(void *) &&
           d->resource_slot_count > 0 && d->resource_slots && d->set_resource;
}

#ifdef __cplusplus
}
#endif

#endif
