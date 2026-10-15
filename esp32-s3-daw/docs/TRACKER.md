# Tracker modules (MOD, XM, S3M, IT)

Tracker songs play through the **Tracker** instrument plugin (`org.smhgzll.tracker`, from the SDK),
which runs [libxmp](https://github.com/libxmp/libxmp) 4.7.4's "lite" player (MIT; MOD, XM, S3M and
IT only), with changes for its scan row hook, exact tick lengths, deferred mixing, exact
calculation reuse / hot voice storage, mixing on two cores and per-part arithmetic without software
doubles (the SDK's `plugins/tracker/PROVENANCE.md`). The DAW does
not convert a module into its own notes: libxmp plays the song as its tracker would, with all of
its effects, and the DAW places, mixes and processes that playback like any instrument's.

## In the browser

**File > Import tracker module (MOD, XM, S3M, IT)…**, a module dropped on the browser panel, or a
module in the sample library clicked there:

1. the file goes to the device's sample library (a library file like a sample; refused while
   playing, like every upload; over 1 MB it is written to flash as it arrives). Reopening the
   same name, size **and CRC32** reuses it without writing flash. The listing is refreshed first,
   so another browser's imports / deletes cannot leave this check stale;
2. the device reads it with the Tracker plugin (`samples.probe`): length, channels, orders,
   instruments, the song's tempo and the player memory it needs;
3. the song becomes **one note on one or more Tracker channels in one pattern with one clip**:
   as a new project (tempo = the song's, 4/4) or on a new playlist track of the current project.

Channels: the whole song on one channel, or groups of 4 / 8 tracker channels, or one DAW channel per
tracker channel (each with its mixer track, effects, mute and solo). Every DAW channel runs its own
copy of the player: memory and CPU grow with the number of channels (the dialog shows the memory).

Module opening uses a **single-song cache**, as requested by the user. Opening a file clears
**all other MOD / XM / S3M / IT files**, even with plenty of free space and even when current,
autosaved or saved projects refer to them. SoundFonts and WAVs remain. Project documents are
not deleted, but their older module files must be reimported before those projects can load them.
The local originals are the source of truth; saving a project does not pin its module in flash.

Local uploads request `reclaim=modules`, so old modules are cleared before staging a new file.
The selected same-name old copy is retained during replacement when there is room; if it blocks
staging, it is cleared too. An interrupted upload can therefore leave the old songs gone without
a new song. Cached / library opens call `samples.keepModule` before probing, so they clear other
songs without rewriting the selected song. Cleanup is refused while playing or while another
streamed upload owns a reservation. Ordinary sample uploads do not trigger this cache policy.

## How the song follows the DAW

- **A note plays the song.** Its NOTE_ON starts the song, its NOTE_OFF ends it (5 ms fade). The
  song is as long as the note; with *Loop song* on, a longer note plays it again.
- **Note chase** (plugin ABI 1.2): when the transport starts, jumps or loops inside the note, the
  engine sends the note again with how much of it has passed (`DAW_NOTE_FLAG_CHASE`,
  `chase_mbeats`), and the song continues from there — seeking and loop regions work as with audio.
  Only plugins that ask for it (`DAW_CAP_NOTE_CHASE`) get chased notes (docs/AUDIO_ENGINE.md).
- **Exact tick lengths**: libxmp renders a tick as a whole number of samples; the patched mixer
  carries the fraction to the next tick, so a song whose tick is not a whole number of samples
  (121 BPM: 911.16) stays on the project's beat grid instead of drifting (50 ms over 5 minutes).
- **Tempo**: the *Song tempo* parameter is the project tempo at which the song plays at its own
  speed. The import sets it, and the project tempo, to the song's first tempo (a quarter note =
  four rows at its first speed), halved or doubled into the DAW's 20–300 BPM (a speed-1 song at
  125 BPM: 750 → 187.5 BPM, eight rows per beat). Another project tempo, or tempo automation, plays the song
  faster or slower (libxmp's tempo factor: tick lengths change, pitches do not), and the song
  keeps its number of beats: it stays on the grid.
- **Seeking**: when a module loads, libxmp's scan of the song (its flow: speeds, tempos, breaks,
  jumps, loops, delays) runs once more and reports where every row starts, with its speed, tempo
  and global volume (0.06–0.3 s on the board for songs of up to 5 minutes and 32 channels). A chase
  continues at the **next row start** (silent until then, at most one row) with that row's tempo
  state. Notes that were sounding before that row are not restarted, and effect memory or channel
  volumes set earlier in the song are not restored (as when a tracker starts playing from a row).

## Parameters

| key | | |
|---|---|---|
| `tempo` | Song tempo, BPM | see above |
| `level` | Level | output gain (dB scale) |
| `stereo` | Stereo separation, % | libxmp's mix (100 = hard Amiga panning; default 70) |
| `interp` | Interpolation | nearest / linear (default) / spline: quality against CPU |
| `first`, `last` | First / last channel | the tracker channels this DAW channel plays (the others are muted) |
| `loop` | Loop song | a note longer than the song plays it again |

Read-only, hidden (`samples.probe` reports them, projects do not store them): `length` (s),
`song_tempo`, `channels`, `format` (1 MOD, 2 XM, 3 S3M, 4 IT), `orders`, `instruments`, `memory`
(KB the player took for this module).

## Limits

- **Formats**: what libxmp-lite reads. MOD files signed `M.K.`, `xCHN` (1–9) or `xxCH` (10–32);
  XM; S3M; IT (with compressed samples). Other MOD variants (`M!K!`, StarTrekker `FLT4` / `FLT8`,
  15-sample Soundtracker, Octalyser, ...) are refused at upload. Packed / compressed module files
  (zip, gzip, MMCMP, ...) are not read.
- **Length**: one pattern holds the song, and patterns, notes and clips are at most 1000 bars of
  4/4 (raised from 64 bars for this; a 30-minute song at 133 BPM). The dialog refuses longer songs.
- **Memory**: the module's file is loaded once into PSRAM (shared by its channels) and each
  Tracker channel holds libxmp's copy of the samples plus about 250 KB of player state and mix
  buffers (measured: 370 KB for a 121 KB MOD, 1.07 MB for a 428 KB XM), and about 90 KB more for
  the second core's tick buffer when the host offers two cores. Its hot mixer voice array
  requests about 23 KiB of internal RAM (24 KiB descriptor budget); the host falls back to PSRAM
  when internal RAM is short. Sample data remains in PSRAM. The device keeps 2 MB of PSRAM free; an import into the current project that would not
  fit is refused in the dialog.
- **CPU** (measured on the board, docs/PERFORMANCE.md "Tracker modules"): the whole render with
  one Tracker channel took 12 % for a 4-channel MOD, 23 % for a 21-channel XM and 34 % for a dense
  32-channel XM (the plugin's share 6.5 / 17 / 28 %), with no late block. libxmp runs its player at
  a tick's start and mixes the tick in parts of 256 frames as the blocks need them. A split song
  runs one player per DAW channel, and their tick starts land in the same block: a 4-channel MOD
  split 4 ways took 29 % and was late for 55 blocks a minute (the dialog warns).
  A mixer part's voices are mixed on both cores (grouped by sample, through the host's ABI 1.3
  parallel service; the output is the same as on one core), and a voice's per-part position
  arithmetic avoids the S3's software doubles. The user's dense IT `ac-ootb.it` (50 channels, up
  to 127 voices) measured at its heaviest sections (150 s / 268 s, 12 s each) 42-45 % mean dry,
  56-59 % at most, and 61-64 % with its reverb, with no DMA underrun; before, 72-76 % dry with 6-11
  underruns and 91-93 % with reverb with 77-93 underruns per window. Individual blocks still
  overrun (36 dry, 188 with reverb in 12 s; the DMA ring absorbs them). No voices or tails were
  removed; see docs/PERFORMANCE.md. Tracker prepares a tick's last fixed mixer part in a block
  without player work when possible; no extra audio latency or new voice ceiling.
- Seeking lands on row starts (above). Pattern loops (E6x / SBx) and pattern delays are part of the
  row map, but a chase into the second pass of a pattern loop starts that loop again.

## Protocol

`samples.probe` (`name`, `plugin`) → ack with `values`: the plugin's read-only parameters after
loading the file in a temporary instance that never reaches the engine (docs/PROTOCOL.md). The
library lists modules with `type: "module"`.

## Tests

SDK `testTracker` (the same song as MOD, XM, S3M and IT: lengths, chases bit-identical at row
starts, tempo following, note-off, loop, channel range, refused files, no allocation on the audio
thread), `test_sequencer` `testChase` (the engine's note chase), `test_session` (library type,
`samples.probe`, a Tracker channel played from the middle of its song), `test_samples` (module
signatures, streamed upload), web unit tests (`tracker/module.test.ts`), e2e (upload, probe, a
Tracker channel sounding; also in QEMU on the Xtensa build), browser (the import dialog end to end).
