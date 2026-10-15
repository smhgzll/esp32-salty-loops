# Progress

Status 2026-10-15, firmware 0.1.0. Milestones as defined in the project brief (M0–M11).
"Done" means implemented **and** covered by passing tests on the targets available here (Linux,
QEMU, plus the hardware checks below). Hardware tests beyond those checks remain
UNVERIFIED; the first board was connected on 2026-10-11.

| milestone | status |
|---|---|
| M0 research, platform, skeletons | done |
| M1 hardware and audio foundation | done in software; hardware boot / I2S initialization verified; **DAC audio UNVERIFIED** |
| M2 browser infrastructure | done; hardware STA Wi-Fi, HTTP / WebSocket and browser startup verified; AP / fallback UNVERIFIED |
| M3 core musical system | done |
| M4 plugin SDK and first synths | done (Sine, 3xOSC, DrumLab, Delay, Filter, Limiter, Utility) |
| M5 piano roll | done |
| M6 mixer and effects | done (four basic effects; more in M8) |
| M7 song arrangement | done |
| M8 additional plugin engines | in progress: Acid303 (Open303), FM6, MacroSynth (Braids), Sampler, SoundFont (TinySoundFont), Tracker (libxmp: MOD / XM / S3M / IT) and six effects done; OPL3 (optional) open |
| M9 persistence and OTA | done: projects, samples, user presets, export / import, OTA + rollback (verified in QEMU); full-device backup open |
| M10 quality and optimization | partly: multi-client, stress, mobile done; all 18 plugins / 136 cases measured (the 19th, Tracker, on six songs); 4/6/8 live arrangements pass; dense chains / 30-minute stability still limited |
| M11 dynamic plugins | not started (deliberately last) |

## MIDI file expression retained and played — 2026-10-15

SMF imports retain timed pitch bend, all controller types, program changes, channel/poly pressure,
SysEx fragments and metadata for the selected parts. Notes keep original velocity, release velocity
and same-tick ordering. SoundFont supports dynamic presets, pedals, common RPN tuning and default
pressure/modulation vibrato; future preset/sample dependencies are loaded selectively before audio
playback, also after project reload. Fixed synths gain common controller state and offline pedal
gates. Unknown manufacturer/effect commands remain data, with an explicit UI notice; this is not
full MT-32/GS/XG synthesis. Live SysEx/recording is a separate path. Details: docs/MIDI.md.

Legacy project exports stay schema 1; new MIDI data uses schema 2, rejected by older firmware.
ABI 1.4 is opt in and retains the 32-byte event. Seek controller replay is bounded, with no audio
allocation/I/O, new voice cap, shortened tail, quality reduction or clock change. Canonical same-tick
ordering keeps repeated riffs shared even when other channels or setup messages differ.

Passed: SDK 1,499 plugin checks + 350 DSP checks and ABI checks; all 10 core suites (40 MIDI,
427 session checks), 48 web unit tests, network end-to-end/MIDI bridge and 26 browser MIDI checks.
All 19 installed plugins produced byte-identical legacy PCM over 670,320 stereo frames. Physical
board: real-browser TimGM6mb import, export/reload, 5 s playback, seek and stop; zero new underruns,
overruns, event/note drops, guard drops or runtime error counters, no held voice after stop.
The original seven-channel Stayin' Alive project played for 5 s before and after OTA with zero
new underruns/overruns; these partly silent windows are smoke checks, not a performance claim.

Deployed boot **129**, OTA valid, **240 MHz**, image **1,845,840 B**, SHA-256
`bc77b2deaf03946a79598e484d832efe06e94cfc099106c7b0acb45ab7bdd612`.
Original project export, all six library files and stopped Song/tick-0 transport verified restored.
Reimport original `.mid` files to recover expression older imports discarded. No QEMU or long soak.
Evidence: docs/performance/2026-10-15-midi-expression.json.

## 260 MHz Wi-Fi recovery attempts — 2026-10-15

User requested Wi-Fi at 260 MHz. Tried stop-at 240/start-at 260, complete driver deinit/init
at 260, and stop/start with flash/PSRAM dividers doubled. CPU independently remained260 MHz,
all Wi-Fi API calls succeeded and 64 KiB RAM canaries passed, but all three reported
**NO_AP_FOUND (201)**; explicit scans found0 APs, no IP or HTTP/WS access. The song stayed
stopped, sampled DMA/overrun counters did not rise and steady idle render load was17-20%.
This is not just the web server failing under dense song load.

Identical full driver reinit at 240 obtained IP after 3.441 s, then served 12 HTTP responses and 11
WS diagnostics while playing the heavy IT section for 12 s, with zero new DMA underruns/guard
drops. A stock-configured APB GPTimer versus XTAL SYSTIMER independently measured
**80.000→86.670→80.000 MHz** through the final260 trial and restoration. Lower memory clocks
did not restore Wi-Fi. Strong support for shared-clock incompatibility; exact PHY internals
and all possible alternative firmware methods are not established by these short trials.

Temporary-only CPU guard disable, endpoint, reconnect/fallback suppression and probes removed;
production sources/config restored exactly. Every run deliberately returned/rebooted at 240;
project/library and transport preserved. No voltage change, undocumented RF registers, QEMU
or long soak. Details, primary reference, evidence and unapplied patches: docs/PERFORMANCE.md
and docs/performance/2026-10-15-260-wifi-trials.json.
Final normal firmware: boot **120**, OTA valid, **240 MHz**, image **1,744,704 B**; playback
resumed, HTTP/ping work, project/library rechecked, temporary endpoint and probes absent.

## Higher-clock attempts, from the top down — 2026-10-15

User requested higher clocks starting at the top of the discussed range. Physical-board
**320 → 300 → 280 MHz**, stock voltage: each reset during the clock transition before a
measured high frequency, canary result or song-load window. Automatic stock-clock recovery
worked; fresh project/library backups matched after every attempt and transport was restored.
An independent RTC watchdog covered the switch. No overclock at boot.

The same harness at **240 MHz** completed its canaries and 12 s heavy-IT / Full-reverb window:
**239.925 MHz** independently measured, **65.03 %** mean / **77.9 %** maximum published CPU,
**112** deadline overruns, zero new DMA underruns or guard drops; deliberate reboot110.
One further **320 MHz** attempt with flash/PSRAM dividers doubled also reset before measurement
(boot112). No usable speed gain demonstrated; this does not prove a CPU-only clock ceiling.
300/280 crash summaries identify the temporary ELF and cache-resume/panic paths; retained old
dumps after 320 attempts are distinguished in the evidence. Wi-Fi failure cause remains open.

Production source/config restored exactly; temporary endpoints removed. No voltage increase,
QEMU or long soak. Evidence and both unapplied patches: docs/PERFORMANCE.md and
docs/performance/2026-10-15-high-clock-trials.json.
Final normal firmware: boot **113**, OTA valid, **240 MHz**, image **1,744,704 B**;
project/library rechecked, playback resumed, temporary test endpoint absent.

## Overclock tried on the physical board — 2026-10-15

Temporary, manually triggered 20 s BBPLL520 / CPU260 experiment, stock voltage, automatic return
to 240 MHz followed by a deliberate reboot. CCOUNT against XTAL SYSTIMER measured 260.0 MHz.
I2S was XTAL-clocked in both comparisons: approximately 44,098 frames/s, unchanged playback rate.
Same heavy IT window with the user's current **Bright Plate / Full** reverb: mean render load
**63.71 → 57.57 %**, roughly 20 s deadline overruns **185 → 74**, zero new DMA underruns in
either window. A 64 KiB PSRAM canary passed. Slightly offset publication windows and the loss of
network work limit attribution; this is a short experiment, not a stability claim.

**Rejected for normal use:** HTTP/WS unreachable throughout overclock; restored after automatic
stock-clock reboot (103 → 104). No higher clock or voltage attempted. Temporary firmware code
and endpoint removed; pre-experiment production source/config restored exactly. Project,
sample library and transport preserved, including the user's updated Full reverb settings.
Normal firmware rebuilt and deployed: boot **105**, OTA valid, **240 MHz**, **1,744,704 B**;
playback resumed and the temporary test endpoint is absent.
Evidence / unapplied patch and measurement details: docs/PERFORMANCE.md and
docs/performance/2026-10-15-ac-ootb-clock-trial.json. No QEMU.

## Dense IT on two cores — 2026-10-15

Tracker now mixes a part's voices on both cores (grouped by sample, ABI 1.3 parallel service, a
second tick buffer added after the join) and does a voice's per-part position arithmetic without
software doubles; the player skips the frame info's doubles. Same PCM: byte-identical to the
previous plugin over libxmp's test corpus and the user's song with all three interpolations,
chases, tempos, odd blocks, a real second thread and a declining host; ThreadSanitizer clean.
New SDK test `testTrackerParallel` (SDK checks **1442**); DAW host tests pass.

Board, the user's `ac-ootb.it` project as found, 12 s windows at the song's heaviest sections
(150 s / 268 s, found by a host scan) and at 180 s: dry **72-76 → 42-45 %** mean, **97-99 → 56-59 %**
at most, DMA underruns **6-11 → 0** per window; with its reverb **91-93 → 61-64 %**, **77-93 → 0**
underruns. The earlier 180 s window: **48 → 30 %** dry. Remaining: overrun blocks (36 dry / 188
with reverb per 12 s at 268 s, absorbed by the DMA ring), one 24-41 ms block within a minute after
each restart (KI-9). Final boot **128**, valid, image **1,751,072 B**, SHA-256
`48bf349999d9f3dfe7edf6834940f421f7b4a7cbfdb0212722a7dd5e308e419b`; project, library, transport and
the disabled reverb as found. docs/PERFORMANCE.md; evidence:
docs/performance/2026-10-15-ac-ootb-two-core.json. No QEMU.

## Dense IT follow-up: all proposed areas tried — 2026-10-15

Kept Tracker tick-tail prefetch at the original mixer boundaries, contiguous reverb delay-line
processing and optional parallel left/right reverb. ABI **1.3** adds a size-checked host DSP job
service sharing the existing worker, with serial fallback for busy / nested / absent workers.
No voices, tails, filters or interpolation removed; no extra audio latency. Slower linear kernel
unrolling / exact-zero branch and an inconclusive extra 8 KiB FAST tick buffer were reverted;
temporary profiling removed.

Board, `ac-ootb.it`, 12 s chased from 180 s, same user settings: whole-render mean CPU dry
**47.83 → 44.88 %**, Light reverb **64.38 → 57.93 %**; deadline overruns **62 → 17** dry /
**243 → 78** wet. Final windows: zero DMA underruns, audioLate or WS drops. Individual deadline
spikes remain; no full-song stability claim. Measured boot **101**; final boot **102**, valid,
image **1,744,704 B**, SHA-256 `ec8bcfd1a78f9096d7861f2439a3a18a21764bca1b52b2482460867a3c00e7be`.
The last image updates package notices with the same measured plugin archives; project / library and
transport restored, latest Light reverb disabled state retained. Full candidate table and
measurement limits: docs/PERFORMANCE.md; evidence: docs/performance/2026-10-15-ac-ootb-followup.json.

Native PCM byte-identical: Tracker **4,851,000** stereo frames, Full / Light reverb **1,323,000**
each (real song input, variable blocks / automation / freeze, real persistent worker). Passing
checks: SDK Tracker **230**, effects **127**, DAW session **383**, parallel engine **289**, ABI,
hardware build / package symbol checks. No QEMU.

## Dense IT playback: preserve every voice — 2026-10-15

Optimized the SDK Tracker's exact pitch / filter / step calculations and sample interior spans;
the hot voice array now requests about 23 KiB of internal RAM. Samples and other player data
remain in PSRAM. Host FAST allocation uses the existing LARGE fallback when a request would
leave less than 32 KiB internally. No new voice limit, shortened tails, removed filters, lower
interpolation or sample rate; original 256-frame mix boundaries retained.

`ac-ootb.it`, one Tracker channel, all song channels, same 15 s window chased from 180 s on the
board: mean whole-render CPU **66.49 → 47.21 %**, maximum one-second CPU **91.5 → 71.2 %**,
deadline overruns **627 → 105**. With existing Light reverb: **60.62 %** mean / **87.5 %** maximum,
**317** overruns. Zero DMA underruns / audioLate in these windows, but individual deadline spikes
remain: this does not establish glitch-free playback through the song. A smaller mixing-quantum
experiment was rejected and reverted (higher CPU, wet DMA underruns).

Deployed final firmware boot **94**, valid, image **1,738,848 B**, SHA-256
`90f210cc5f35d65c1b762216547264188032a1e8b4bf8e2033e0851ecd0990ac`.
Fresh pre-OTA project / library snapshot and transport restored unchanged, including the user's
later Light reverb settings (already enabled). Final 6 s check with those settings: **58.92 %**
mean / **70.6 %** maximum, **87** overruns, zero DMA underruns / audioLate / WS drops. Native
before/after PCM: **4,851,000 stereo float frames byte-identical**, four formats / five windows;
focused Tracker **230**, host session **383** passing checks. Vendor patch reproduces all seven
changed files; package archives match the builds. No QEMU / full-song soak test. Evidence:
docs/performance/2026-10-15-ac-ootb-optimization.json; interpretation in docs/PERFORMANCE.md.

## Module opening: keep only the opened song (user preference) — 2026-10-14

The user explicitly chose clearing **all previous module files on each open**, including module
files referenced by current / saved projects. Module files are now a single-song cache; WAVs,
SoundFonts and project documents remain. Reimport an older module to use it again; saving a
project no longer pins that file. This supersedes boot 88's least-bytes / protected-project policy.
Local uploads clear other modules before staging (`reclaim=modules`); cached / library opens call
`samples.keepModule`, keeping an identical selected file without rewriting it. If staging a changed
same-name module cannot fit beside its old copy, the old module can be discarded too. In-flight
uploads and playing guards remain. The obsolete sector-window eviction planner was removed.

Deployed boot **89**, valid, image **1,736,624 B**. Short direct-board / Chromium checks:
cached Go West → **3,179,835 B IT** (streamed) → Go West → cached Go West. Exactly one module
remained after each open; SF2 / WAV entries and saved slot documents stayed unchanged. Final
library free **7,114,752 B**; cached Go West reopened in **2.25 s**, with no upload or flash change.
The large IT took **23.92 s** including upload / probe / import; no full-song test. Go West 5 s
playback had nonzero output, zero new deadline overruns, DMA underruns, audioLate or WS drops.
Left Go West stopped. Focused native checks: samples **169**, session **383**, including cached /
streamed cleanup, references in saved projects no longer pinning files, failed deletes, same-name
replacement and pending-upload protection; hardware / web build pass. **No QEMU**.
Evidence: docs/performance/2026-10-14-module-single-cache.json.

## Module opening: initial full flash recovery (boot 88; superseded) — 2026-10-14

Fixed `GOWEST.mod` refusing to open: only 98,304 B remained in the 22 MiB flash sample library,
not a PSRAM leak. The initial boot-88 fix opted into reclaiming unused modules only when a contiguous
run is needed. Current and saved projects (autosave included), SoundFonts, WAVs and the previous
copy of a replaced file are protected; an impossible plan or unreadable saved document deletes
nothing. The planner minimizes whole-file bytes removed, including fragmented free sectors.
Fresh library size + CRC matching avoids uploads for identical reopenings and detects changed
same-size files. Ordinary sample uploads retain their existing non-reclaiming behaviour.

Deployed boot **88**, image **1,739,632 B**. Direct board / Chromium test: Go West → Go West →
Airwolf → Take On Me → Go West, all complete in 2.2–5.5 s; both identical Go West reopenings
made no upload or library change. Removed four unreferenced modules (DREAMS2.S3M, a-----06.it,
VOODOO.mod, FLT_.MOD), all archived locally with size / device CRC verified; SF2 / WAV entries
and saved slots unchanged. Go West 10 s playback: mean / max 1 s CPU **10.7 / 13.5 %**, output
peak 0.225; zero new deadline overruns, DMA underruns, audioLate or dropped WS frames. Left
complete Go West stopped; previous x-files2 project backed up locally. Native focused checks:
samples **194**, session **365**, tracker web **9** plus typecheck pass. Hardware build clean;
**no QEMU or long test run**, as requested. Evidence: docs/performance/2026-10-14-module-opening.json.
The user subsequently requested clearing all previous modules on every open; that policy replaces
the protected-file / least-bytes policy above (see the latest entry).

## Tracker modules: MOD, XM, S3M, IT through libxmp — 2026-10-14

The user asked for MOD / XM / S3M / IT import and playback with libxmp, compatible with
everything else. Implemented across both repositories and deployed to the board (boot 87):

- **SDK**: plugin ABI **1.2** — note chase, an opt-in capability: `DAW_CAP_NOTE_CHASE`,
  `DAW_NOTE_FLAG_CHASE`, `chase_mbeats` (sharing `slide_ms`'s place; layout unchanged, 1.1
  plugins see 1.1 behaviour). New **Tracker** instrument (`org.smhgzll.tracker`): libxmp 4.7.4's
  lite file set vendored at `5d4f6aa2`, its C library calls redirected by a force-included shim
  (host allocator, no stdio), patched three ways (`third_party/patches/libxmp-esp32-daw.patch`:
  a scan hook for the row map, exact tick lengths, mixing a tick in parts). A note plays the
  song; a chase continues it at the next row start with that row's tempo state; the project
  tempo speeds it up or slows it down; channel range, stereo separation, interpolation, loop.
  Tests: the same song written as MOD / XM / S3M / IT (`host/modgen.cpp`), bit-identical chases,
  tempo following, tick lengths exact to 4 samples over 11 s, refused files; locally (not in the
  repository) libxmp's 271 test modules and 960 mutated copies under a guard-page allocator with
  trapping UBSan: no fault, leak or NaN.
- **DAW**: the engine chases notes for plugins that ask (Play, Continue, Seek, mode change, loop
  and song wraps; `test_sequencer` `testChase`). The sample library takes modules (type 4,
  libxmp-lite's signatures; streamed over 1 MB). `samples.probe` reads a file through a plugin's
  read-only parameters in a temporary instance; read-only parameters are no longer stored in
  projects. Patterns, notes and clips may be 1000 bars (were 64; note starts 256), so a song is
  one note in one pattern. Browser: File > Import tracker module, drop on the browser, click a
  module in the library: upload with progress, probe, a new project or a new playlist track, the
  whole song on one channel or split per tracker channel / in groups of 4 or 8 (with its measured
  cost). Docs: docs/TRACKER.md.
- **Board** (user-approved OTA and measurement session; their project saved to slot 1 first and
  restored exactly after each run): loading took 12–26 s per channel with the first row map (a
  silent playthrough), 0.06–0.30 s with libxmp's scan. One-channel songs, 60 s each: 4-channel
  MOD 12.3 / 14.0 %, 21-channel XM 23.0 / 27.9 %, dense 32-channel XM 34.2 / 34.5 %, no late
  block, every error counter zero — after mixing in 256-frame parts; mixing a tick within one
  block had made the dense XM late 34 times a minute. Split songs still have late blocks (KI-20).
  Seeking inside a playing song: sound in the first status frame. A bug found there: the channel
  range was applied before the channel count was known (split channels all played everything);
  fixed with a regression test. Raw reports: docs/performance/2026-10-14-tracker-*.json.
- Tests: SDK host PASS (test_plugins 1,385); DAW host PASS (test_sequencer 102, test_session 313,
  test_samples 168, e2e 59, web unit 32); browser UI PASS (11 module checks); QEMU self-test,
  render MATCH, 76 network checks (the Tracker plugin on the Xtensa build), OTA + rollback PASS.
  Firmware image 1,734,736 B (55.1 % of the slot).

## Selective SF2 loading and complete Saragossa import — 2026-10-14

The user's selective-RAM proposal is implemented and deployed. Planned MIDI imports / stored
projects retain only selected SoundFont presets and their sample dependencies in one shared
compact file. The full source and preset menu remain in flash. Manual preset changes prepare
missing samples on the control thread; bank/program automation and legacy unplanned bindings
keep the full font. CRC checks scan in 4 KiB chunks; checked allocations and the 2 MiB reserve
remain. Phoenix memory (shared file + channel indices/voices): Alphaville **11.32 → 2.10 MB**,
Saragossa **9.32 → 1.81 MB**. Native original/subset render comparison: 510 presets and
47,001,600 stereo frames bit-identical.

Saragossa's separate disconnect was a fragmented browser message, not its slot estimate or an
MT-32 allocation reset. Firmware now joins TEXT / CONTINUATION through FIN (aggregate ≤ 256 KiB).
Compact default note tuples reduce its largest operation 87,116 → 59,576 B; 100 KB fragmented
ping regression passes on the board. Real UI imports complete: Saragossa **9 / 144 / 144 /
13,997** channels/patterns/clips/notes, Alphaville **15 / 97 / 210 / 6,333**. Controlled Song
windows from tick 0: Saragossa 60 s **36.7 / 54.3 %**, Alphaville 30 s **40.3 / 50.7 %** mean/max,
all ten audio error deltas zero, no edits/reboot/library changes. These are not full-song tests.
Final firmware boot 81 preserves the complete Saragossa project and sample library.
Host/core/web tests and QEMU self-test / cross-target render / 54 network checks pass; see
docs/PERFORMANCE.md and selective-SF2 raw reports for details.

## MT-32 warm import: reusable file buffers — 2026-10-14

Fixed the user's 15-channel Alphaville import resetting at its first Phoenix MT-32 binding:
`std::vector::resize` aborted when no contiguous 6.3 MB block remained, despite 16 MB free.
Checked allocation alone prevented the reset but still refused a later warm switch; retaining
unreferenced file buffers, with bounded headroom for nearby font sizes, lets both TimGM → MT-32
browser imports complete (boot 79 unchanged, 15 channels / 97 patterns / 210 clips / 6,333 notes).
Failed bindings now return an error and retain the old channel. Existing samples are preserved.
Host / web / QEMU checks pass. The 280 s live playback observation has 9 deadline overruns,
0 CPU-guard drops and 0 DMA underruns; user edits and a near-end pause prevent a controlled
complete-song comparison. The MT-32 project and user edits remain on the board; its autosave
is 28,456 B in a 128 KB slot. See docs/PERFORMANCE.md and the 2026-10-14 raw reports.

## Real-world MIDI files: reader, SoundFont channels, deflated slots — 2026-10-13

A folder of 1,042 pop / dance MIDI files (the user's, not in the repository) showed why imports
failed or skipped notes. Fixed and verified on Linux, in QEMU and **on the board**: three songs of
9–15 TimGM6mb parts imported through the board's UI play at 33–37 % mean / 41–56 % maximum with
every error counter at zero (docs/PERFORMANCE.md, "GM songs with TimGM6mb"):

- **Reader**: a SysEx event was skipped one byte short (`p += vlq()` reads `p` before `vlq()`
  advances it), shifting every later event: a third of the files failed ("longer than the
  playlist"). Now 1,038 read with exactly the notes and song ends of an independent reader
  (mido); one RIFF and one corrupt file are refused. `smf.test.ts` +1 (fails on the old reader).
- **SoundFont channels** (SDK): voices in LARGE memory (FAST 5,428 → 56 B per channel). On the
  board a nine-part GM import stopped at its eighth channel ("not enough internal RAM"), leaving
  an empty project. The TinySoundFont voice loop runs most samples (96.4 % with TimGM6mb) without
  per-sample end / loop tests and caches the envelope's `powf`: 137 renders of every TimGM6mb
  program and nine kits are byte-identical to the previous loop; the inner loop is 26 / 34
  instructions (without / with low-pass) with a zero-overhead loop. A Voices parameter (1–24);
  a note finding no free voice takes the oldest instead of being dropped (TimGM6mb's String
  Ensemble 1 plays five samples per note for 2.6 s after it ends: four held notes filled all 24
  voices, and later notes were silently dropped).
- **Import**: SoundFont parts get sample voices by their notes (4×, 6–24; drums ≥ 8), fewer first
  when the estimate does not fit; the dialog estimates the deflated size.
- **Slots store the project deflated** (ROM miniz on the device, zlib on Linux; header version 2,
  version 1 still loads): imported songs of 220–390 KB of JSON took 26–53 KB. `test_storage`
  2,417 checks (power cuts, v1 halves, corrupt streams), QEMU self-test (autosave across a
  restart) and e2e (a 282 KB project saved deflated and loaded back with all 6,080 notes).
- **Large projects reach the browser**: a state frame larger than the device's 192 KB per-client
  queue (the `welcome` of a ~190 KB+ project) was always dropped and re-sent forever, so a
  browser could not load such a project. It is now queued alone; found by the new QEMU e2e check.
- **Compact projects**: rows without content and all-off steps are left out, notes without
  trailing default fields, steps / notes written as JSON text (no cJSON node per number). A
  15-part song's snapshot had come out `null` (the tree did not fit in PSRAM next to 15 SoundFont
  channels) and the autosave stored it; its JSON is 176 KB now (297 KB), and a failed
  serialization is never stored. The browser fills the left-out fields in.
- **Many-part songs**: a 23-part GM song imported as 23 SoundFont channels left 1 MB of PSRAM
  (no sound, no snapshot). The import merges parts played by one GM instrument (default above 16
  parts: 23 → 11 channels, 25.1 / 38.5 %, zero errors on the board); the device refuses a new
  instrument below 2 MB of free PSRAM; cJSON allocates from PSRAM only (an import next to a large
  song had taken internal RAM down to 28 B). Web unit 24, browser 77 pass.
- **22 MB sample library** on 32 MB boards (`config/partitions-32mb.csv`, flashed once over USB;
  projects, settings and existing samples kept) and **streamed SoundFont uploads** (no PSRAM copy;
  `test_samples` +33 checks: byte-identical to a whole put, aborts, power cuts, replacement). On the
  board: Phoenix MT-32 (6.3 MB) stored across the 16 MB boundary in 41 s; TimGM6mb kept.
- daw-host closed HTTP connections that took more than a millisecond to read (an unsigned
  wrap-around in the idle check): sample uploads of several MB failed on Linux only.
- Tests: SDK 1,154 plugin checks; DAW host suites (test_storage 2,417, e2e 53), web unit 23,
  browser 77, QEMU self-test / render MATCH / e2e / OTA + rollback all pass. Board: five OTA
  updates (boot 62–66, all valid), 221 / 176 KB projects autosaved deflated and restored after
  restarts. Final image 1,584,464 B. The import's load estimate is conservative (61–74 %
  estimated against 41–56 % measured).

## SF2 MIDI import / stale CPU admission — 2026-10-13

- Fixed project replacement retaining the previous song's one-second load. Audio-thread
  ResetLoad and a completed render fence prevent a new MIDI import's first channel being
  refused at the old 85 % figure. 75 % admission / 90 % note guard remain active for the
  current project; lifetime error counters are retained.
- Regression reproduces six failures before the fix; final 260 session checks pass,
  including four SF2 additions, old guard recovery, empty → empty replacement, and current
  overload still refusing additions. All host, web/network/MIDI and QEMU checks pass.
- Hardware warm test: 76.2 % old load → 0 % empty load; all four TimGM6mb channels bound.
  Project/library restored exactly. Current hardware boot 57 / ota_1 self-confirmed valid.
- E1M1: four SF2 channels, 39 patterns/clips, 2,332 stored notes, about 109 KB. User's Light
  drum reverb retained. 180 s Song playback: 59.9 / 83.7 %, zero guard skips, overruns,
  DMA underruns and other error deltas. Raw records: docs/PERFORMANCE.md, "MIDI import".

## 3xOSC guitars — 2026-10-12/13

- Deployed oscillator/envelope tiles, PolyBLEP FastDiv, clamped filter cache and local voice
  state. Only sample kernels allow Xtensa multiply/add contraction. Presets, six/eight-voice
  limits, envelope times, control period and 44.1 kHz remain unchanged.
- Final priority-24 hardware Full run (180 s): 72.9 / 85.8 % elapsed render load,
  406 guard skips / 146 overruns / zero DMA underruns. Still exceeds capacity in peaks.
- User-approved Light trial (180 s): 66.9 / 83.5 %, 172 skips / 1 overruns / 0 DMA underruns.
  Full restored as requested because skips persisted. Project/library exactly preserved.
- SDK: 1,572 reference cases / 21,675,784 stereo frames bit-identical natively; simulated
  target math maximum 1.5 PCM24 units, error/signal -139.54 dB. Final native E1M1 100 s WAV
  byte-identical. SDK, DAW host and final QEMU render/network checks pass; initial-candidate
  QEMU OTA rollback passes. Current hardware boot 56 / ota_0 self-confirmed valid.
- Reports include all unsuccessful candidates and explain unequal playback windows:
  docs/PERFORMANCE.md, "3xOSC guitars". CPU-guard calibration and dense peak load remain open.

## Drums and reverb, faster DrumLab, Reverb Light, MacroSynth on both cores — 2026-10-12

- An imported rock song with a Reverb on its drums skipped notes on the board (68.7 % mean,
  72 skipped notes in 100 s): DrumLab → Reverb is one chain on one core.
- SDK: DrumLab's DaisySP models without libm or `__divsf3` calls per sample (polynomial sines,
  `FastDiv` = a reciprocal seed from the bits + three Newton steps on the S3, inline filters, no per-sample `memcpy`):
  DrumLab 32.9 → 24.0 %, host renders within 1-8 LSB. MacroSynth's Braids noise generator is
  per instance (byte-identical for one instance), so the DAW's MacroSynth-on-one-core rule is
  gone. Distortion's ADAA without `__divsf3`. Reverb `quality`: Full (default, byte-identical) or
  Light (network at half rate, 21.9 → 13.9 %, wet content above ~8 kHz reduced).
- Result on the board, whole song: 58.4 % mean / 72.6 % max with the Reverb at Full, 50.7 / 63.8
  with Light; no skipped note, overrun or underrun.
- MIDI import: GM overdriven / distortion guitars get a Distortion on their insert; the load
  estimate counts effects and drops automatic ones last when the song would not fit.
- Tests: SDK test_plugins 762 (Light reverb), DAW test_parallel 289 (two MacroSynths on both
  cores, bit-identical to serial), web unit 21, browser +3 (guitar effects).

## MIDI file import — 2026-10-12

- Standard MIDI Files (formats 0 and 1) are read in the browser (`web/src/midi/`) and built with
  the ordinary editing operations (docs/MIDI.md, "MIDI files"); the device only gained an optional
  `length` for `pattern.add`. **As a project**: a channel per part with a GM-matched built-in synth
  or a SoundFont preset, patterns of a few bars with repeats shared, a playlist track per part,
  tempo automation, markers, Song mode. **Into the piano roll**: chosen parts and bars into the
  edited channel, replace or add, GM drums onto DrumLab, one undo step.
- Volume / expression changes become note velocities, sustain lengthens notes; pitch bend and
  other controllers are listed as not imported (KI-19).
- Tests: 21 web unit tests (reader incl. 3,000 corrupted files, arrangement incl. a random-song
  property test that the playlist plays every note exactly), `test_session` 240 (+5), browser
  74 (+23: import as a project compared note for note with the file, voices and load estimate, piano-roll import and undo,
  a public-domain piece saved to a slot, a non-MIDI file refused). Host, browser and QEMU suites
  pass (see the test inventory).
- Example files: two original grooves (format 0 and 1, `tests/web/midigen.py`) and three
  public-domain Mutopia pieces (Joplin, Bach, Haydn) with provenance in `examples/midi/README.md`.
  "The Entertainer" (2,621 notes) imports as 41 patterns / 76 clips, a 78 KB project that saves.
- Limit found: a project slot holds ~128 KB of JSON (~2,500–3,000 notes); bigger imports play
  but cannot be saved (NEXT_STEPS 4).
- On the board: a 4-part rock file first imported with the guitars on MacroSynth at the preset's
  4 voices skipped notes (80 % load, 236 overruns in 15 s). The importer now sets each channel's
  voices to what its part plays at once (2 here) and estimates the load from measured costs,
  lightening automatic instruments above 75 %. Re-imported through the board's UI in 6.9 s; the
  whole song then played at 54.9 % mean / 66.6 % max with no overrun, skipped note or underrun
  (docs/PERFORMANCE.md, "MIDI import load").

## FM6 Electric Piano loop stutter fix — 2026-10-12

- Read-only observation reproduced stutter in the user's current **9-instrument / 3-FX**
  Song project (one FX bypassed, mono Pad muted). FM6 Electric Piano reaches **4 voices**
  alongside Kick, open Hat, Bass and active Delay/Limiter. In 30 s: **77.20 %** mean render
  load, **194 overruns, 5 I2S underruns and 20 guard-skipped notes**; FM6 alone averages
  **54.68 %**. The earlier overlapping-pad result used a different instrument arrangement.
- FM6 now renders 16-sample operator tiles with **512 B stack scratch**, preserving event
  offsets, envelopes, all 32 routings and the sample-ordered feedback groups in algorithms
  4/6. Fused phase wrapping and an interpolated quarter-wave sine table use **1,028 B shared
  internal data**; only the sample kernel goes in IRAM. No per-instance allocation increase,
  polyphony reduction, note deletion, envelope shortening or sample-rate change.
- Current project ran **120 s**: **64.95 %** mean / **70.3 %** max 1 s load; FM6
  **37.58 %** mean / **47.3 %** max with four voices. All ten lifetime and steady error counters
  are **zero**; lifetime worst render **5,264 µs < 5,804 µs**. JSON unchanged throughout this
  run; the user's mixer edits between earlier runs were kept.
- IRAM-only and unrolled variants still skipped notes and were superseded. Tiling alone
  passed **384 byte-identical** preset/algorithm/feedback reference renders but still dropped
  notes. The final sine lookup changes rounding: tested sine error stays below **1e-5**;
  the actual 24 s song differs by at most **9.54e-7** full scale, RMS error **-113.87 dB**
  relative to the reference. Final native serial and parallel output remain byte-identical.
- SDK ABI, **350 DSP / 754 plugin checks**, all 19 plugin validators and reference renders
  pass. QEMU boot/render/persistence/network passes on the final image. Read-only Chromium
  profile: **6.84 % browser task load**, no JS errors/observer leak, mouse/touch window closing
  works and playing state stays unchanged. These are browser measurements.
- Current-project free internal RAM **56,319 B** (minimum **12,744 B**, largest **31,744 B**),
  PSRAM **15,189,340 B** free; audio/worker stack free **3,760 / 4,472 B**. Shared IRAM/data
  uses some headroom for throughput; per-instance voice storage is unchanged.
- Image **1,549,808 B**, SHA-256
  `e28c66bd34a27433c3f61754c0c3a80df26d4fd1efef18ce5cf2979714386b78`;
  physical OTA boot **27 / ota_0 valid**, project and Song position/playing restored.
  Reproduction fixture: `examples/performance/fm6-electric-piano-loop.json`.
  Raw evidence and final capacity regressions: `docs/PERFORMANCE.md`, `docs/performance/`.
- Final regression: 4/6/8 live examples **59.66 / 60.28 / 62.76 %**, and simultaneous three
  synths **73.17 %**, all ten startup/steady counters zero in 15 s windows. Full six-operator
  FM6 at eight voices and the 20-FX dense arrangement still exceed capacity; all **28/28**
  dense instances load. Neither overrun nor skipped-note averages are treated as passes.
- Found and fixed a warm-project RAM peak: the dense project still occupied internal memory
  while its replacement was constructed, so returning to the user project initially left
  FM6/Limiter unloadable. Session now waits for a stopped render-block fence, reclaims retired
  instances and skips empty FX-chain messages before constructing the new project. Native
  Session **235 checks**, parallel **44**, full host network **50** and MIDI **6** pass.
  A second dense probe returns **`restored: true`**, with all user DSP restored.
- **Final deployed image 1,550,320 B**, SHA-256
  `497cf8ed5d5f56c851f2c5e4e6c0f7650bf40de1284836913b7daf07769f281a`;
  OTA boot **29 / ota_1 valid**. The 120 s figures above use the preceding DSP-identical image;
  the final image additionally contains the project-transition memory fix.
- Final clean boot **30 / ota_1 valid**: the latest user project now also includes Clap.
  **90 s at 68.84 % mean / 74.6 % max 1 s load**, FM6 **37.68 %** mean with four voices.
  All **12/12** instances loaded, all ten lifetime/steady counters **zero**; worst render
  **5,486 µs < 5,804 µs**. Export identical within the window, **Song/playing** retained.
  Internal free/min/largest **56,311 / 12,732 / 31,744 B**, PSRAM free **15,189,300 B**;
  audio/worker stack free **3,952 / 4,472 B**. Final QEMU boot/render/persistence/network passes.
  Raw final check: `docs/performance/2026-10-12-final-user-loop.json`.

## Simultaneous synth / overlapping-pad fix — 2026-10-11/12

- The user's **Song-mode** kick/open-hat/bass/pad arrangement reproduced the remaining failure:
  45 s at **76.07 %** average, **68 guard skips / 39 overruns** in the steady window. Earlier
  selected-pattern and alternating-lead runs did not cover overlapping pad chords.
- Keep every note, original 1.8 s pad release, polyphony and **44.1 kHz / 240 MHz**. One
  persistent second-core worker overlaps instrument rendering. Join at the original slot keeps
  sample offsets and mix accumulation byte-identical. Swaps/effects/routing remain on the audio
  task; jobs finish before retirement. MacroSynth callbacks sharing Braids globals stay serial.
- Final deployed image: original song **58.42 %** mean / **71.6 %** maximum 1 s load over 45 s;
  simultaneous Fat Bass/Pluck/Warm Pad **74.51 %** mean / **77.9 %** maximum, 45 s.
  All **10/10 and 5/5 instances** loaded, pad reaches **8 voices**, all ten error counters
  zero (original song: lifetime as well as steady). Previous worker versions also pass 60/75 s
  per arrangement. Worker builds use the monotonic timer, including join waits, for the deadline.
- Added `chords-overlap.json`, `three-synths-overlap.json`; runner supports Song mode, checks
  scheduled channels, and can reuse sample resources without writing flash.
- Waveform block dispatch was slower and reverted. Full 3xOSC render in 10,760 B IRAM alone
  still skipped notes; in the first worker build it also reduced dense-load capacity. Final
  IRAM contains **only the 6,304 B voice sample loop**; controls remain in flash. All **28/28**
  dense-test instances now load. That sustained 20-effect arrangement still exceeds the deadline
  and is recorded as a failure. Existing 4/6/8 live grooves pass at **60.23 / 60.57 / 62.84 %**
  in 15 s windows on the first worker image (alternating leads; different from the synth test).
- Worker reserves 6 KB stack + 2 KB PCM; original-project internal free RAM approximately
  **63 KB**, minimum **19,420 B** after clean boot, PSRAM free **15.90 MB**. This trades some of
  the previous pass's ~78 KB internal headroom for actual concurrent processing.
- Native worker regression: **44 checks**, including shared routing, parameter events,
  replacement/removal, transport/panic and shared Braids state. Nine 16 s arrangement renders
  and both 24 s chord renders match the pre-change serial engine byte for byte. SDK/core/network/
  MIDI tests pass; QEMU boot/render/persistence/network pass with the compact kernel. The worker
  architecture also passes the 16 OTA/rollback checks; final physical OTA confirms itself.
- Final image **1,544,880 B**, SHA-256
  `5dfd1476997c5fcdfe3490b2efa3471e90b7a462e59e998d109d9030df2c2f44`; OTA boot **22 / ota_0 valid**. Clean boot **23** restores the original project,
  Song mode, position and **playing** state; export identical, all ten lifetime counters zero.
  Raw evidence and remaining limits: `docs/PERFORMANCE.md`, `docs/performance/`.

## Full instrument / mixer / memory pass — 2026-10-11

- All **18 firmware plugins** covered: 8 instruments + 10 effects. Real-board matrix: 136
  default/mode/model/algorithm/wave/polyphony/arrangement cases, **129 clean passes**. All
  default plugins pass. FM6 at 8 voices and six dense sustained mixer arrangements still exceed
  capacity; their dropped-note averages are not successes. Three heavy MacroSynth models
  additionally pass at 4 voices. Native catalog: **241 audible cases**, no real-time violations.
- DSP/common compiler, cached oscillator/filter controls, mono Sampler filtering, safe silent
  effect paths, exact settled gain handling, PSRAM event/queue placement and borrowed SoundFont
  PCM16 are implemented. No reduced default algorithms or global fast-math. Failed plugin
  instantiation is now visible in its editor. Internal free RAM rises roughly 30.5 KB → 78.3 KB
  in the preserved user project; raw project snapshots have different client activity.
- Generated nine 4/6/8-track projects plus WAV/SF2 fixtures in `examples/performance/`.
  Each **six-effect live project ran 120 s** on the final firmware: 4 tracks **65.97 %**,
  6 **65.88 %**, 8 **68.28 %** mean DSP load; every channel audible, every expected instance
  loaded, ten startup/steady error counters unchanged. Leads alternate with short releases.
  Separate dense files deliberately test the limit (12/16/20 effects).
- 32 kHz trial: live examples **48.08 / 48.14 / 49.83 %**, no errors in 12 s windows; dense
  chains still fail. `DAW_SAMPLE_RATE`/`DAW_HW_BUILD` support an optional capacity build.
  The board was returned to **44.1 kHz**, CPU **240 MHz**, flash/PSRAM **80 MHz**.
  The actual clock driver rejects 280/300/320 MHz configurations; no unsupported clock was set.
- All-render IRAM and transposed Reverb experiments were discarded: internal RAM cost without
  useful board timing gain. Safe pristine Reverb suspension remains, without extra FAST scratch.
- 236/241 catalog WAVs exactly match; five SoundFont cases differ by ≤4 units of 24-bit PCM,
  RMS error 8.02e-8 full scale. A large-font held note crosses sample 130,000 / a loop wrap with
  ≤2-unit difference; plugin-owned LARGE memory 534,616 B → 400 B (raw font excluded).
  All nine 16 s DAW fixture renders match the previous engine exactly.
- Piano roll/Playlist/Automation observers persist across edits; live drawing coalesces and
  pauses in hidden tabs. Mouse/touch window X close works. Native/core/network/MIDI tests,
  51 browser checks + five viewport screenshots and QEMU boot/render/persistence/network/16
  OTA checks pass. SDK: ABI 1.1 / 345 DSP / **626 plugin** checks, validate ×19.
- Final image **1,543,536 B**, SHA-256
  `2637dc70bb95d3bad55b3dd508894de7c17e1e8a6255ad748f15b257edb572e7`.
  OTA boot 15 / `ota_0` confirmed valid; exported original project preserved exactly. Detailed
  settings, raw reports and capacity limits: `docs/PERFORMANCE.md` and `docs/performance/`.
  A clean boot 16 then passes 75 s on the original selected pattern: **20.57 %** mean
  DSP, all 10 expected instances loaded, every lifetime/steady error counter zero, original
  project JSON identical and playback running. Final-image UI profile during playback:
  6.44 % Chromium task time over 30 s; mouse/touch close, no errors/observer leak.
  Analog recording/listening, 30-minute/four-browser/thermal tests remain unverified.

## M0 — research and platform

- ESP-IDF v5.5.5 (existing installation, unmodified), xtensa-esp-elf GCC 14.2, Espressif QEMU 9.2.2
  verified; `scripts/doctor.sh` checks all of it.
- Measured PCM5102A wiring (GPIO16 / 6 / 7) adopted as the Kconfig defaults; printed by every
  hardware build.
- QEMU constraints found and worked around (KNOWN_ISSUES KI-1..3).
- Two independent repositories: `esp32-s3-daw` (GPL-3.0-or-later), `esp32-daw-plugin-sdk` (MIT).

## First hardware run — 2026-10-11

- Flashed firmware 0.1.0 over `/dev/ttyACM0`; all written regions passed esptool hash verification.
  The connected ESP32-S3 rev v0.2 has **32 MB octal flash and 16 MB octal PSRAM**, rather than
  the reference profile's 16 MB flash. `config/local.env` selects `DAW_FLASH_SIZE_MB=32` and
  `DAW_OCTAL_FLASH=1`; flash / PSRAM run at 80 MHz, flash in OPI DTR mode. The existing 16 MB
  partition map is retained, leaving the upper 16 MB unused.
- Boot PASS: PSRAM memory test, 18 linked plugins, I2S0 at 44.1 kHz on GPIO16 / 6 / 7, session
  and web server initialized without a panic. Analog output has not been listened to or measured.
- STA connected to the configured network and obtained a DHCP address (local address omitted).
  Eleven smoke checks passed: HTTP status, embedded UI, project JSON, admin login, STA settings,
  WebSocket welcome / ping, Chromium UI (8 channel rows, no JavaScript errors), stable boot,
  valid OTA image state, and zero underruns / overruns. No project edits or playback were made.
- At 120.6 s uptime, stopped: render load 31.6 %, internal heap 32,131 B free (minimum 7,796 B),
  PSRAM 15,957,684 B free; underruns / overruns / audioLate all 0. This is an initial stopped
  baseline, not the H3 / H4 playback or per-plugin benchmark. H1 PASS; H7 PARTIAL (STA only).

## Hardware kick CPU fix — 2026-10-11

- Reproduced the user's single-kick overload: analog Kick used 55.7 % of the audio-core block
  budget, total load averaged 88.9 % and fast load reached 129.5 %. The CPU guard had skipped
  319 notes; underruns and overruns were increasing. Kick 2 also cost about 45.1 % per instance.
- Optimized the SDK's DaisySP kick models: cache coefficients until parameters change; update
  the analog filter directly from Plaits Q, removing redundant damping and general exponentiation.
  Rebuilt / imported DrumLab with all 18 plugins retained. Twelve reference renders (six per
  model, varying decay / tone / tuning) matched the previous 24-bit PCM byte for byte.
- Loaded the final firmware by authenticated Wi-Fi OTA; the inactive slot booted and confirmed
  itself valid. Exported project JSON matched exactly before / after the update and after restoring
  the temporary benchmark settings. Firmware SHA-256:
  `6786a1c4323a9791c61b385ace103b96bca0b6412e7a0afc2ba79da5cc7fe4b9`.
- Same analog kick settings as the fault report: **48.8 % total / 15.2 % kick CPU**, 25 s playback.
  Current Tight preset (Kick 2): **51.2 % total / 17.6 % kick CPU**, 35 s playback. No underruns,
  overruns, guard skips, note / event drops or NaN resets in either run. Channel and master meters
  showed audio. The original Tight preset and project were restored, with playback running.
- SDK ABI / DSP / plugin tests and validation passed; DAW core tests, 50 network e2e checks and
  6 MIDI bridge checks passed. H3 / H4 / H9 have partial hardware coverage; DAC listening,
  long playback, the rest of the plugin matrix and hardware rollback still need verification.
  The project's roughly 32 % stopped baseline was addressed in the follow-up below.

## Hi-hat CPU and window fixes — 2026-10-11

- Reproduced Kick 2 + closed hi-hat overload in the user's unchanged 8-channel project:
  total render load 120.9 %, hi-hat 76.2 %, increasing underruns / overruns and CPU guard skips.
- Cached DaisySP hi-hat coefficients, filter configurations and metallic oscillator increments
  until parameters change. Replaced the Limiter's per-sample 64-value minimum scan with a
  monotonic queue. Empty Delay lines skip PSRAM access while advancing their timing / smoothing;
  existing tails still render. Silent mixer tracks skip zero-sample mixing, preserving gain /
  send smoothing and meter decay; effects continue to render for tails and generated signals.
  The same coefficient / filter caching also covers both snare models, whose repeated
  exponentiation / filter setup otherwise causes overload when trying other DrumLab sounds.
- Eight hi-hat, two Limiter and four Delay reference renders matched previous 24-bit PCM
  byte for byte. The complete demo song (16 s) and user's drum pattern (8 s) also remained
  byte-identical after all plugin / mixer changes. Delay tests cover initial silence, waking
  on an impulse, the delayed echo and waking again after the lines fully drain.
  Eight additional snare reference renders also matched byte for byte; both complete project
  renders and all SDK / DAW host tests passed again after importing the final DrumLab package.
- Hardware stopped CPU fell **31.6 % → 9.8 %**. Kick + hi-hat averaged **41.6 %** (41.4–42.0 %),
  down from 120.9 %; hi-hat **76.2 % → 10.2 %**, Kick 2 17.6 %, Limiter 3.5 %, idle Delay 0.8 %.
  Mean render time 2,417 µs, lifetime maximum 2,948 µs against the 5,804 µs budget. During a
  75 s playback run, all ten error counters stayed at zero and both channel meters showed audio.
  All 18 plugins remain linked; no effects or project settings were disabled for measurement.
- Fixed floating editor title-bar pointer capture swallowing clicks on the close button,
  including its SVG child. Dragging still works; cancellation clears the drag state. Also fixed
  piano-roll / playlist playhead following recursing before layout or in a narrow canvas when
  opening the page during playback.
- Authenticated Wi-Fi OTA confirmed a healthy new image and preserved exported project JSON
  exactly. Later edits / sound experiments were preserved; the user selected Bassline. A final
  75 s run on the final image averaged 23.3 % CPU with Bass playing, all ten error counters zero,
  and playback running. The selection was not forced back to the drum pattern. Detailed
  measurements and the final firmware hash are in docs/PERFORMANCE.md.
- SDK ABI / 345 DSP / 614 plugin checks and validation passed; DAW core tests, 50 network
  e2e checks and 6 MIDI bridge checks passed. Browser coverage includes title dragging, close
  button clicks, touch closing, and reload / narrow layout during pattern and song playback.
  All 51 browser checks passed. Six targeted checks also passed on the real board during
  playback (title drag, mouse / touch close, FX close, reload, no JavaScript errors).

## M1 — audio foundation

- I2S0 master TX, Philips, 44.1 kHz, 32-bit slots / 24-bit data, DMA 4 × 256, auto-clear
  (`components/daw_hal/audio_out.cpp`); null backend for QEMU; ALSA + null + offline WAV on Linux.
- Audio task on core 1, priority 20, internal-RAM buffers; sample clock = rendered blocks.
- Test signals: 440 Hz, left / right, −60 dBFS, silence, click; diagnostics (CPU, render time,
  underruns, heap, per-plugin CPU / memory).
- Tests: engine unit tests, offline render identical on Linux and Xtensa-in-QEMU.
- UNVERIFIED: sound from the DAC (docs/HARDWARE_TESTS.md H2–H3).

## M2 — browser infrastructure

- HTTP + WebSocket server (ESP-IDF httpd with own frame writer; POSIX server for Linux), protocol
  v1 with revisions, patches, backpressure, resync, origin checks, admin login.
- Browser shell: desktop (FL Studio–style) and phone (FL Mobile–style tabs) layouts; virtual and
  computer keyboard; tones from the browser.
- Wi-Fi AP (WPA2, random password if none configured) / STA with AP fallback, mDNS.
- Tests: network e2e (32 checks) against daw-host and against the firmware in QEMU; browser suite
  (29 interaction checks + 5 viewport screenshots) against both.

## M3 — core musical system

- Transport (play / pause / stop, count-in, metronome, tempo 20–300, time signatures), pattern
  model, channel rack (32 channels), step sequencer (≤ 128 steps / row, polymeter, swing,
  velocity / pan / pitch / accent / probability), pattern selection and queueing, sample-accurate
  drift-free scheduling, live input ordering.
- Tests: `test_sequencer` (75 checks), `test_session` (155 checks).

## M4 — plugin SDK

- C ABI 1.0, C++ adapter, DSP helpers, Linux test host, `dawplug` (list / info / render / bench /
  validate), `.espplug` packages with symbol checks, ESP32-S3 cross-builds, import into the DAW
  with ABI / checksum / symbol verification. Plugins carry their license texts.
- Plugins: Sine, 3xOSC, DrumLab (DaisySP, pinned + patched, provenance), Delay, Filter, Limiter,
  Utility, a C example (tremolo).
- Tests: SDK `test_plugins` (113 checks), `dawplug validate` for every plugin, reference renders.

## M5 — piano roll

- Canvas editor: draw / move / resize / delete / duplicate, selection, snap, zoom / scroll, pinch
  and two-finger gestures, velocity / pan / release / fine-pitch / probability lanes, slide notes,
  quantize / legato / transpose / chop / randomize, ghost notes, note properties, undo / redo.
- Tests: browser (draw, move, delete), engine (slide ordering), session (note operations).

## M6 — mixer and effects

- Master + 16 inserts, routing DAG (cycles refused), 4 sends, 4 effect slots with bypass / mix /
  order / presets, smoothed gains, meters, NaN containment.
- Tests: session (routing, effects, sends), browser (fader).

## M7 — song arrangement

- Playlist tracks, pattern clips with offset / length, move / resize / slice / duplicate / make
  unique, markers, song mode with loop region, automation clips (all automatable targets incl.
  tempo) created from any parameter (flat at its current value) and played back; a point editor
  (add / move / delete / curve tension / length / snap, values in the target's units, undo).
- Tests: engine (automation), session (clips), browser (create, add / move / delete a point, undo).

## M8 — additional plugin engines

- **Acid303**: Open303 (MIT, pinned) ladder filter, envelopes, accent, slide and measured cutoff /
  env-mod mapping, ported to single precision; PolyBLEP SAW303 / SQUARE303 oscillator, 2x
  oversampling with a halfband FIR, fixed note array (Open303's wavetables, unstable-in-float
  decimator and allocating note list replaced; plugins/acid303/PROVENANCE.md in the SDK).
  Tests: pitch, slide without retrigger, accent, filter envelope, note priority, sleep, extremes.
- **FM6** (original): six operators, the DX7's 32 algorithm routings, feedback, per-operator
  ratio / level / ADSR / velocity, vibrato. Tests: pitch, sidebands with modulator level, feedback,
  velocity, polyphony, all 32 algorithms stable at maximum settings.
- **Sampler** with the infrastructure it needs: plugin ABI 1.1 (resources: decoded audio or raw
  files the host owns, set on the control thread before activation), a power-safe sample library on
  the 6 MB partition (upload / list / delete over HTTP and WebSocket, refused while playing on the
  device), WAV decoding, a shared decoded-sample cache, project references by name with missing
  files preserved, the browser's Samples tab and the editor's sample selector. Tests: store with
  power cuts (92), session flow (21), e2e upload → Sampler sounds (6), browser upload → channel.
- **SoundFont** player: TinySoundFont (MIT, pinned) with its voice loop ported to single precision
  (matches upstream within 2.3e-5), fonts parsed on the control thread into PSRAM, no allocation on
  the audio path. Tests: SDK (21), e2e upload of an .sf2 → SoundFont channel sounds.
  Preset menu: the device reads a font's PHDR records piecewise from flash (`samples.presets`);
  the editor lists them and sets bank / program. Tests: parser incl. damaged files (16), e2e,
  browser (list + select).
- **MacroSynth**: the 48 models of Mutable Instruments Braids (MIT, pinned, unmodified) as a
  polyphonic instrument (up to 4 voices, 2 by default): run at twice the host rate with pitch
  correction (Braids' tables are for 96 kHz) and a halfband decimator, Strike on note-on for the
  physical models, Timbre / Color, ADSR. Oscillator state (17 KB per voice) in PSRAM; firmware
  +88 KB (tables). Tests: SDK pitch at 44.1 / 48 kHz, every model audible and bounded, struck
  models decay and restrike, reproducible noise; e2e MacroSynth channel sounds.
- **Effects** (original, MIT): Reverb (comb / all-pass network, pre-delay, freeze; PSRAM lines),
  Chorus, Flanger, six-band EQ, soft-knee Compressor, Distortion with anti-aliased clipping (ADAA),
  wavefolder and bit crusher. Tests: identity at 0 % mix, measured EQ gains, compressor static
  curve, flanger comb notch and feedback stability, reverb decay / freeze / mono width, aliasing
  of the clipper below −30 dB.
- Found and fixed on the way: the SDK adapter rounded negative integer parameters toward zero
  (a 3xOSC octave of −1 played at 0, transpose −12 at −11) — every plugin rebuilt;
  DaisySP's drum models and the delay did double-precision math on the
  audio path (software on the S3); the SDK build now warns about it.
- Resource usage on the board: both kick models measured and optimized; the remaining engine
  matrix is pending (KI-7, docs/PERFORMANCE.md).

## MIDI file import (beyond the milestones)

- Formats 0 / 1 as a project or into the piano roll, read in the browser; see the dated section
  above and docs/MIDI.md.

## MIDI recording (beyond the milestones)

- Record button: live notes while a pattern plays become piano-roll notes, timed by the engine at
  block resolution (not network arrival), optional input grid, overdub, loop-end wrap. Tests:
  session (11), browser (computer keyboard → note in the pattern).

## Device settings from the browser

- Wi-Fi mode / network / password, device name, admin password: `GET` / `POST /api/settings`
  behind the admin token (passwords write-only; a new admin password revokes tokens), stored in
  NVS on the device. Tests: validation (17), e2e (6, host and QEMU), browser (form saves).

## M9 — persistence and OTA

- Power-safe project slots (A/B halves, CRC, sequence numbers), autosave while stopped, schema
  versioning with migrations, missing-plugin preservation, export / import, factory reset.
- Browser firmware update: profile / chip / project checks, SHA-256 verification, health
  confirmation, bootloader rollback — all exercised in QEMU (`tests/web/test_ota.py`).
- Third-party notices served by the device (`/licenses.txt`).
- User presets on the device (512 KB partition, the same power-safe store), offered in the editor
  and the browser's Presets tab. Tests: session (14), browser (save → offered).
- Missing: full-device backup (all projects, samples and presets in one file).

## M10 — quality

- **Two-core scheduling** (2026-10-12, docs/PERFORMANCE.md): the worker also takes channel
  strips (instrument + its own insert chain) and effect chains at the same routing depth;
  routing stays in the serial order, output bit-identical (host fixture renders, threaded test
  under ThreadSanitizer, 200 checks). Board A/B against the previous image: user project
  68.8 → 64.5 %, three synths 73.9 → 66.0 %, dense 8-track underruns 832 → 0; chords and
  shared-bus grooves unchanged within ±1.5 points. New `workerWait` / `workerBusy` diagnostics.
  Found with them: an out-of-line `memcpy` in the NaN check and atomic library calls in the
  per-block setup (both fixed). NaN / Inf containment now has a test on both cores.
- Done: multi-client tests (sync, stale edits, reconnect storm at the client limit, slow client),
  MIDI burst stress, power-cut simulation of every byte of a save (2397 checks), mobile layouts.
- Measured: firmware size, web bundle size, heap in QEMU, Linux render speed, stopped hardware
  baseline, two kick models and Kick + hi-hat on the real ESP32-S3 (docs/PERFORMANCE.md).
- CPU guard and admission from the measured render load (KI-8, docs/AUDIO_ENGINE.md): new notes
  skipped above 90 %, new instruments / effects refused above 75 %, "CPU FULL" on the load meter;
  off under QEMU. Tests: session (simulated load: skip, refuse, recover; off = no effect), browser
  (meter). Thresholds UNVERIFIED on the board (H4).
- Missing: 30-minute / thermal playback, four-effect-copy capacity profiling and predictive
  admission, Wi-Fi AP / fallback / reconnection
  behaviour.

## Test inventory (all passing, 2026-10-14)

| suite | checks | runs on |
|---|---|---|
| SDK host tests (`esp32-daw-plugin-sdk/scripts/test-host.sh`) | ABI 1.2, 350 DSP checks, 1,385 plugin checks, validate × 20 | Linux |
| `test_project` / `test_render` / `test_samples` / `test_sequencer` / `test_session` / `test_storage` / `test_parallel` / `test_settings` / `test_sf2_subset` | 57 / 3 / 168 / 102 / 313 / 2417 / 289 / 17 / 23 | Linux (test_parallel also under ThreadSanitizer) |
| network e2e | 59 on daw-host (incl. a 282 KB project saved deflated and loaded back, a tracker module probed and played); 76 network checks in QEMU | daw-host, QEMU firmware |
| MIDI bridge | 6 | daw-host |
| web unit tests (MIDI file reader incl. SysEx, arrangement, load incl. SoundFont voices, tracker module headers and import plans) | 32 | node (Linux) |
| browser UI | 88 (incl. 11 tracker-module checks) + 5 viewport screenshots on current build; previous QEMU run 44 | daw-host, QEMU firmware |
| QEMU self-test + cross-target render | boot, render, persist, restart, restore | QEMU |
| OTA | 16 | QEMU firmware |
| hardware startup smoke | 11 | ESP32-S3, 32 MB octal flash / 16 MB PSRAM; H1 PASS, H7 PARTIAL; initial stopped checks only |
| kick optimization reference comparison | 12 renders, identical 24-bit PCM | SDK on Linux |
| SoundFont voice-loop reference comparison | 137 TimGM6mb renders (128 programs, 9 kits), byte-identical float output | SDK on Linux |
| hi-hat / snare / effect optimization reference comparison | 22 plugin renders + 2 complete project renders, identical 24-bit PCM | SDK / DAW on Linux |
| hardware kick playback | 2 models, 60 s total, no underruns / overruns / skipped notes | ESP32-S3; H3 / H4 PARTIAL; analog output not listened to |
| hardware Kick + Hat playback | 75 s, 41.6 % mean CPU, ten error counters unchanged at zero | ESP32-S3; H3 / H4 PARTIAL |
| final hardware playback / UI | 75 s current Bassline, 23.3 % mean CPU, ten error counters zero; six window checks | ESP32-S3, final deployed image |
| hardware firmware update | authenticated OTA, valid new slot, identical exported project | ESP32-S3; H9 PARTIAL; rollback not tested |

`scripts/test-all.sh` runs everything available on the machine and prints one summary.
