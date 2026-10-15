# SoundFont provenance

| Part | Source | Revision | License |
| --- | --- | --- | --- |
| ABI adapter, host-allocator bridge (TSF_MALLOC / REALLOC / FREE), event mapping, parameters, instant all-sound-off | original (smhgzll) | — | MIT |
| SoundFont 2 parser and voice engine (`tsf.h`) | [TinySoundFont](https://github.com/schellingb/TinySoundFont) (Bernhard Schelling; based on SFZero by Steve Folta) | `853a0a171759f1ddba0de1442133a75912bbeffa` | MIT |

## Changes against TinySoundFont

`third_party/patches/tinysoundfont-esp32-daw.patch` (apply inside the upstream checkout with `patch -p1 --binary`, reproduces
`third_party/TinySoundFont/tsf.h`): the voice render loop in single precision — the ESP32-S3 FPU
has no double precision:

- playback position as an integer sample index plus a float fraction (a float index would lose its
  fraction after 2^16 samples of the font's sample buffer);
- pitch ratio in float, computed with `powf` once per 64-sample block (was double `pow`);
- the per-voice low-pass filter in float (was double), its coefficients with `tanf`.
- optional `TSF_SHORT_SAMPLES`: borrow aligned PCM16 from an in-memory ABI resource; other
  streams and unaligned sources use a PCM16 copy. Default float mode remains available.
  The host must keep the raw resource alive until the font closes. The DAW's resource owners
  and the SDK CLI's resource bindings enforce this lifetime.
- split stereo output directly into the host's two buffers, removing the adapter's temporary
  2 KB FAST buffer.
- sample runs (2026-10-13): within a 64-sample block, the samples that can reach neither the loop
  end nor the sample end (the position moves by at most n × pitch ratio plus the fraction and
  rounding; kept 4 samples away) run in a loop without the per-sample end / loop tests and the
  output-mode switch, with and without the voice low-pass. Same arithmetic in the same order: the
  output is bit-identical. The samples next to a loop wrap or the sample end take the original
  per-sample path. With TimGM6mb, 96.4 % of voice samples ran in runs.
- the exponential envelope step `powf(slope, n)` is cached per segment and block length instead
  of being computed every 64 samples (the same value: bit-identical).
- voice stealing (2026-10-13): a note that finds no free voice takes the voice furthest into its
  release (upstream), else the voice that has played longest; upstream dropped the note then.
  Never a voice of the same note. The plugin's Voices parameter limits TinySoundFont to the first
  n of its 24 preallocated voices (`voiceNum`), silencing the others; layered GM presets (five
  samples per note for TimGM6mb's String Ensemble 1) and long releases otherwise keep all 24
  voices busy with four held notes.
- voices are allocated in LARGE memory with the metadata (2026-10-13; they were FAST): the voice
  loop keeps a voice's position, fraction and filter state in locals and touches the voice once
  per 64-sample block. 24 voices took 5.4 KB of the ESP32-S3's internal RAM per channel; a GM
  song with eight or more SoundFont channels ran out of the internal RAM the DAW keeps free.

Against the preceding float-pool adapter, five catalog reference renders differ by at most
4 units of 24-bit PCM (RMS difference 8.02e-8 full scale). A held note in a three-second font,
including sample positions beyond 130,000 and a loop wrap, differs by at most 2 units.
For that 264,864 B font, plugin-owned LARGE memory fell 534,616 B → 400 B; the shared raw font
is counted separately. FAST memory increased 2,112 B → 5,448 B on the 64-bit host because hot
voices then lived internally (moved to LARGE on 2026-10-13, see above). Aligned borrowing, unaligned copying, detach and real-time
allocation checks pass. `docs/PERFORMANCE.md` distinguishes host and board measurements.

Checked against unpatched upstream: the same font (a 3 s sample, positions beyond 130 000) and
note sequence render with a maximum difference of 2.3e-5 (−93 dB relative to the signal).

Sample runs and the envelope cache were checked against the previous loop with the TimGM6mb GM
font (GPL-2.0, not distributed here): all 128 melodic programs and nine drum kits, notes from key
21 to 108, a held chord with pitch bend sweeps, sustain pedal and drum rolls, 6 s each: all 137
renders are byte-identical (raw float output, host build with `-ffp-contract=off`, the flag used
for the ESP32-S3 too). `tests/test_plugins.cpp` pins two hashes recorded with the previous loop
(a looping test font without and with the low-pass, high notes, pitch bend).

Remaining double precision (reviewed, `doubleMath` in plugin.json): at note-on (pitch ratio and
filter Q, about 20 operations per voice) and two operations per voice per block (the position is
kept as a double between calls). Loading a font runs on the control thread.

Not used: TinySoundFont's file I/O (`TSF_NO_STDIO`) and its OGG (SF3) support.


## MIDI stream adapter (2026-10-15)

Original MIT adapter work, not a new TinySoundFont vendor patch: ABI 1.4 timed bank/program,
14-bit bend, common CC/RPN 0/1/2, sustain/sostenuto/soft pedal, pressure/modulation default vibrato
and documented universal/GS/XG global packets. Per-voice region copies and controller state
are allocated in LARGE memory before playback and freed with the font. Shared font regions
are immutable. Held voices retain their original preset; host note IDs prevent a late note-off
from ending a newer stolen/same-key voice. No render allocation, flash read, new voice ceiling,
change to the existing sample kernel or reduction in audio quality. Old, unflagged events keep
the previous path. Unknown vendor/effect/NRPN commands are kept by the DAW, not emulated here.
Focused tests check actual frequency, held-note gain, pedals, split SysEx, program continuity,
private modulation and resource/RT allocation accounting; docs/MIDI.md in the DAW lists coverage.
