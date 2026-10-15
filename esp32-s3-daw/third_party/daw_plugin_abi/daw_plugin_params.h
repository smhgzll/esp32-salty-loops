/* SPDX-License-Identifier: MIT
 * esp32-daw-plugin-sdk: parameter and preset descriptors.
 *
 * All strings and arrays referenced from these structs are owned by the plugin and must stay
 * valid (normally: static const data) for as long as the plugin's code is linked. */
#ifndef DAW_PLUGIN_PARAMS_H
#define DAW_PLUGIN_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum daw_param_type {
    DAW_PARAM_FLOAT = 0,
    DAW_PARAM_INT = 1,   /* integral values between min and max */
    DAW_PARAM_BOOL = 2,  /* 0 or 1 */
    DAW_PARAM_ENUM = 3   /* 0 .. enum_count-1, labels in enum_labels */
} daw_param_type_t;

/* how a UI maps a 0..1 control position onto min..max (the value itself is always plain) */
typedef enum daw_param_scale {
    DAW_SCALE_LINEAR = 0,
    DAW_SCALE_LOG = 1,      /* exponential: frequencies, times (min must be > 0) */
    DAW_SCALE_DB = 2,       /* value is a linear gain, shown in dB */
    DAW_SCALE_BIPOLAR = 3   /* linear, centred on (min+max)/2 (pan, detune) */
} daw_param_scale_t;

#define DAW_PARAM_AUTOMATABLE 0x0001u /* may be driven by automation clips */
#define DAW_PARAM_HIDDEN      0x0002u /* not shown in the generated editor */
#define DAW_PARAM_READONLY    0x0004u /* output / meter, written by the plugin */
#define DAW_PARAM_NO_SMOOTH   0x0008u /* discrete: the host must not interpolate */

typedef struct daw_param_desc {
    uint32_t id;               /* stable numeric id, unique inside the plugin, never reused */
    const char *key;           /* stable string id ("cutoff"); used in project files */
    const char *name;          /* display name */
    const char *group;         /* UI group ("Osc 1", "Filter", "Envelope"), may be NULL */
    const char *unit;          /* "Hz", "ms", "dB", "%", "st", may be NULL */
    uint32_t type;             /* daw_param_type_t */
    uint32_t scale;            /* daw_param_scale_t */
    float min, max, def;
    uint32_t flags;            /* DAW_PARAM_* */
    uint32_t enum_count;
    const char *const *enum_labels;
} daw_param_desc_t;

/* a factory preset: values for every parameter, in descriptor order */
typedef struct daw_preset_desc {
    const char *name;
    const char *category;      /* may be NULL */
    const float *values;       /* param_count values */
} daw_preset_desc_t;

#ifdef __cplusplus
}
#endif

#endif
