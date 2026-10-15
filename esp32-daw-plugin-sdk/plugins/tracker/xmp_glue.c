/* SPDX-License-Identifier: MIT
 * Tracker plugin: row map and seeking on top of libxmp's player (see xmp_glue.h). */
#include "xmp_glue.h"

#include "common.h"
#include "mixer.h"

_Static_assert(SMIX_NUMVOC * sizeof(struct mixer_voice) + 16 <= DAWXMP_VOICE_FAST_BUDGET,
               "Tracker's declared FAST memory covers all mixer voices");

#define MAX_ROWS (1 << 20)

/* the libxmp patch's scan hook (third_party/patches/libxmp-esp32-daw.patch) */
extern void (*libxmp_daw_row_hook)(void *arg, int ord, int row, double time, int speed, int bpm, int gvl);
extern void *libxmp_daw_row_hook_arg;

struct rowmap {
    dawxmp_row *rows;
    int n, cap, failed;
};

static void add_row(void *arg, int ord, int row, double time, int speed, int bpm, int gvl)
{
    struct rowmap *r = (struct rowmap *)arg;
    if (r->failed)
        return;
    if (r->n == r->cap) {
        int ncap = r->cap ? r->cap * 2 : 1024;
        dawxmp_row *q;
        if (ncap > MAX_ROWS || !(q = (dawxmp_row *)realloc(r->rows, (size_t)ncap * sizeof(*q)))) {
            r->failed = 1;
            return;
        }
        r->rows = q;
        r->cap = ncap;
    }
    dawxmp_row *e = &r->rows[r->n++];
    e->ms = time;
    e->ord = (uint16_t)ord;
    e->row = (uint16_t)row;
    e->bpm = (uint16_t)bpm;
    e->speed = (uint8_t)speed;
    e->gvol = (uint8_t)gvl;
}

int dawxmp_rows(xmp_context c, dawxmp_row **rows, double *length_ms)
{
    struct context_data *ctx = (struct context_data *)c;
    struct rowmap r = {NULL, 0, 0, 0};

    *rows = NULL;
    *length_ms = 0;
    if (ctx->state < XMP_STATE_LOADED || ctx->state >= XMP_STATE_PLAYING)
        return -1;
    /* libxmp's own scan again (it repeats exactly what loading computed), reporting every row */
    libxmp_daw_row_hook_arg = &r;
    libxmp_daw_row_hook = add_row;
    const int ret = libxmp_scan_sequences(ctx);
    libxmp_daw_row_hook = NULL;
    libxmp_daw_row_hook_arg = NULL;
    if (ret < 0 || r.failed || r.n == 0 || ctx->m.num_sequences < 1 || ctx->p.scan[0].time <= 0) {
        free(r.rows);
        return -1;
    }
    *rows = r.rows;
    *length_ms = ctx->p.scan[0].time;
    return r.n;
}

int dawxmp_seek(xmp_context c, const dawxmp_row *r)
{
    struct context_data *ctx = (struct context_data *)c;
    struct player_data *p = &ctx->p;
    struct module_data *m = &ctx->m;
    struct ord_data saved;
    int ret;

    if (ctx->state < XMP_STATE_PLAYING || r->ord >= m->mod.len)
        return -XMP_ERROR_STATE;
    ret = xmp_set_position(c, r->ord);  /* the new p->pos: -1 for order 0 ("restart") */
    if (ret < -1)
        return ret;
    /* After set_position (which clears the flow state): start at the row, and let the
     * reposition restore the row's tempo state instead of the order's. */
    p->flow.jumpline = r->row;
    p->flow.force_reposition = 1;
    p->loop_count = 0;
    ctx->s.tick_carry = 0;  /* the same frame lengths from here on, wherever playback came from */
    saved = m->xxo_info[r->ord];
    m->xxo_info[r->ord].speed = r->speed;
    m->xxo_info[r->ord].bpm = r->bpm;
    m->xxo_info[r->ord].gvl = r->gvol;
    ret = xmp_play_frame(c);
    m->xxo_info[r->ord] = saved;
    return ret;
}

const int32_t *dawxmp_buffer(xmp_context c, uint32_t *frames)
{
    struct context_data *ctx = (struct context_data *)c;
    *frames = ctx->s.ticksize > 0 ? (uint32_t)ctx->s.ticksize : 0;
    /* the mix before libxmp's integer downmix: no clipping (int32: int; int32_t: long on Xtensa) */
    return (const int32_t *)ctx->s.buf32;
}

void dawxmp_set_deferred(xmp_context c, int on) { ((struct context_data *)c)->s.deferred = on; }

int dawxmp_mix(xmp_context c, int frames) { return libxmp_mixer_softmixer_part((struct context_data *)c, frames); }

/* below this much work (frames x 4 per mono voice: 8 voices of 128 frames) a hand-off to the other
 * core costs more than it saves */
#define SPLIT_MIN_WORK (8 * 4 * 128)

void *dawxmp_split_new(xmp_context c)
{
    struct context_data *ctx = (struct context_data *)c;
    struct mixer_data *s = &ctx->s;
    struct libxmp_mix_split *x;

    if (ctx->state < XMP_STATE_PLAYING || s->buf32b)
        return NULL;
    x = (struct libxmp_mix_split *)calloc(1, sizeof(*x));
    s->buf32b = (int32 *)calloc((size_t)s->total_size, sizeof(int32));
    if (!x || !s->buf32b) {
        free(s->buf32b);
        s->buf32b = NULL;
        free(x);
        return NULL;
    }
    s->b_pending = 0;
    return x;
}

int dawxmp_mix_split(xmp_context c, void *split, int frames, dawxmp_parallel_fn parallel, void *host_ctx)
{
    struct context_data *ctx = (struct context_data *)c;
    struct libxmp_mix_split *x = (struct libxmp_mix_split *)split;

    /* the split changes nothing until its jobs run: a declined call mixes the part here */
    if (!x || !parallel || libxmp_mixer_part_split(ctx, frames, SPLIT_MIN_WORK, x) <= 0 ||
        !parallel(host_ctx, libxmp_mixer_job_run, &x->job[0], libxmp_mixer_job_run, &x->job[1]))
        return libxmp_mixer_softmixer_part(ctx, frames);
    libxmp_mixer_part_join(ctx, x);
    return x->job[0].count;
}

int dawxmp_loops(xmp_context c) { return ((struct context_data *)c)->p.loop_count; }

int dawxmp_voices(xmp_context c) { return ((struct context_data *)c)->p.virt.virt_used; }
