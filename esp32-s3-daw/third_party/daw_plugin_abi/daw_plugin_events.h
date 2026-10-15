/* SPDX-License-Identifier: MIT
 * esp32-daw-plugin-sdk: timestamped events delivered to a plugin's process() call.
 *
 * Every event is a fixed-size POD (32 bytes). Events in one block are sorted by `offset`
 * (sample index inside the block, 0 <= offset < frames); the host guarantees the order. */
#ifndef DAW_PLUGIN_EVENTS_H
#define DAW_PLUGIN_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum daw_event_type {
    DAW_EV_NOTE_ON = 1,        /* note: key, velocity, note_id, flags (SLIDE / ACCENT / LEGATO) */
    DAW_EV_NOTE_OFF = 2,       /* note: key, note_id, release velocity in `velocity` */
    DAW_EV_NOTE_EXPR = 3,      /* expr: per-note expression change for note_id (DAW_CAP_NOTE_* caps) */
    DAW_EV_PARAM = 4,          /* param: parameter id, plain value (inside the descriptor's min..max) */
    DAW_EV_CC = 5,             /* midi: controller number and value 0..1 */
    DAW_EV_PITCH_BEND = 6,     /* midi: channel-wide bend, value -1..1 (range is the plugin's) */
    DAW_EV_CHANNEL_PRESSURE = 7,
    DAW_EV_PROGRAM = 8,        /* midi: program number in `index` */
    DAW_EV_ALL_NOTES_OFF = 9,  /* release every voice (envelopes run their release) */
    DAW_EV_ALL_SOUND_OFF = 10, /* silence immediately, no tails */
    DAW_EV_SUSTAIN = 11,       /* midi: sustain pedal, value >= 0.5 is down */
    DAW_EV_POLY_PRESSURE = 12, /* midi: number = key, value 0..1 */
    DAW_EV_MIDI_BYTES = 13,    /* bytes: opaque SysEx / system message, lifetime = process() call */
    DAW_EV_MIDI_RESET = 14,    /* reset MIDI controllers to defaults, retain chosen patch / params */
    DAW_EV_CHANNEL_PITCH = 15 /* midi.value: channel pitch offset in semitones (ABI 1.4) */
} daw_event_type_t;

/* DAW_EV_NOTE_ON flags */
#define DAW_NOTE_FLAG_SLIDE  0x0001u /* glide from the sounding note to this key (DAW_CAP_SLIDE) */
#define DAW_NOTE_FLAG_ACCENT 0x0002u /* accented step (DAW_CAP_ACCENT); velocity is already boosted */
#define DAW_NOTE_FLAG_LEGATO 0x0004u /* do not retrigger envelopes if a voice is already sounding */
#define DAW_NOTE_FLAG_PREVIEW 0x0008u /* audition from an editor (piano roll key, browser) */
/* Imported MIDI events opt into MIDI controller semantics; ordinary DAW notes are unchanged. */
#define DAW_NOTE_FLAG_MIDI 0x0020u
/* ABI 1.2, only to plugins with DAW_CAP_NOTE_CHASE: the transport started (or jumped, or looped)
 * inside this note, which began `chase_mbeats` / 1000 quarter notes before the event */
#define DAW_NOTE_FLAG_CHASE  0x0010u

/* per-note expressions (DAW_EV_NOTE_EXPR `expr`), also the initial values carried by NOTE_ON */
typedef enum daw_note_expr {
    DAW_EXPR_PAN = 0,      /* -1 (left) .. +1 (right) */
    DAW_EXPR_PITCH = 1,    /* semitones offset, fractional allowed (fine pitch) */
    DAW_EXPR_VOLUME = 2,   /* gain 0..2 */
    DAW_EXPR_CUTOFF = 3,   /* -1..+1 relative modulation of the plugin's filter */
    DAW_EXPR_RESONANCE = 4,
    DAW_EXPR_COUNT
} daw_note_expr_t;

typedef struct daw_event {
    uint32_t offset;   /* sample offset inside the current block */
    uint16_t type;     /* daw_event_type_t */
    uint16_t flags;    /* DAW_NOTE_FLAG_*; MIDI flag also applies to controller/system events */
    union {
        struct {
            int32_t note_id;   /* host-unique id of the note instance; -1 when unknown (live MIDI) */
            uint8_t key;       /* MIDI key 0..127 */
            uint8_t channel;   /* MIDI channel 0..15 (live input), 0 for sequenced notes */
            uint16_t reserved;
            float velocity;    /* 0..1 (NOTE_OFF: release velocity) */
            float pan;         /* initial DAW_EXPR_PAN, -1..1 */
            float pitch;       /* initial DAW_EXPR_PITCH, semitones */
            union {
                uint32_t slide_ms;     /* NOTE_ON with SLIDE: glide duration hint, 0 = plugin's own */
                uint32_t chase_mbeats; /* NOTE_ON with CHASE (1.2): elapsed part, 1/1000 quarter note */
            };
        } note;
        struct {
            int32_t note_id;
            uint8_t key;
            uint8_t expr;      /* daw_note_expr_t */
            uint16_t reserved;
            float value;
        } expr;
        struct {
            uint32_t id;       /* daw_param_desc_t.id */
            float value;       /* plain value */
        } param;
        struct {
            uint8_t channel;
            uint8_t number;    /* CC number / program */
            uint16_t index;
            float value;       /* CC 0..1, pitch bend -1..1, pressure 0..1, sustain 0/1 */
        } midi;
        struct {
            const uint8_t *data; /* borrowed, never retain / mutate / free */
            uint32_t size;
        } bytes;
        uint8_t raw[24];
    } u;
} daw_event_t;

#ifdef __cplusplus
}
#endif

#endif
