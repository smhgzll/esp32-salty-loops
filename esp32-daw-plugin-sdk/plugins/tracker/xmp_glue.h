/* SPDX-License-Identifier: MIT
 * Tracker plugin: what the plugin needs from libxmp beyond its public API (xmp.h), implemented
 * with libxmp's internal player state in xmp_glue.c. */
#ifndef DAWXMP_GLUE_H
#define DAWXMP_GLUE_H

#include <stdint.h>

#include "xmp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound for 128 mixer voice records plus their allocation header on either target. */
enum { DAWXMP_VOICE_FAST_BUDGET = 24 * 1024 };

/* where a row starts on the song's first pass, at tempo factor 1 (libxmp's scan) */
typedef struct dawxmp_row {
    double ms;          /* time of the row's first tick, from the start of the song */
    uint16_t ord, row;  /* order (position) and row */
    uint16_t bpm;       /* tempo, speed and global volume before the row's events */
    uint8_t speed, gvol;
} dawxmp_row;

/* Control thread, module loaded and the player stopped: libxmp's scan of the song's first pass
 * once more, recording every row it visits (patched in: libxmp_daw_row_hook). *rows comes from
 * the shim allocator (dawxmp_free); returns the row count or -1. */
int dawxmp_rows(xmp_context c, dawxmp_row **rows, double *length_ms);

/* [RT] player started: continue at `r` with its tempo state and play that row's first frame
 * (output in dawxmp_buffer). Voices sounding before stop. Returns xmp_play_frame's result. */
int dawxmp_seek(xmp_context c, const dawxmp_row *r);

/* Deferred mixing (patched in): xmp_play_frame runs the player for the tick and leaves the mix
 * buffer cleared; dawxmp_mix then mixes the tick's next `frames` frames into it, in order, as they
 * are needed. Returns the frames mixed (0 when the tick is complete). [RT] */
void dawxmp_set_deferred(xmp_context c, int on);
int dawxmp_mix(xmp_context c, int frames);

/* Two cores (patched in: libxmp_mixer_part_split): a part's voices split between the calling core
 * and another one through the host's parallel service (daw_host_api_t.parallel, ABI 1.3). The output
 * is identical to dawxmp_mix: the mix is a sum of integers.
 * dawxmp_split_new: control thread, player started; the second tick buffer (freed by
 * xmp_end_player) and the jobs (dawxmp_free). NULL: no memory (dawxmp_mix only).
 * dawxmp_mix_split: [RT] as dawxmp_mix; small parts, a declined call or a ProTracker sample swap
 * mix on this core. */
typedef int32_t (*dawxmp_parallel_fn)(void *host_ctx, void (*local)(void *), void *local_arg,
                                      void (*remote)(void *), void *remote_arg);
void *dawxmp_split_new(xmp_context c);
int dawxmp_mix_split(xmp_context c, void *split, int frames, dawxmp_parallel_fn parallel, void *host_ctx);

/* [RT] the last frame's mix (stereo, interleaved) and its length in frames. Full scale of libxmp's
 * default amplification (1) is 1 << 26: the 16-bit output is this >> 11 before clipping. */
const int32_t *dawxmp_buffer(xmp_context c, uint32_t *frames);
int dawxmp_loops(xmp_context c);   /* [RT] how often the song has ended and restarted */
int dawxmp_voices(xmp_context c);  /* [RT] mixer voices in use */

#ifdef __cplusplus
}
#endif

#endif
