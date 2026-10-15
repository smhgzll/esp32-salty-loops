# Known issues

Status 2026-10-14. Numbered so code comments can refer to them (`KNOWN_ISSUES KI-n`).

## Verification gaps

**KI-7 — Hardware coverage is still partial.** On 2026-10-11 the real ESP32-S3 passed boot,
octal PSRAM checks, I2S initialization, STA Wi-Fi, HTTP / WebSocket / Chromium startup and
firmware updates with exact project restoration. Short kick / hi-hat playback and real per-instance CPU
measurements and the complete 136-case catalog matrix are recorded in docs/PERFORMANCE.md.
Live 4/6/8-track projects also pass; dense sustained chains exceed capacity. Analog output from
the PCM5102A, 30-minute playback, AP / fallback, flash-stall durations and thermal behaviour still need the
remaining checks in docs/HARDWARE_TESTS.md.

**KI-8 — CPU protection is measured, not predicted.** The CPU guard skips new notes above 90 %
render load and admission refuses new instruments / effects above 75 % (docs/AUDIO_ENGINE.md), both
from the measured load. A single expensive note-on is admitted when the load is below the
threshold, and a project loaded over budget still loads. Note skipping was observed with the
unoptimized analog kick and hi-hat; their optimizations removed skips in the measured runs.
The 136-case hardware matrix confirms that FM6 at eight voices and dense Filter/EQ plus
Delay/Reverb chains can overrun even with the guard. A dropped-note average is not usable
capacity; the live fixtures stay below it. Predictive admission / full threshold calibration
remain pending (H4). The second-core worker fixes the measured overlapping-pad / simultaneous
three-synth failures; it does not make arbitrary dense effects fit. Plugin cost estimates in the
catalog are still uncalibrated for admission; the worker uses previous cycles to select overlap.
Fixed on 2026-10-13: project replacement previously retained the old one-second CPU average,
so an immediate MIDI import could refuse its first `channel.add` in an empty project. The audio
thread now resets the load window/guard hysteresis and completes a render fence before the live
owner acknowledges replacement. Lifetime error counters remain intact; thresholds are unchanged.

## Emulator / toolchain

**KI-1 — QEMU reads stdin.** A non-interactive QEMU (CI, background) must not inherit a terminal:
`QEMU_LOG=file scripts/run-qemu.sh` redirects stdin from /dev/null.

**KI-2 — QEMU multi-threaded TCG crashes the emulator.** `qemu-system-xtensa` 9.2.2 occasionally
segfaults (the host process, not the firmware) under MTTCG, mostly around flash writes. Tests run
single-threaded TCG; the self-test retries once on an emulator crash only.

**KI-3 — Octal PSRAM in QEMU.** Octal PSRAM data accesses crash QEMU 9.2.2. The QEMU profile uses
quad mode (same 16 MB, same memory map); octal mode is only exercised on hardware (KI-7).

**KI-4 — ESP-IDF 5.5 httpd does not call the URI handler for the WebSocket handshake.** Origin
and client-limit checks run in `ws_pre_handshake_cb`, client registration in `ws_post_handshake_cb`
(`CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`, `..._POST_...`).

**KI-5 — `httpd_ws_send_frame_async` ignores partial sends.** Large frames to a slow socket arrived
corrupted. The server writes WebSocket frames with its own complete-write sender, from one flush
job per round over per-client FIFOs.

**KI-6 — Flash writes from PSRAM buffers are slow.** The flash driver copies non-internal sources
in small pieces; the project store bounces through a 4 KB internal buffer instead.

## Product limitations (by design for now)

**KI-9 — Flash writes stall audio on the ESP32-S3.** Saving / deleting a project is refused while
playing; the autosave waits until the transport stops; updates stop the transport. A diagnostic
tone running during a save will drop out (docs/STORAGE.md). Observed on the board with a dense
tracker song: one render block of 24-41 ms within the first minute after every restart, and in
some stop → play transitions (a play 0.3 s after a stop, after project changes), with up to 5 DMA
underruns. Consistent with a flash write (the autosave starts when the transport stops) still
running when playback restarts; not yet traced (docs/PERFORMANCE.md, "Dense IT on two cores").

**KI-10 — Plain HTTP, editing without login.** See docs/SECURITY.md: passwords and tokens are not
encrypted on the network; any same-origin browser on the network can edit projects; project export
is unauthenticated.

**KI-11 — Undo is per browser.** Each browser undoes its own edits by restoring the entity it saw;
an undo overwrites another client's later edit of the same pattern row / clip list.

**KI-12 — Not implemented yet:** USB-MIDI device mode, staged full-device backups, audio recording
(no audio input on this hardware).

**KI-13 — Plugin set.** Eighteen plugins are linked: instruments Sine, 3xOSC, FM6, DrumLab, Acid303, MacroSynth, Sampler, SoundFont;
effects Reverb, Delay, Chorus, Flanger, EQ, Filter, Compressor, Limiter, Distortion, Utility.
OPL3 is planned (ROADMAP.md). FM6 (six sines per voice and
sample) is likely the most expensive instrument; it defaults to 4 voices until measured. Acid303's CPU cost on the S3 is unmeasured
(its 2x-oversampled ladder is an expensive voice; KI-7, KI-8). MacroSynth's cost depends on the
model (Braids ran one voice at 96 kHz on a 72 MHz Cortex-M3); its catalog figure of 150 ‰ per voice
is a conservative estimate, and it defaults to 2 voices until measured.

**KI-17 — Sample library limits.** No compaction (fragmentation can leave the largest free run
below the free total); names ≤ 39 characters. SoundFont uploads over 1 MB stream to flash;
other uploads are buffered whole in PSRAM. The tested 32 MB board has a 22 MB library.
Playback still needs RAM: decoded samples need 2 × (16-bit) or 1.33 × (24-bit) their file size of PSRAM; aligned SoundFont PCM16 is borrowed from the retained file bytes, with no float
sample-pool copy (unaligned sources copy PCM16). Large WAV decode still needs its raw bytes
and decoded float data at the same time; streamed decode is a remaining memory improvement.
Fixed 2026-10-14: a warm TimGM6mb → Phoenix MT-32 load could abort allocating its 6.3 MB raw
file despite 16 MB free in total. File buffers now survive project changes and can be reused only
after the last resource owner retires. Nearby font sizes share allocation headroom (≤ 1 MiB);
unused buffers yield to memory pressure. Insufficient contiguous or total memory gives a
`resources` reply and keeps the previous channel binding. This does not make every combination
of large fonts fit in 16 MB of PSRAM. Planned MIDI imports and project loads now retain only the
selected presets and their sample dependencies; the full source stays in flash. Manual preset
changes prepare missing samples on the control thread. Bank/program automation and legacy
bindings without any selection fields use the whole font. Refresh cached browser pages after
updating to use selective imports. Large selections can still exceed available memory.
The SoundFont editor lists at most 1024 presets of a font; presets with banks above 16383 or programs
above 127 are not listed (the player cannot select them).

**KI-18 — MIDI recording.** Pattern mode only (song mode plays clips and does not record);
timing at audio-block resolution (5.8 ms) before the input grid; recorded notes are not part of a
browser's undo history (delete them in the piano roll).

**KI-19 — MIDI file import.** MIDI 1.0 channel/system/SysEx messages and metadata are now
retained with timing (2026-10-15; docs/MIDI.md). SoundFont supports timed programs, 14-bit bend,
common CC/RPN, pedals and pressure/modulation; fixed presets keep their selected instrument.
Vendor patches, unassigned CC/NRPN, device effects/transport commands and later grid meters are
preserved without full synthesis/emulation; the dialog reports the distinction. Original MIDI
files must be reimported to recover previously discarded data. Format 2, SMPTE and RIFF MIDI
remain explicit file-format errors. MIDI projects require schema 2; legacy projects stay schema 1.
Slots store projects deflated
(2026-10-13): 128 KB hold roughly 500 KB of JSON (20,000+ notes); a larger import plays but cannot
be saved, and the autosave keeps the previous project; the dialog shows the estimate. GM drum keys
DrumLab has no piece for are dropped on DrumLab (a SoundFont keeps them). Every SoundFont channel
keeps its own index of the project's selected preset set (the samples are shared). Whole-font
TimGM6mb used about 350 KB per channel; selective Phoenix MT-32 used about 38 KB per channel in
the 15-part Alphaville test. There is no universal 18-channel RAM limit: the stale warning was
removed. The import still merges parts above 16 by default, the channel limit is 32, and the
device reserves 2 MiB of PSRAM for project/protocol work. A SoundFont part plays
at most its Voices (the import gives it a share by its notes); when a note finds none free it
takes the oldest one, so long releases end sooner. The import's DSP estimate for SoundFont parts is
conservative (61–74 % estimated against 41–56 % measured on three songs), so it may give parts
fewer voices than they could have. The overdriven / distortion
guitar settings (Distortion on the insert) were chosen by reasoning, not by ear against the GM
originals. The DSP-load estimate in the
dialog comes from a handful of board measurements (docs/PERFORMANCE.md, "MIDI import load");
other presets and denser songs can differ, and the CPU guard skips notes when the device runs out
of time.

**KI-20 — Tracker modules** (docs/TRACKER.md). libxmp-lite reads MOD (`M.K.`, `xCHN`, `xxCH`),
XM, S3M and IT only: `M!K!`, StarTrekker `FLT4` / `FLT8`, 15-sample Soundtracker and other MOD
variants, and packed module files, are refused at upload. A song is one note in one pattern, so
it must fit in 1000 bars at its tempo. Starting inside a song waits for the next row start (at
most one row of silence) and does not restore notes still sounding from earlier rows, effect
memory or channel volumes; a chase into the second pass of a pattern loop plays that loop again.
The player runs at each tracker tick's start (every channel's effects, in software double
precision on the S3) and the mixing is spread over the blocks in 256-frame parts (patched into
libxmp): songs on one Tracker channel ran without a late block on the board (12–34 %), but every
DAW channel of a split song runs its own player with its own copy of the samples, and their tick
starts land in the same block: a 4-channel MOD split 4 ways was late for 55 blocks a minute, a
21-channel XM in 3 groups for 142 (no DMA underrun in those 60 s runs). Mixing in parts rounds
voice positions per part: against libxmp's whole-tick mixing the output differs by -59 dB of the
signal (inaudible phase differences under 0.013 samples per tick). A dense IT (50 channels, up to
127 voices) mixes on both cores and played its heaviest sections at 42-45 % without DMA underruns
(61-64 % with reverb), but individual blocks there still overrun their deadline (the player's
tick, up to 3.7 ms, runs on one core).

**KI-14 — Automation is evaluated per block** (5.8 ms steps). Gains are smoothed per sample, so
volume / pan automation does not zipper; plugin parameters receive block-rate values (plugins
smooth their own parameters).

**KI-15 — Web MIDI.** Not available in iOS Safari and needs a permission in other browsers; the
on-screen keyboard and `scripts/midi-bridge.py` (Linux) are the alternatives.

**KI-16 — Browser coverage.** The automated UI tests run Chromium (desktop and mobile emulation);
Firefox and Safari / iOS have only been reasoned about, not tested.
