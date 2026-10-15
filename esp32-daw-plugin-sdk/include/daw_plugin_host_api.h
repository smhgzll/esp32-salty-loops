/* SPDX-License-Identifier: MIT
 * esp32-daw-plugin-sdk: services the host offers a plugin instance.
 *
 * The host passes a pointer to this table to create(); it stays valid until destroy() returns.
 * Functions marked [RT] may be called from process(); all others only from create(),
 * destroy(), activate(), deactivate(), load_state() and save_state(). */
#ifndef DAW_PLUGIN_HOST_API_H
#define DAW_PLUGIN_HOST_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* memory classes for alloc(): the host maps them to its RAM types (ESP32-S3: internal SRAM or
 * PSRAM; Linux: the same heap). Never assume FAST memory is unlimited: an ESP32-S3 has a few
 * hundred KB of internal RAM shared with Wi-Fi and DMA. */
#define DAW_MEM_FAST  0x1u   /* hot DSP state, read every sample: internal SRAM */
#define DAW_MEM_LARGE 0x2u   /* delay lines, wavetables, samples: PSRAM is fine */
#define DAW_MEM_ZERO  0x100u /* zero-filled */

typedef enum daw_log_level {
    DAW_LOG_ERROR = 0,
    DAW_LOG_WARN = 1,
    DAW_LOG_INFO = 2,
    DAW_LOG_DEBUG = 3
} daw_log_level_t;

typedef struct daw_host_api {
    uint32_t struct_size;   /* sizeof(daw_host_api_t) of the host; check before using new fields */
    uint16_t api_major;
    uint16_t api_minor;
    void *host_ctx;         /* opaque, pass back unchanged */

    /* returns NULL on failure; alignment is a power of two <= 64. The plugin owns the block until
     * it passes it to free(); everything allocated must be freed before destroy() returns. */
    void *(*alloc)(void *host_ctx, size_t size, size_t align, uint32_t mem_flags);
    void (*free)(void *host_ctx, void *ptr);

    /* message is copied; not for process() */
    void (*log)(void *host_ctx, int level, const char *message);

    /* [RT] a non-blocking error / status report (overload, NaN reset, voice steal). The host
     * records the latest code and a counter; it never blocks or allocates. */
    void (*report)(void *host_ctx, int32_t code);

    /* ABI 1.3, optional [RT]. Try running two independent DSP jobs, one on another core.
     * Return 1 only after BOTH callbacks finished, or 0 without calling either callback when
     * unavailable (caller then runs them serially). Callbacks must be independent, bounded,
     * allocation-free DSP; their arguments stay alive until this function returns. May decline
     * nested calls / a busy worker. Check struct_size before reading this trailing pointer. */
    int32_t (*parallel)(void *host_ctx, void (*local)(void *), void *local_arg,
                        void (*remote)(void *), void *remote_arg);
} daw_host_api_t;

/* report() codes */
#define DAW_REPORT_NAN_RESET   1 /* the plugin found NaN / Inf in its state and reset it */
#define DAW_REPORT_VOICE_STEAL 2 /* a voice was stolen */
#define DAW_REPORT_EVENT_DROP  3 /* an event could not be handled (e.g. per-note queue full) */

#ifdef __cplusplus
}
#endif

#endif
