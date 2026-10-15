# Tracker provenance

| Part | Source | Revision | License |
| --- | --- | --- | --- |
| ABI adapter, note chase and tempo following, row map and seek glue (`tracker.cpp`, `xmp_glue.*`), C library shim (`xmp_shim.*`) | original (smhgzll) | — | MIT |
| Module loaders (MOD, XM, S3M, IT), player and mixer | [libxmp](https://github.com/libxmp/libxmp) (Claudio Matsuoka, Hipolito Carraro Jr and contributors), the "lite" file set | tag `libxmp-4.7.4`, `5d4f6aa2c7dcdf8e9cd8129fe579d45b36a302da` | MIT |

## libxmp: what is used and what changed

`third_party/libxmp/` holds exactly the files libxmp's own `Makefile.lite` puts into the
libxmp-lite distribution (`lite/src/Makefile`, `lite/src/loaders/Makefile`) except `win32.c` and
`md5.c` (not compiled for this target), plus `include/xmp.h` and the license (`LICENSE`, upstream's
`docs/COPYING`). They are compiled with `LIBXMP_CORE_PLAYER` (the lite configuration: the MOD,
XM, S3M and IT loaders only, no depackers).

`third_party/patches/libxmp-esp32-daw.patch` (apply inside the upstream checkout with
`patch -p1`; reproduces `src/scan.c`, `src/mixer.c`, `src/mixer.h`, `src/mix_all.c`, `src/common.h`,
`src/player.c`, `src/player.h`, `src/virtual.c`) changes these things:

- **scan.c**: a hook (`libxmp_daw_row_hook`, NULL unless the plugin sets it around a scan) called
  for every row the main sequence's scan visits, with the row's start time (ms) and the speed,
  tempo and global volume before its events. The plugin's row map for seeking comes from libxmp's
  own flow scan (breaks, jumps, pattern loops and delays, speed and tempo changes). Before, the
  plugin played the song once with the mixer silenced to find the rows: 12 s for a 5-minute
  4-channel MOD and 26 s for a 21-channel XM on the ESP32-S3 (libxmp's per-channel period math is
  double precision, in software there); the scan takes 0.09 s / 0.30 s.
- **mixer.c / common.h**: `libxmp_mixer_prepare` carries the fraction of a sample from tick to
  tick (`tick_carry` in `struct mixer_data`): ticks average their exact length instead of being
  truncated, so a song at 121 BPM (911.16 samples per tick) no longer drifts 50 ms in 5 minutes
  against the DAW's beat grid (and against the scan's times). Ticks of a whole number of samples
  (125 BPM: 882) are unchanged.
- **mixer.c / mixer.h / common.h, deferred mixing**: with `deferred` set in `struct mixer_data`,
  `xmp_play_frame` runs the player for the tick and leaves the mix buffer cleared;
  `libxmp_mixer_softmixer_part` then mixes the tick's next frames as the plugin needs them. The
  voice loop is now `softmixer_range(off, count)`: volume ramps continue from part to part
  (`ramp_left`, `ramp_dl`, `ramp_dr`, `tick_step` in `struct mixer_voice`), anticlick discharges reach to the
  tick's end, a voice's previous volume is set when its tick is complete. Without `deferred`,
  libxmp mixes the whole tick as before (`softmixer_range(0, ticksize)` plus its downmix). Why: on
  the ESP32-S3 one tick (882 frames at 125 BPM) was mixed inside the one 256-frame block it began
  in, and a dense 32-channel XM overran that block's deadline 34 times a minute at a 30 % mean
  load. The plugin mixes in parts of 256 frames counted from the tick's start, so the parts are
  the same whichever block boundaries a tick falls on (renders stay reproducible). A voice's step
  (pitch) is computed once per tick and kept in `tick_step` for the tick's later parts, as
  libxmp keeps it for the whole tick (two double divisions per voice and part saved: on the
  ESP32-S3 doubles are software). Mixing in parts
  rounds each voice's position again at every part (libxmp re-derives its fixed-point position
  from the double one per call): against whole-tick mixing the output differs by -59 dB of the
  signal (at most 0.0056 of full scale, a phase difference under 0.013 samples per tick) on a
  real 4-channel MOD; whole-tick mixing matches unpatched libxmp's 16-bit output within its
  quantization (-76 dB).

- **player.c / player.h / mixer.c / mixer.h, exact math reuse**: each channel caches pure
  period-to-bend, note/bend-to-period and IT-filter coefficient results keyed by every input.
  NNA channel copies carry valid pure results; zeroed channels invalidate them. A bounded
  per-player period cache shares results between voices, with 128 unbent note periods warmed
  during control-thread loading using the original function on the target itself. Voices reuse the
  exact cached sample step across ticks until period, source sample rate or output rate changes.
  Interior mixer spans compare distance with step × requested frames before using divide/ceil at
  an edge. This avoids repeated software double / transcendental work on the S3 without limiting
  voices, removing IT filters, changing interpolation or approximating pitch. Fixed tick mixing
  boundaries remain unchanged.

- **virtual.c, hot mixer voice storage**: the array of at most 128 mixer voice records uses
  the shim's `dawxmp_calloc_fast` (control thread), about 23 KiB. Samples, channel/player data and
  mix buffers remain LARGE. The plugin declares a conservative 24 KiB FAST budget, checked by
  a static assertion. The DAW allocator falls back to LARGE when FAST does not fit or the request
  would leave less than 32 KiB of internal RAM at allocation time, including transient probes /
  channel replacements. This check does not reserve an arena against other tasks' allocations.
  Allocation class changes do not change samples or the mixer arithmetic.

- **mixer.c / mixer.h / common.h, two cores**: `libxmp_mixer_part_split` divides a mixer part's
  voices into two jobs that `libxmp_mixer_job_run` mixes on two cores (the host's ABI 1.3
  `parallel` service, through the glue's `dawxmp_mix_split`), job 0 into `buf32`, job 1 into a
  second tick buffer `buf32b`; `libxmp_mixer_part_join` adds the part's frames of `buf32b` into
  `buf32` (modulo 2^32, as the mixers' own sums: the output is byte-identical to one core).
  A voice's loop wraparound writes into its sample while it mixes (`init_sample_wraparound`), so
  all voices of one sample data go to the same job; groups are spread by estimated work, heaviest
  first, once per tick (voices only start at a tick's start; later parts reuse the jobs). The two
  calls that change the player's shared voice counters (`libxmp_virt_resetvoice` at a sample end
  with the RSTCHN quirk and for a voice without period) wait for the join, in voice order; a
  muted reset moves its voice to job 0, whose anticlick discharge from the tick's start lands in
  `buf32` as before. A ProTracker sample swap (`SAMPLE_QUEUED`) mixes its part on one core.
  `softmixer_range` became `softmixer_voices` (a voice list and a buffer).
- **mixer.c / mixer.h / mix_all.c, the per-part arithmetic without software doubles**: the
  ESP32-S3 computes doubles in software (tens to hundreds of cycles each); per voice and 256-frame
  part libxmp did about twenty. The integer position / fraction of a voice (`(int)pos` and
  `(1 << 16) * (pos - (int)pos)`) and its comparisons with the sample's loop end and with zero are
  read from the bits of the IEEE 754 double (`libxmp_pos_split`, `libxmp_pos_ge`,
  `libxmp_pos_lt0`; exact for |pos| < 2^31, other values take the original expression); the
  mixers' integer step, the step's range check and `step * 256` (an exponent change) are computed
  once per tick; a span that fits in whole takes its frames without the comparison's conversions.
  The 8-bit linear kernel computes `(a << 8) + (((frac >> 1) * (b - a)) >> 7)`, the same value
  as libxmp's `(a << 8) + (((frac >> 1) * ((b - a) << 8)) >> 15)` (no overflow), the kernels
  address the sample through a pointer instead of base + index, and the volume-ramp loop counts
  its own frames: 23 instead of 25 instructions per frame in the commonest kernel (8-bit mono to
  stereo) and both loops zero-overhead.
- **player.c / player.h, the player's per-tick doubles**: `xmp_get_frame_info`'s period and
  position (never read by the plugin) are not computed; instrument vibrato without depth (0 / x)
  or without sweep (a division by 4096, exact as a multiplication) needs no double division; the
  ProTracker period range (two `pow()` per channel and tick) is cached per channel and finetune.
  These are exact: libxmp's own value in every case.

What makes them usable inside a plugin is outside them:

- `tracker.cpp` can finish the current tick's final fixed mixer part in a host block without
  player work when the next host block crosses that tick. It retains the original 256-frame
  part boundaries and arithmetic; it changes when that last part is computed. This reduces
  work coinciding with the next tick's player logic without a new voice limit or audio delay.
- `xmp_shim.h`, force-included (`-include`) into every libxmp source, renames the C library
  functions libxmp calls: `malloc` / `calloc` / `realloc` / `free` go to the host allocator
  (LARGE memory except the FAST mixer voice array, only while the control thread loads, starts or ends a module; an allocation on
  the audio thread fails instead), `fopen` / `fread` / ... / `stat` fail (modules load from
  memory only), `snprintf` / `vsnprintf` use a small formatter for the loaders' tracker names,
  `time()` returns 0 (libxmp seeds its random generator with it: renders are reproducible),
  `isprint` / `isdigit` / `strncpy` / `strncmp` / `errno` are local.
- `xmp_glue.c` reads and sets libxmp's internal player state (`common.h`): the row map (the
  scan hook above) and seeking to a row with its tempo state (the order's info is overridden for
  the one reposition frame, then restored; the tick carry starts from zero). It owns the two-core
  jobs (`dawxmp_split_new`: the second tick buffer, about 88 KB at 44.1 kHz, and 2.4 KB of jobs,
  LARGE) and calls the host's parallel service for parts of at least about 8 voices x 128 frames;
  a host without it, or declining, mixes the part on the calling core.

## Formats

libxmp-lite reads MOD files signed `M.K.` (ProTracker), `xCHN` (1–9 channels, FastTracker) or
`xxCH` (10–32 channels), FastTracker 2 XM, Scream Tracker 3 S3M and Impulse Tracker IT (including
compressed samples). It does not read the other MOD variants libxmp's full build handles
(`M!K!`, StarTrekker `FLT4` / `FLT8` and AM, 15-sample Soundtracker, His Master's Noise, ICE,
Octalyser `CD61` / `CD81`, TakeTracker `TDZx`, ...): those are refused (the plugin's
`set_resource` returns `DAW_ERR_ARG`; the DAW refuses them at upload).

## Checks

- `tests/test_plugins.cpp` `testTracker`: the same two-pattern song written as MOD, XM, S3M and IT
  (`host/modgen.cpp`): length, channels, format and suggested tempo; the song plays to its end
  and then stops; a chase onto a row starts there at once and a chase inside a row starts at the
  next row (bit-identical output, at the song tempo and at twice the song tempo, also after the
  song's tempo change); a song at a tempo whose ticks are not whole samples ends within 4 samples
  of its length; the chased
  output equals the song played from the start once its channels play new notes; note-off,
  other notes' note-off, past the end, Loop song, the channel range, stereo separation; refused
  files leave nothing allocated; no allocation on the audio thread.
- `testTrackerParallel`: an IT of 16 channels on four samples (8-bit, 16-bit, ping-pong loop,
  one-shot ending inside ticks; `makeTestModule("it-dense")`) rendered with no parallel service,
  a real second thread, a host declining every third call and an older host table, chased into a
  tick, at another tempo, in blocks of 1 / 73 / 200 / 256 frames: identical output, every job
  run, nothing left allocated.
- Two-core and arithmetic changes (2026-10-15, local, not in the repository): byte-identical PCM
  against the previous build for libxmp's `test-dev/data` and a dense user IT with nearest /
  linear / spline interpolation, chases, other host tempos and odd blocks, with a real second
  thread and a declining host; clang ThreadSanitizer over 134 IT / XM files and the dense IT.
- Local corpus runs (not in the repository): libxmp's `test-dev/data` (271 MOD / XM / S3M / IT
  files: 209 load and play, 62 refused, among them every deliberately broken file and the
  unsupported MOD variants) and 960 mutated copies of them, under a guard-page allocator with
  trapping UBSan: no fault, leak or NaN.
