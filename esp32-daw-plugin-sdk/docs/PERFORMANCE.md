# Performance

## What is measured where

| Number | Source | Valid for |
| --- | --- | --- |
| `dawplug bench` µs per block | this Linux machine | ranking plugins, regressions |
| descriptor `cpu_permille_*` | estimates, not yet calibrated against hardware | catalog hints; admission uses measured total render load |
| ESP32-S3 render time per plugin | DAW diagnostics on the real board | measured for selected drums / effects below; full 136-case matrix measured; see the DAW report |

QEMU timing is not CPU timing and is never used here.

## Dense IT: two cores and exact arithmetic (2026-10-15)

The same user IT (`ac-ootb.it`, one Tracker channel, all 50 song channels, linear interpolation,
44.1 kHz / 256 frames) measured on the board in 12 s windows, now also at its heaviest sections
(150 s and 268 s: 90-120 of the 128 voices; the earlier window at 180 s has about 55). Whole-render
CPU, mean / maximum one-second, and DMA underruns per window:

| build | 268 s dry | 150 s dry | 180 s dry | 268 s with reverb | 150 s with reverb |
|---|---|---|---|---|---|
| before (DAW boot 121) | 76.4 / 99.1 %, 6 | 72.4 / 97.3 %, 11 | 48.3 / 58.9 %, 0 | 92.6 / 115.4 %, 93 | 90.8 / 118 %, 77 |
| two-core mixing | 49.3 / 63.8 %, 0 | 51.1 / 67.6 %, 0 | 33.3 / 41.8 %, 0 | 69.0 / 83.8 %, 0 | 65.3 / 82.1 %, 0 |
| + exact arithmetic (final) | 43.6 / 55.9 %, 0 | 44.9 / 58.9 %, 0 | 29.5 / 35.7 %, 0 | 62.9 / 74.9 %, 0 | 61.7 / 74.1 %, 0 |
| deployed (DAW boot 128) | 44.0 / 56.8 %, 0 | 42.2 / 59.0 %, 0 | 31.2 / 36.1 %, 0 | 63.8 / 73.4 %, 0 | 61.1 / 76.3 %, 0 |

Deadline overruns in the 268 s window: 490 → 36 dry, 773 → 188 with reverb (absorbed by the
DMA ring). What changed (PROVENANCE.md): a mixer part's voices are split between two cores by
sample through the ABI 1.3 `parallel` service (the two halves were within 1 % of each other on
the board); a voice's per-part position arithmetic is read from the double's bits instead of
software double operations; the 8-bit kernel needs 23 instead of 25 instructions per frame; the
player skips the frame info's doubles and caches pure results. A temporary profile at 268 s:
tracker on the audio core 469 → 421 ms per second of audio between the two-core build and the
final one (player 113 → 105, local mixing 316 → 275). Output byte-identical to the previous
build (272 files x three interpolations, chases, tempos, odd blocks, real second thread,
declining host); clang ThreadSanitizer clean. DAW evidence:
`docs/performance/2026-10-15-ac-ootb-two-core.json`. No QEMU.

## Dense IT follow-up: scheduling / reverb (2026-10-15)

The next board pass tried all proposed areas on `ac-ootb.it`. Same controlled 12 s window chased
from 180 s, one Tracker channel, all song channels, linear interpolation, 44.1 kHz / 256 frames.
Whole-render CPU (mean one-second diagnostics excluding the first publication): **47.83 → 44.88 %**
dry, **64.38 → 57.93 %** with the user's Light reverb. Overruns: **62 → 17** dry, **243 → 78** wet.
No DMA underruns / audioLate / WS drop deltas in the final two windows. Short block overruns
remain; short-run variation prevents treating these numbers as a guarantee for the entire song.

Kept: Tracker computes a tick's last fixed mixer part in a block without player work when the
next block crosses the tick, retaining its exact part boundaries; Reverb processes each delay
line contiguously in bounded 64-frame chunks and optionally runs independent left/right networks
through the ABI 1.3 host parallel service. Absent / busy / nested workers retain the serial path.
Rejected: common linear-mixer unrolling / an exact-zero branch increased board load; an extra
8 KiB FAST tick buffer did not establish enough gain and was removed. Temporary profiling was
also removed. Samples remain in PSRAM; the earlier FAST mixer voice array remains unchanged.

Final native Tracker PCM: **4,851,000 stereo frames byte-identical** across five windows / four
formats. Full and Light reverb: **1,323,000 frames each byte-identical** to the previous samplewise
implementation, including a real persistent worker, odd block sizes, event splits and freeze.
Focused tests: Tracker **230**, effects **127** (including parallel refusal, older host tables and
in-place buffers). DAW evidence: `docs/performance/2026-10-15-ac-ootb-followup.json`. No QEMU.

## Dense IT, exact calculation reuse (2026-10-15)

The DAW's short board comparison of `ac-ootb.it` (one Tracker instance, all tracker channels,
linear interpolation, 44.1 kHz / 256 frames, 15 s chased from 180 s) measured **66.49 → 47.21 %**
mean whole-render CPU, maximum one-second CPU **91.5 → 71.2 %**, and deadline overruns
**627 → 105**. With its existing Light reverb enabled, the optimized version measured **60.62 %**
mean / **87.5 %** maximum and **317** overruns. All three windows had zero DMA underruns and
`audioLate` deltas. Remaining short block spikes prevent a claim of glitch-free full-song audio.

The vendor patch caches exact pitch / bend and IT-filter results by all inputs, shares and warms
exact unbent periods on the control thread, reuses unchanged sample steps across ticks, and skips
divide/ceil for sample spans wholly inside their bounds. About 23 KiB of hot mixer voice records
requests FAST memory (24 KiB descriptor budget); samples and the other player data remain LARGE.
The DAW falls back to LARGE when FAST allocation would cross its internal-RAM allowance.
All voices, note tails, filters, interpolation, sample rate and 256-frame mix boundaries remain
as before. Native before/after renders are byte-identical for **4,851,000 stereo float frames**
over four formats and five windows; focused Tracker tests pass **230 checks**. The DAW report is
`docs/performance/2026-10-15-ac-ootb-optimization.json`. No QEMU or full-song soak test for this change.

## Tracker (libxmp) (2026-10-14)

Measured on the ESP32-S3 in the DAW (its docs/PERFORMANCE.md, "Tracker modules"): the plugin's
own share of the render was 6.5 % for a 4-channel MOD, 17.2 % for a 21-channel XM and 28.4 % for
a dense synthetic 32-channel XM (60 s each, no late block, deferred mixing in 256-frame parts).
Loading (row map from libxmp's scan): 0.06–0.30 s on the board. What changed on the way, all in
`third_party/patches/libxmp-esp32-daw.patch`:

- the row map came from a silent playthrough (12 s / 26 s on the board), now from libxmp's scan;
- one tick was mixed inside one 256-frame block (34 late blocks a minute for the dense XM), now in
  parts as the blocks need them; parts of 128 frames cost 38.5 % for the dense XM (every part
  repeats the voices' setup in software doubles), parts of 256 with the step kept per tick 28.4 %.

The descriptor's `cpu_permille_base` / `_voice` (2 % + 2 % per playing song) remain estimates for
the engine's first-block scheduling only; the measured shares above depend on the song.

## SoundFont voice loop and voices (2026-10-13)

GM songs imported with the TimGM6mb font skipped notes on the board. On the board (DAW report,
2026-10-13) a GM drum part held its 24 voices at 36.7 % of a block, and a nine-part song could
not add its eighth SoundFont channel (each held 24 voices, 5.4 KB, in internal RAM). Counting
TinySoundFont's voices per part of real songs (functional, on Linux) showed why: TimGM6mb's
String Ensemble 1 plays five samples per note and releases for 2.6 s, so a part with four held
notes keeps all 24 voices busy; upstream TinySoundFont then dropped new notes.

- **Sample runs**: the samples of a 64-sample block that can reach neither the loop end nor the
  sample end run without the per-sample end / loop tests and the output-mode switch. With
  TimGM6mb 96.4 % of all voice samples ran in runs. The ESP32-S3 inner loop is 26 instructions
  without and 34 with the voice low-pass, under a zero-overhead `loop` (previously one loop of
  38 instructions with five taken branches and two loads of `tsf` fields per sample).
- The exponential envelope step `powf(slope, 64)` is computed once per segment instead of every
  64 samples.
- Both keep the arithmetic and its order: 137 renders (all 128 TimGM6mb programs and nine drum
  kits, 6 s each: scale across the keyboard, held chord with pitch bend, sustain pedal, rolls) are
  byte-identical to the previous loop.
- Voices moved to LARGE memory (the loop touches a voice once per block); a Voices parameter
  (1–24) and oldest-voice stealing (a new note is never dropped). Five of the 137 renders differ
  from the first sustained note that found all 24 voices busy (sustain pedal, 4.4–4.6 s): there
  the old loop dropped the note.

On the board (DAW report, "GM songs with TimGM6mb"): one TimGM6mb String Ensemble voice costs
about 1.0 % of a 256-frame block (24 / 16 / 12 voices: 23.6 / 16.1 / 12.5 %); GM songs of 9–15
SoundFont parts ran at 33–37 % mean / 41–56 % maximum without a skipped note. The previous loop
was measured only on other presets (a drum kit at 24 voices: 36.7 %), so this is not a controlled
A/B.

## 3xOSC guitars (2026-10-12/13)

The measured E1M1 arrangement used two 3xOSC guitars with six/eight-voice limits, Acid303 bass,
DrumLab and Full Reverb. The optimized plugin is installed on the board. The final
180 s Full window measured 72.9 % mean / 85.8 % maximum one-second elapsed render load,
with 406 guard skips, 146 overruns and zero DMA underruns. Guitar shares were 24.1 / 23.4 %;
the earlier 30 s baseline reported 30.7 / 26.0 %. These windows cover different song positions
and missing notes depress failing averages: they do not establish a precise speedup or
skip-free capacity. A user-approved 180 s Light Reverb trial measured 66.9 / 83.5 % with
172 guard skips, one overrun and zero DMA underruns. Full was restored because skips persisted.
[Board reports and all intermediate failures](../../esp32-s3-daw/docs/PERFORMANCE.md#3xosc-guitars-2026-10-1213).

The implementation keeps presets, polyphony, envelope times, control period and sample rate:

- Render oscillators into 16-sample stack tiles; dispatch waveforms once per tile. The
  three-saw and square/saw/sine combinations mix in one loop. Noise retains its original
  sample/oscillator/voice order, including muted noise oscillators; all phases still advance.
- Use `dsp::fastDiv` for this plugin's PolyBLEP normalization. Xtensa sample kernels have
  no `__divsf3` call; pitch/coefficient setup keeps the original arithmetic.
- Cache the SVF's clamped frequency, avoiding repeated identical `tanf` work above the limit.
- Dispatch envelope stages once per tile and retain local filter, envelope, pan and fade
  state in registers. Only sample kernels allow Xtensa multiply/add contraction and use IRAM.

Against SDK `3ca80c6`, 1,572 native cases (125 waveform combinations, four filter modes,
three sample rates, fragmented blocks, event offsets, slides, voice stealing and releases)
produce 21,675,784 bit-identical stereo frames. The final 100 s native E1M1 WAV is also
byte-identical. Compiling the Xtensa arithmetic path on the host gives maximum error
1.5 PCM24 units and aggregate error/signal -139.54 dB; this checks arithmetic, not board
sound or timing. Global multiply/add contraction was rejected because it changed pitch
rounding and accumulated phase error (maximum 0.001575 full scale); contraction is confined
to the sample kernels. [Arithmetic evidence](performance/2026-10-12-threeosc-pcm.json).

The packaged sample kernels/literals occupy 6,191 B before linker relaxation (previously
6,304 B); the voice kernel stack frame is 384 B. Instance memory is unchanged: 1,744 B on
Xtensa / 1,752 B on this native host. SDK ABI, 350 DSP / 1,137 plugin checks and 19 validators
pass. The tile-only prototype was slower for Super Saw on this host (22.69 → 26.85 µs/block);
host benchmarks are not ESP32 capacity measurements.

## Drums, reverb, MacroSynth, distortion (2026-10-12)

Measured in the DAW on the board with an imported rock song (DrumLab, two MacroSynth guitars,
3xOSC bass) and a Reverb on the drums, which was skipping notes (DAW PERFORMANCE.md, "Drums and
reverb"):

- DrumLab: on the ESP32-S3 every float division is a libgcc call (`__divsf3`, about 25 dependent
  FPU instructions) and `sinf` a call with range reduction, which is why its pieces cost 5-17 %
  each on the board but little on the host. Polynomial sines, `FastDiv` (a reciprocal seed from the bits and three
  Newton steps), inline filters and no per-sample `memcpy`: **32.9 → 24.0 %** in the song.
  Host renders stay within 1-8 LSB of 24-bit PCM.
- Reverb: the network is bound by PSRAM traffic (25 lines read and written every sample). A
  line-major rewrite with the kernel in IRAM was byte-identical but no faster on the board
  (21.5 → 21.7 %) and was reverted. New `quality` parameter: **Light** runs the network at half
  the sample rate between 7-tap half-band filters, **21.9 → 13.9 %**; the wet signal loses
  content above about 8 kHz (-4 dB at 8-10 kHz, -10 dB at 12-16 kHz); its level and the dry
  signal are unchanged. Full stays the default and is byte-identical to before.
- MacroSynth: the noise generator state moved from a static into the instance (byte-identical
  for one instance), so the DAW no longer keeps MacroSynth instances on one core.
- Distortion: the ADAA quotient uses `dsp::fastDiv` (no `__divsf3` call per sample and channel).

## FM6 operator-loop pass (2026-10-12)

The user's full six-operator Electric Piano reached four voices alongside drums, bass and
active Delay/Limiter. In the actual Song loop, FM6's real-board share fell **54.68 → 37.58 %**;
total elapsed DAW render load fell **77.20 → 64.95 %**. The final 120 s window has zero
overruns, DMA underruns, guard skips and all other error counters. Before, a 30 s window had
194 overruns, five underruns and 20 skipped notes. The DAW's performance report retains
the settings and unsuccessful intermediate variants; dropped notes depress failing averages.

FM6 now uses 16-sample operator tiles and 512 B stack scratch, keeping event offsets, envelopes,
carrier-sum order and all 32 algorithms. Coupled feedback groups in algorithms 4/6 stay
sample-ordered. Its interpolated quarter-wave table fuses wrapping into integer cell coordinates:
1,028 B read-only internal data shared by all instances, with no per-instance storage increase.
Only the sample kernel uses IRAM. No sample-rate, polyphony or envelope-time reduction.

Tiling alone produced **384 byte-identical** reference renders (six presets × 32 algorithms ×
two feedback settings, with vibrato): [raw comparison](performance/2026-10-12-fm6-pcm.json).
The final lookup keeps the **1e-5 absolute sine-error** bound for normalized, negative and
multi-turn phases; it changes rounding and is not universally byte-identical. The real 24 s
song's maximum difference is **9.54e-7** full scale, relative RMS **-113.87 dB**. Separate
Electric Piano/Bell/Brass/Organ comparisons are measured in
[lookup/audio evidence](performance/2026-10-12-fm6-lookup-audio.json). High feedback can
amplify tiny numerical differences. Final serial/parallel DAW song output remains identical.

SDK ABI, **350 DSP / 754 plugin checks**, all 19 plugin validators and reference renders pass.
All 32 algorithms are tested across 17/256-frame processing, with algorithm changes and
note events at arbitrary offsets; release clears the voices. Four-voice Electric Piano's
native benchmark fell about **80.27 → 55.06 µs** per block on this machine, with no RT
allocation/logging violations. Native percentages do not estimate ESP32 capacity.

## Host benchmarks (2026-10-11, `scripts/benchmark.sh`)

See `build-host/bench.jsonl` after running the script; results are machine-specific and are not
committed as reference values. Selected ESP32-S3 measurements are below; the remaining hardware
checklist is in the DAW's docs/HARDWARE_TESTS.md. Every ESP32 CPU figure in descriptors remains
an estimate until the full matrix is calibrated.

## ESP32-S3 selected instances (2026-10-11)

ESP32-S3 rev v0.2, 240 MHz, 32 MB OPI DTR flash / 16 MB octal PSRAM at 80 MHz, STA Wi-Fi.
DAW block size 256 at 44.1 kHz (5,804 µs reported budget). The user's unchanged 8-channel project
plays four Kick 2 (Tight) and four Closed Hat hits per 16-step pattern at 128 BPM. Master Limiter
and Lead Delay are enabled. Means exclude the first three diagnostic frames of a 75 s run.

| instance | before optimization | measured after |
|---|---|---|
| DrumLab Closed Hat | 76.2 % | 10.2 % |
| DrumLab Kick 2 (Tight) | ~45.1 % before the earlier kick fix | 17.6 % |
| Master Limiter | 7.3 % | 3.5 % |
| Lead Delay, empty line / silent input | 7.8 % | 0.8 % |

Percentages are shares of one audio-core block period, not of both cores. Effect cost depends
on the input: the Delay number is for known-empty lines, not an active echo. Total DAW render
load fell 120.9 % → 41.6 %, stopped load 31.6 % → 9.8 %; all ten error counters stayed zero.
Full details, settings and deployed firmware hash are in `esp32-s3-daw/docs/PERFORMANCE.md`.

Optimizations preserve the algorithms: parameter coefficient caching in DaisySP kicks / hats,
monotonic window minimum in Limiter, and skipping PSRAM access only when the Delay's lines /
filter states are known zero. Both snare models also cache coefficients / filter settings,
mode gains and static modulation factors. Twelve kick reference renders plus twenty-two
hat / snare / effect renders matched previous 24-bit PCM byte for byte. SDK ABI, 345 DSP and
626 plugin checks pass.

## ESP32-S3 code size (xtensa GCC 14.2, -O2)

`scripts/build-esp32.sh` prints the total; at the 0.1.0 set (8 plugins) it is ~55 KB of
code + read-only data.

## Catalog pass — 2026-10-11

All 18 firmware plugins were measured on the ESP32-S3; the SDK's extra example Tremolo is
included in the 19-plugin native catalog. `scripts/profile-catalog.py` runs **241 audible**
preset, enum/model and polyphony benchmarks with actual WAV/SF2 bindings. Every benchmark
reports zero real-time allocation/logging violations. Raw host data:
[241 cases](performance/2026-10-11-host-catalog.json).

The hardware matrix has 136 cases (all 9 DrumLab modes/8 kit pieces, MacroSynth's 48 models,
FM6's 32 algorithms, 3xOSC's five waves and supported 4/8/max voices). 129 pass; FM6 at eight
voices and six dense sustained arrangements exceed capacity. Three heavy MacroSynth models
additionally pass at four voices. Every instrument and effect's default case passes. The DAW's
`docs/PERFORMANCE.md` contains the per-plugin table, exact firmware hashes and raw hardware
reports. Host timings do not estimate ESP32 percentages.

Changes: `-O3 -fno-math-errno` without fast-math, unit-phase sine/floor operations, 3xOSC/SVF
coefficient caching, idle Filter/EQ and pristine Chorus/Flanger/Reverb paths, Acid303 pure-saw
branch skipping, halfband loop unrolling, mono Sampler filtering once, and SoundFont borrowed
PCM16/direct split output with preallocated internal voices. Existing drum coefficient caching,
Limiter deque and Delay empty-line suspension remain. Compressor, Distortion and Utility were
fully audited/profiled; their new optimization is the common compiler flags. All tail and
finite-value guards remain.

236/241 reference WAVs are byte-identical. Five SoundFont renders differ by at most four
24-bit PCM units (RMS error 8.02e-8 full scale). Requested-voice benchmarks use that polyphony;
reference WAVs for voice entries use the standard chord sequence.
[Comparison](performance/2026-10-11-audio-comparison.json). A held-note/loop test beyond sample
130,000 differs by at most two units. For that 264,864 B font, LARGE usage falls 534,616 B →
400 B (raw shared font excluded), and native FAST usage rises 2,112 B → 5,448 B.
[Large-font evidence](performance/2026-10-11-large-font.json).

The all-render IRAM and transposed Reverb experiments were discarded after hardware tests:
they consumed internal RAM without a useful timing gain. Final Reverb needs no additional
block scratch. TinySoundFont's refreshed patch applies byte-exactly to its pinned upstream
CRLF header. ABI 1.1, 345 DSP, 626 plugin checks and validation × 19 pass.
