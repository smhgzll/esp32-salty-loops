# MIDI and live input

All live input ends up in the same place: the session's `midiIn` / `liveNote`
(`components/daw_core/src/session.cpp`), which turns it into engine events for an instrument slot.
Live notes are tracked per client, so a disconnecting browser or bridge never leaves a note hanging.

## Sources

| source | path | target channel |
|---|---|---|
| on-screen keyboard (touch, mouse; multi-touch chords, glissando) | `note` messages | the selected channel |
| computer keyboard: `Z S X D C V G B H N J M` (lower octave), `Q 2 W 3 E R 5 T 6 Y 7 U` (upper) | `note` | the selected channel |
| piano roll keys, note preview while drawing | `note` | the edited channel |
| Web MIDI (desktop Chrome / Edge / Firefox with permission) | `midi` (raw bytes + timestamps) | the selected channel |
| `scripts/midi-bridge.py` on a Linux computer | `midi` | MIDI channel *n* → channel rack row *n*+1, or `--channel ID` |
| USB-MIDI device mode (the S3 as a USB MIDI device) | – | not implemented (ROADMAP) |

## Messages understood

Running status, real-time bytes interleaved anywhere, SysEx skipped (`MidiParser`).

| MIDI | effect |
|---|---|
| Note on / off (on with velocity 0 = off) | live note on the target instrument, velocity scaled to 0–1 |
| CC 64 | sustain (`DAW_EV_SUSTAIN`) |
| other CC | `DAW_EV_CC` to the instrument |
| CC 120 / 123 | all sound off / all notes off |
| pitch bend | `DAW_EV_PITCH_BEND`, ±1 normalised (the plugin applies its bend range) |
| program change | `DAW_EV_PROGRAM` |
| clock 0xF8 | external clock follower |
| start 0xFA / continue 0xFB | switch to external clock; start from the beginning / continue |
| stop 0xFC | pause |
| song position 0xF2 | seek (in 16ths) |

## Recording

The record button (top bar) arms MIDI recording. While the transport plays a pattern (pattern
mode, after any count-in), every live note — on-screen or computer keyboard, Web MIDI, the Linux
bridge — that reaches an instrument is written into the current pattern as a piano-roll note of
that channel (overdub; a step row is converted to notes first, so its steps keep playing).

Timing comes from the engine, not from the network: it records the pattern tick of the audio block
in which it played the note (block resolution, 5.8 ms), so Wi-Fi jitter does not move notes. The
input grid (right-click the button) quantizes the start to 1/32, a step, 1/8 or a beat, or keeps
it as played; the length is as played. A note held over the pattern's end wraps into the next
loop; a note still held when recording or the transport stops ends at the pattern's end.

## External clock

`ClockFollower` estimates the tempo from the time spanned by the last 48 clock ticks (two beats),
using the **sender's timestamps** (Web MIDI's `timeStamp`, the bridge's monotonic clock), so
WebSocket / Wi-Fi jitter in packet arrival does not become tempo jitter. A tick much later than
expected (dropped tick, pause) restarts the window; a duplicate is ignored. Once locked, the
project tempo follows when it differs by more than 0.25 BPM, at most every 250 ms, rounded to
0.1 BPM. `tests/core/test_session.cpp` feeds a clock with ±0.6 ms timestamp jitter and checks that
the tempo settles at the sender's 100 BPM.

## The Linux bridge

```sh
scripts/midi-bridge.py --list
scripts/midi-bridge.py --input hw:1,0 --host esp32daw.local       # a USB keyboard (raw ALSA device)
scripts/midi-bridge.py --input seq:20:0 --host 192.168.4.1        # an ALSA sequencer port (aseqdump)
scripts/midi-bridge.py --input - --host 127.0.0.1 --port 18190 < bytes.bin   # raw MIDI bytes on stdin
```

The bridge sends a live byte stream; a MIDI *file* (`.mid`) is imported in the browser instead
("MIDI files" below). Python standard library only (no python-rtmidi needed). It drops active sensing, batches whatever
bytes are available into one message (≤ 768 bytes), stamps them with its monotonic clock, drains
the DAW's status broadcasts so it is never treated as a slow client, and reconnects with backoff.
Raw devices need read access to `/dev/snd/midiC*D*` (the `audio` group on most distributions).
Tested by `tests/web/test_midi_bridge.py` (notes sound and stop, held notes released when the
bridge disconnects, aseqdump parsing) as part of `scripts/test-host.sh`; with a real keyboard:
UNVERIFIED (HARDWARE_TESTS H8).

## MIDI files

Changing SoundFonts during a project import reuses unreferenced file buffers (2026-10-14).
Previously, a warm TimGM6mb → Phoenix MT-32 import could reset at its first `channel.resource`:
16 MB free in total still lacked a contiguous 6.3 MB block. Checked allocation and retained
buffers fix that case; insufficient playback memory now produces an error instead of a reset.
Project-slot compression and the DSP estimate describe separate limits.

MIDI imports now send the complete preset selection for each SoundFont together with the first
binding. Firmware builds one shared compact font containing only those presets and their samples
(docs/STORAGE.md). The full font remains in flash and the preset menu still lists everything.
The Saragossa MT-32 import also exposed fragmented incoming WebSocket messages: its large
`notes.set` was split by Chromium, and firmware closed on the continuation. Firmware now joins
the complete message (≤ 256 KiB); default note tuples use five fields to reduce this song's largest
operation from 87,116 to 59,576 bytes. Both Saragossa and Alphaville import completely on the
board. Reload a cached browser page after updating firmware to use the new planned bindings.

Standard MIDI Files (`.mid`, `.midi`, `.kar`; formats 0 and 1, musical time) are read **in the
browser** (`web/src/midi/smf.ts`) and turned into ordinary editing operations, so the device needs
neither a file parser nor a privileged endpoint. Two ways in:

- **As a project**: File > Import MIDI file… (phone: More), or drop the file on the playlist or the
  sample browser. After a confirmation a new project replaces the current one:
  - a channel per *part* (a track and MIDI channel that has notes; at most 32), named after the
    track (format 1) or the GM program (format 0). *Channels* can merge the parts a GM synth plays
    with one instrument: one per MIDI channel and instrument (the separate drum tracks of a format
    1 file become one kit), or one per instrument (any channel or bank). Above 16 parts the import
    merges by default (`mergeParts`, `web/src/midi/arrange.ts`): a 23-part rock song became 11
    channels. Independent controller streams stay separate; original note velocity is retained;
  - its instrument: *Auto* picks the built-in synth closest to the part's GM program
    (`web/src/midi/gm.ts`: pianos → FM6 Electric Piano, basses → 3xOSC Fat Bass, strings and pads →
    3xOSC Warm Pad, guitars → MacroSynth Plucked String, brass → FM6 Brass, drums → DrumLab, ...),
    or, when a SoundFont from the sample library is chosen, the font's preset with the part's GM bank
    and program (drums: bank 128; the closest preset the font has). The font must follow the GM
    program order: an MT-32 font (Roland MT-32 order) plays GM files with the wrong instruments. Each part's instrument can be
    changed in the dialog;
  - effects that bring a built-in instrument closer to its GM original: the overdriven (29) and
    distortion (30) guitars get a Distortion on their mixer insert (soft / hard clipping and a
    cabinet-like tone low-pass); a SoundFont needs none;
  - its voices: as many as the part plays at once (the plugin's polyphony is set to that), since
    every extra voice costs CPU in release tails. A SoundFont part's voices are *sample* voices: a
    GM preset can layer several samples per note and ring on in its release (TimGM6mb's String
    Ensemble 1 plays five samples per note for 2.6 s after it ends), so a part with four notes at
    once kept all 24 voices busy. Such a part gets four times its simultaneous notes (6–24, drums at
    least 8) as its Voices; a note finding no free voice takes the oldest one. The dialog estimates
    the DSP load from costs measured on the board (`web/src/midi/load.ts`, docs/PERFORMANCE.md
    "MIDI import load" and "GM songs with TimGM6mb": about 1.1 % per sample voice, 60 % of a
    SoundFont part's Voices counted as busy); above 75 % it first gives the SoundFont parts with the most voices fewer
    (down to twice their notes, at least 4), then swaps automatic instruments for lighter ones
    (MacroSynth / FM6 → 3xOSC → Sine), then drops their automatic effects, and says so. Instruments
    chosen by hand stay;
  - the parts are cut into patterns of 1–16 bars (default 4) and laid out on the playlist, a
    playlist track per part. A cut moves to a bar line that no note crosses, so no note is
    shortened, and segments with the same notes share one pattern (`web/src/midi/arrange.ts`).
    When the device's 256 patterns or 2,048 clips would not suffice, longer patterns are used;
  - the first tempo becomes the project's, later changes a tempo automation clip (steps); the first
    time signature becomes the project's (1–16 / 2, 4, 8 or 16, else 4/4); markers become playlist
    markers. The project opens in Song mode.

- **Into the piano roll**: the piano roll's import button (or its menu, or drop the file on it). The
  notes of one or more parts, from bar *a* to bar *b*, replace or are added to the edited channel's
  notes in the current pattern, in one undo step. On a DrumLab channel GM drum keys play DrumLab's
  closest pieces (crash → open hat, ride → closed hat, congas → toms, ...). Optionally sets the
  project tempo, and makes a fixed pattern length automatic when the notes need more room.

Project replacement finishes retiring the old instruments and resets the measured audio load
before acknowledging the new project. A MIDI import that immediately adds its channels therefore
cannot be refused against the previous song's CPU average. The 75 % admission / 90 % note guard
still protect the new project's measured load; the sample library is retained.

MIDI 1.0 channel messages now keep their original timing and same-tick order. Notes retain
physical note-offs and release velocity; the other messages live in controller patterns that
run once over the song, independently of deduplicated note patterns. Control-only tracks and
SMF MIDI Port metadata share the correct channel state. Editing ordinary notes retains their
controller data; a piano-roll import replaces/adds notes and MIDI together in one undo step.
A range import replays its preceding controller setup before its first note. Independent MIDI
controller streams cannot be collapsed into one piano-roll channel; import them as a project.

| in the file | in the DAW |
|---|---|
| note on / off | editable notes, original velocity, release velocity and event order; a repeated still-held key ends the previous gate; missing note-offs end at the file end; minimum 1/128 note |
| pitch bend | full 14-bit value, timed within the audio block; Sine, 3xOSC and Sampler now support channel bend too |
| all CC 0–127 | retained and delivered; known controls are applied, unassigned controls remain data and are listed in the dialog |
| volume / expression / pan / balance | channel state, including LSB messages; updates affect held audio, without rewriting note velocity |
| sustain / sostenuto / soft pedal | SoundFont applies them live; engines without native pedals receive offline-computed note gates, without a pending-voice limit |
| RPN 0 / 1 / 2 | bend sensitivity, fine tuning and coarse tuning; data entry and increment/decrement; NRPN/other RPN data is retained, not an arbitrary synth mapping |
| bank / program change | timed SoundFont preset changes; all planned presets/sample dependencies prepared on the control thread, including on project reload. Held voices keep their old sample/preset. Fixed built-in presets keep their chosen sound |
| channel / polyphonic pressure | retained; SoundFont applies default pressure vibrato with private per-voice region copies, also used for modulation wheel |
| SysEx and system messages | bytes and timing retained, including SMF F0/F7 fragments; universal GM/GM2 reset, master volume/balance/tuning and checked GS/XG global reset/volume/transpose packets are applied. Manufacturer patch dumps, effects and device-specific commands require their original synthesizer; they are not emulated by loading an SF2 |
| tempo, meter, markers, names, text/lyrics, key signature and other metadata | original metadata retained; tempo automation, first project meter, markers/names displayed as before. Later meter changes are data; the grid keeps one meter |

Playback uses the file tempo map; embedded system clock/transport messages do not seize the
DAW transport. The dialog distinguishes data kept from commands the selected instrument applies.
Capacity/frame/preset/playlist failures are checked before replacement where the browser can
predict them. Very dense tempo maps split into more automation clips rather than silently
thinning events; maps beyond the device limits are refused. Existing 20–300 BPM, polyphony,
admission and slot limits remain. Existing imported projects need their original MIDI reimported
to recover messages discarded by older firmware.

**Compatibility.** Projects without the new MIDI fields still export schema 1 unchanged. New
MIDI projects use schema 2; older firmware refuses them rather than dropping their expression.
Event storage and compiled snapshots live in PSRAM. A seek replays controller state in bounded
batches while holding the clock, then resumes sequencing; ordinary non-MIDI projects keep their
previous path. The plugin ABI 1.4 adds opt-in MIDI events without changing the 32-byte POD.
No new voice limit, tail shortening, lower sample rate/interpolation, or overclock.

This section describes file import. The live-input parser's SysEx skipping and note-only recording
remain separate features. A controller-only source channel with no notes does not create a part;
system messages are retained on selected note parts. Source event order is canonicalized per
instrument/tick while preserving relative order, so unrelated setup messages do not defeat shared
patterns. The physical-board and PCM checks are recorded in
[2026-10-15-midi-expression.json](performance/2026-10-15-midi-expression.json).

Primary message definitions: [MIDI 1.0 messages](https://midi.org/summary-of-midi-1-0-messages),
[CC / RPN](https://midi.org/midi-1-0-control-change-messages),
[universal SysEx](https://midi.org/midi-1-0-universal-system-exclusive-messages),
[Roland GS reset](https://support.roland.com/hc/en-us/articles/201924639-PMA-5-Sending-a-GS-Reset),
[Roland global implementation](https://cdn.roland.com/assets/media/pdf/F-140R_MIDI_Imple_e01_W.pdf),
[Yamaha XG data format](https://europe.yamaha.com/en/download/files/2090960/).

Not supported, with a message saying so: format 2 (independent sequences), SMPTE time code, RIFF
MIDI (`.rmi`). Files up to 8 MB and 200,000 notes.

**Saving.** The device keeps a project as deflated JSON in a 128 KB slot (docs/STORAGE.md): pop
songs of 5,000–7,000 notes took 220–390 KB of JSON and 26–53 KB in the slot. The dialog estimates
the project's size, and its deflated size conservatively (a quarter); a larger import plays but
cannot be saved (KI-19). Repeated sections cost little: they share patterns. A file recorded from
a live performance (notes off the bar grid) repeats nothing exactly and shares nothing.

**Real-world files.** A folder of 1,042 pop / dance MIDI files from the 1990s–2010s (not part of
this repository) was read with the import code and compared with an independent reader (mido):
1,038 read with the same notes and song ends; one is RIFF MIDI, one is corrupt. Before 2026-10-13
a third of them failed: a SysEx event (the GM / GS / XG reset at the start of most such files)
was skipped one byte short, which shifted every later event's time ("the song is longer than the
playlist"). `smf.test.ts` covers SysEx now.

Tests: `web/src/midi/*.test.ts` (reader, arrangement; a random-song property test checks that the
playlist plays every note exactly; corrupted files never crash it) run in `scripts/test-host.sh`;
`tests/browser/test_ui.py` imports files through the real UI (as a project, into the piano roll,
undo, a public-domain piece saved to a slot) and compares the device's project with the file.
Example files and their licenses: `examples/midi/README.md`; `tests/web/midigen.py` writes the
test files.

## Limits

1024 bytes per `midi` message; live events are bounded by the engine's event pool (512 per
block) and a client's held-note list (128). Live notes carry no voice id: a note-off ends a voice
of the same key, as with a MIDI keyboard. If two people hold the same key on the same instrument,
the first release ends one of the two voices.
