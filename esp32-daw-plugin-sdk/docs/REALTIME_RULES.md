# Real-time rules for process()

The audio thread on the ESP32-S3 renders a 256-frame block every 5.8 ms on core 1, shared by every
instrument and effect in the project. A plugin that blocks for a millisecond costs everyone.

In `process()` (and anything it calls):

- no allocation or freeing (`malloc`, `new`, STL containers that grow, `host->alloc`);
- no locks, no waiting, no system calls, no logging (`host->log`), no file / network access;
- bounded work per sample and per event; no unbounded loops over data that users can grow;
- no exceptions, RTTI or thread-safe static initialisation (`-fno-*` enforced by the build);
- handle every event at its `offset`; release voices on NOTE_OFF / ALL_NOTES_OFF; stop sound on
  ALL_SOUND_OFF within the block;
- keep state finite: flush denormals and NaN (`dsp::sanitize`), clamp feedback paths;
- report problems with `host->report(code)` (lock-free counter), never by blocking.

ABI 1.3's optional `host->parallel` is an explicit host-owned DSP service: it runs two independent
bounded callbacks and joins them before returning, or declines without invoking either. The host
owns the persistent worker and completion wait. Plugins still create no threads or application
locks; each callback follows all the rules above. Check the host table size / pointer and retain
the serial fallback (docs/ABI.md).

Enforced automatically:

| Check | Where |
| --- | --- |
| allocation / free / log during process() | `dawplug validate` (host counts calls) |
| forbidden external symbols (malloc, new, printf, pthread, FreeRTOS, esp_*, __cxa_throw, guards) | `tools/package.py`, DAW importer |
| silence without notes, sound starting exactly at the event offset | `dawplug validate` |
| no stuck voices 10 s after note-off / ALL_NOTES_OFF; silence after ALL_SOUND_OFF | `dawplug validate` |
| finite output with all parameters at min / max / random; NaN parameter values rejected | `dawplug validate` |
| deterministic output for identical input | `dawplug validate` |
| no leaks after destroy (20 create/destroy cycles) | `dawplug validate` |

Cost tips for the ESP32-S3 (single-precision FPU, no hardware divide, 240 MHz): avoid `/` and
libm in per-sample code (precompute reciprocals; `dsp::fastDiv` where the divisor varies: every
`/` is a `__divsf3` call there; `dsp::sinTurns`, `dsp::fastExp2`), update filter
coefficients at a control rate (16 samples), keep per-voice state compact in FAST memory and large
buffers in LARGE memory, and let idle voices / pieces skip processing entirely.
