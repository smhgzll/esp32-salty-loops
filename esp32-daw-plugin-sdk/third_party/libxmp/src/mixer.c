/* Extended Module Player
 * Copyright (C) 1996-2026 Claudio Matsuoka and Hipolito Carraro Jr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <math.h>
#include "common.h"
#include "virtual.h"
#include "mixer.h"
#include "mix_all.h"
#include "period.h"
#include "player.h"	/* for set_sample_end() */

#ifdef LIBXMP_PAULA_SIMULATOR
#include "paula.h"
#endif


#define DOWNMIX_SHIFT	 12
#define LIM8_HI		 127
#define LIM8_LO		-128
#define LIM16_HI	 32767
#define LIM16_LO	-32768
#define LIM32_HI	2147483647
#define LIM32_LO	(-2147483647 - 1)

#define ANTICLICK_FPSHIFT	24

struct loop_data
{
#define LOOP_PROLOGUE 1
#define LOOP_EPILOGUE 2
	void *sptr;
	int start;
	int end;
	int first_loop;
	int _16bit;
	int active;
	int prologue_num;
	int epilogue_num;
	uint8 prologue[LOOP_PROLOGUE * 2 /* 16-bit */ * 2 /* stereo */];
	uint8 epilogue[LOOP_EPILOGUE * 2 /* 16-bit */ * 2 /* stereo */];
};

/* See mix_all.h for a full explanation of these flags. */
#define FLAG_16_BITS	0x01
#define FLAG_STEREO	0x02
#define FLAG_STEREOOUT	0x04
#define FLAG_FILTER	0x08
#define FLAG_ACTIVE	0x10
/* #define FLAG_SYNTH	0x20 */
#define FIDX_FLAGMASK	(FLAG_16_BITS | FLAG_STEREO | FLAG_STEREOOUT | FLAG_FILTER)


/* Downmix 32bit samples to 8bit, signed or unsigned, mono or stereo output */
static void downmix_int_8bit(int8 *LIBXMP_RESTRICT dest,
			     const int32 *src, int num, int amp, unsigned offs)
{
	int smp;
	int shift = DOWNMIX_SHIFT + 8 - amp;

	for (; num--; src++, dest++) {
		smp = *src >> shift;
		if (smp > LIM8_HI) {
			*dest = LIM8_HI;
		} else if (smp < LIM8_LO) {
			*dest = LIM8_LO;
		} else {
			*dest = smp;
		}

		if (offs) *dest = (int8)((uint8)*dest + offs);
	}
}


/* Downmix 32bit samples to 16bit, signed or unsigned, mono or stereo output */
static void downmix_int_16bit(int16 *LIBXMP_RESTRICT dest,
			      const int32 *src, int num, int amp, unsigned offs)
{
	int smp;
	int shift = DOWNMIX_SHIFT - amp;

	for (; num--; src++, dest++) {
		smp = *src >> shift;
		if (smp > LIM16_HI) {
			*dest = LIM16_HI;
		} else if (smp < LIM16_LO) {
			*dest = LIM16_LO;
		} else {
			*dest = smp;
		}

		if (offs) *dest = (int16)((uint16)*dest + offs);
	}
}

/* Saturate 32bit samples, signed or unsigned, mono or stereo output */
static void downmix_int_32bit(int32 *LIBXMP_RESTRICT dest,
			      const int32 *src, int num, int amp, unsigned offs)
{
	int32 smp;
	int shift = (16 - DOWNMIX_SHIFT) + amp;

	/* Signed left shift overflow is UB; clamp with pre-shifted bounds. */
	int max_value = LIM32_HI >> shift;
	int min_value = LIM32_LO >> shift;

	for (; num--; src++, dest++) {
		smp = *src;
		if (smp >= max_value) {
			*dest = LIM32_HI;
		} else if (smp <= min_value) {
			*dest = LIM32_LO;
		} else {
			*dest = XMP_ASL(smp, shift);
		}

		if (offs) *dest = (int32)((uint32)*dest + offs);
	}
}

static void anticlick(struct mixer_voice *vi)
{
	vi->flags |= ANTICLICK;
	vi->old_vl = 0;
	vi->old_vr = 0;
}

/* Ok, it's messy, but it works :-) Hipolito */
static void do_anticlick(struct context_data *ctx, int voc, int32 *buf, int count)
{
	struct player_data *p = &ctx->p;
	struct mixer_data *s = &ctx->s;
	struct mixer_voice *vi = &p->virt.voice_array[voc];
	int smp_l, smp_r;
	int discharge = s->ticksize >> ANTICLICK_SHIFT;
	int stepmul, stepval;
	uint32 stepmul_sq;

	smp_l = vi->sleft;
	smp_r = vi->sright;
	vi->sright = vi->sleft = 0;

	if (smp_l == 0 && smp_r == 0) {
		return;
	}

	if (buf == NULL) {
		buf = s->buf32;
		count = discharge;
	} else if (count > discharge) {
		count = discharge;
	}

	if (count <= 0) {
		return;
	}

	stepval = (1 << ANTICLICK_FPSHIFT) / count;
	stepmul = stepval * count;

	if (~s->format & XMP_FORMAT_MONO) {
		while ((stepmul -= stepval) > 0) {
			/* Truncate to 16-bits of precision so the product is 32-bits. */
			stepmul_sq = stepmul >> (ANTICLICK_FPSHIFT - 16);
			stepmul_sq *= stepmul_sq;
			*buf++ += (stepmul_sq * (int64)smp_l) >> 32;
			*buf++ += (stepmul_sq * (int64)smp_r) >> 32;
		}
	} else {
		while ((stepmul -= stepval) > 0) {
			stepmul_sq = stepmul >> (ANTICLICK_FPSHIFT - 16);
			stepmul_sq *= stepmul_sq;
			*buf++ += (stepmul_sq * (int64)smp_l) >> 32;
		}
	}
}

/* esp32-daw: a voice reset changes the player's voice counters, shared by both cores' jobs: a
 * job (not NULL) records it for libxmp_mixer_part_join, after which nothing reads the voice */
static void reset_voice(struct context_data *ctx, struct libxmp_mix_job *job, int voc, int mute)
{
	if (job == NULL) {
		libxmp_virt_resetvoice(ctx, voc, mute);
		return;
	}
	job->reset[job->nreset] = voc;
	job->reset_mute[job->nreset++] = mute;
}

static void set_sample_end(struct context_data *ctx, struct libxmp_mix_job *job, int voc, int end)
{
	struct player_data *p = &ctx->p;
	struct module_data *m = &ctx->m;
	struct mixer_voice *vi = &p->virt.voice_array[voc];
	struct channel_data *xc;

	if ((uint32)voc >= p->virt.maxvoc)
		return;

	xc = &p->xc_data[vi->chn];

	if (end) {
		SET_NOTE(NOTE_SAMPLE_END);
		vi->fidx &= ~FLAG_ACTIVE;
		if (HAS_QUIRK(QUIRK_RSTCHN)) {
			reset_voice(ctx, job, voc, 0);
		}
	} else {
		RESET_NOTE(NOTE_SAMPLE_END);
	}
}

/* Back up sample data before and after loop and replace it for interpolation.
 * TODO: if higher order interpolation than spline is added, the copy needs to
 *       properly wrap around the loop data (modulo) for correct small loops.
 * TODO: use an overlap buffer like OpenMPT? This is easier, but a little dirty. */
static void init_sample_wraparound(struct mixer_data *s, struct loop_data *ld,
				   struct mixer_voice *vi, struct xmp_sample *xxs)
{
	int prologue_num = LOOP_PROLOGUE;
	int epilogue_num = LOOP_EPILOGUE;
	int bidir;
	int i;

	if (!vi->sptr || s->interp == XMP_INTERP_NEAREST || (~xxs->flg & XMP_SAMPLE_LOOP)) {
		ld->active = 0;
		return;
	}

	ld->sptr = vi->sptr;
	ld->start = vi->start;
	ld->end = vi->end;
	ld->first_loop = !(vi->flags & SAMPLE_LOOP);
	ld->_16bit = (xxs->flg & XMP_SAMPLE_16BIT);
	ld->active = 1;

	/* Stereo */
	if (xxs->flg & XMP_SAMPLE_STEREO) {
		ld->start <<= 1;
		ld->end <<= 1;
		prologue_num <<= 1;
		epilogue_num <<= 1;
	}
	ld->prologue_num = prologue_num;
	ld->epilogue_num = epilogue_num;

	bidir = vi->flags & VOICE_BIDIR;

	if (ld->_16bit) {
		uint16 *start = (uint16 *)ld->sptr + ld->start;
		uint16 *end = (uint16 *)ld->sptr + ld->end;

		memcpy(ld->prologue, start - prologue_num, prologue_num * 2);
		memcpy(ld->epilogue, end, epilogue_num * 2);

		if (!ld->first_loop) {
			for (i = 0; i < prologue_num; i++) {
				int j = i - prologue_num;
				start[j] = bidir ? start[-1 - j] : end[j];
			}
		}
		for (i = 0; i < epilogue_num; i++) {
			end[i] = bidir ? end[-1 - i] : start[i];
		}
	} else {
		uint8 *start = (uint8 *)ld->sptr + ld->start;
		uint8 *end = (uint8 *)ld->sptr + ld->end;

		memcpy(ld->prologue, start - prologue_num, prologue_num);
		memcpy(ld->epilogue, end, epilogue_num);

		if (!ld->first_loop) {
			for (i = 0; i < prologue_num; i++) {
				int j = i - prologue_num;
				start[j] = bidir ? start[-1 - j] : end[j];
			}
		}
		for (i = 0; i < epilogue_num; i++) {
			end[i] = bidir ? end[-1 - i] : start[i];
		}
	}
}

/* Restore old sample data from before and after loop. */
static void reset_sample_wraparound(struct loop_data *ld)
{
	int prologue_num = ld->prologue_num;
	int epilogue_num = ld->epilogue_num;

	if (!ld->active)
		return;

	if (ld->_16bit) {
		uint16 *start = (uint16 *)ld->sptr + ld->start;
		uint16 *end = (uint16 *)ld->sptr + ld->end;

		memcpy(start - prologue_num, ld->prologue, prologue_num * 2);
		memcpy(end, ld->epilogue, epilogue_num * 2);
	} else {
		uint8 *start = (uint8 *)ld->sptr + ld->start;
		uint8 *end = (uint8 *)ld->sptr + ld->end;

		memcpy(start - prologue_num, ld->prologue, prologue_num);
		memcpy(end, ld->epilogue, epilogue_num);
	}
}

static int has_active_sustain_loop(struct context_data *ctx, struct mixer_voice *vi,
				   struct xmp_sample *xxs)
{
#ifndef LIBXMP_CORE_DISABLE_IT
	struct module_data *m = &ctx->m;
	return vi->smp < m->mod.smp && (xxs->flg & XMP_SAMPLE_SLOOP) && (~vi->flags & VOICE_RELEASE);
#else
	return 0;
#endif
}

static int has_active_loop(struct context_data *ctx, struct mixer_voice *vi,
			   struct xmp_sample *xxs)
{
	return (xxs->flg & XMP_SAMPLE_LOOP) || has_active_sustain_loop(ctx, vi, xxs);
}

/* Update the voice endpoints based on current sample loop state. */
static void adjust_voice_end(struct context_data *ctx, struct mixer_voice *vi,
			     struct xmp_sample *xxs, struct extra_sample_data *xtra)
{
	vi->flags &= ~VOICE_BIDIR;

	if (xtra && has_active_sustain_loop(ctx, vi, xxs)) {
		vi->start = xtra->sus;
		vi->end = xtra->sue;
		if (xxs->flg & XMP_SAMPLE_SLOOP_BIDIR) vi->flags |= VOICE_BIDIR;

	} else if (xxs->flg & XMP_SAMPLE_LOOP) {
		vi->start = xxs->lps;
		if ((xxs->flg & XMP_SAMPLE_LOOP_FULL) && (~vi->flags & SAMPLE_LOOP)) {
			vi->end = xxs->len;
		} else {
			vi->end = xxs->lpe;
			if (xxs->flg & XMP_SAMPLE_LOOP_BIDIR) vi->flags |= VOICE_BIDIR;
		}
	} else {
		vi->start = 0;
		vi->end = xxs->len;
	}
}

static int loop_reposition(struct context_data *ctx, struct mixer_voice *vi,
			   struct xmp_sample *xxs, struct extra_sample_data *xtra)
{
	int loop_changed = !(vi->flags & SAMPLE_LOOP);

	vi->flags |= SAMPLE_LOOP;

	if(loop_changed)
		adjust_voice_end(ctx, vi, xxs, xtra);

	if (~vi->flags & VOICE_BIDIR) {
		/* Reposition for next loop */
		if (~vi->flags & VOICE_REVERSE)
			vi->pos -= vi->end - vi->start;
		else
			vi->pos += vi->end - vi->start;
	} else {
		/* Bidirectional loop: switch directions */
		vi->flags ^= VOICE_REVERSE;

		/* Wrap voice position around endpoint */
		if (vi->flags & VOICE_REVERSE) {
			/* OpenMPT Bidi-Loops.it: "In Impulse Tracker's software
			 * mixer, ping-pong loops are shortened by one sample."
			 */
			vi->pos = vi->end * 2 - ctx->s.bidir_adjust - vi->pos;
		} else {
			vi->pos = vi->start * 2 - vi->pos;
		}
	}
	/* Safety check: pos should not be excessively past the sample end.
	 * This only seems to happen with very low sample rates. */
	if (vi->pos > xxs->len + 1) {
		vi->pos = xxs->len + 1;
	}
	return loop_changed;
}

static void hotswap_sample(struct context_data *ctx, struct mixer_voice *vi,
 int voc, int smp)
{
	int vol = vi->vol;
	int pan = vi->pan;
	libxmp_mixer_setpatch(ctx, voc, smp, 0);
	vi->flags |= SAMPLE_LOOP;
	vi->vol = vol;
	vi->pan = pan;
}

static void get_current_sample(struct context_data *ctx, struct mixer_voice *vi,
 struct xmp_sample **xxs, struct extra_sample_data **xtra, int *c5spd)
{
	struct module_data *m = &ctx->m;
	struct xmp_module *mod = &m->mod;

	if (vi->smp < mod->smp) {
		*xxs = &mod->xxs[vi->smp];
		*xtra = &m->xtra[vi->smp];
		*c5spd = m->xtra[vi->smp].c5spd;
	} else {
		*xxs = &ctx->smix.xxs[vi->smp - mod->smp];
		*xtra = NULL;
		*c5spd = m->c4rate;
	}
	adjust_voice_end(ctx, vi, *xxs, *xtra);
}

/* Calculate the required number of sample frames to render a tick.
 * Returns -1 if any of the parameters are invalid. */
int libxmp_mixer_get_ticksize(int freq, double time_factor, double rrate, int bpm)
{
	double calc;
	int ticksize;

	if (freq <= 0 || bpm <= 0 || time_factor <= 0.0 || rrate <= 0.0) {
		return -1;
	}

	calc = freq * time_factor * rrate / bpm / 1000;
	if (calc > INT_MAX || calc != calc /* NaN */) {
		return -1;
	}

	ticksize = (int)calc;

	if (ticksize < (1 << ANTICLICK_SHIFT))
		ticksize = 1 << ANTICLICK_SHIFT;

	return ticksize;
}

/* Prepare the mixer for the next tick */
static void libxmp_mixer_prepare(struct context_data *ctx)
{
	struct player_data *p = &ctx->p;
	struct module_data *m = &ctx->m;
	struct mixer_data *s = &ctx->s;
	int bytelen;

	s->ticksize = libxmp_mixer_get_ticksize(s->freq,
		m->time_factor * p->time_factor_relative, m->rrate, p->bpm);

	/* esp32-daw: carry the fraction of a frame from tick to tick, so ticks
	 * average their exact length (a song stays on a DAW's beat grid) */
	if (s->ticksize > (1 << ANTICLICK_SHIFT)) {
		double exact = (double)s->freq * m->time_factor *
			p->time_factor_relative * m->rrate / p->bpm / 1000 +
			s->tick_carry;
		if (exact >= s->ticksize && exact < s->ticksize + 2.0) {
			s->ticksize = (int)exact;
			s->tick_carry = exact - s->ticksize;
		} else {
			s->tick_carry = 0.0;
		}
	}

	/* Protect the mixer from broken values caused by xmp_set_tempo_factor. */
	if (s->ticksize < 0 || s->ticksize > (s->total_size / 2)) {
		s->ticksize = s->total_size / 2;
	}

	bytelen = s->ticksize * sizeof(int32);
	if (~s->format & XMP_FORMAT_MONO) {
		bytelen *= 2;
	}
	memset(s->buf32, 0, bytelen);

	s->split_valid = 0;	/* esp32-daw: new voices */

	/* esp32-daw: the last tick was left before its end (a seek): clear what it left */
	if (s->b_pending) {
		memset(s->buf32b, 0, s->total_size * sizeof(int32));
		s->b_pending = 0;
	}
}

/* Fill the output buffer calling one of the handlers. The buffer contains
 * sound for one tick (a PAL frame or 1/50s for standard vblank-timed mods)
 */
/* esp32-daw: mix frames [off, off + count) of the current tick (all of it: 0, ticksize). The
 * volume ramps continue from part to part (ramp_left / ramp_dl / ramp_dr), discharges reach
 * to the tick's end, and a voice's previous volume is set when the tick is complete.
 * job NULL: every voice into buf32; else the job's voices into its buffer (base). */
static void softmixer_voices(struct context_data *ctx, struct libxmp_mix_job *job,
			     int32 *base, int off, int count)
{
	struct player_data *p = &ctx->p;
	struct mixer_data *s = &ctx->s;
	struct module_data *m = &ctx->m;
	struct xmp_module *mod = &m->mod;
	struct extra_sample_data *xtra;
	struct xmp_sample *xxs;
	struct mixer_voice *vi;
	struct loop_data loop_data;
	double step, step_dir;
	int samples, size;
	int vol, vol_l, vol_r, voc, usmp, i, istep_dir;
	int prev_l, prev_r = 0;
	int32 *buf_pos;
	MIXER_FP  mix_fn;
	const MIXER_FP *mixerset;
	const int tail = s->ticksize - (off + count);	/* frames of the tick after this part */

	switch (s->interp) {
	case XMP_INTERP_NEAREST:
		mixerset = libxmp_nearest_mixers;
		break;
	case XMP_INTERP_LINEAR:
		mixerset = libxmp_linear_mixers;
		break;
	case XMP_INTERP_SPLINE:
		mixerset = libxmp_spline_mixers;
		break;
	default:
		mixerset = libxmp_linear_mixers;
	}

#ifdef LIBXMP_PAULA_SIMULATOR
	if (p->flags & XMP_FLAGS_A500) {
		if (IS_AMIGA_MOD()) {
			if (p->filter) {
				mixerset = libxmp_a500led_mixers;
			} else {
				mixerset = libxmp_a500_mixers;
			}
		}
	}
#endif

#ifndef LIBXMP_CORE_DISABLE_IT
	/* OpenMPT Bidi-Loops.it: "In Impulse Tracker's software
	 * mixer, ping-pong loops are shortened by one sample."
	 */
	if (job == NULL) {	/* jobs: set by libxmp_mixer_part_split */
		s->bidir_adjust = IS_PLAYER_MODE_IT() ? 1 : 0;
	}
#endif

	for (i = 0; i < (job ? job->nvoc : p->virt.maxvoc); i++) {
		int c5spd, rampsize, delta_l, delta_r;
		int ended = 0;

		voc = job ? job->voc[i] : i;
		vi = &p->virt.voice_array[voc];

		if (vi->flags & ANTICLICK) {
			if (s->interp > XMP_INTERP_NEAREST) {
				do_anticlick(ctx, voc, base, s->ticksize);	/* from the tick's start */
			}
			vi->flags &= ~ANTICLICK;
		}

		if (vi->chn < 0) {
			continue;
		}

		if (vi->period < 1) {
			reset_voice(ctx, job, voc, 1);
			continue;
		}

		/* Negative positions can be left over from some
		 * loop edge cases. These can be safely clamped. */
		if (libxmp_pos_lt0(vi->pos))
			vi->pos = 0.0;

		if (off == 0) {
			vi->pos0 = vi->pos;
		}

		buf_pos = base + ((~s->format & XMP_FORMAT_MONO) ? 2 * off : off);
		vol = vi->vol;

		/* Mix volume (S3M and IT) */
		if (m->mvolbase > 0 && m->mvol != m->mvolbase) {
			vol = vol * m->mvol / m->mvolbase;
		}

		if (vi->pan == PAN_SURROUND) {
			vol_l = vol * 0x80;
			vol_r = -vol * 0x80;
		} else {
			vol_l = vol * (0x80 - vi->pan);
			vol_r = vol * (0x80 + vi->pan);
		}

		/* Sample is paused - skip channel unless a new sample is queued. */
		if (vi->flags & SAMPLE_PAUSED) {
			if ((~vi->flags & SAMPLE_QUEUED) || vi->queued.smp < 0) {
				vi->flags &= ~SAMPLE_QUEUED;
				continue;
			}
			hotswap_sample(ctx, vi, voc, vi->queued.smp);
			get_current_sample(ctx, vi, &xxs, &xtra, &c5spd);
			vi->pos = vi->start;
		} else {
			get_current_sample(ctx, vi, &xxs, &xtra, &c5spd);
		}

		/* esp32-daw: once per tick, as libxmp keeps it for the tick (parts reuse it) */
		if (off == 0 && (vi->step_period != vi->period || vi->step_rate != s->freq || vi->step_c5spd != c5spd)) {
			step = C4_PERIOD * c5spd / s->freq / vi->period;
			vi->tick_step = step;
			/* what every part asked of the step: computed once for the tick */
			vi->tick_step_ok = !(step < 0.001 || step > (double)SHRT_MAX);
			vi->tick_istep = vi->tick_step_ok ? (int)(step * (1 << SMIX_SHIFT)) : 0;
			vi->step_period = vi->period;
			vi->step_rate = s->freq;
			vi->step_c5spd = c5spd;
		} else {
			step = vi->tick_step;
		}

		/* Don't allow <=0, otherwise m5v-nwlf.it crashes
		 * Extremely high values that can cause undefined float/int
		 * conversion are also possible for c5spd modules. */
		if (!vi->tick_step_ok) {	/* esp32-daw: step < 0.001 || step > SHRT_MAX */
			continue;
		}

		init_sample_wraparound(s, &loop_data, vi, xxs);

		if (off == 0) {
			rampsize = s->ticksize >> ANTICLICK_SHIFT;
			delta_l = (vol_l - vi->old_vl) / rampsize;
			delta_r = (vol_r - vi->old_vr) / rampsize;
		} else {
			rampsize = vi->ramp_left;
			delta_l = vi->ramp_dl;
			delta_r = vi->ramp_dr;
		}

		for (size = usmp = count; size > 0; ) {
			int split_noloop = 0;

			if (p->xc_data[vi->chn].split) {
				split_noloop = 1;
			}

			/* How many samples we can write before the loop break
			 * or sample end... (esp32-daw: a span that fits in
			 * whole takes all frames without a division; a part's
			 * usual 256 frames scale the step exactly) */
			if (~vi->flags & VOICE_REVERSE) {
				if (libxmp_pos_ge(vi->pos, vi->end)) {
					samples = 0;
					if (--usmp <= 0)
						break;
				} else {
					const double distance = (double)vi->end - vi->pos;
					if (libxmp_pos_dge(distance, size == 256 ? libxmp_times256(step) : step * size)) {
						samples = size;
					} else {
						double c = ceil(distance / step);
						/* ...inside the tick boundaries */
						if (c > size) {
							c = size;
						}
						samples = c;
					}
				}
				step_dir = step;
				istep_dir = vi->tick_istep;
			} else {
				/* Reverse */
				if (vi->pos <= vi->start) {
					samples = 0;
					if (--usmp <= 0)
						break;
				} else {
					const double distance = vi->pos - (double)vi->start;
					if (libxmp_pos_dge(distance, size == 256 ? libxmp_times256(step) : step * size)) {
						samples = size;
					} else {
						double c = ceil(distance / step);
						if (c > size) {
							c = size;
						}
						samples = c;
					}
				}
				step_dir = -step;
				istep_dir = -vi->tick_istep;
			}

			if (vi->vol) {
				int mix_size = samples;
				int mixer_id = vi->fidx & FIDX_FLAGMASK;

				if (~s->format & XMP_FORMAT_MONO) {
					mix_size *= 2;
				}

				/* For Hipolito's anticlick routine */
				if (samples > 0) {
					if (~s->format & XMP_FORMAT_MONO) {
						prev_l = buf_pos[mix_size - 2];
						prev_r = buf_pos[mix_size - 1];
					} else {
						prev_l = buf_pos[mix_size - 1];
					}
				} else {
					prev_r = prev_l = 0;
				}

#ifndef LIBXMP_CORE_DISABLE_IT
				/* See OpenMPT env-flt-max.it */
				if (vi->filter.cutoff >= 0xfe &&
				    vi->filter.resonance == 0) {
					mixer_id &= ~FLAG_FILTER;
				}
#endif

				mix_fn = mixerset[mixer_id];

				/* Call the output handler */
				if (samples > 0 && vi->sptr != NULL) {
					int rsize = 0;

					if (rampsize > samples) {
						rampsize -= samples;
					} else {
						rsize = samples - rampsize;
						rampsize = 0;
					}

					if (delta_l == 0 && delta_r == 0) {
						/* no need to ramp */
						rsize = samples;
					}

					if (mix_fn != NULL) {
						mix_fn(vi, buf_pos, samples,
							vol_l >> 8, vol_r >> 8, istep_dir, rsize, delta_l, delta_r);
					}

					buf_pos += mix_size;
					vi->old_vl += samples * delta_l;
					vi->old_vr += samples * delta_r;

					/* For Hipolito's anticlick routine */
					if (~s->format & XMP_FORMAT_MONO) {
						vi->sleft = buf_pos[-2] - prev_l;
						vi->sright = buf_pos[-1] - prev_r;
					} else {
						vi->sleft = buf_pos[-1] - prev_l;
					}
				}
			}

			if (samples == 256) {	/* esp32-daw: step_dir * 256, exactly */
				vi->pos += istep_dir < 0 ? -libxmp_times256(step) : libxmp_times256(step);
			} else {
				vi->pos += step_dir * samples;
			}
			size -= samples;

			/* One-shot samples do not loop. */
			if ((!has_active_loop(ctx, vi, xxs) || split_noloop) &&
			    !(vi->flags & SAMPLE_QUEUED)) {
				if (size > 0) {
					do_anticlick(ctx, voc, buf_pos, size + tail);
					set_sample_end(ctx, job, voc, 1);
					/* Next sample should ramp. */
					vol_l = vol_r = 0;
					ended = 1;
				}
				size = 0;
				continue;
			}

			/* Loop before continuing to the next channel if the
			 * tick is complete. This is particularly important
			 * for reverse loops to avoid position clamping. */
			if (size > 0 ||
			    ((~vi->flags & VOICE_REVERSE) && libxmp_pos_ge(vi->pos, vi->end)) ||
			     ((vi->flags & VOICE_REVERSE) && vi->pos <= vi->start)) {
				if (vi->flags & SAMPLE_QUEUED) {
					/* Protracker sample swap */
					do_anticlick(ctx, voc, buf_pos, size + tail);
					if (vi->queued.smp < 0 ||
					    (!has_active_loop(ctx, vi, xxs) &&
					     !(mod->xxs[vi->queued.smp].flg & XMP_SAMPLE_LOOP))) {
						/* Invalid samples and one-shots that
						 * are being replaced by one-shots
						 * (OpenMPT PTStoppedSwap.mod) stop
						 * the current sample. If the current
						 * sample is looped, it needs to be paused.
						 */
						vi->flags &= ~SAMPLE_QUEUED;
						vi->flags |= SAMPLE_PAUSED;
						set_sample_end(ctx, job, voc, 1);
						/* Next sample should ramp. */
						vol_l = vol_r = 0;
						ended = 1;
						size = 0;
						continue;
					}
					reset_sample_wraparound(&loop_data);
					hotswap_sample(ctx, vi, voc, vi->queued.smp);
					get_current_sample(ctx, vi, &xxs, &xtra, &c5spd);
					init_sample_wraparound(s, &loop_data, vi, xxs);
					vi->pos = vi->start;
					continue;
				}
				if (loop_reposition(ctx, vi, xxs, xtra)) {
					reset_sample_wraparound(&loop_data);
					init_sample_wraparound(s, &loop_data, vi, xxs);
				}
			}
		}

		reset_sample_wraparound(&loop_data);
		vi->ramp_left = rampsize;
		vi->ramp_dl = delta_l;
		vi->ramp_dr = delta_r;
		if (ended || tail <= 0) {
			vi->old_vl = vol_l;
			vi->old_vr = vol_r;
		}
	}
}

void libxmp_mixer_softmixer(struct context_data *ctx)
{
	struct mixer_data *s = &ctx->s;
	int size;

	libxmp_mixer_prepare(ctx);

	/* esp32-daw: the caller mixes this tick in parts (libxmp_mixer_softmixer_part) */
	if (s->deferred) {
		s->mixed = 0;
		return;
	}

	softmixer_voices(ctx, NULL, s->buf32, 0, s->ticksize);

	/* Render final frame */

	size = s->ticksize;
	if (~s->format & XMP_FORMAT_MONO) {
		size *= 2;
	}

	if (size > s->total_size) {
		size = s->total_size;
	}

	if (s->format & XMP_FORMAT_32BIT) {
		downmix_int_32bit((int32 *)s->buffer, s->buf32, size, s->amplify,
				s->format & XMP_FORMAT_UNSIGNED ? 0x80000000u : 0u);
	} else if (~s->format & XMP_FORMAT_8BIT) {
		downmix_int_16bit((int16 *)s->buffer, s->buf32, size, s->amplify,
				s->format & XMP_FORMAT_UNSIGNED ? 0x8000u : 0u);
	} else {
		downmix_int_8bit((int8 *)s->buffer, s->buf32, size, s->amplify,
				s->format & XMP_FORMAT_UNSIGNED ? 0x80u : 0u);
	}

	s->dtright = s->dtleft = 0;
}

/* esp32-daw: the part's frames from the second core's buffer added into buf32, leaving them zero
 * (that buffer also holds discharges reaching into later parts of the tick) */
static void end_part(struct mixer_data *s, int off, int count)
{
	if (s->b_pending) {
		const int chn = (~s->format & XMP_FORMAT_MONO) ? 2 : 1;
		int32 *a = s->buf32 + chn * off, *b = s->buf32b + chn * off;
		int n = chn * count;

		/* modulo 2^32 like the mixers' own sums: the order of the voices cannot matter */
		for (; n > 0; n--, a++, b++) {
			*a = (int32)((uint32)*a + (uint32)*b);
			*b = 0;
		}
	}
	s->mixed = off + count;
	if (s->mixed >= s->ticksize) {
		s->b_pending = 0;
	}
}

/* esp32-daw: with s->deferred set, mix the next `count` frames of the current tick into buf32
 * (no downmix); returns the frames mixed */
int libxmp_mixer_softmixer_part(struct context_data *ctx, int count)
{
	struct mixer_data *s = &ctx->s;

	if (count > s->ticksize - s->mixed) {
		count = s->ticksize - s->mixed;
	}
	if (count <= 0) {
		return 0;
	}
	softmixer_voices(ctx, NULL, s->buf32, s->mixed, count);
	end_part(s, s->mixed, count);
	s->split_valid = 0;	/* its voice resets were not the jobs' */
	return count;
}

/* esp32-daw: the next part of the tick (as libxmp_mixer_softmixer_part) as two jobs for two cores:
 * job 0 into buf32, job 1 into buf32b. A loop's wraparound writes into its sample while a voice
 * mixes (init_sample_wraparound), so all voices of one sample go to the same job; the groups are
 * spread by their estimated work, heaviest first. Returns the part's frames, or 0 (nothing
 * changed): mix it on one core, because it is small (work below min_work: frames x 4 per mono
 * voice), has a single group, or swaps a sample during the mix (ProTracker).
 * The jobs are made at a tick's first part and serve its later parts: voices only start at a
 * tick's start (the player), and the ones a part resets (libxmp_mixer_part_join) only leave. */
int libxmp_mixer_part_split(struct context_data *ctx, int count, int min_work,
			    struct libxmp_mix_split *x)
{
	struct player_data *p = &ctx->p;
	struct mixer_data *s = &ctx->s;
	struct module_data *m = &ctx->m;
	struct libxmp_mix_job *a = &x->job[0], *b = &x->job[1];
	int ng = 0, total = 0, wa = 0, wb = 0, voc, g, i, j;

	if (count > s->ticksize - s->mixed) {
		count = s->ticksize - s->mixed;
	}
	if (count <= 0 || s->buf32b == NULL || p->virt.maxvoc > SMIX_NUMVOC) {
		return 0;
	}
	if (s->mixed > 0 && s->split_valid) {
		goto jobs;
	}
	s->split_valid = 0;
	for (voc = 0; voc < p->virt.maxvoc; voc++) {
		struct mixer_voice *vi = &p->virt.voice_array[voc];
		int w = 1;

		x->group[voc] = 0xff;
		if (vi->chn < 0 && (~vi->flags & ANTICLICK)) {
			continue;
		}
		if (vi->flags & SAMPLE_QUEUED) {
			return 0;
		}
		if (vi->chn >= 0 && vi->vol) {
			w = (vi->fidx & FLAG_STEREO) ? 6 : 4;
			if (vi->fidx & FLAG_FILTER) {
				w *= 2;
			}
		}
		for (g = 0; g < ng && x->key[g] != vi->sptr; g++);
		if (g == ng) {
			x->key[g] = vi->sptr;
			x->weight[g] = 0;
			x->force_a[g] = 0;
			ng++;
		}
		x->weight[g] += w;
		/* a discharge from the tick's start after its first part: into buf32 itself, as the
		 * frames before this part are not added from buf32b again */
		if (s->mixed > 0 && (vi->flags & ANTICLICK)) {
			x->force_a[g] = 1;
		}
		x->group[voc] = g;
		total += w;
	}
	if (ng < 2) {
		return 0;
	}
	for (i = 0; i < ng; i++) {
		for (j = i; j > 0 && x->weight[x->order[j - 1]] < x->weight[i]; j--) {
			x->order[j] = x->order[j - 1];
		}
		x->order[j] = i;
	}
	for (g = 0; g < ng; g++) {
		if (x->force_a[g]) {
			x->bin[g] = 0;
			wa += x->weight[g];
		}
	}
	for (i = 0; i < ng; i++) {
		g = x->order[i];
		if (x->force_a[g]) {
			continue;
		}
		x->bin[g] = wa <= wb ? 0 : 1;
		*(wa <= wb ? &wa : &wb) += x->weight[g];
	}
	if (wb == 0) {
		return 0;
	}
	a->ctx = b->ctx = ctx;
	a->base = s->buf32;
	b->base = s->buf32b;
	a->nvoc = b->nvoc = 0;
	for (voc = 0; voc < p->virt.maxvoc; voc++) {
		if (x->group[voc] != 0xff) {
			struct libxmp_mix_job *job = x->bin[x->group[voc]] ? b : a;
			job->voc[job->nvoc++] = voc;
		}
	}
	x->total = total;
	s->split_valid = 1;
    jobs:
	if (x->total * count < min_work) {
		return 0;
	}
	a->off = b->off = s->mixed;
	a->count = b->count = count;
	a->nreset = b->nreset = 0;
#ifndef LIBXMP_CORE_DISABLE_IT
	s->bidir_adjust = IS_PLAYER_MODE_IT() ? 1 : 0;
#endif
	return count;
}

/* esp32-daw: one job of libxmp_mixer_part_split (on either core) */
void libxmp_mixer_job_run(void *arg)
{
	struct libxmp_mix_job *job = (struct libxmp_mix_job *)arg;

	softmixer_voices(job->ctx, job, job->base, job->off, job->count);
}

/* esp32-daw: after both jobs: their voice resets (in voice order, as one core makes them), the
 * second buffer's frames of the part into buf32 */
void libxmp_mixer_part_join(struct context_data *ctx, struct libxmp_mix_split *x)
{
	struct libxmp_mix_job *a = &x->job[0], *b = &x->job[1];
	int i = 0, j = 0;

	while (i < a->nreset || j < b->nreset) {
		if (j >= b->nreset || (i < a->nreset && a->reset[i] < b->reset[j])) {
			libxmp_virt_resetvoice(ctx, a->reset[i], a->reset_mute[i]);
			i++;
		} else {
			libxmp_virt_resetvoice(ctx, b->reset[j], b->reset_mute[j]);
			j++;
		}
	}
	/* A muting reset starts an anticlick discharge from the tick's start in the voice's next
	 * part (do_anticlick): into buf32, so the voice moves to job 0. It reads no sample again. */
	for (j = 0; j < b->nreset; j++) {
		if (b->reset_mute[j]) {
			for (i = 0; i < b->nvoc && b->voc[i] != b->reset[j]; i++);
			if (i < b->nvoc) {
				b->voc[i] = b->voc[--b->nvoc];
				a->voc[a->nvoc++] = b->reset[j];
			}
		}
	}
	ctx->s.b_pending = 1;
	end_part(&ctx->s, a->off, a->count);
}

void libxmp_mixer_voicepos(struct context_data *ctx, int voc, double pos, int ac)
{
	struct player_data *p = &ctx->p;
	struct module_data *m = &ctx->m;
	struct mixer_voice *vi = &p->virt.voice_array[voc];
	struct xmp_sample *xxs;
	struct extra_sample_data *xtra;

	/* Position changes e.g. retrigger make the new sample take effect
	 * if queued (OpenMPT InstrSwapRetrigger.mod). */
	if (vi->flags & SAMPLE_QUEUED) {
		vi->flags &= ~SAMPLE_QUEUED;
		if (vi->queued.smp < 0) {
			vi->flags |= SAMPLE_PAUSED;
		} else if (vi->smp != vi->queued.smp) {
			hotswap_sample(ctx, vi, voc, vi->queued.smp);
		}
		vi->flags |= SAMPLE_LOOP;
	}

	if (vi->smp < m->mod.smp) {
		xxs = &m->mod.xxs[vi->smp];
		xtra = &m->xtra[vi->smp];
	} else {
		xxs = &ctx->smix.xxs[vi->smp - m->mod.smp];
		xtra = NULL;
	}

	if (xxs->flg & XMP_SAMPLE_SYNTH) {
		return;
	}

	vi->pos = pos;

	adjust_voice_end(ctx, vi, xxs, xtra);

	if (vi->pos >= vi->end) {
		vi->pos = vi->end;
		/* Restart forward sample loops. */
		if ((~vi->flags & VOICE_REVERSE) && has_active_loop(ctx, vi, xxs))
			loop_reposition(ctx, vi, xxs, xtra);
	} else if ((vi->flags & VOICE_REVERSE) && vi->pos <= 0.1) {
		/* Hack: 0 maps to the end for reversed samples. */
		vi->pos = vi->end;
	}

	if (ac) {
		anticlick(vi);
	}
}

double libxmp_mixer_getvoicepos(struct context_data *ctx, int voc)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];
	struct xmp_sample *xxs;

	xxs = libxmp_get_sample(ctx, vi->smp);

	if (xxs->flg & XMP_SAMPLE_SYNTH) {
		return 0;
	}

	return vi->pos;
}

void libxmp_mixer_setpatch(struct context_data *ctx, int voc, int smp, int ac)
{
	struct player_data *p = &ctx->p;
#ifndef LIBXMP_CORE_DISABLE_IT
	struct module_data *m = &ctx->m;
#endif
	struct mixer_data *s = &ctx->s;
	struct mixer_voice *vi = &p->virt.voice_array[voc];
	struct xmp_sample *xxs;

	xxs = libxmp_get_sample(ctx, smp);

	vi->smp = smp;
	vi->vol = 0;
	vi->pan = 0;
	vi->flags &= ~(SAMPLE_LOOP | SAMPLE_QUEUED | SAMPLE_PAUSED | VOICE_REVERSE | VOICE_BIDIR);

	vi->fidx = 0;

	if (~s->format & XMP_FORMAT_MONO) {
		vi->fidx |= FLAG_STEREOOUT;
	}

	set_sample_end(ctx, NULL, voc, 0);

	/*mixer_setvol(ctx, voc, 0);*/

	vi->sptr = xxs->data;
	vi->fidx |= FLAG_ACTIVE;

#ifndef LIBXMP_CORE_DISABLE_IT
	if (HAS_QUIRK(QUIRK_FILTER) && s->dsp & XMP_DSP_LOWPASS) {
		vi->fidx |= FLAG_FILTER;
	}
#endif

	if (xxs->flg & XMP_SAMPLE_16BIT) {
		vi->fidx |= FLAG_16_BITS;
	}
	if (xxs->flg & XMP_SAMPLE_STEREO) {
		vi->fidx |= FLAG_STEREO;
	}

	libxmp_mixer_voicepos(ctx, voc, 0, ac);
}

/**
 * Replace the current playing sample when it reaches the end of its
 * sample loop, a la Protracker 1/2. The new sample will begin playing
 * at the start of its loop if it is looped, the start of the sample if
 * it is a one-shot, and it will not play and instead pause the channel
 * if both the original and the new sample are one-shots or if the new
 * sample is empty/invalid/-1.
 */
void libxmp_mixer_queuepatch(struct context_data *ctx, int voc, int smp)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	if (smp != vi->smp || (vi->flags & SAMPLE_PAUSED)) {
		vi->queued.smp = smp;
		vi->flags |= SAMPLE_QUEUED;
	}
}

void libxmp_mixer_setnote(struct context_data *ctx, int voc, int note)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	/* FIXME: Workaround for crash on notes that are too high
	 *        see 6nations.it (+114 transposition on instrument 16)
	 */
	if (note > 149) {
		note = 149;
	}

	vi->note = note;
	vi->period = libxmp_note_to_period_mix(note, 0);

	anticlick(vi);
}

void libxmp_mixer_setperiod(struct context_data *ctx, int voc, double period)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	vi->period = period;
}

void libxmp_mixer_setvol(struct context_data *ctx, int voc, int vol)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	if (vol == 0) {
		anticlick(vi);
	}

	vi->vol = vol;
}

void libxmp_mixer_release(struct context_data *ctx, int voc, int rel)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	if (rel) {
#ifndef LIBXMP_CORE_DISABLE_IT
		/* Cancel voice reverse when releasing an active sustain loop,
		 * unless the main loop is bidirectional. This is done both for
		 * bidirectional sustain loops and for forward sustain loops
		 * that have been reversed with MPT S9F Play Backward. */
		if (~vi->flags & VOICE_RELEASE) {
			struct xmp_sample *xxs = libxmp_get_sample(ctx, vi->smp);

			if (has_active_sustain_loop(ctx, vi, xxs) &&
			    (~xxs->flg & XMP_SAMPLE_LOOP_BIDIR))
				vi->flags &= ~VOICE_REVERSE;
		}
#endif
		vi->flags |= VOICE_RELEASE;
	} else {
		vi->flags &= ~VOICE_RELEASE;
	}
}

void libxmp_mixer_reverse(struct context_data *ctx, int voc, int rev)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	/* Don't reverse samples that have already ended */
	if (~vi->fidx & FLAG_ACTIVE) {
		return;
	}

	if (rev) {
		vi->flags |= VOICE_REVERSE;
	} else {
		vi->flags &= ~VOICE_REVERSE;
	}
}

void libxmp_mixer_seteffect(struct context_data *ctx, int voc, int type, int val)
{
#ifndef LIBXMP_CORE_DISABLE_IT
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	switch (type) {
	case DSP_EFFECT_CUTOFF:
		vi->filter.cutoff = val;
		break;
	case DSP_EFFECT_RESONANCE:
		vi->filter.resonance = val;
		break;
	case DSP_EFFECT_FILTER_A0:
		vi->filter.a0 = val;
		break;
	case DSP_EFFECT_FILTER_B0:
		vi->filter.b0 = val;
		break;
	case DSP_EFFECT_FILTER_B1:
		vi->filter.b1 = val;
		break;
	}
#endif
}

void libxmp_mixer_setpan(struct context_data *ctx, int voc, int pan)
{
	struct player_data *p = &ctx->p;
	struct mixer_voice *vi = &p->virt.voice_array[voc];

	vi->pan = pan;
}

int libxmp_mixer_numvoices(struct context_data *ctx, int num)
{
	struct mixer_data *s = &ctx->s;

	if (num > s->numvoc || num < 0) {
		return s->numvoc;
	} else {
		return num;
	}
}

int libxmp_mixer_on(struct context_data *ctx, int rate, int format, int c4rate)
{
	struct mixer_data *s = &ctx->s;
	int total_size;
	int sample_size;
	int output_chn;

	if (format & XMP_FORMAT_32BIT) {
		sample_size = 4;
	} else if (~format & XMP_FORMAT_8BIT) {
		sample_size = 2;
	} else {
		sample_size = 1;
	}

	if (format & XMP_FORMAT_MONO) {
		output_chn = 1;
	} else {
		output_chn = 2;
	}

	/* This should be equivalent to the XMP_MAX_FRAMESIZE calculation. */
	total_size = output_chn * libxmp_mixer_get_ticksize(rate,
				DEFAULT_TIME_FACTOR * 2, PAL_RATE, XMP_MIN_BPM);
	if (total_size < 0)
		goto err;

	/* These allocations were made with a fixed framesize based on the rate
	 * 49170 for a long time, so make that the minimum size for now. */
	CLAMP(total_size, 5 * 49170 * 2 / 20, XMP_MAX_FRAMESIZE);

	s->buffer = (char *) calloc(total_size, sample_size);
	if (s->buffer == NULL)
		goto err;

	s->buf32 = (int32 *) calloc(total_size, sizeof(int32));
	if (s->buf32 == NULL)
		goto err1;

	s->total_size = total_size;
	s->sample_size = sample_size;
	s->output_chn = output_chn;
	s->freq = rate;
	s->format = format;
	s->amplify = DEFAULT_AMPLIFY;
	s->mix = DEFAULT_MIX;
	/* s->pbase = C4_PERIOD * c4rate / s->freq; */
	s->interp = XMP_INTERP_LINEAR;	/* default interpolation type */
	s->dsp = XMP_DSP_LOWPASS;	/* enable filters by default */
	/* s->numvoc = SMIX_NUMVOC; */
	s->dtright = s->dtleft = 0;
	s->bidir_adjust = 0;

	return 0;

    err1:
	free(s->buffer);
	s->buffer = NULL;
    err:
	return -1;
}

void libxmp_mixer_off(struct context_data *ctx)
{
	struct mixer_data *s = &ctx->s;

	free(s->buffer);
	free(s->buf32);
	free(s->buf32b);	/* esp32-daw */
	s->buf32 = NULL;
	s->buf32b = NULL;
	s->b_pending = 0;
	s->buffer = NULL;
}
