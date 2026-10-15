# Audio engine

`components/daw_core/src/engine.cpp`, `compile.cpp`. Portable C++17; the same code renders on the
ESP32-S3 (audio task, core 1, configured priority; hardware default 24), in QEMU and on Linux (ALSA, offline WAV).

Live project replacement stops/panics the transport, removes the previous instances and
waits for an engine command fence acknowledged **after** rendering/worker joins. The control
thread drains retired objects before instantiating the replacement. This avoids holding both
projects' DSP allocations at once. Hardware/live native owners yield through
`Session::Config::waitForAudio`; offline owners leave it empty and advance the engine themselves.
Empty FX-chain messages are skipped after teardown. Allocation/free stays on the control side.

## Format

44100 Hz, stereo, 32-bit float internally, blocks of 256 frames (5.805 ms). Output: signed 24-bit in
32-bit I2S slots (`toI2S32`, optional TPDF dither of ±1 LSB, off by default). Timing: 960 PPQN,
20–300 BPM, time signatures 1–16 over 2, 4, 8 or 16.

## Real-time rules (enforced by structure, not by convention)

`render()` and everything it calls:

- never allocates or frees (plugin instances, compiled models and effect chains arrive by pointer;
  old ones leave through the retire queue);
- never takes an application mutex, logs, or touches flash, network or the project document;
  the optional platform worker uses task notifications to wake and join one preallocated job;
- has bounded work: at most 512 events per block, 256 active sequenced notes, 32 instruments,
  17 mixer tracks × 4 effects, 64 segments per block;
- survives misbehaving plugins: non-finite output is detected per block (instrument muted and reset
  with all-sound-off, effect bypassed until re-created, master clamped to ±1).

Plugins follow the SDK's rules (no allocation in `process`, memory requested up front in FAST /
LARGE classes), and the importer refuses plugin libraries that reference allocation, stdio, thread
or RTOS symbols at all (docs/PLUGIN_INTEGRATION.md).

## One block

```
render(frames)
 ├─ drain the command queue (transport, swaps, parameters, live notes)        — bounded (256)
 ├─ sequence(frames)   ticks → sample offsets, emits events into the block pool
 │    └─ segments: [block start .. loop end) [loop start .. ) ...  (fractional sample positions)
 │         per segment: pattern or song events, automation, metronome, note-offs
 ├─ processBlock
 │    1. sort events: counting sort by target, insertion sort by (offset, priority) — stable
 │    2. instruments: skipped when idle (no events, active_voices() == 0); NaN check;
 │       5 ms smoothed vol / pan / mute → their mixer insert; fade-out before an instrument swap
 │    3. mixer: inserts in dependency order (Kahn), each: effect chain (dry/wet mix, bypass),
 │       smoothed vol / pan, sends (pre-fader levels into other inserts), route; master last
 │    4. master → metronome click, diagnostic tone, NaN guard, safety clamp
 └─ publish: CPU (cycles / block period), meters, voices, counters → atomics
```

### Parallel instruments on the hardware

`CONFIG_DAW_PARALLEL_INSTRUMENTS` starts one persistent 6 KB task on the other core, at the
same priority as the audio task (hardware default 24, above Wi-Fi). The engine reserves 2 KB internal and 6 KB
PSRAM PCM buffers plus about 1 KB of internal scheduling state. Allocation or task creation failure retains the serial renderer; QEMU defaults
this option off. Linux `daw-host --render ... --parallel-render` tests the same worker contract.

Each block has up to two worker phases; the platform's start/join contract is unchanged.

- **Instruments.** After grouping immutable events, the engine lists the sounding slots the
  worker may take (not crossfading swaps or removals, which retire instances here). The worker
  claims them from the **end** of the slot list with an atomic compare-and-swap, rendering each
  into its own buffer (four per block: one in internal RAM, three in PSRAM). The audio core
  renders from the **front**; the two meet in the middle. Jobs below
  `Engine::Config::workerMinPermille` of the block (last block's cost; 25 ‰ on the board) stay
  on the audio core: small hand-offs cost more than they save. When the audio
  core reaches a slot the worker claimed, it waits for that slot only, then accumulates it at its
  original position.
- **Channel strips.** A slot that is the only writer of an insert track with effects (nothing
  else routed or sent there) is mixed into that track by the core that rendered it, which then
  runs the track's chain: an FM6 and its Delay run back to back on the worker. Without effects
  on the track the slot stays a plain instrument job (the mixing would only move to the worker).
- **Effect chains.** The audio core walks the mixer order as before. A track's chain (its
  effects, gain and meter) only touches its own buffer, so the worker may run it as soon as
  every source of that track (instrument slots, routes and sends of earlier tracks) has been
  accumulated: per-track counters, decremented by the audio core with release ordering, say
  when. The worker takes the ready chain the audio core will reach last, and only when the audio
  core has other work meanwhile (another free chain, or a chain it is running itself); the
  master chain stays on the audio core. This phase starts only when two chains sit at the same
  depth of the routing graph (inserts, or a delay bus beside a reverb bus): a pure series of
  buses would gain nothing but hand-offs. Routing and sends are always accumulated by the audio
  core, in the serial order.

Mix accumulation order, sample offsets, release tails and floating-point arithmetic stay
identical to the serial renderer: `tests/core/test_parallel.cpp` compares serial and threaded
output bit for bit over the dense, shared-bus, send-bus and FM6 fixtures in both transport
modes (also under ThreadSanitizer), and `daw-host --render` gives byte-identical WAVs with and
without `--parallel-render`. Each claimed item finishes before anything can be retired or
reused; worker publication / completion use acquire / release. Plugin instances share no
mutable state (MacroSynth's Braids noise generator is per instance since 2026-10-12; before,
MacroSynth instances were kept on the audio core), so any of them may run on either core;
`test_parallel` renders two plucked-string MacroSynths on both cores against the serial
renderer. No task, queue, plugin or PCM buffer is allocated from render. The worker
spins only while a chain it could take waits for its last
source; it returns as soon as nothing is left that the audio core has not reached.

The worker normally sleeps on its next notification. If a worker job takes at least one
audio block period, it publishes completion and then delays one RTOS tick, allowing lower
priority network/control tasks to run during overload. Startup passes the configured block
period in microseconds; no allocation is added to rendering.

For worker builds the monotonic microsecond timer measures the entire render, including the
worker join; plugin profiling still uses local core cycles. `cpu`/`renderUsAvg` include that wait: they still measure elapsed render
time against the audio deadline, **not total utilization of both CPUs**. Plugin CPU shares
measure each plugin's own cycles, so their sum may exceed the elapsed render share when jobs
overlap. `parallelInstruments` and `dspStackFree` are available in hardware status/diagnostics; `workerWait` (the audio core
waiting for worker results) and `workerBusy` (the worker rendering) are 1 s averages in % of the
block, in `diag`. A waiting core polls with a short pause (`spinPause`) between reads.
`tests/core/test_parallel.cpp` checks byte-identical concurrent rendering across overlapping
chords, shared routes, parameter changes, live replacement/removal and transport changes.

### Sequencing: sample-accurate, drift-free

The transport runs a tick clock (`tick_`, double) and a monotonic run clock (`run_`, ticks since
play). A block is cut into **segments** at loop / pattern / song ends; a segment can start at a
*fractional* sample position, so a loop boundary falls exactly where the tick clock reaches it and
loops never drift against the sample clock (`tests/core/test_sequencer.cpp`: ≤ 1 sample of error
after many loops at 20–300 BPM, including tempo changes).

Each note-on gets its sample offset from its tick inside the segment, and an entry in the
active-note table with its note-off time on the *run* clock — so a note that crosses a loop
boundary or a pattern switch still ends on time. Note-offs are emitted from the run clock.

**Note chase** (plugin ABI 1.2, for plugins with `DAW_CAP_NOTE_CHASE` only: the Tracker plugin):
when playback starts, continues after a pause, seeks, changes mode or wraps at a loop / song end,
the next segment first ends the held notes of those slots (NOTE_OFF) and then sends a NOTE_ON with
`DAW_NOTE_FLAG_CHASE` for every note of theirs the position lands inside — begun before it and
still sounding, within the clip that plays it (also from the pattern's previous repetition) —
with the part already passed in `chase_mbeats` (1/1000 quarter notes) and an active-note entry for
the rest. A note starting exactly at the position is sent as usual, not chased. Other plugins see
exactly what they saw before (`test_sequencer` `testChase`).

Events at the same sample are ordered: sequenced slide note-ons (0) → note-offs (1) → note-ons (2)
→ parameters (3). Live input (keyboard, MIDI) uses one priority (2) and keeps its arrival order,
so a fast on-off-on burst in one block cannot leave a stuck note.

### Patterns, steps, song

- **Step rows** compile to notes (`compile.cpp`): a step = 1/16 note (240 ticks), velocity, pan,
  pitch, accent, probability. Swing delays every second step by `swing × step / 2`.
- **Polymeter**: a step row shorter than its pattern loops on its own length.
- **Probability**: a deterministic hash of (note id, loop count) — the same take renders the same
  way twice (offline renders are reproducible), but varies from loop to loop.
- **Pattern mode** loops the current pattern; a *queued* pattern takes over when the current one
  ends. **Song mode** plays playlist clips (pattern clips with offset / length, automation clips)
  with an optional loop region.
- **Count-in** (bars of metronome before the transport starts) and **metronome** clicks are
  generated in the engine (accent on the bar).

### Automation

Automation clips are evaluated per segment (block rate, ≤ 5.8 ms steps): linear interpolation
between points (a per-point tension bends the segment), mapped to the target's range (plugin parameter scales: linear, log, dB, bipolar).
Targets: channel volume / pan / plugin parameter, track volume / pan, effect parameter / mix, send
level, tempo. Gain targets are additionally smoothed per sample (5 ms), so block-rate automation
does not zipper.

### Mixer

Master + 16 inserts. Each insert: up to 4 effects, volume, pan, mute / solo, a route (master or
another insert), 4 sends. The routing graph is compiled on the control task into a processing
order (Kahn's algorithm); cycles are refused there with an error, so the engine only ever sees a
valid DAG (`MixModel`, swapped atomically). Changing an effect chain sends the complete new chain
(`SetFxChain`): instances that stay keep their state wherever they move.

### Instruments

32 channel slots. Replacing an instrument (another plugin, a new sample) crossfades over 5 ms:
from the moment the replacement is installed it receives every event, so a note played right after
the change is not lost; the old instance only fades out (an instrument with no sounding voices is
swapped at once). Removing a sounding instrument fades the channel out first. Retired instances
go back to the control thread for destruction. Sequenced notes carry voice ids (note-offs target
exactly their voice); live notes match by key like a MIDI keyboard; a disconnecting client's held
notes are released.

## Status

Audio sample buffers and DSP state remain in internal RAM. The block event arrays and the
three SPSC queue payloads use PSRAM; queue indices remain internal. They are allocated on the
control thread, and render never allocates. Silent effects still run to preserve tails and
automation; only effects whose internal states are known zero skip their sample work. Mixer
tracks with actual zero output skip routing/sends. Gain/mix smoothing stops doing redundant
arithmetic once the next float step equals the current value; PCM is unchanged.

`EngineStatus` atomics, published every block: transport, position, pattern, BPM, voices, CPU
(per-mille, 1 s average), render time average / max, budget (block period), and counters:
`overruns` (render slower than real time), `eventDrops`, `noteDrops`, `nanResets`, `fxFailures`,
`cmdOverflow`, `retireOverflow`, `cpuGuardDrops`. All of them appear in the `diag` message and the
device page. On QEMU and Linux these timings describe the emulator / PC, never the ESP32-S3.
Hardware diagnostics also expose `cpuMHz`, `cpuClockSupport` and `heapInternalLargest`.
Instantiation failures appear as runtime-only `plugin.loadError` in the editor instead of
leaving an unexplained silent slot. The internal-memory reserve is 32 KiB.

## CPU guard and admission

Overload protection from the **measured** render time (not from the plugins' estimated costs, which
are unmeasured until H4):

- **Guard (audio thread).** After every block the engine updates a load figure smoothed over ~8
  blocks (render time / block period, x1000). Above `Engine::Config::cpuGuardPermille` (board:
  900, `CONFIG_DAW_CPU_GUARD_PERMILLE`) new note-ons are skipped until the load is 100 below the
  threshold again; note-offs, slides (they retune a sounding voice), parameters and everything
  else pass. Fewer new voices is the gentlest way to shed load: sounding notes finish, nothing
  clicks. Skipped notes are counted (`cpuGuardDrops`, `st.cpuSkips`); the browser's load meter
  reads "CPU FULL" for 3 s after one.
- **Admission (control thread).** `channel.add`, `fx.add` and re-enabling an effect are refused
  with `cpu` while the 1 s load is above `Session::Config::cpuAdmitPermille` (board: 750,
  `CONFIG_DAW_CPU_ADMIT_PERMILLE`). Replacing an instrument, removing or disabling things and
  lowering a polyphony stay allowed, since they are how a user gets out of an overload.
- Project replacement resets the load window and guard hysteresis on the audio thread after
  stopping/panicking and removing old instances. The live owner's render fence completes before
  the new project is acknowledged: immediate MIDI `channel.add` requests use the new project's
  load instead of the previous song's one-second average. Boot before audio starts does not wait;
  offline owners consume the reset on their first render. Lifetime error counters and the worst
  render are retained.
- Both are off under QEMU (emulated time) and in offline renders; `daw-host` uses the board's
  values (`--cpu-guard`, `--cpu-admit`; `--test-hooks` adds `POST /api/test/cpu-load` for tests,
  which adds a simulated load to every measurement).

Not covered: a single note-on that is expensive by itself (it is admitted while the load is
below the threshold), and projects loaded while already over budget (they load; the guard then
limits their new notes). The thresholds are first guesses to be checked in H4.

## Tests

`tests/core/test_sequencer.cpp` (tempo accuracy, loop wraps, swing, polymeter, piano roll + slide
ordering, pattern queue, transport, tempo changes, song clips, automation, probability, overload), `test_render.cpp` (offline render of the demo project
with reference statistics: the same numbers on Linux and on the Xtensa build in QEMU),
`test_session.cpp` (session ↔ engine integration, live note bursts, stuck-note prevention), and the
plugin SDK's own tests.
