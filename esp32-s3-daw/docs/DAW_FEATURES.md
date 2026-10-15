# DAW features

Status per feature, 0.1.0 (2026-10-11). **Tested** = covered by automated tests (unit, e2e, browser,
QEMU). **Built** = implemented, exercised by hand, no automated test. **Planned** = not implemented.
Everything audible on the real board is additionally UNVERIFIED until docs/HARDWARE_TESTS.md runs.

## Transport

| feature | status |
|---|---|
| play / pause / stop, pattern and song mode, position display (bar:beat:tick) | Tested |
| tempo 20–300 BPM (drag, type, tap tempo), time signature 1–16 / 2, 4, 8, 16 | Tested |
| song loop region (set on the playlist ruler), loop the whole song | Tested |
| metronome, count-in | Tested (engine), Built (UI) |
| external MIDI clock follow, start / stop / continue / song position | Tested |
| audio recording | not possible: the PCM5102A board has no input |
| MIDI file import (formats 0 / 1): as a new project (a channel per part, repeats sharing patterns, playlist, tempo automation, markers) or into the piano roll (parts, bar range, replace / add, GM drums onto DrumLab) | Tested (unit, browser) |
| Tracker module import (MOD, XM, S3M, IT; docs/TRACKER.md): the song played by libxmp on Tracker channels (whole song, per tracker channel or in groups), as a new project or on a new playlist track; starts anywhere inside the song (note chase), follows the project tempo | Tested (SDK, unit, session, e2e, browser, QEMU); measured on the board |
| MIDI recording into the current pattern (overdub, input grid as played / 1/32 / step / 1/8 / beat, notes held over the loop end wrap) | Tested (engine + session, browser) |

## Channel rack and step sequencer

| feature | status |
|---|---|
| up to 32 channels; add, delete, reorder, rename, color, mute, solo, volume, pan, mixer routing | Tested |
| step rows up to 128 steps, independent length per row (polymeter) | Tested |
| paint steps by dragging (one undo step), velocity graph mode | Tested (browser) |
| per-step velocity, accent, probability, pitch; fill every 2 / 4 / 8, invert, shift, randomize, copy / paste | Built |
| swing per pattern | Tested |
| convert steps to piano-roll notes | Tested |
| patterns: new, clone, delete, rename, color, select, queue (switch at the pattern end) | Tested |

## Piano roll

| feature | status |
|---|---|
| draw, move, resize, delete (right-click / erase tool), select (rectangle, shift-add), duplicate (Ctrl-drag) | Tested (browser) |
| snap (none, fine steps, step, beat, bar), zoom, scroll, pinch / two-finger pan on touch | Built |
| per-note velocity, pan, release, fine pitch, probability lanes | Built |
| slide notes (glide into the next note, plugins with the slide capability) | Tested (engine + 3xOSC) |
| quantize, legato, transpose, octave, chop, randomize velocity, double / halve length | Built |
| ghost notes of other channels, follow playhead, note properties dialog (double-click / double-tap) | Built |
| computer keyboard and on-screen keyboard preview | Built |

## Playlist (song arrangement)

| feature | status |
|---|---|
| tracks (add, rename, color, mute, solo), pattern clips: place, move, resize, slice, delete, duplicate | Tested (browser: place + move) |
| clip offset / length (a pattern clip can show part of its pattern), make unique (clone the pattern) | Built |
| markers | Built |
| automation clips: created from any automatable parameter (right-click → Create automation clip, flat at the current value), placed and played | Tested (engine, browser) |
| automation point editor: add, move, delete, curve tension, length, snap, undo; values shown in the target's units | Tested (browser) |

## Mixer

| feature | status |
|---|---|
| master + 16 inserts: volume, pan, mute, solo, route to master or another insert | Tested |
| 4 sends per insert, routing cycles refused | Tested |
| 4 effect slots per insert: add, remove, reorder, bypass, dry / wet, presets | Tested |
| peak meters per insert and per channel | Built |

## Instruments and effects

Instruments: Sine, 3xOSC, FM6, DrumLab, Acid303, MacroSynth, Sampler, SoundFont, Tracker. Effects: Reverb, Delay, Chorus, Flanger, EQ, Filter,
Compressor, Limiter, Distortion, Utility (docs/PLUGIN_INTEGRATION.md). Generic editor
generated from parameter descriptors (knobs, enums, toggles, groups), presets, right-click → reset
/ create automation. OPL3: Planned.

## Projects

| feature | status |
|---|---|
| 8 slots on the device (power-safe), autosave while stopped, restore at boot | Tested |
| export / import as a file (import needs the admin login) | Tested |
| demo song and empty templates | Tested |
| missing plugins preserved, schema versioning | Tested |
| sample library on the device (6 MB, power-safe): upload (button or drag and drop), list, delete, one click → Sampler / SoundFont channel (a tracker module opens its import); a SoundFont's own presets listed in its editor | Tested (store, session, e2e, browser) |
| projects reference samples by name; missing ones are kept and reported | Tested |
| user presets saved on the device (any instrument or effect), offered next to the factory presets, deletable | Tested (session, browser) |

## Collaboration and devices

| feature | status |
|---|---|
| several browsers at once (4), live sync of every edit, stale-edit protection | Tested |
| desktop layout (FL Studio–style windows), tablet, phone portrait / landscape (FL Mobile–style tabs) | Tested (browser, 5 viewports) |
| undo / redo (per browser) | Tested (browser) |
| Web MIDI input, Linux MIDI bridge, on-screen and computer keyboard | Tested (bridge), Built (Web MIDI) |
| USB-MIDI device mode | Planned |

## Device

| feature | status |
|---|---|
| diagnostics page: CPU, render times, underruns, heap, PSRAM, clients, per-plugin CPU and memory | Built |
| test signals (440 Hz, left / right, −60 dBFS, silence, click), panic | Tested (engine) |
| browser firmware update with verification and rollback | Tested (QEMU) |
| reboot, factory reset (admin login) | Built |
| licenses and third-party notices page | Tested (served by firmware and host) |
| device settings from the browser: Wi-Fi mode / network / password, device name, admin password (admin login) | Tested (unit, e2e, browser) |
