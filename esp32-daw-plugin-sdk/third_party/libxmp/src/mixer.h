#ifndef LIBXMP_MIXER_H
#define LIBXMP_MIXER_H

#include "common.h"

#define C4_PERIOD	428.0

#define SMIX_NUMVOC	128	/* default number of softmixer voices */
#define SMIX_SHIFT	16
#define SMIX_MASK	0xffff

#define FILTER_SHIFT	22
#define ANTICLICK_SHIFT	3

#ifdef LIBXMP_PAULA_SIMULATOR
#include "paula.h"
#endif

struct mixer_voice {
	int chn;		/* channel number */
	int root;		/* */
	int note;		/* */
#define PAN_SURROUND 0x8000
	int pan;		/* */
	int vol;		/* */
	int tick_istep;		/* esp32-daw: (int)(tick_step * (1 << SMIX_SHIFT)), the mixers' step */
	double period;		/* current period */
	double pos;		/* position in sample */
	int pos0;		/* position in sample before mixing */
	int fidx;		/* mixer function index */
	int ins;		/* instrument number */
	int smp;		/* sample number */
	int start;		/* loop start */
	int end;		/* loop end */
	int act;		/* nna info & status of voice */
	int key;		/* key for DCA note check */
	int old_vl;		/* previous volume, left channel */
	int old_vr;		/* previous volume, right channel */
	int ramp_left;		/* esp32-daw: volume ramp frames left in this tick */
	int ramp_dl;		/* esp32-daw: volume ramp steps */
	int ramp_dr;
	int tick_step_ok;	/* esp32-daw: 0.001 <= tick_step <= SHRT_MAX */
	double tick_step;	/* esp32-daw: the voice's step for this tick */
	double step_period;	/* esp32-daw: step cache inputs, unchanged across steady ticks */
	int step_rate, step_c5spd;
	int sleft;		/* last left sample output, in 32bit */
	int sright;		/* last right sample output, in 32bit */
#define VOICE_RELEASE	(1 << 0)
#define ANTICLICK	(1 << 1)
#define SAMPLE_LOOP	(1 << 2)
#define VOICE_REVERSE	(1 << 3)
#define VOICE_BIDIR	(1 << 4)
#define SAMPLE_QUEUED	(1 << 5)
#define SAMPLE_PAUSED	(1 << 6)
	int flags;		/* flags */
	void *sptr;		/* sample pointer */
#ifdef LIBXMP_PAULA_SIMULATOR
	struct paula_state *paula; /* paula simulation state */
#endif

	struct {		/* Protracker queued instrument change */
		int smp;
	} queued;

#ifndef LIBXMP_CORE_DISABLE_IT
	struct {
		int r1;		/* filter variables */
		int r2;
		int l1;
		int l2;
		int a0;
		int b0;
		int b1;
		int cutoff;
		int resonance;
	} filter;
#endif
};

/* esp32-daw: the mixer's per-part position arithmetic without double operations (software on the
 * ESP32-S3), read from the bits of the IEEE 754 double: each gives exactly what the expression it
 * replaces gives. Non-finite or huge values take that expression itself. */
union libxmp_dbits {
	double d;
	uint64 u;
};

/* (int)pos, and *frac = (1 << SMIX_SHIFT) * (pos - (int)pos) truncated: exact for |pos| < 2^31 */
static inline int libxmp_pos_split(double pos, int *frac)
{
	union libxmp_dbits v;
	int e, ip, fp;
	uint64 f;

	v.d = pos;
	e = (int)((v.u >> 52) & 0x7ff) - 1023;
	if (e < -SMIX_SHIFT) {		/* |pos| < 2^-16: zero, subnormal */
		*frac = 0;
		return 0;
	}
	if (e > 30) {			/* |pos| >= 2^31, Inf, NaN */
		*frac = (1 << SMIX_SHIFT) * (pos - (int)pos);
		return (int)pos;
	}
	/* floor(|pos| * 2^16): the significand shifted to 16 bits below the point */
	f = ((v.u & (((uint64)1 << 52) - 1)) | ((uint64)1 << 52)) >> (52 - SMIX_SHIFT - e);
	ip = (int)(f >> SMIX_SHIFT);
	fp = (int)(f & SMIX_MASK);
	if (v.u >> 63) {		/* both truncate towards zero */
		ip = -ip;
		fp = -fp;
	}
	*frac = fp;
	return ip;
}

/* pos >= n */
static inline int libxmp_pos_ge(double pos, int n)
{
	union libxmp_dbits v;
	int e;

	v.d = pos;
	e = (int)((v.u >> 52) & 0x7ff) - 1023;
	if ((v.u >> 63) || n < 0 || e > 30) {	/* negative, huge, Inf, NaN */
		return pos >= n;
	}
	/* pos >= n <=> floor(pos) >= n, for pos >= 0 and an integer n */
	return (e < 0 ? 0 : (int)((((v.u & (((uint64)1 << 52) - 1)) | ((uint64)1 << 52)) >> (52 - e)))) >= n;
}

/* pos < 0.0 */
static inline int libxmp_pos_lt0(double pos)
{
	union libxmp_dbits v;

	v.d = pos;
	/* negative, not -0.0, not NaN */
	return (v.u >> 63) && (v.u << 1) != 0 && (v.u << 1) <= ((uint64)0x7ff << 53);
}

/* a >= b for finite a > 0 and b > 0 (the bit patterns order like the values); a non-finite a
 * compares as a double */
static inline int libxmp_pos_dge(double a, double b)
{
	union libxmp_dbits va, vb;

	va.d = a;
	vb.d = b;
	if (((va.u >> 52) & 0x7ff) == 0x7ff) {
		return a >= b;
	}
	return va.u >= vb.u;
}

/* x * 256 for a normal x well inside the exponent range (a step: 0.001 .. SHRT_MAX): exact */
static inline double libxmp_times256(double x)
{
	union libxmp_dbits v;

	v.d = x;
	v.u += (uint64)8 << 52;
	return v.d;
}

/* esp32-daw: one of the two halves of a part mixed on two cores. A job mixes its voices (ascending)
 * into its own tick buffer; the voice resets that touch the player's shared counters wait for
 * libxmp_mixer_part_join. */
struct libxmp_mix_job {
	struct context_data *ctx;
	int32 *base;		/* the job's tick buffer (frame 0 of the tick) */
	int off, count;		/* the part's frames */
	int nvoc, nreset;
	unsigned char voc[SMIX_NUMVOC];
	unsigned char reset[SMIX_NUMVOC];
	unsigned char reset_mute[SMIX_NUMVOC];
};

/* esp32-daw: the two jobs of a part and the scratch of their split (no stack on the audio task) */
struct libxmp_mix_split {
	struct libxmp_mix_job job[2];
	void *key[SMIX_NUMVOC];		/* a group's sample data */
	int weight[SMIX_NUMVOC];	/* a group's estimated work */
	unsigned char order[SMIX_NUMVOC], bin[SMIX_NUMVOC], force_a[SMIX_NUMVOC];
	unsigned char group[SMIX_NUMVOC];	/* a voice's group, 0xff: nothing to mix */
	int total;			/* the estimated work of a frame (all voices) */
};

LIBXMP_BEGIN_DECLS

int	libxmp_mixer_on		(struct context_data *, int, int, int);
void	libxmp_mixer_off	(struct context_data *);
void    libxmp_mixer_setvol	(struct context_data *, int, int);
void    libxmp_mixer_seteffect	(struct context_data *, int, int, int);
void    libxmp_mixer_setpan	(struct context_data *, int, int);
int	libxmp_mixer_numvoices	(struct context_data *, int);
void	libxmp_mixer_softmixer	(struct context_data *);
int	libxmp_mixer_softmixer_part	(struct context_data *, int);	/* esp32-daw */
int	libxmp_mixer_part_split	(struct context_data *, int, int, struct libxmp_mix_split *);	/* esp32-daw */
void	libxmp_mixer_job_run	(void *);	/* esp32-daw */
void	libxmp_mixer_part_join	(struct context_data *, struct libxmp_mix_split *);	/* esp32-daw */
void	libxmp_mixer_reset	(struct context_data *);
void	libxmp_mixer_setpatch	(struct context_data *, int, int, int);
void	libxmp_mixer_queuepatch	(struct context_data *, int, int);
void	libxmp_mixer_voicepos	(struct context_data *, int, double, int);
double	libxmp_mixer_getvoicepos(struct context_data *, int);
void	libxmp_mixer_setnote	(struct context_data *, int, int);
void	libxmp_mixer_setperiod	(struct context_data *, int, double);
void	libxmp_mixer_release	(struct context_data *, int, int);
void	libxmp_mixer_reverse	(struct context_data *, int, int);
int	libxmp_mixer_get_ticksize(int freq, double time_factor, double rrate, int bpm);

LIBXMP_END_DECLS

#endif /* LIBXMP_MIXER_H */
