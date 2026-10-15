# Performance

**MIDI expression update (2026-10-15):** boot 129, 240 MHz, OTA valid. Timed MIDI controls and
SoundFont program changes now play; unknown commands remain data (docs/MIDI.md). Legacy renders
of all 19 installed plugins remained byte-identical across 670,320 stereo frames. The physical
MIDI import/reload/seek check had zero new audio/error counter deltas. The user's seven-channel
SoundFont project also passed short before/after playback; no speedup claim from those windows.
Evidence: [2026-10-15-midi-expression.json](performance/2026-10-15-midi-expression.json).

**Latest: Tracker mixes on both cores; `ac-ootb.it`'s heaviest sections from 72-76 % to 42-45 % dry and
from 91-93 % to 61-64 % with reverb, DMA underruns from 6-93 per 12 s window to none.** Same PCM.
**Before that: three 260 MHz Wi-Fi recovery attempts failed; APB clock independently measured at 86.7 MHz.**
Earlier 320 / 300 / 280 MHz transitions reset; the original 260 MHz run reduced DSP load but lost Wi-Fi/web.
The temporary trials returned to 240 MHz automatically. The retained software optimizations below
remain in the normal firmware.
**Latest optimization: a second pass on `ac-ootb.it` measured 45 % mean dry load and 58 % with Light reverb.**
Tick-tail scheduling and parallel reverb preserve the tested audio exactly. Short board windows still contain deadline
overruns; this is not a claim of glitch-free playback through the whole song.
Tracker modules (MOD / XM / S3M / IT) play through the Tracker plugin
(libxmp); earlier one-channel songs of 4–32 tracker channels measured 12–34 % (below).
**Before that: selective SF2 loading and Saragossa import complete on the board.** Only the project's
selected presets and samples are retained in PSRAM. Fragmented browser operations are joined
before parsing. Saragossa and Alphaville import completely; their controlled 60 s / 30 s playback
windows have zero audio error deltas. Earlier measurements remain below; QEMU timing is never
reported as ESP32 CPU usage.

## Dense IT on two cores, exact arithmetic (2026-10-15)

The user's project as found: `ac-ootb.it` / **Out of The Blue** on one Tracker channel (all 50 song
channels, linear interpolation, song tempo 252), its Bright Plate reverb disabled; 240 MHz,
44.1 kHz / 256-frame blocks. Twelve-second windows chased to 180 s (the earlier reports' window,
about 55 voices) and to the song's heaviest sections, found by scanning the whole song on the host:
150 s and 268 s (90-120 of libxmp's 128 voices). The project was never replaced; transport and
reverb were restored after every run. CPU is the whole render's mean / maximum one-second value;
underruns and overruns are counter deltas over each window.

| build | 268 s | 150 s | 180 s |
|---|---:|---:|---:|
| before (boot 121), dry | 76.4 / 99.1 %, 6 underruns, 490 overruns | 72.4 / 97.3 %, 11 / 407 | 48.3 / 58.9 %, 0 / 13 |
| before, reverb on | 92.6 / 115.4 %, 93 / 773 | 90.8 / 118 %, 77 / 711 | 62.3 / 75.4 %, 0 / 115 |
| two-core mixing (boot 122), dry | 49.3 / 63.8 %, 0 / 66 | 51.1 / 67.6 %, 0 / 67 | 33.3 / 41.8 %, 0 / 3 |
| two-core mixing, reverb on | 69.0 / 83.8 %, 0 / 294 | 65.3 / 82.1 %, 0 / 203 | 48.3 / 59.6 %, 0 / 19 |
| **final DSP (boot 127), dry** | **43.6 / 55.9 %, 0 / 36** | **44.9 / 58.9 %, 0 / 29** | **29.5 / 35.7 %, 0 / 2** |
| **final DSP, reverb on** | **62.9 / 74.9 %, 0 / 188** | **61.7 / 74.1 %, 0 / 129** | **47.9 / 54.4 %, 0 / 10** |

The earlier 180 s window under-stated the song: its heaviest sections dropped audio before this
change even without reverb. Reverb itself is unchanged (14.3 % of the core).

What changed (SDK `plugins/tracker/PROVENANCE.md`):

- **Two cores.** A host scan of the song found 61 audible voices on average (127 at most), no IT
  filter, 14 distinct samples per tick and 8-bit mono samples for about 95 % of the mixed frames.
  Each 256-frame mixer part is now split in two jobs, grouped by sample (libxmp writes a loop's
  wraparound into the sample while a voice mixes), the second running on the DSP worker through
  the ABI 1.3 parallel service into its own tick buffer; the buffers are added after both finish.
  The two halves measured within 1 % of each other. Voice resets that touch the player's shared
  counters wait for the join; a ProTracker sample swap mixes on one core. The worker was idle
  during the instrument render of a one-instrument project (the engine's own two-core scheduling
  starts at two sounding instruments).
- **No software doubles per part.** The S3 emulates doubles; libxmp did about twenty per voice
  and part. Positions are split and compared from the double's bits, step values are computed
  once per tick; the 8-bit kernel takes 23 instead of 25 instructions per frame; the player skips
  the frame info's doubles and caches pure results; the jobs are made once per tick.

A temporary cycle profile at 268 s (removed again), per second of audio on the audio core: the
plugin 469 → 421 ms between the two-core build and the final one; player 113 → 105 ms (one block
per tick: up to 3.7 ms), local mixing 316 → 275 ms (225 ms in the sample kernel), job split 21 ms
→ made once per tick. Remaining serial work: the player (about 10 % of the core in these
sections) and the engine outside the plugin (about 6 %).

Same PCM: byte-identical to the boot-121 plugin for 272 files (libxmp's test-dev corpus and this
song) with nearest / linear / spline interpolation, chases, other host tempos and odd block sizes,
with a real second thread and with a host declining every third call; clang ThreadSanitizer found
no race (the song and 134 IT / XM files). SDK checks **1442** (a new two-core Tracker test), DAW
host tests pass. No QEMU.

Seen in every boot of these builds and of the earlier ones (boots 103 / 106 / 109 / 111): one
render block of 24-41 ms within the first minute after a restart, and in some stop → play
transitions of the measurement (a play 0.3 s after a stop), with up to 5 DMA underruns at that
moment. Inside the measured windows no audio was lost. Suspected: a flash write (the autosave
starts when the transport stops) stalling the code that runs from flash (KI-9); not traced here.

The deployed image differs from boot 127 only in the Tracker's declared LARGE memory estimate
(160 → 256 KB, for the second tick buffer): at boot **128** it measured 44.0 / 56.8 % (268 s),
42.2 / 59.0 % (150 s) and 31.2 / 36.1 % (180 s) dry, 63.8 / 73.4 % and 61.1 / 76.3 % with reverb,
no DMA underrun. Image **1,751,072 B** (+6 KB: the mixers' inlined position code), SHA-256
`48bf349999d9f3dfe7edf6834940f421f7b4a7cbfdb0212722a7dd5e308e419b`, valid; the user's project,
library, transport and disabled reverb as found. Evidence:
[2026-10-15-ac-ootb-two-core.json](performance/2026-10-15-ac-ootb-two-core.json).

## Try Wi-Fi at a genuinely measured 260 MHz (2026-10-15)

The user asked to recover Wi-Fi at 260 MHz. Three short physical-board attempts tested
different startup sequences, with a successful 240 MHz control. No voltage increase, QEMU,
voice cap or quality reduction. Wi-Fi configuration stayed in RAM. Every request flushed
autosave at stock clock, closed old live sockets to avoid stale WS send retries blocking HTTP,
and temporarily suppressed the normal reconnect/AP-fallback handler. RTC traces, an independent
slow-clock watchdog and explicit return to PLL 480/CPU 240 covered network loss; no OC at boot.

| method | CPU measured after Wi-Fi start | result |
|---|---:|---|
| stop Wi-Fi at 240, confirm STA_STOP, switch260, start/connect | 260.137 MHz | reason201, no IP, scan finds 0 APs |
| stop at 240, switch260, fully deinit/init/configure/start/connect | 260.050 MHz | same failure; all API calls succeed |
| stop/start260, with doubled flash/PSRAM dividers | 259.972 MHz | same failure; RAM canary passes, scan finds 0 APs |
| **control: same full deinit/init flow at 240** | **239.976 MHz** | **IP after 3.441 s; HTTP and WS work while playing** |

CPU frequency was measured with CCOUNT against XTAL-driven SYSTIMER before/after Wi-Fi
initialization and once per second, independently of ROM cached MHz. Wi-Fi initialization did
**not** silently undo the overclock. All high-clock API calls returned ESP_OK; the driver then
reported **WIFI_REASON_NO_AP_FOUND (201)**, and an explicit active scan completed with zero
networks. Initial reason8 and later reason36 were deliberate stop/disconnect operations.
No high-clock HTTP or WS replies arrived. Control ping replies verify ICMP was available;
at 240 the control served **12 HTTP status responses and 11 WS diagnostics** during the 12 s song
window. It used the same module, credentials, station interface and current Full/Bright Plate
reverb settings, chasing the heavy `ac-ootb.it` section from 180 s.

The three failed260 cases kept the song **stopped**. Excluding the initial stale/partial render
publications, idle render load was about 17% with normal memory and 20% with slower memory;
sampled overruns, DMA underruns and guard-drop counters did not increase in these windows.
The 240 control also had zero new DMA underruns/guard drops. Existing counters before triggering
are retained in the evidence and must not be attributed to the Wi-Fi test. A 64 KiB PSRAM
write/read canary passed in each attempt; this is not a complete memory-stability test.

The final experiment also measured **APB hardware timing**, rather than trusting its nominal
software value. A GPTimer was configured at stock on APB 80 with resolution 1 MHz (prescaler 80).
Its raw counter delta was compared with XTAL SYSTIMER over 2 ms:

| phase | measured APB estimate |
|---|---:|
| boot, CPU 240 | 80.000 MHz |
| after Wi-Fi start, CPU 260 | **86.670 MHz** |
| restored CPU 240 | 80.000 MHz |

Flash/SPI0/SPI1 and PSRAM divider registers changed from **0x00010001 to0x00030103** in that
attempt (divide 2 to divide 4, nominal 160 MHz MSPI core). Their intended rate with PLL 520 was
about 43.3 MHz; memory pin clocks were not measured directly. The lower memory clocks did not
restore networking. I2S used XTAL in all temporary builds, keeping the audio rate independent
of BBPLL. The existing CPU guard was disabled only in these temporary builds; production
configuration was restored exactly afterward.

Espressif's [S3 TRM clock diagram and Wi-Fi requirements, pp529-533](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf#page=529)
route APB to Wi-Fi and require the PLL to be active for Wi-Fi. The independently observed
peripheral-clock increase strongly supports shared-clock incompatibility with this custom PLL
method. It does not separately identify every internal PHY clock/failure mechanism or prove
that every possible firmware method is impossible. **These attempts did not produce working
Wi-Fi at 260 MHz**, so no overclock was retained. No undocumented RF-register changes were tried.

Trial boots 114→115 (stop/start),115→116 (full reinit),116→117 (240 control),118→119 (lower
memory clocks). All returns were deliberate software resets, with the current project/library
matching fresh backups after each reboot and transport restored. Normal production sources,
network event handling and config were restored exactly; temporary endpoints/probes removed.
Final normal firmware: boot **120**, OTA valid, **240 MHz**, I2S, playback resumed; image
**1,744,704 B**, SHA-256 `aeb4cf35f85ffc01ada487e243bdd42da141e0965a1e832a052e481add963542`.
HTTP, ping and project/library comparisons passed after restoration; temporary POST route
returns405 through the normal GET wildcard fallback and experiment diagnostic fields are absent.
Evidence: [260 MHz Wi-Fi board runs](performance/2026-10-15-260-wifi-trials.json).
Unapplied patches: [restart/reinit harness](performance/2026-10-15-260-wifi-trials.patch),
[lower-memory/APB-measurement variant](performance/2026-10-15-260-wifi-slow-apb.patch).

## Higher clocks, tested from the top down (2026-10-15)

At the user's request, tried **320 → 300 → 280 MHz** on the physical board, stock voltage.
Each temporary, authenticated request armed an RTC slow-clock watchdog before the switch;
the firmware never overclocked at boot. A 2 ms CCOUNT/XTAL measurement, integer-compute canary
and 64 KiB PSRAM canary gated a planned 12 s playback window of the same heavy IT section
from 180 s, with the user's Full/Bright Plate reverb. Results survive reset in RTC RAM.
I2S remained on XTAL to prevent speeding playback. The cache-off clock-switch routine and
its call/literal addresses were checked in IRAM/ROM in the built ELF.

| requested clock / memory setup | outcome | boot / reset |
|---|---|---|
| **320 MHz**, normal memory dividers | switch did not return; speed / canaries / song load not measured | 106 → 107, interrupt watchdog |
| **300 MHz**, normal memory dividers | switch did not return; no playback measurement | 107 → 108, panic |
| **280 MHz**, normal memory dividers | switch did not return; no playback measurement | 108 → 109, panic |
| **240 MHz control**, identical first harness | completed canaries and 12 s song window | 109 → 110, deliberate software restart |
| **320 MHz**, doubled flash / PSRAM dividers | switch still did not return; no confirmed high-clock measurement | 111 → 112, panic |

The 240 MHz control independently measured **239.925 MHz** and completed the same clock-switch
and recovery code. Mean / maximum published CPU, excluding the first two samples:
**65.03 / 77.9 %**; **112** deadline overruns, **zero** new DMA underruns, guard drops or NaN
resets. These are separate 12 s publications, not directly comparable to the preceding 20 s
260 MHz experiment. Web access remained available through the control window.

The normal 80 MHz flash/PSRAM dividers would also raise memory clock with the PLL; actual
high-clock pin frequencies were not measured. To investigate that coupling, one additional
320 MHz attempt doubled SPI0/SPI1 flash and SPI0 PSRAM dividers before raising the PLL, using
the IDF's inline MSPI clock operations, and restored their original registers after PLL480.
Original divider register values were **0x00010001**, MSPI core selection2 (nominal160 MHz);
with PLL640 the intended memory rate was about **53.3 MHz**. This attempt also reset before
active-register capture or a frequency/canary result. It does not isolate a CPU-only limit.
The second harness disabled the existing CPU guard during the experiment; it reached no song
window. The first harness's 240 MHz control recorded zero guard drops with the existing guard.
Normal firmware retains its previous configuration; no new voice or quality limit was added.

New crash summaries from the 300/280 MHz attempts point into cache-resume/panic paths and
identify the experimental ELF `2348941bb`. The summaries visible after the 320 MHz attempts
were older retained dumps, not evidence of a newly captured crash at those targets. The first
harness's Wi-Fi event count included deliberate reboot (control reported reason8); it cannot
diagnose Wi-Fi loss during overclock. The second restricted events to the clock window, but
no high-clock window completed. **No new conclusion about the radio's exact failure was obtained.**

All attempts recovered at 240 MHz; project and sample-library JSON matched their fresh backups
after every reboot, and prior mode / position / playback state were restored. **No usable
higher-clock performance gain was demonstrated by these trials.** The earlier measured
260 MHz result remains valid, including its web-access failure. Higher requested transitions
failing with this method do not establish that the CPU can never run at those frequencies.
Temporary code/config were removed and the original production sources restored exactly.
Final normal image: **1,744,704 B**, SHA-256
`ba2826aa93c539f51f9b8fb00496aea2fae278d5432f5d682fe6db705632abb9`;
boot **113**, OTA valid, **240 MHz**, I2S, playback resumed. The temporary POST endpoint is
absent (405 from the normal server's GET wildcard fallback); project/library checked again.

Evidence: [higher-clock board runs](performance/2026-10-15-high-clock-trials.json).
Unapplied patches: [first harness](performance/2026-10-15-high-clock-trials.patch),
[lower-memory-clock variant](performance/2026-10-15-high-clock-slow-memory.patch).
No QEMU, voltage increase, higher-than-320 MHz attempt or long stability test.

## Short physical-board overclock experiment (2026-10-15)

The user authorized a real overclock trial. The normal ESP-IDF CPU-divider API rejects clocks
above 240 MHz; this is a driver restriction, not proof that the hardware cannot be overclocked.
A temporary authenticated endpoint changed the BBPLL from 480 to 520 MHz, retaining the CPU
divide-by-two and stock voltage. It never ran at boot. Both cores/caches were parked using the
IDF flash critical section; all clock-switch instructions/literals were verified in IRAM/ROM.
Register approach: ESP-IDF's S3 clock LL, with experimental precedent in
[OpenWatchFace](https://github.com/Neol00/OpenWatchFace/blob/main/OpenWatchFace/overclock.h).

I2S used **XTAL for both baseline and trial**, so increasing the shared PLL could not speed up
the song and masquerade as a DSP improvement. Independent CCOUNT / XTAL-SYSTIMER measurement
confirmed **260.0 MHz**, and approximately **240.5 MHz** after restoration (short 2 ms probe).
ROM timing scales and the host cycle-to-time conversion followed the experimental frequency.
After 20 real seconds the device restored PLL480 / CPU240, then deliberately restarted once
to restore radio initialization too. The trace survived in RTC RAM; no flash write was needed
to capture it, and no network command was needed for recovery. Boots **103 → 104**.

Same `ac-ootb.it`, chased to 180 s, all voices/filters/interpolation retained. The user had
changed reverb to **Bright Plate / Full** before deployment: size75, damping11000, predelay0,
lowcut200, width100, mix32.2813. Both windows retained those current settings. These measurements
must not be compared directly with the preceding Light-reverb trials.

| short board window | 240 MHz | 260 MHz |
|---|---:|---:|
| mean published render load | 63.71 % | 57.57 % |
| maximum published render load | 87.6 % | 75.3 % |
| deadline overruns, approximately 20 s | 185 | 74 |
| I2S frames / XTAL second | 44,097.6 | 44,097.9 |
| new DMA underruns during measured window | 0 | 0 |
| HTTP / WebSocket access | available | unavailable |

Means omit initial partial publications (first baseline diagnostic; first two trial samples).
The trial plays for about half a second at stock clock before the switch, so publication/song
positions are slightly offset. It is one short comparison, and losing network work can also
reduce load; the roughly 10 % mean reduction is not an isolated CPU-only benchmark. The first
on-device sample and last sample delimit 20 s for the trial overrun / DMA / sample-rate deltas.
There were already **5 DMA underruns before triggering** and the same 5 throughout the trial;
those existing errors cannot be attributed to the clock change. A 64 KiB PSRAM write/read canary
passed before and after the high-clock interval; this does not establish full-memory stability.

Every HTTP probe during the trial timed out; no WS diagnostics arrived. The first successful
probe was about 30 s after triggering, following automatic reboot at stock clock. **Rejected
for normal DAW use:** a modest measured gain does not compensate for losing the web interface.
No higher clock or voltage was attempted. Production source/config were restored exactly;
the temporary endpoint and clock code are absent from normal firmware. Latest project, sample
library and transport were restored, including the user's Full reverb settings.
Final normal firmware: boot **105**, OTA **valid**, **240 MHz**, hardware I2S, playback resumed;
image **1,744,704 B**, SHA-256 `2def60c0913001fee1c57e487de8f3f5600f11496de7c2f4939b2de2f4b21cc8`.
The temporary POST route is absent (normal server returns 405 via its GET wildcard fallback).

Evidence: [board measurements](performance/2026-10-15-ac-ootb-clock-trial.json).
The [unapplied temporary patch](performance/2026-10-15-clock-trial.patch) records the experiment;
it is not a supported overclock feature. No QEMU, long soak, temperature or power measurement.

## Dense IT follow-up: scheduling and parallel reverb (2026-10-15)

The user asked to try every proposed area on the same heavy song, retaining all notes, tails,
filters and interpolation. Same ESP32-S3, 240 MHz, 44.1 kHz / 256 frames; one Tracker instance,
all tracker channels, the user's existing Light reverb settings. Each board window plays 12 s
after a row chase to 180 s; restores project / effect state and the previous transport. Mean /
maximum CPU use the one-second diagnostics after excluding the first stale / partial publication.
Overrun and DMA deltas cover the whole 12 s. These are short comparisons, not a full-song soak.

| build / candidate | mean CPU, dry / wet | deadline overruns, dry / wet | decision |
|---|---:|---:|---|
| previously deployed boot 94, baseline | 47.83 / 64.38 % | 62 / 243 | reference |
| temporary Tracker stage profiler | 48.03 / 60.18 % | 64 / 235 | removed from final build |
| add 8 KiB FAST tick buffer | 47.65 / 63.58 % | 55 / 215 | gain inconclusive; returned RAM |
| add contiguous reverb line processing | — / 63.28 % | — / 210 | kept |
| add unrolled linear kernels / exact-zero branch | 51.16 / 66.22 % | 81 / 258 | slower; reverted |
| add same-boundary tick-tail prefetch | 50.93 / 66.41 % | 21 / 104 | kept |
| add parallel left/right reverb | — / 64.55 % | — / 85 | kept |
| **selected: prefetch + blocked / parallel reverb, rejected experiments removed** | **44.88 / 57.93 %** | **17 / 78** | **deployed boot 101** |

Candidates are incremental; the final row removes the slower kernel and inconclusive FAST-buffer
experiments. Baseline and final maximum one-second CPU: dry **60.4 → 58.0 %**, wet **75.8 → 70.3 %**.
Final overrun counts fell **73 % dry / 68 % wet**; remaining individual block spikes are not fixed.
Every window had zero DMA underrun / audioLate deltas. Some intermediate windows dropped one WS
live frame; the final two windows dropped none. Short-run CPU variation is visible in the table;
these numbers do not establish a universal speedup for other songs or song positions.

The temporary stage profiler found about **2 ms** of sample mixing and **0.4–0.5 ms** of player
logic per host block on average. Player logic peaked at **3.94 ms**, and a host block sometimes
needed two mixer parts. Kept changes:

- **Tracker scheduling:** when a host block ran no player tick and the next block would cross
  the current tick, prepare its final mixer part early. Original 256-frame parts, arithmetic,
  sample positions and audio latency remain unchanged; no speculative next-tick player state.
- **Reverb locality:** process each comb / all-pass line contiguously in chunks of at most 64
  frames, preserving each feedback recurrence and each frame's sum order. Full and Light keep
  their existing topology, delay lengths, rate and filters.
- **Reverb parallelism:** optional ABI 1.3 host service shares the existing DSP task between
  independent left/right networks. Busy / nested / absent workers use the serial fallback.
  No render-time allocation or extra task; jobs finish before output is mixed. The final wet
  window recorded thousands of successful jobs and no declined job. Audio stack free: 2,520 B.

The final Tracker renders match the previous version byte for byte for **4,851,000 stereo float
frames** across MOD / XM / S3M / IT and five windows. Full and Light reverb each match the old
implementation for **1,323,000 stereo frames** with real song input, variable / odd block sizes,
parameter event splits and freeze. A real persistent native worker also produces identical PCM;
focused regression checks cover declined jobs, an older host table and in-place effects.
Checks passed: Tracker **230**, effects **127**, DAW session **383**, DAW parallel engine **289**,
ABI and hardware build. The final library/package archives match the measured builds; the
existing seven-file vendor patch still reproduces the vendored code. No QEMU.

Final boot **102**, valid, image **1,744,704 B** (package notice updates; same measured plugin
archives as boot 101). Project / library and transport were restored; latest Light settings
retained, reverb disabled as found. Live changes to room size / width / mix between the first
snapshot and baseline were retained; all comparison windows use those later settings.
Evidence: [2026-10-15-ac-ootb-followup.json](performance/2026-10-15-ac-ootb-followup.json).

## Dense IT playback, with all voices preserved (2026-10-15)

The user's `ac-ootb.it` / **Out of The Blue** exceeds the audio deadline at some positions even
without reverb. The user explicitly ruled out limiting voices or shortening their tails.
The SDK now reuses exact period/bend, note-period, IT-filter and sample-step calculations when
all their inputs are unchanged; a per-player pitch cache shares exact results and warms unbent
notes on the control thread. Mixer spans wholly inside a sample skip an unnecessary division.
The hot mixer voice array (about 23 KiB) uses FAST/internal RAM; samples, player state and mix
buffers remain LARGE/PSRAM. FAST allocation falls back to LARGE if it would cross the host's
32 KiB internal-RAM allowance at allocation time. This is not a permanently reserved arena.

Same board, 240 MHz, 44.1 kHz, 256-frame blocks, one Tracker channel, all tracker channels,
linear interpolation, unchanged IT filters and existing 128-voice libxmp ceiling. Each short
measurement starts by chasing to 180 s and plays 15 s. CPU is the whole render's mean / maximum
**one-second** window; overrun counts measure individual blocks against their deadline.

| configuration | mean CPU | maximum 1 s CPU | deadline overruns / 15 s | DMA underruns |
|---|---:|---:|---:|---:|
| original, dry | 66.49 % | 91.5 % | 627 | 0 |
| optimized, dry | 47.21 % | 71.2 % | 105 | 0 |
| optimized, existing Light reverb enabled | 60.62 % | 87.5 % | 317 | 0 |

Dry mean load fell 29 % and deadline overruns 83 %. Short block spikes remain (up to 9.87 ms
observed, against a 5.80 ms deadline), including with reverb. `audioLate` and WS drop deltas were
zero in these three windows. These are short, row-chased comparisons, not full-song tests.
The 15 s reverb test restored its previous disabled state and retained all parameter values.
Before final deployment the user had enabled Light reverb and changed its settings; those later
settings were preserved. A final 6 s check with that reverb already enabled measured 58.92 % mean /
70.6 % maximum one-second CPU, 87 overruns and zero DMA underruns / audioLate / WS drops. Its
duration and reverb settings differ from the controlled comparison above.

The final native plugin archive is byte-identical to the one used for these optimized board
measurements. Native before/after renders are byte-identical for **4,851,000 stereo float
frames**: MOD, XM, S3M and IT from their starts (20 s each), plus this IT from 180 s (30 s).
Focused SDK Tracker checks: **230**, host session checks: **383**, no RT allocation. The vendor
patch reproduces all seven changed upstream files. A 128-frame mixing experiment increased
mean CPU and produced DMA underruns with reverb; it was reverted to the original 256-frame
mixing boundaries before final deployment. No QEMU test was used for this change.

Evidence: [2026-10-15-ac-ootb-optimization.json](performance/2026-10-15-ac-ootb-optimization.json).

## Tracker modules: libxmp on the board (2026-10-14)

The Tracker plugin (libxmp 4.7.4 lite, docs/TRACKER.md) on the ESP32-S3, firmware boot 83–87,
`scripts/performance.py board --mode song --seconds 60`: each project plays one song as one note
from the song's start; the user's project was restored after every run (`restored: true`). Songs:
a 4-channel ProTracker MOD of 4:53 (the user's), a 21-channel XM of 5:03 (28 instruments, 1 MB
of samples), a synthetic dense 32-channel XM (a new note on every channel every 8 rows) and a
1-channel IT test file (libxmp's test data). Raw reports: `performance/2026-10-14-tracker-*.json`.

**Loading** (`samples.probe`: load the module, row map, start the player). First version: the
row map came from playing the song once with the mixer silenced: **12.1 s** for the MOD and
**26.4 s** for the XM on the board (libxmp's per-channel period math is double precision, in
software on the S3) — a project with such channels would have stalled the control thread for
that long per channel. Now the row map comes from a hook in libxmp's own flow scan: **0.09 s**
(MOD), **0.30 s** (XM), 0.11 s (dense XM), 0.06 s (IT).

**Playback**, mean / max CPU of the 1 s windows (the whole render: engine, mixer and the Tracker
channels; "Tracker" is the plugin's own share), late blocks (`overruns`) in 60 s:

| song, channels | whole tick per block | parts of 128 | **parts of 256 + step once per tick (deployed)** |
|---|---|---|---|
| MOD 4 ch, one Tracker channel | 12.1 / 13.7 %, Tracker 6.5 %, 0 | 13.1 / 15.3 %, 7.4 %, 0 | **12.3 / 14.0 %, Tracker 6.5 %, 0** |
| MOD 4 ch, one Tracker channel each (4) | (channel-range bug: all 4 played everything) | 28.2 / 31.5 %, 14 late | **29.2 / 34.5 %, 4 × 5.0–6.2 %, 55 late** |
| XM 21 ch, one Tracker channel | 21.0 / 25.6 %, Tracker 15.2 %, 1 late | 29.7 / 35.0 %, 23.8 %, 0 | **23.0 / 27.9 %, Tracker 17.2 %, 0** |
| XM 21 ch, groups of 8 (3) | (channel-range bug) | 50.8 / 58.6 %, 529 late | **37.9 / 44.5 %, 3 × 14.6–16.1 %, 142 late** |
| XM 32 ch dense, one Tracker channel | 29.8 / 30.3 %, Tracker 24.1 %, **34 late** | 44.3 / 44.7 %, 38.5 %, 0 | **34.2 / 34.5 %, Tracker 28.4 %, 0** |
| IT test (1 ch) | 7.9 / 8.6 %, 2.5 %, 0 | 8.0 / 8.9 %, 2.6 %, 0 | **7.8 / 8.6 %, 2.4 %, 0** |

DMA underruns, CPU-guard skips, note / event drops and NaN resets stayed at zero in every run.

- **Whole tick per block**: libxmp mixes a tick (882 frames at 125 BPM) at once, inside the one
  256-frame block where the tick starts: that block carries 3–4 blocks' worth of mixing, and the
  dense XM was late 34 times a minute at a 30 % mean.
- **Deferred mixing** (patched into libxmp, the SDK's `plugins/tracker/PROVENANCE.md`): the
  player runs at the tick's start, the mixing in parts as the blocks need them. With parts of
  128 frames every one-channel song ran without a late block, but each part repeats every voice's
  setup (software doubles on the S3): the dense XM cost 38.5 % instead of 24.1 %. Parts of 256
  frames, with the voice's step computed once per tick (as libxmp keeps it), cost 28.4 % and stay
  without late blocks.
- **Splitting a song** runs one player per channel; their tick starts (player logic for every
  tracker channel, also the muted ones) fall into the same block. The split cases are late
  55 / 142 times a minute; the dialog says so. The first runs of the split cases are void: the
  channel range was applied before the plugin knew the channel count (every instance played the
  whole song, equal peaks in the report); fixed, with a regression test.
- **Starting inside a song** (`performance/2026-10-14-tracker-chase.json`): play from bar 80 of
  the XM, then a seek to bar 20 while playing; the MOD from bar 100, then bar 3: the channel
  sounded in the first status frame each time, every error counter unchanged.
- Memory per Tracker channel (the player's copy of the samples, voices, mix buffers): 370 KB for
  the MOD (121 KB file), 1.07 MB for the XM (428 KB file), 411 KB for the dense XM. The file itself
  is held once in PSRAM in addition.

## Selective SF2 loading / Saragossa fragmented import (2026-10-14)

The user's **Saragossa Band - Dance With The Saragossa Band Nr 8 Medley.mid** disconnected
after creating its last pattern, before `clips.set`. Unlike the earlier MT-32 reset, boot did
not change. The last `notes.set` was 87,116 B, within the 256 KiB message limit. A single-frame
operation worked, but a deliberately fragmented one closed the connection on the old firmware.
Firmware now accepts TEXT + CONTINUATION frames and dispatches the joined message only after
FIN, bounded by the aggregate limit. A 100,000-byte fragmented ping receives one ACK. MIDI note
tuples omit default fields, reducing this song's largest operation to 59,576 B.

The loader now builds an ordinary compact SF2 from flash tables and only the selected presets'
sample dependencies, including stereo links, loop guards and address offsets. Overlapping PCM
ranges are copied once; indices are relocated while generators/modulators are retained. Source
CRC verification scans the whole source in 4 KiB chunks without retaining it. Every MIDI font
binding includes its initial bank/program and the complete per-font preset plan; all channels
share the same compact file. Stored projects derive the plan from their channel states. Each
plugin still indexes the entire selected set, rather than only its own one preset.

The full source font in flash and its preset list are unchanged. Manual bank/program changes
prepare missing samples on the control thread; program/bank automation retains a full font.
Legacy bindings without any selection fields retain a full font for compatibility, so reload a
cached browser page. Used file buffers are never overwritten; oversized unused buffers are
discarded. Allocation/validation failures remain recoverable and retain the previous channel.

**Both real Chromium imports completed**, with no reboot (boot 80):

| song | channels | patterns | clips | stored notes | selected presets |
|---|---:|---:|---:|---:|---:|
| Saragossa medley | 9 | 144 | 144 | 13,997 | 9 |
| Alphaville, Sounds Like A Melody | 15 | 97 | 210 | 6,333 | 14 |

Memory figures include shared file allocation capacity plus per-channel plugin LARGE allocations
(indices and voices); MB below is decimal. The whole-font reference uses the verified Phoenix
file size 6,308,938 B and 334,200 B per channel. It compares these components, not entire-project
heap usage; the original failed Saragossa project had no clips.

| song | whole-font reference | selective file + channel memory | PSRAM free during playback |
|---|---:|---:|---:|
| Saragossa | 9,316,738 B (9.32 MB) | 1,810,218 B (1.81 MB) | 13,462,356 B |
| Alphaville | 11,321,938 B (11.32 MB) | 2,098,818 B (2.10 MB) | 13,835,284 B |

The shared allocated buffer was 1,533,018 B in both windows; Alphaville reused Saragossa's buffer.
Plugin LARGE memory was 277,200 B / 565,800 B respectively, spare buffers zero. The actual
Alphaville result exceeds an earlier ~1.5 MB estimate because every channel indexes all 14
selected presets. This reduces retained memory without changing sample data or the DSP loop;
it does not imply the same proportional reduction in CPU time.

Song mode from tick 0, one-second diagnostic samples, with no user edits in either window:

| raw report | seconds | elapsed render mean / max | overruns / CPU guard / DMA underruns |
|---|---:|---:|---:|
| [Saragossa](performance/2026-10-14-sf2-selective-saragossa.json) | 60 | 36.7 / 54.3 % | 0 / 0 / 0 |
| [Alphaville](performance/2026-10-14-sf2-selective-alphaville.json) | 30 | 40.3 / 50.7 % | 0 / 0 / 0 |

All ten audio-error counter deltas were zero; project and sample library were unchanged. These
are measured windows, not complete-song stability tests. The imported voice fitting remains
enabled. Long-note, pitch-bend and unsupported-controller warnings retain their prior meaning.

Native comparison with the vendored TinySoundFont rendered all **510 presets / 47,001,600 stereo
frames bit-identically** from original and compact fonts: Phoenix MT-32, the local TimGM6mb
variant (5,994,284 B, distinct from the stored 5,969,788 B variant), and win-gm. Each preset used
low/mid/high chords, pitch wheel and release. Core tests cover shared/stereo samples, missing
preset fallback, malformed tables, manual program changes beyond the initial subset, failed
binding rollback, automation preparation and legacy binding. `test_sf2_subset` 23,
`test_session` 284, `test_samples` 150, storage 2,417; 24 web unit tests, 54 host network checks,
6 MIDI bridge checks pass. A test-client timeout bug discovered during QEMU verification is
covered separately: a partial incoming frame retains its header until the full payload arrives.
Final QEMU self-test, cross-target render MATCH and all 54 network checks pass, including the
fragmented-operation check (`QEMU_SKIP_OTA=1`; real-board OTA was verified separately below).

Final hardware image **1,621,136 B**, SHA256
`62bdb33c846d3deb919b7384137228551fb4fa8353fccbe79662291543cdbe80`, boot **81**, `ota_1`, valid.
The complete Saragossa project and sample library survived OTA; Saragossa remains on the board.
Complete local project exports are under the workspace's `backups/` directory. Raw
[import / PCM summary](performance/2026-10-14-sf2-selective-import.json).

A later [read-only follow-up](performance/2026-10-14-sf2-selective-live-followup.json) found the
user had adjusted the mixer and paused at tick 673,460 (arrangement end 675,840). Current-boot
counters then showed **2 deadline overruns / 0 DMA underruns**. The additional 20 s observation
was paused and added no errors; it is not a playback benchmark. No user settings or transport
were changed. This does not establish zero-error full-medley playback.

## MT-32 warm MIDI import / contiguous PSRAM (2026-10-14)

The user's **Alphaville - Sounds Like A Melody.mid** imported with TimGM6mb, but selecting
Phoenix MT-32 reset the board at the first `channel.resource`, before any notes were sent.
The stored font (6,308,938 B) was valid: the crash backtrace was `operator new` → byte-vector
resize → `SampleStore::read` → `Session::loadResource`. A checked-allocation trial exposed the
fragmentation: **16,238 KiB free**, but **6,016 KiB largest block**, against **6,161 KiB needed**.
The project-slot estimate (~60 KB deflated) and DSP estimate (~74 %) did not cause this reset.

Fixed in the DAW core, retaining Claude's streamed uploads and 22 MB sample partition:

- CRC-checked reads into buffers allocated through the fallible platform allocator.
- File buffers retained across project changes, overwritten only with no resource owners left;
  adjacent font sizes reserve the larger size already in the library (at most 1 MiB headroom).
  TimGM6mb → Phoenix MT-32 now reuses one allocation. Spare buffers yield to memory pressure.
- `channel.resource` validates a replacement before changing state; failures reply `resources`
  and leave the old plugin / binding intact. The 2 MiB reserve remains enforced.
- `psramLargest`, reset reason and the last stored crash summary available in hardware diagnostics.

**Two browser imports on the board completed** after warm TimGM → MT-32 changes and a return to
TimGM: **15 channels, 97 patterns, 210 clips, 6,333 stored notes**, all bound to Phoenix MT-32,
no errors or reboot (boot **79**, `ota_1`, valid). The sample library stayed unchanged.
A separate insufficient-memory trial returned `resources`, kept the project byte-for-byte and
did not reboot. Raw evidence: [import / memory failure](performance/2026-10-14-mt32-import.json).

**280 s live playback observation:** **49.1 % mean / 74.0 % maximum one-second load**;
**9 deadline overruns**, **0 CPU-guard drops**, **0 DMA underruns**, other audio error deltas zero,
no reboot, all plugin reports zero. **This fails the zero-error playback criterion.** The user
changed the master volume / loop bounds and paused at tick 410,137 (source end 414,744); the last
stopped samples are included in the mean, so this is neither a controlled benchmark nor a
complete-song stability pass. Those user edits and subsequent playback were retained.
Raw [playback observation](performance/2026-10-14-mt32-playback.json).

The device's autosave lists **187,131 B JSON → 28,456 B stored**, within the 131,008 B slot payload.
The imported project remains on the board. The original TimGM project is backed up locally.
Phoenix uses MT-32 program order, so GM program numbers can select different sounds; win-gm
or TimGM6mb remains the appropriate choice for GM mapping. Long-note/controller import warnings
remain unchanged.

Validation: core suites pass (`test_samples` 148, `test_session` 263, storage 2,417 checks),
24 web unit tests, 53 host network checks, 6 MIDI bridge checks; QEMU self-test / cross-target
render MATCH / network PASS. Hardware image **1,600,048 B**, SHA256
`1d01406f30dc3cf13646ee5dd072730dd24144505e2a43d22347b35648e8c581`.

## GM songs with TimGM6mb: SoundFont voice loop, Voices, compact projects (2026-10-13)

Imports of the user's GM pop songs (MIDI files not in the repository) failed or skipped notes.
Measured on the board, 60 s of Song mode from the start each, one-second figures from `diag`;
every error counter (guard skips, overruns, DMA underruns, late audio, event / note drops)
stayed at **zero** in every window:

| song, firmware | parts | Voices | elapsed render mean / max | busiest part |
|---|---:|---|---:|---|
| [A, faster loop](performance/2026-10-13-gm-song-a-fast-loop.json) (boot 62) | 9 | 24 each | 49.4 / 66.1 % | String Ensemble 24 voices 23.6 % |
| [A, Voices fitted](performance/2026-10-13-gm-song-a-voices-fitted.json) (boot 63) | 9 | 8–14 | 33.6 / 41.4 % | strings 12 voices 12.5 % |
| [A, Voices as given](performance/2026-10-13-gm-song-a-voices.json) (boot 63) | 9 | 8–24 | 35.3 / 44.9 % | strings 16 voices 16.1 % |
| [B](performance/2026-10-13-gm-song-b.json) (boot 65) | 15 | 6–14 | 32.8 / 40.8 % | a pad 14 voices 11.7 % |
| [C](performance/2026-10-13-gm-song-c.json) (boot 66) | 9 | 6–24 | 37.4 / 55.6 % | drums 20 voices 18.7 % |
| [D, 23 parts merged](performance/2026-10-13-gm-song-d-merged.json) (boot 67) | 11 | 6–24 | 25.1 / 38.5 % | a lead 12 voices 8.9 % |

Before (boot 61): song A's import stopped at its eighth channel ("not enough internal RAM": each
SoundFont channel held 24 voices, 5.4 KB, in internal RAM), and once the complete 221 KB project
was loaded, no client received its `welcome` (larger than the 192 KB send queue: dropped and
re-sent forever). Both fixed; with nine SoundFont channels internal RAM keeps 75 KB free.

One TimGM6mb voice now costs about 1.0 % of a block (24 / 16 / 12 voices of String Ensemble:
23.6 / 16.1 / 12.5 %). The previous loop was measured only on other presets (a GM drum kit at
its 24 voices: 36.7 %, earlier in this file), so this is not a controlled A/B; the loop's output
is byte-identical to the previous one (the SDK report). The import's estimate (`load.ts`: 1.1 %
per voice, 60 % of the Voices busy) said 61–74 % for these songs against 41–56 % measured
maxima: on the safe side, so it can give parts fewer voices than they could have.

Song D has 23 parts (four drum tracks, four muted-guitar tracks, ...). Imported as 23 SoundFont
channels (boot 66) it left 1 MB of PSRAM: no snapshot, no autosave, silent channels. Each channel
keeps its own index of the font (2,063 regions × 168 B = 338 KB for TimGM6mb). The import now
merges parts a GM synth plays with one instrument (by default above 16 parts; 23 → 11 channels:
5.3 MB of PSRAM left), and the device refuses a new instrument below 2 MB of free PSRAM.

Projects are stored deflated and written compactly (rows without content and all-off steps left
out, notes without trailing default fields, arrays as JSON text instead of cJSON nodes). Song B
(15 parts, 216 patterns) had made the device's snapshot `null`: the cJSON tree of every empty
16-step row did not fit in PSRAM next to 15 SoundFont channels (358 KB each), and the autosave
stored that `null` over the previous project. Its JSON is 176 KB now (297 KB before), 24 KB
deflated; songs A and B autosaved and came back complete after restarts (boot 63, 66). A failed
serialization is never stored any more. Final image (boot 66): 1,584,464 B, SHA-256
`3318bb8b9abce19420fc8753dd8e35ad2708a691e9f547d9ac9d301e5d44625d`.

## MIDI import: stale CPU admission and four TimGM6mb parts (2026-10-13)

Importing E1M1 immediately after an overloaded 3xOSC arrangement could fail its first
`channel.add` at the old song's 85 % load, leaving an empty/partial new project. The CPU
figure is published once per second and was retained through project replacement. This
was not a measurement of four SoundFont channels: no imported notes were playing yet.
A native regression reproduced the rejection with a simulated 95 % old load (six failed
checks before the fix).

`Session::start` now posts an audio-thread `ResetLoad` after stopping/panicking and removing
old instances. It clears the one-second accumulators, smoothed load and guard hysteresis;
the live owner's render fence completes before acknowledging the new project. Runtime
empty → empty replacement also waits; first boot before audio starts does not. Lifetime
error counters and the worst render remain available. Thresholds stay at 75 % admission /
90 % note guard; an overloaded new project is still protected. No SoundFont DSP, voice
limit, sample rate or musical setting changed in this fix.

The real-board [warm-import check](performance/2026-10-13-sf2-import-warm-import.json)
started at **76.2 %**, above the admission limit. After `project.new`, load was
**0.0 %** and all four SF2 channels were added and bound to TimGM6mb with their
original bank/program. The first add ACK took 69 ms. The backed-up project and
sample library were restored exactly, and Song playback resumed at tick 33363.

The user's completed import contains four channels, 39 patterns, 39 clips and all 2,332
stored notes (about 109 KB compact JSON, within the project slot). Programs: bank 0,
30 Distortion Guitar / 29 Overdriven Guitar / 34 Picked Bass; drums bank 128, program 0.
The user's FX 84 Reverb **Light** setting and its other parameters were preserved.

| Song window | seconds | elapsed render mean / max one-second | guard skips / overruns / DMA underruns |
|---|---:|---:|---:|
| [before admission fix](performance/2026-10-13-sf2-import-baseline.json) | 120 | 59.9 / 83.2 % | 0 / 0 / 0 |
| [fixed firmware](performance/2026-10-13-sf2-import-playback.json) | 180 | 59.9 / 83.7 % | 0 / 0 / 0 |

This is an import/admission fix, not a DSP speedup. Windows have different start positions;
the load is already essentially the same once the SF2 project is fully loaded. Every other
error-counter delta and the fixed run's startup counters are also zero. The fixed run's
worst render is 5,728 µs against 5,804 µs; this short test does not replace long-run
hardware stability checks. Per-plugin shares overlap on two cores:

| instance | share of one block | maximum sample voices |
|---|---:|---:|
| SF2 Distortion Guitar | 6.8 % | 7 |
| SF2 Overdriven Guitar | 7.7 % | 6 |
| SF2 Picked Bass | 2.0 % | 2 |
| SF2 GM Drums | 36.7 % | 24 |
| Reverb Light | 13.4 % | — |

The drum kit's sample voices reach the existing 24-voice player limit; MIDI note overlap
alone does not predict sample/release-tail cost. The two sampled guitars are much lighter
than the previous six/eight-voice 3xOSC settings. All channels share the cached SF2 file and
borrow its aligned PCM16 pool; each has its own voice state/metadata. In the fixed run,
internal heap free was 53,783 B and PSRAM free 8,802,100 B; audio/worker stack headroom
was 4,048 / 4,696 B. Different fonts/presets can cost more.

Deployed boot 57 / ota_1, OTA valid, 1,577,424 B, SHA-256
`ddb217c473728cf98dd178ce07ad66bd08c162e9e08fdd56ae44ca22bbf8a09e`. All DAW host suites pass (260 session checks),
21 web unit / 50 network / six MIDI checks; QEMU self-test, cross-target render MATCH and
network checks pass. The new regression also verifies that current-project overload still
refuses additions and lifetime counters survive replacement.

## 3xOSC guitars (2026-10-12/13)

The E1M1 guitar trial retained its six-voice Super Saw guitar, eight-voice modified Pluck guitar,
Acid303 bass, DrumLab, 44.1 kHz and all musical settings. The optimized 3xOSC is installed;
**notes still skip with both Full and Light Reverb**. Light was tested with the user's approval
and Full restored because the user's condition was to keep Light only if skips ended.

| firmware / quality | seconds | elapsed render mean / max | guard skips / overruns / DMA underruns | Super Saw / Pluck share |
|---|---:|---:|---:|---:|
| [baseline](performance/2026-10-12-threeosc-baseline.json), Full | 30 | 79.1 / 86.9 % | 182 / 56 / 0 | 30.7 / 26.0 % |
| [oscillator tiles + filter cache](performance/2026-10-12-threeosc-candidate1.json), Full | 120 | 75.2 / 87.2 % | 500 / 257 / 2 | 26.4 / 25.0 % |
| [sample-kernel multiply/add](performance/2026-10-12-threeosc-candidate2.json), Full | 120 | 73.1 / 87.1 % | 365 / 193 / 2 | 24.7 / 23.1 % |
| [envelope tiles + local state](performance/2026-10-12-threeosc-candidate3.json), Full | 120 | 70.0 / 86.3 % | 238 / 119 / 1 | 21.3 / 22.7 % |
| [same DSP, task priority 24](performance/2026-10-12-threeosc-candidate4.json), Full | 180 | 72.9 / 85.8 % | 406 / 146 / 0 | 24.1 / 23.4 % |
| [same firmware, Light trial](performance/2026-10-13-threeosc-light.json) | 180 | 66.9 / 83.5 % | 172 / 1 / 0 | 23.2 / 24.1 % |

These are consecutive Song playback windows at different positions and lengths, not a
controlled A/B. Skipped notes depress failing averages; maxima are one-second averages,
not the worst blocks. Plugin shares overlap on the two cores and do not sum to elapsed CPU.
No trial passes the no-skips criterion. Priority 24 produced no DMA underruns in the final
180 s Full window, but that alone does not establish long-run stability. The final Full
window still has a 6,045 µs worst render against the 5,804 µs deadline.

Full Reverb occupied 21.7 % of a block in the final Full window; Light occupied 13.3 %.
DrumLab → Reverb is an indivisible channel-strip job on one core, sharing work with Acid303;
this remains a substantial critical path after the guitar pass. Light runs the wet network
at half rate and attenuates its content above about 8 kHz. Dry audio is unchanged. It helps
load but does not meet the user's condition in this project.

3xOSC tiles oscillators and envelopes, removes per-sample PolyBLEP division calls on Xtensa,
caches the effective filter frequency and keeps per-voice state in registers. Contraction
is allowed only in sample kernels; global contraction changed pitch rounding and was rejected.
Presets, voice limits, envelope times and control period are retained. The final 100 s native
E1M1 render matches the previous plugin version byte for byte. Packaged sample code/literals occupy
6,191 B before linker relaxation (previously 6,304 B), voice kernel frame 384 B, instance
1,744 B on Xtensa / 1,752 B native. The SDK records the waveform matrix and simulated-target
arithmetic error in [its report](../../esp32-daw-plugin-sdk/docs/PERFORMANCE.md#3xosc-guitars-2026-10-1213).

Hardware defaults now use audio/worker priority 24, above Wi-Fi. The worker sleeps between
jobs and, after an over-budget job, publishes completion then delays one RTOS tick so lower
priority control/network tasks can run. The 90 % guard / 75 % admission thresholds are
unchanged. All four OTA trials returned with valid images; project and sample library matched
before/after, and playback resumed at the saved Song position. The Light trial changed only
FX 84's quality and restored it; its final export and library match the backup exactly.

Guitar-trial hardware boot 56 / ota_0, valid image, 1,577,280 B, SHA-256
`26b2ae7e1dd06e4e36bc3b185941992727a83507baba69a279bef8be9f0258b2`.
SDK tests (350 DSP / 1,137 plugin checks, 19 validators), all DAW host suites,
21 web unit / 50 network / six MIDI checks pass. Final QEMU self-test/render MATCH and
network checks pass; OTA rollback passed on the initial oscillator candidate. QEMU does
not measure chip timing or real I2S output. Hardware listening / 30-minute stability remain
separate checks.

## Drums and reverb: DrumLab, Reverb Light, MacroSynth on both cores (2026-10-12)

The user put a Reverb (size 59 %, damping 6 kHz, pre-delay 29 ms, mix 33 %) on the drums of the
imported rock song below (two MacroSynth guitars at 2 voices, 3xOSC bass, DrumLab), and it skipped
notes: DrumLab and the Reverb after it form one chain on one core, the longest path of the block.
Whole song (100 s) in Song mode on the board, no change to the project:

| firmware | DSP load mean / max | skipped notes, overruns, underruns | DrumLab | Reverb |
|---|---:|---|---:|---:|
| before (MIDI import commit) | 68.7 / 83.5 % | 72, 1, 0 | 32.9 % | 21.7 % |
| DrumLab without `sinf` in its filters, no per-sample `memcpy` | 63.2 / 77.1 % | 0, 0, 0 | 27.9 % | 21.5 % |
| + DrumLab without `__divsf3` (`FastDiv`), MacroSynths free to use both cores | **58.4 / 72.6 %** | 0, 0, 0 | **24.0 %** | 21.7 % |
| + the drum Reverb set to quality **Light** (user's choice; Full is the default) | **50.7 / 63.8 %** | 0, 0, 0 | 24.2 % | **13.9 %** |

- On the ESP32-S3 every float division in C is a call to libgcc's `__divsf3` (about 25 dependent
  FPU instructions) and `sinf` a libm call; the DaisySP drum models did several per sample, which
  is why one DrumLab piece costs 5-17 % of a block on the board but almost nothing on a PC.
- A line-major Reverb with its kernel in IRAM was byte-identical but no faster (21.5 → 21.7 %):
  the network is bound by PSRAM traffic, 25 lines read and written every sample. It was reverted.
  The Light quality halves the traffic by running the network at half rate (its wet signal loses
  content above about 8 kHz; level and dry signal unchanged).

The same song imported again with the new GM guitar mapping (a Distortion on each guitar's
insert) and the same drum Reverb, 100 s each:

| guitars | Reverb | mean / max | skipped notes, overruns |
|---|---|---:|---|
| 3xOSC Pluck + Distortion, MacroSynth + Overdrive (what the importer chose: its estimate 72 %) | Full | 68.3 / 77.9 % | 0, 0 |
| both MacroSynth + Distortion | Full | 72.0 / 83.8 % | 0, 0 |
| both MacroSynth + Distortion | Light | 67.1 / 77.9 % | 0, 0 |

A Distortion costs 6-8 % per instance (stereo, ADAA). The user's own project was restored byte
for byte afterwards (only the Reverb's new `quality` parameter appears, at its Full default).

## MIDI import load (2026-10-12)

A rock MIDI file imported as a project (the user's own test file, not in the repository: two
distorted guitars, bass, drums; 47 bars, 2,332 notes; GM programs 30 / 29 / 34 / drums) played
in Song mode on the board, firmware with the two-core scheduler. The first importer gave both
guitars MacroSynth *Plucked String* with the preset's 4 voices; their parts never play more than
2 notes at once. MacroSynth instances run one after the other (shared Braids state), so the two
guitars share one core.

| guitars (both) | DSP load mean / max | overruns, skipped notes, underruns | guitar plugin share |
|---|---:|---|---:|
| MacroSynth Plucked String, 4 voices (as first imported) | 80.2 / 93.3 % (15 s) | 236, 246, 1 | 32–39 % each |
| MacroSynth Plucked String, 3 voices | 72.4 / 77.2 % (15 s) | 0, 0, 0 | 30–32 % |
| **MacroSynth Plucked String, 2 voices (the importer now)** | **54.9 / 66.6 % (100 s, whole song)** | **0, 0, 0** | 19–21 % |
| FM6 Electric Piano, 2 voices | 54.9 / 70.0 % (15 s) | 0, 0, 0 | 20–28 % |
| 3xOSC Pluck, 2 voices | 46.6 / 59.2 % (15 s) | 0, 0, 0 | 13 % |
| Sine Pluck, 2 voices | 49.1 / 61.6 % (15 s) | 0, 0, 0 | 5–6 % |

DrumLab (up to 4 voices) took 27–40 %, the 3xOSC bass 2–9 %. The importer now gives each channel
as many voices as its part plays at once and estimates the load from these per-plugin costs
(`web/src/midi/load.ts`; the plugin catalog's own estimates are lower than measured, KI-8): ~69 %
for this song, above 75 % it swaps automatic instruments for lighter ones. Re-imported through
the board's UI over Wi-Fi: 6.9 s, then 100 s of playback with no overrun, skipped note or underrun;
three playback starts clean. 5 overruns / 1 underrun were counted while the transport was
stopped during the import (most likely the autosave's flash write; the output was silent).

## Two-core scheduling: effect chains and channel strips (2026-10-12)

The worker used to take one instrument per block; every effect chain, the mixer and the master
ran on the audio core, so dense projects were bound by their **serial effects** (8-track
fixture: about 79 % of the block in effects, 27 % in instruments). The scheduler now
(docs/AUDIO_ENGINE.md, "Parallel instruments"):

- **Instruments:** the worker claims sounding slots from the end of the slot list, the audio
  core renders from the front. Jobs under 2.5 % of the block (last block's cost) stay on the
  audio core (`Engine::Config::workerMinPermille`, 25 on the board).
- **Channel strips:** a slot that is the only writer of an insert with effects is mixed into it
  and its chain runs on the core that rendered it (FM6 and its Delay; a lead and its Delay).
- **Effect chains:** when two chains sit at the same routing depth (inserts, a delay bus beside
  a reverb bus), chains whose inputs are complete run on either core; a series of buses stays on
  the audio core. Routing and sends stay on the audio core, in the serial order.
- Output stays **bit-identical** to the serial engine (host: fixture renders and the user's
  project against the previous commit, serial and threaded; threaded comparisons also under
  ThreadSanitizer). Diagnostics gain `workerWait` (audio core waiting for the worker) and
  `workerBusy`, 1 s averages in % of the block.

**A/B on the board**, same day, same windows: the previous commit's image (boot 38) and the
final image (boot 44, `ef6a64f7f0990e41b9718f60add3adafb9a6963a5a9c6c9b23e2e58329b7354f`,
1,557,200 B, `ota_1` valid); 44.1 kHz / 240 MHz / 80 MHz, STA Wi-Fi, `scripts/performance.py
board`, 20 s (pattern) / 30 s (Song) after two diagnostic frames.

| case | previous mean / max 1 s | final mean / max 1 s | errors |
| --- | ---: | ---: | --- |
| the user's current project (9 instruments, FM6 + Delay, Limiter), Song | 68.84 / 74.2 % | **64.52 / 71.4 %** | none in either |
| three simultaneous synths + Delay / Limiter, Song | 73.85 / 77.6 % | **65.96 / 69.8 %** | none in either |
| overlapping chords (Pad 42 % alone), Song | 59.23 / 71.5 % | 59.56 / 73.5 % | none in either |
| 4 / 6 / 8-track live grooves (one shared bus), pattern | 59.45 / 60.02 / 62.29 % | 60.72 / 61.34 / 62.75 % | none in either |
| dense 8 tracks, 20 effects, pattern | 126.87 %: 1,645 overruns, **832 underruns** | 82.51 %: 326 overruns, **0 underruns** | over capacity |

Repeated runs of one case on different builds vary by about ±1.5 points (code placement changes
the instruction-cache behaviour): the chord and live-groove results are unchanged within that.
Their critical paths are a single instrument (the 42 % Pad) or a series of buses, which no
scheduling can split. Dense fixtures overload the CPU guard, which skips different notes per
run, so their means are not like-for-like; the overrun / underrun counters show the change. The
dense 6-track and shared-bus fixtures (92.7 / 93.6 %, earlier image 108.2 / 89.8 % with three
channels silenced by the guard) still exceed capacity.

What limits them now (`workerWait`): in the dense 8-track fixture the audio core waits 23 % of
the block. Its chain *inserts → (Delay bus ‖ Reverb bus) → master* puts the Reverb (~24 % by
itself) on the critical path, and plugins on the worker core run about 10 % slower than on the
audio core (that core also serves Wi-Fi). In the user's project the audio core waits 6 % and
the worker is 51 % busy; the rest is drums, bass, master Limiter and routing on the audio core.

Found on the way, measured on the board and fixed: (1) `memcpy` used for float-to-bits reads
is an out-of-line call in the ESP-IDF build (inlined on Linux): the integer NaN / Inf check
first made every block about 6 points slower on the board while it was 4 % faster on the host;
it now reads through a `may_alias` type. (2) Atomic read-modify-writes are library calls on
this target: the per-block setup counts in plain arrays and touches atomics only when the
effect phase starts. Experiments without a measurable effect, reverted: worker instrument
outputs in internal instead of PSRAM RAM (6 KB), a longer spin pause.

Internal RAM: about 1 KB of scheduling state (user project 56,319 → 55,283 B free); the three
extra instrument outputs (6 KB) are in PSRAM. Raw reports:
[previous image, pattern](performance/2026-10-12-scheduler-ab-previous-pattern.json),
[previous image, Song](performance/2026-10-12-scheduler-ab-previous-song.json),
[final, pattern](performance/2026-10-12-scheduler-final-pattern.json),
[final, Song](performance/2026-10-12-scheduler-final-song.json),
[boot 30 dense / Song baselines](performance/2026-10-12-scheduler-baseline-dense.json),
([Song](performance/2026-10-12-scheduler-baseline-song.json)).

## FM6 Electric Piano in the user's current loop (2026-10-12)

The current Song project loads nine instruments and three FX (one bypassed). Kick, open Hat,
Bass and four-voice FM6 Electric Piano feed active Delay and master Limiter; the mono Pad is
muted but retains its original voice processing. This is a new arrangement relative to the
earlier Warm Pad chord project. A read-only capture reproduced actual DMA starvation and
dropped note-ons; CPU means alone did not show the individual block peaks.

| Measurement | Before, 30 s | Final firmware, 120 s |
| --- | ---: | ---: |
| Total elapsed render/deadline mean | 77.20 % | **64.95 %** |
| Maximum one-second render load | 87.6 % | **70.3 %** |
| FM6 per-plugin mean / max one-second share | 54.68 / 66.1 % | **37.58 / 47.3 %** |
| Steady overruns / I2S underruns / guard skips | 194 / 5 / 20 | **0 / 0 / 0** |
| FM6 voice maximum | 4 | **4** |

Final boot 27 has **zero in every lifetime and steady error counter**. Its worst individual
render is **5,264 µs**, below the **5,804 µs** budget. The failing mean was depressed by
skipped notes. The user changed mixer levels between early runs; every mutation was preserved,
and notes, synth parameters, release/polyphony and routing remain unchanged in the final run.
Project export stayed identical throughout the 120 s window. These diagnostics validate the
digital render/DMA path; no analog DAC recording was taken.

FM6 renders one voice in 16-sample operator tiles. Each operator's routing and coefficients
are shared across the inner loop; 512 B scratch is stack-local. Self-feedback retains its
two-sample history; algorithms 4/6 keep their coupled operators sample-ordered. The quarter-wave
sine lookup fuses phase wrapping into integer table coordinates and linearly interpolates
**257 values / 1,028 B** shared internal data. The per-instance FAST allocation remains
**2,436 B** on the board. The sample kernel stays in IRAM; control/event/preset work stays
in flash. Sample rate is **44.1 kHz**, CPU **240 MHz**, flash/PSRAM **80 MHz**.

The lookup satisfies the existing **1e-5 absolute sine-error** test bound over all quadrants,
negative and multi-turn modulation. Tiling alone was byte-identical in 384 preset/algorithm/
feedback comparisons. The final lookup changes rounding: the actual 24 s song's maximum
error is **9.54e-7** full scale, RMS **1.55e-7** (-113.87 dB relative to the original audio).
Electric Piano, Bell, Brass and Organ reference differences are similarly measured; this is
not a claim that every high-feedback waveform is byte-identical. Native final serial/parallel
song renders remain byte-identical. SDK tests cover all 32 algorithms and arbitrary event/
algorithm-change offsets across 17/256-frame host blocks.

IRAM-only rendering still had 193 overruns / 26 skips in 60 s. Explicit operator unrolling
also failed (393 overruns / four underruns / 54 skips in 90 s), using more IRAM. Tiling with
the polynomial reduced overruns to 21 but still skipped 30 notes in 90 s. These results are
retained and superseded by the fused lookup implementation.

Current-project internal free memory is **56,319 B**, minimum **12,744 B**, largest block
**31,744 B**; PSRAM free **15,189,340 B**. Audio/worker stack free is **3,760 / 4,472 B**.
Code/table reservations spend internal headroom; no extra heap allocation occurs per voice
or per render. Chromium's 20 s read-only profile reports **6.84 % browser task load**, no
observer leak/JS errors, and working mouse/touch window closing. Browser load is separate
from ESP32 render load. SDK ABI / 350 DSP / 754 plugin checks, all 19 validators and final
QEMU boot/render/persistence/network pass.

The 120 s FM6 measurement image is **1,549,808 B**, SHA-256
`e28c66bd34a27433c3f61754c0c3a80df26d4fd1efef18ce5cf2979714386b78`.
OTA confirmed valid slot **ota_0**, boot **27**. Reproduce the actual arrangement using
[`fm6-electric-piano-loop.json`](../examples/performance/fm6-electric-piano-loop.json) in Song
mode; the eight-voice capacity probe uses
[`fm6-eight-voices.json`](../examples/performance/fm6-eight-voices.json).

Raw reports: [before](performance/2026-10-12-fm6-loop-baseline.json),
[IRAM only](performance/2026-10-12-fm6-iram-only.json),
[unrolling](performance/2026-10-12-fm6-unroll-rejected.json),
[tiles with polynomial](performance/2026-10-12-fm6-tile-only.json),
[final 120 s actual loop](performance/2026-10-12-fm6-loop-fixed.json),
[browser profile](performance/2026-10-12-fm6-loop-ui.json),
[tiling PCM](../../esp32-daw-plugin-sdk/docs/performance/2026-10-12-fm6-pcm.json),
[final lookup/audio comparison](../../esp32-daw-plugin-sdk/docs/performance/2026-10-12-fm6-lookup-audio.json).

The capacity regression keeps failures visible:

| Fixture | Mean load | Loaded | Result |
| --- | ---: | ---: | --- |
| 4 / 6 / 8 track live grooves, 15 s each | 59.66 / 60.28 / 62.76 % | 10 / 12 / 14 | All scheduled channels audible, ten startup/steady counters zero |
| Simultaneous three synths, 15 s | 73.17 % | 5/5 | Eight pad voices; ten startup/steady counters zero |
| Full six-operator Electric Piano, eight voices, 15 s | 108.72 % | 1/1 | Fails: overruns, DMA underruns and guard skips |
| Algorithms 4 / 6 with eight-voice pools, 15 s each | 124.33 / 119.67 % | 1/1 | Fail: overruns and DMA underruns |
| Dense eight tracks / 20 effects, 15 s | 128.19 % | 28/28 | Fails; skipped channels/notes and underruns |

The full Electric Piano probe is heavier than the previous init-preset FM6 voice test.
Four-note input chords in the algorithm 4/6 probes overlap their original release tails;
the pool reaches **eight**, not four, sounding voices. These are capacity limits, not
passes inferred from CPU averages. The live-groove leads alternate as documented earlier.
[Raw capacity run](performance/2026-10-12-fm6-capacity-first.json).

This regression also exposed a **project-transition RAM peak**: restoring the user's project
before the old dense project's DSP instances were retired left FM6/Limiter unloadable.
The first run records `restored: false`; the project contents were retained with runtime
load errors. The final Session waits for a stopped render-block command fence, drains the
retired instances, then constructs the next project's DSP. It also avoids allocating empty
FX-chain messages. No freeing/allocation moves onto the audio thread; worker jobs finish
before the fence acknowledges. Live hardware/native owners provide a yielding callback;
offline owners retain their synchronous render workflow. Native Session has **235 checks**
including warm import/reclamation; the parallel suite retains 44 checks.

The final image is **1,550,320 B**, SHA-256
`497cf8ed5d5f56c851f2c5e4e6c0f7650bf40de1284836913b7daf07769f281a`.
Physical OTA boot **29 / ota_1 valid** preserved the latest user project/transport. Another
dense probe still exceeds the CPU budget, loads **28/28**, and now returns
**`restored: true`** with every user instrument/effect available.
[Project-transition evidence](performance/2026-10-12-project-transition-fixed.json).

After deliberate overload testing, clean boot **30 / ota_1 valid** measured the user's
latest project, now with **Clap added**. In 90 s: **68.84 %** mean / **74.6 %** maximum 1 s
load; FM6 **37.68 %** mean with four voices. **12/12** instances load, every lifetime and
steady error counter stays **zero**, worst render **5,486 µs < 5,804 µs**. Export is unchanged
throughout the window, Song playback stays active. This includes the user's newer edits and
is not a controlled repeat of the earlier four-channel 120 s window. Internal free/min/largest
is **56,311 / 12,732 / 31,744 B**, PSRAM free **15,189,300 B**, audio/worker stack free
**3,952 / 4,472 B**. Final native host suites, 50 network / six MIDI checks, and QEMU
boot/render/persistence/network pass.
[Final user-loop evidence](performance/2026-10-12-final-user-loop.json).

## Overlapping chords and three simultaneous synths (2026-10-11/12)

Reproducing the user's **Song-mode** arrangement exposed a case absent from earlier
selected-pattern / alternating-lead checks: a three-note Warm Pad chord keeps its 1.8 s
release while the next chord starts. Pad voices can reach eight alongside bass and drums.
Removing notes is not a capacity fix.

| Arrangement | Serial mean / max 1 s load | Final worker mean / max 1 s load | Final result |
| --- | --- | --- | --- |
| Kick, open hat, bass and overlapping pad; 8 instruments + Limiter/idle Delay | 76.07 / 86.0 % | **58.42 / 71.6 %** | 45 s, 10/10 loaded; every lifetime/steady error counter zero |
| Simultaneous Fat Bass, Pluck and Warm Pad + Delay/Limiter | 86.45 / 91.5 % with selective IRAM already enabled | **74.51 / 77.9 %** | 45 s, 5/5 loaded; all channels audible; every startup/steady error counter zero |

The first serial run dropped **68 note-ons** and overran **39 blocks** in its 45 s steady
window, plus four skips/two overruns at startup. The second serial run dropped **280 note-ons**,
with 269 overruns/two I2S underruns in 60 s, plus two startup skips. Their CPU means are
depressed by missing notes. The worker processes the original envelopes and permits eight pad
voices. These maxima are **one-second averages**, not worst individual blocks. After clean
boot 23, the original song's lifetime worst render is **4,887 µs** against **5,804 µs**.
The first worker image passes both arrangements for 75 s at 57.89/74.16 %; the compact kernel
passes both for 60 s at 58.25/74.36 %. Final monotonic timing includes every worker join wait.

At this stage, one persistent 6 KB task on core 0, priority 20 below Wi-Fi, overlapped one instrument with
earlier slots on core 1. One extra 2 KB internal PCM buffer stores its output. Joining at the
original slot preserves accumulation order, events, tails and retirement ownership. Effects
remain serial on the audio core. MacroSynth instances share Braids RNG/oscillator globals;
their callbacks retain serial slot order while other instruments can overlap them. See
[audio worker design](AUDIO_ENGINE.md#parallel-instruments-on-the-hardware).

The overall deadline uses the monotonic microsecond timer in worker builds; per-plugin shares
use local core cycles. `cpu` measures **elapsed render/deadline time**, including waits, rather
than total physical utilization of both CPUs. Plugin shares can sum to more than elapsed time.

A waveform block dispatcher was byte-identical but slower and was reverted. Moving the full
3xOSC render into 10,760 B IRAM alone still skipped notes and failed the three-synth test. It
also caused eight FX to fail the internal-RAM preflight while loading the dense fixture in the
first worker build. The final kernel keeps **only 6,304 B of sample-loop code in IRAM**: all
**28/28** dense-fixture instances load. The arrangement still overruns and drops notes, so its
127.87 % measured average is explicitly a failure. First-worker 4/6/8 live-groove regressions
pass at **60.23 / 60.57 / 62.84 %** in 15 s windows; those leads alternate and do not substitute
for the simultaneous synth test.

Final original-project internal free memory is **62,991 B**, minimum
**19,420 B**, largest block **31,744 B**; PSRAM free
**15,902,280 B**. The code/worker reservations spend some of the previous ~78 KB
internal headroom to complete more actual audio work. Worker stack free is **5,032 B**,
audio stack free **4,144 B** in this run. Sample rate remains **44.1 kHz**, CPU
**240 MHz**, flash/PSRAM **80 MHz**; release/polyphony are unchanged.

Native concurrency regression passes **44 checks**, including shared routing, parameters, live
replacement/removal, transport/panic and shared Braids state. Nine 16 s arrangements plus both
24 s chord fixtures match the pre-change serial renderer byte for byte. SDK/core/network/MIDI
checks pass. QEMU boot/render/persistence/network pass with the compact kernel; the worker
architecture also passes 16 OTA/rollback checks. QEMU keeps the worker off and gives no real
ESP32 timing. Actual physical OTA confirms the final image.

Final firmware: **1,544,880 B**, SHA-256
`5dfd1476997c5fcdfe3490b2efa3471e90b7a462e59e998d109d9030df2c2f44`, OTA boot **22 / ota_0 valid**. Clean boot **23** preserves the user's project export
exactly and restores their Song mode, position and playing state, with all ten lifetime counters
zero through the 45 s verification. Capacity applies to these measured arrangements; arbitrary
dense effect chains still exceed it.

```sh
python3 scripts/performance.py board --host esp32daw.local --mode song --seconds 75 \
  --no-upload-resources --project examples/performance/chords-overlap.json \
  --project examples/performance/three-synths-overlap.json --out /tmp/chords.json
```

Raw evidence: [serial failure](performance/2026-10-11-chords-baseline.json),
[serial IRAM failure](performance/2026-10-11-chords-serial-iram.json),
[first worker 75 s runs](performance/2026-10-11-chords-dual-core-first.json),
[compact-kernel 60 s runs](performance/2026-10-11-chords-compact-iram.json),
[monotonic-timer runs](performance/2026-10-11-chords-timer-final.json),
[final three synths](performance/2026-10-12-three-synths-release.json),
[final original song](performance/2026-10-12-user-song-release.json),
[4/6/8 and RAM regression](performance/2026-10-11-parallel-regression-first.json),
[28-instance dense failure](performance/2026-10-11-parallel-dense-final.json),
[nine PCM comparisons](performance/2026-10-11-parallel-pcm.json).

## Budget

| | value |
|---|---|
| block | 256 frames at 44100 Hz = **5805 µs** |
| CPU clock | 240 MHz → 1.39 M cycles per block on the audio core (core 1) |
| target | render ≤ 70 % of the block on average, worst case < 90 % (the rest: DMA, interrupts, cache misses on PSRAM, Wi-Fi interrupts landing on core 1) |
| DMA headroom | 4 × 256 frames = 23 ms of buffered audio before an underrun |

What the device reports, live (`diag` message, device page, `/api/status`): `cpu` (render time /
block period, 1 s average), `renderUsAvg`, `renderUsMax`, `budgetUs`, `overruns`, I2S `underruns`,
voices, and the CPU share of every plugin instance (device page, "Plugins" table). Short kick
and hi-hat measurements are below; the full H3 / H4 checklist remains incomplete.

## Measured

| what | value | where / how |
|---|---|---|
| firmware image, tested hardware profile | 1,543,536 bytes with 18 plugins = 49.1 % of the 3 MB OTA slot (MacroSynth's Braids tables: about 88 KB) | `scripts/build-hardware.sh` after catalog / memory / UI fixes (xtensa GCC 14.2, DSP -O3, 32 MB OPI DTR flash), 2026-10-11 |
| firmware image, QEMU profile | 818,592 B = 26.0 % | `scripts/build-qemu.sh` |
| embedded web application | 78,600 bytes, four assets including third-party notices | `embed-web.py`, gzip where useful |
| browser JS bundle | 145,623 bytes, 50,921 bytes gzipped | Vite 7 production build, gzip level 9 |
| plugin code, 16 SDK plugins | 115 KB code + rodata | SDK `scripts/build-esp32.sh` |
| internal heap free after boot (QEMU, no Wi-Fi) | 96–117 KB, minimum 64 KB | `/api/status` in QEMU; hardware with Wi-Fi will have less |
| PSRAM free after boot | ~16.0 MB of 16 MB | QEMU |
| offline render, Linux | 60 s of the demo song in 2.83 s (≈ 21× real time, one core of an i7-9750H) | `daw-host --render --mode song --seconds 60`, RelWithDebInfo, 3 runs 2.82–2.86 s |
| cross-target equality | Linux and Xtensa (QEMU) renders of the same project: identical RMS / peak / non-zero counts | `scripts/test-qemu.sh` |

The Linux render speed only ranks changes against each other (a regression shows up there first);
it says nothing about the ESP32-S3, whose cores, caches, PSRAM and FPU differ completely.

### ESP32-S3 initial stopped baseline (2026-10-11)

Firmware 0.1.0, ESP32-S3 rev v0.2 at 240 MHz, 32 MB octal flash / 16 MB octal PSRAM at 80 MHz,
STA Wi-Fi connected. The default project had 8 channels; transport stayed stopped. At 120.6 s
uptime, after HTTP / WebSocket / Chromium smoke checks, `/api/status` reported:

| measurement | value |
|---|---|
| render load (`cpu`) | 31.6 % |
| internal heap free / minimum | 32,131 B / 7,796 B |
| PSRAM free | 15,957,684 B |
| underruns / overruns / audioLate | 0 / 0 / 0 |
| maximum I2S write wait | 4,065 µs |

These are a stopped baseline only; they do not verify audible output, long playback stability,
maximum polyphony or the admission thresholds. H3 / H4 later gained the partial coverage below.

### ESP32-S3 kick playback before / after optimization (2026-10-11)

Same board, clock, memory, STA Wi-Fi and 8-channel project as above; only the Kick channel had
notes in the selected 16-step pattern (four hits at 128 BPM). The existing master Limiter and
Lead Delay remained enabled. The original analog-kick settings were mode 1, master 0.7602,
kick level 0.753, tune +3.2256, decay 0.7, tone 0.4, character 0.2. The user's later Tight
preset used mode 2 (Kick 2), tune +3, decay 0.3 and tone 0.6.

| model | plugin CPU before → after | total render CPU before → after | mean render time after | playback |
|---|---|---|---|---|
| Analog Kick | 55.7 % → 15.2 % | 88.9 % → 48.8 % (48.8–48.9 %) | 2,837 µs | 25 s |
| Kick 2, Tight | ~45.1 % → 17.6 % | ~78.8 % → 51.2 % (51.1–51.4 %) | 2,977 µs | 35 s |

The original analog kick's fast render load reached 129.5 %, with a lifetime maximum block of
10,163 µs against the 5,804 µs reported budget; guard skips had reached 319 and underruns /
overruns were increasing. After optimization, all six monitored counters (underruns, overruns,
guard skips, note drops, event drops, NaN resets) stayed at zero throughout the final 60 s run.
The lifetime maximum render block, including startup and control work, was 5,629 µs. Channel /
master meters were nonzero; this verifies digital output, not the analog DAC sound.

Means and ranges exclude the first three 1 s diagnostic frames of each case. Firmware 0.1.0,
image SHA-256 `6786a1c4323a9791c61b385ace103b96bca0b6412e7a0afc2ba79da5cc7fe4b9`.
Authenticated OTA confirmed the new slot valid and preserved the exported project exactly.
Temporary analog-kick benchmark parameters were restored to the user's Tight preset afterward.

The SDK caches parameter / sample-rate coefficients and sets the analog filter directly from
Plaits Q, avoiding per-sample general exponentiation and an inactive stability limit. Six renders
per model across decay / tone / tuning extremes matched the original 24-bit PCM byte for byte;
SDK and DAW host tests passed. This build retained the roughly 32 % stopped baseline; the
follow-up below reduces it. Neither run establishes maximum polyphony or 30-minute stability.

### ESP32-S3 Kick + hi-hat and idle pipeline optimization (2026-10-11)

Same board / clock / memory / STA Wi-Fi, with the user's 8-channel project preserved exactly.
Kick 2 uses the Tight preset; Hat uses mode 5 (Closed Hat). Both have four hits in the current
16-step Beat pattern at 128 BPM. Master Limiter and Lead Delay remain enabled; other channels
and routing were not removed or bypassed. Chromium also opened the page during playback.

| measurement | before | after |
|---|---|---|
| stopped total render CPU | 31.6 % | 9.8 % (9.8–9.9 %) |
| Kick + Hat total render CPU | 120.9 % | 41.6 % (41.4–42.0 %) |
| Closed Hat instance CPU | 76.2 % | 10.2 % |
| Kick 2 instance CPU | 10.9 % with guard skips | 17.6 % with every note admitted |
| Master Limiter CPU | 7.3 % | 3.5 % |
| idle Lead Delay CPU | 7.8 % | 0.8 % |
| mean render time, Kick + Hat | 7,022 µs | 2,417 µs |
| lifetime maximum render time | 11,078 µs | 2,948 µs |

The stopped mean is 574 µs. Means / ranges exclude the first three 1 s diagnostic frames.
Before optimization the snapshot had 10,224 overruns, 7,223 underruns and 191 guard skips,
with those counters increasing. After optimization a 75 s playback window (73.5 s between
its first / last diagnostics) had zero increments in all ten counters: overruns, underruns,
guard skips, note drops, event drops, NaN resets, FX failures, command overflow, retire overflow
and audioLate. All were also zero since the new boot. Digital channel peaks were 0.554 Kick /
0.287 Hat and master peak 0.443; analog DAC output was not recorded.

Hi-hat parameter / filter coefficients and square-noise oscillator increments are cached.
The Limiter uses a monotonic minimum queue; empty Delay lines avoid PSRAM accesses while
advancing time / smoothing exactly. Silent mixer tracks skip zero-array gain / routing / sends,
while gain / send state and meters still advance. Effects always run so tails and generators
remain valid. No model, sample rate or quality setting changed.

Twenty-two plugin renders (eight hats, eight snares, two Limiter presets, four Delay presets)
and two complete project renders matched the previous 24-bit PCM byte for byte. Both snare
models also cache their coefficients, filter settings, mode gains and static modulation factors
to avoid overload when switching DrumLab sounds. SDK tests, DAW host tests and
browser interaction tests passed (51 checks plus five viewport screenshots on daw-host).
The full plugin / polyphony matrix and 30-minute run remain pending.

During a subsequent interactive run on the UI-only follow-up image, the user tried other
DrumLab modes / parameters: render load briefly reached 291.7 %, with 231 overruns / 720
underruns / six guard skips. That run is not the unchanged Closed Hat benchmark above.
It prompted the additional snare coefficient / filter caching; both sample algorithms
remained byte-identical in the eight references. Snare hardware capacity is still part of
the remaining H4 matrix.

The unchanged Closed Hat pattern above was measured on the pipeline image
`3d4d63d3cf47ea2cd68752ac29410acf3676e3befa999a76d8dabaea0272ce1c`.
Final firmware 0.1.0 also includes snare caching and UI fixes; SHA-256:
`5e5726b03ae5143b3bc7cb8eceebd4bd15c3085402e920bb273848b8f1736935`.
Authenticated Wi-Fi OTA booted `ota_0` (boot 7), confirmed `otaState: valid`, and preserved
exported project JSON exactly. The final image also fixes floating-editor close-button pointer
capture and piano-roll / playlist follow recursion on page opening during playback.

On the final image, a further 75 s monitoring window (73.7 s between diagnostics) followed the
user's newly selected Bassline pattern, without changing their selection or sound parameters.
Mean CPU was 23.3 % (23.2–23.9 %), mean render time 1,354 µs, lifetime maximum 2,919 µs.
All ten error counters remained zero; the Bass channel showed a 0.272 digital peak. Six
real-board Chromium checks passed during playback: window dragging, mouse / touch close,
FX close, reload during playback and no JavaScript errors. Exported project JSON was identical
across the final OTA, and playback remained running after verification.

## Estimates (to be replaced by measurements)

Plugin manifests carry `cpuPermilleBase` / `cpuPermilleVoice` (per mille of one block, base +
per active voice). These are the plugin authors' **estimates**, not measurements. The DAW shows
them through the plugin catalog but does not use them for admission: the CPU guard and admission
limits work from the measured render load instead (docs/AUDIO_ENGINE.md). After H4 the estimates
can be calibrated and used to warn before a plugin is added.

## Design choices made for performance

- One float block pipeline, no per-sample virtual calls; events sorted once per block.
- Idle instruments are skipped (no events, no active voices).
- Engine buffers, plugin FAST memory and the audio stack in internal RAM; only large tables and
  delay lines in PSRAM.
- Fixed-size pools (events, active notes, commands): no allocation, no unbounded loops.
- Fast `sin` (degree-9 polynomial, |err| < 4e-6), `exp2` approximations in the SDK's DSP helpers.
- The control plane (JSON, compiling patterns) runs on core 0; only pointer swaps reach core 1.

## How to measure on the board

1. `scripts/build-hardware.sh && scripts/flash.sh --monitor`, connect a browser.
2. Device page → diagnostics, or poll `/api/status` and the `diag` WebSocket message.
3. For each scenario of docs/HARDWARE_TESTS.md H3 / H4, record `cpu`, `renderUsAvg`, `renderUsMax`,
   `underruns`, `heapInternalMin`, with the firmware version, into the table above under a new
   "ESP32-S3 (measured)" heading — and update the plugin manifests' estimates in the SDK.

## Full catalog optimization pass — 2026-10-11

Board: ESP32-S3 rev v0.2, CPU 240 MHz, 32 MB OPI flash / 16 MB PSRAM at 80 MHz,
STA Wi-Fi, 44.1 kHz stereo, 256 frames (5,804 µs). Figures below are a share of one audio
block period, not both CPU cores. Single-plugin fixtures use a one-note line, including release
overlap; effect fixtures put the effect after Sine. They are not sustained maximum-voice costs.

| plugin fixture | before total DSP % | after total DSP % |
| --- | ---: | ---: |
| drumlab | 19.96 | 19.30 |
| sine | 9.54 | 9.66 |
| 3xosc | 14.71 | 12.58 |
| fm6 | 27.77 | 23.58 |
| acid303 | 25.49 | 22.28 |
| macrosynth | 15.72 | 13.50 |
| sampler | 11.20 | 10.02 |
| soundfont | 6.60 | 6.44 |
| fx.filter | 14.79 | 14.44 |
| fx.eq | 13.85 | 12.18 |
| fx.compressor | 15.86 | 15.72 |
| fx.distortion | 18.90 | 18.92 |
| fx.utility | 11.62 | 11.44 |
| fx.chorus | 38.00 | 27.20 |
| fx.flanger | 24.62 | 22.26 |
| fx.delay | 18.56 | 18.56 |
| fx.reverb | 33.70 | 31.65 |
| fx.limiter | 13.97 | 14.00 |

Before: memory placement fix only, image `0c89f177aba9506f0da08876145acb64809cd9a36a31a8a7b0aab317a148bab9`.
After: optimized DSP/engine, image `95bf9a40af0da765193dad5a946141a2d6087eb968f84f64b9cacfae0234bfc6`.
Short 6–8 s windows exclude two initial diagnostic frames. All 18 default fixtures load their
expected instances, produce channel audio and have zero steady error deltas. Small changes
such as Sine, Compressor, Distortion, Delay and Limiter are not convincing new speed gains.
The earlier drum/limiter/delay fixes are already included in the before column.

The real-board matrix covers all 9 DrumLab modes and all 8 kit pieces, all 48 MacroSynth
models, all 32 FM6 algorithms with all operators enabled, all five 3xOSC waves, and 4/8/max
voices where supported. **129/136 cases pass** the loaded-instance, audio and ten-counter
checks. FM6 at 8 voices and all six dense/shared sustained arrangements fail capacity checks.
Their averages are affected by skipped notes and must not be used to claim performance gains.
The three heaviest measured MacroSynth models (24, 38, 10) also pass at four voices in separate
10 s cases: 57.66 %, 48.10 %, 47.21 % total. Model tests are short, not long-run certification.

Selected clean polyphony fixtures without mixer effects: Sine 16 voices 37.98 %, 3xOSC 8
57.84 %, FM6 4 61.02 %, MacroSynth model 0 at 4 25.68 %, Sampler 16 59.76 %, SoundFont
24 38.90 %. Adding effects consumes the remaining budget; these are not simultaneous capacities.

Raw evidence: [baseline](performance/2026-10-11-board-baseline.json),
[136-case catalog](performance/2026-10-11-board-catalog.json),
[heavy MacroSynth models](performance/2026-10-11-macro-max.json).

### Changes that survived measurement

- Plugins use `-O3 -fno-math-errno`, retaining `-ffp-contract=off` and finite-value protection;
  the engine loop uses `-O3`. No global fast-math or reduced synthesis algorithm is enabled.
- Unit-phase sine/floor operations avoid redundant normalization/library calls. 3xOSC caches
  pitch updates; SVF/Filter/EQ avoid unchanged coefficients and unused exponentiation. Mono
  Sampler data is filtered once; stereo data still has independent filters.
- Acid303 avoids its unused square branch at pure-saw settings. Acid303 and MacroSynth
  halfband FIR loops are unrolled. All effect presets/models were audited, including
  Compressor, Distortion and Utility, whose main new change is the common compiler flags.
- Empty Filter/EQ and pristine Chorus/Flanger/Reverb avoid sample work while preserving
  control smoothers, positions and LFO phase. Active tails/freeze continue. The mixer skips
  routing only for actual zero output; gain/mix updates stop once the next float value is unchanged.
- Audio/DMA buffers and hot voices remain internal. Event arrays and three queue payloads
  move about 47 KiB to PSRAM; indices stay internal. The normal-malloc internal threshold is
  4 KiB; instance preflight retains a 32 KiB reserve. Failed instantiation now appears in the
  plugin editor, instead of silently leaving an unplayed slot.
- SoundFont borrows aligned PCM16 from the retained raw font, outputs split stereo directly,
  and keeps hot voices internal. A 264,864 B test font's plugin-owned LARGE memory falls
  534,616 B → 400 B; the raw font is still owned by the host. Unaligned input copies PCM16.
  Native FAST memory rises 2,112 B → 5,448 B in exchange for faster voice access.
- Piano roll/Playlist keep one observer/subscription per mount, coalesce playhead drawing with
  animation frames and suspend live drawing in hidden tabs. Automation keeps its observer
  across point edits. Window X buttons work with mouse and touch. Diagnostics display CPU
  frequency, internal free/largest-block memory and PSRAM separately.

The user's unchanged project had 30,535 B internal free before this pass and 78,271 B after
these placement changes. PSRAM free changes 15,957,052 B → 15,908,840 B: moving cold data uses
about 48 KB more PSRAM in that project, which has no loaded font to benefit from PCM16 borrowing.
Most of its 16 MB PSRAM remains free. The tradeoff releases scarce internal RAM; it does not
pretend that all RAM classes decreased at once. These are project/status snapshots, not a
controlled heap watermark under identical client activity.

An all-render IRAM experiment consumed about 29 KB of internal RAM, did not show a useful
speed gain and caused missing instances on reload; it was removed. A transposed Reverb block
experiment used another 4 KB FAST and did not improve board timing; it was also removed.
The final Reverb retains only its safe pristine-silence optimization.

### Audio and UI validation

SDK ABI 1.1, 345 DSP checks, 626 plugin checks and validation × 19 pass. Of 241 audible
catalog reference WAVs, 236 are byte-identical; five SoundFont renders differ by at most four
24-bit PCM units (RMS error 8.02e-8 full scale). Polyphony benchmarks use the requested voices;
reference WAVs for those entries use the standard chord sequence. A separate held-note test
crosses sample position 130,000 and a loop wrap, differing by at most two PCM units.
Nine complete 16 s DAW arrangements (4/6/8 × dense/shared/live) match the preceding engine
byte for byte after the exact gain/silence/memory changes.

DAW core 57/3/108/75/226/17/2397 checks, network 50, MIDI bridge 6, browser 51 and five
viewport screenshots pass. QEMU boot/render/persistence, cross-target render statistics,
network e2e and all 16 OTA/rollback checks pass; QEMU timing is excluded from CPU tables.

Read-only Chromium profiling during real-board tests: 30 s, 4.53 % browser task time,
JS heap 4.81 MB → 3.75 MB, two active editor observers before/after repeated editor toggles,
no browser exceptions; mouse/touch X close passed. This describes this Linux Chromium session,
not an ESP32 CPU saving or every phone. [UI evidence](performance/2026-10-11-ui-initial.json).
Use `scripts/profile-ui.py --url http://esp32daw.local/` to reproduce.

### Sample-rate and clock experiments

The same live arrangements also pass 12 s windows at 32 kHz: 4 tracks 48.08 %, 6 tracks
48.14 %, 8 tracks 49.83 %. All dense/shared sustained arrangements still fail; lowering the
rate does not make an overloaded chain valid. The 8-track clean arrangement uses about 27 %
less DSP budget, with a narrower 16 kHz Nyquist bandwidth. Delay/Reverb line memory also
scales down with rate. [32 kHz evidence](performance/2026-10-11-board-32000.json).

32 kHz is optional, not the deployed default. Set `DAW_SAMPLE_RATE=32000` in `config/local.env`
and rebuild, or use a separate build:

```sh
DAW_SAMPLE_RATE=32000 DAW_HW_BUILD=build-hw-32000 scripts/build-hardware.sh
```

To return, remove the local override or set 44100 and rebuild. The final installed image uses
44.1 kHz. I2S remains 24-bit audio in 32-bit slots; no sample format reduction was made.

CPU clock was already 240 MHz. Real-board `rtc_clk_cpu_freq_mhz_to_config` probes accept
240 and reject 280/300/320 MHz; unsupported clocks were not written. This matches
[ESP-IDF v5.5.5's supported S3 CPU frequencies](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/power_management.html).
Flash/PSRAM remain at 80 MHz. The documented 120 MHz DDR mode is experimental and was not
used; see the [flash/PSRAM configuration guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/flash_psram_config.html).

### Final deployed live projects

Image **1,543,536 B**, SHA-256
`2637dc70bb95d3bad55b3dd508894de7c17e1e8a6255ad748f15b257edb572e7`,
boot 15 / `ota_0` confirmed valid. The DSP/engine is the same as the 136-case image; final
changes add the automation observer fix, clock/largest-block display and refreshed notices.
OTA preserved the exported user project exactly. The final build is 44.1 kHz, CPU 240 MHz,
flash/PSRAM 80 MHz. Every live project has six enabled effects and all its instruments loaded.

| fixture | steady window | mean DSP | max 1 s DSP | internal free | PSRAM free | ten error deltas |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4-tracks-live | 118.9 s | 65.97 % | 66.4 % | 86,099 B | 15.17 MiB | 0 |
| 6-tracks-live | 118.9 s | 65.88 % | 67.9 % | 84,415 B | 15.10 MiB | 0 |
| 8-tracks-live | 118.9 s | 68.28 % | 69.7 % | 76,327 B | 14.96 MiB | 0 |

Each ran for 120 s plus startup; startup error deltas also stayed zero, and all channel meters
showed audio. The expensive leads alternate with short releases; this is not eight sustained
chords with 20 insert effects. The six dense/shared fixtures remain intentional overload tests.
[Two-minute reports](performance/2026-10-11-live-long.json),
[projects and import instructions](../examples/performance/README.md).
Analog DAC listening/recording, 30-minute/four-browser/thermal checks remain unverified.

After a clean reboot (boot 16, same image), the original project JSON is unchanged,
all 10 expected instances are loaded, and the user's selected pattern runs for 75 s: **20.57 %**
mean DSP. Every lifetime and steady error counter is zero; internal free 78,259 B,
minimum 34,688 B, PSRAM free 15,902,268 B. Playback is left running.
The earlier stopped sample-upload/import phases accumulated diagnostic overruns; the clean
boot separates them from this playback result. [Final user-project evidence](performance/2026-10-11-user-final.json).

Final-image Chromium during original-project playback: 30 s, **6.44 %** browser
task time, active observers 2 → 2 after repeated editor toggles, mouse/touch close works, no
exceptions. [Final UI evidence](performance/2026-10-11-ui-final.json).
