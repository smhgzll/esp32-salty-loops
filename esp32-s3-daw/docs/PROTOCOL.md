# Browser ↔ device protocol (version 1)

One WebSocket per browser tab at `ws://<device>/ws`, JSON text frames. The device is the single
source of truth: clients send *operations*, the device validates and applies them, then tells every
client what changed. Plus a small HTTP API for things that are not part of a live session.

An incoming text message can span TEXT and CONTINUATION frames. Firmware joins them through
the final frame before parsing or acknowledging the operation; the aggregate limit is 256 KiB.
Invalid continuation order and oversized messages close the connection.

Implementation: `components/daw_core/src/session.cpp` (operations, identical on the ESP32-S3, in
QEMU and in `daw-host`), transports in `components/daw_webserver/web_server.cpp` (ESP-IDF httpd)
and `host/http_server.cpp` (Linux). Client: `web/src/app/protocol.ts`.

## Connection

1. WebSocket upgrade. Refused when the `Origin` header is present and is not `http://<Host>`, and
   when `CONFIG_DAW_MAX_CLIENTS` (default 4) clients are connected (the device closes the
   connection; `daw-host` answers 403).
2. Client → `{"t":"hello","proto":1,"client":"web"}`. Any other operation before `hello` is
   answered with `err` code `no_hello`. A different `proto` is refused with code `proto`.
3. Device → `welcome`:

```json
{"t":"welcome","proto":1,"client":7,"rev":42,
 "server":{"name":"esp32daw","version":"0.1.0","target":"esp32s3","sampleRate":44100,
           "blockFrames":256,"ppq":960,"limits":{"channels":32,"projectBytes":...,"projectDocBytes":...}},
 "project":{ meta, channels, patterns, mixer, playlist, automation },
 "plugins":[ plugin catalog: ids, parameters, presets ],
 "projects":[ saved slots ],
 "samples":{ the sample library }}
```

The `welcome` is a complete snapshot: a client replaces all of its state with it. The same
message is sent again whenever the client must resynchronise (below).

## Messages from the device

| `t`       | class | meaning |
|-----------|-------|---------|
| `welcome` | state | full snapshot (connect, resync) |
| `ack`     | state | operation with an `id` succeeded: `{"t":"ack","id":N,"rev":R, ...result}` |
| `err`     | state | operation failed: `{"t":"err","id":N,"rev":R,"code":"...","msg":"...","op":"..."}` (sent even without an `id`); `code` `cpu`: `channel.add`, `fx.add` or re-enabling an effect refused by CPU admission (docs/AUDIO_ENGINE.md) |
| `patch`   | state | an entity was replaced: `{"t":"patch","rev":R,"path":"pattern","value":{...}}` |
| `set`     | state | continuous control changed: `{"t":"set","rev":R,"path":"channel/5","vol":0.7,"pan":0,"by":7}` |
| `projects`| state | the saved slots changed |
| `userPresets` | state | the user presets changed: `{"t":"userPresets","list":[{plugin,name}]}` (also in the `welcome`) |
| `samples` | state | the sample library changed: `{"t":"samples","library":{"list":[{name,size,type,crc}],"total","free","largest"}}` (`crc`: unsigned CRC32 of file bytes) |
| `st`      | live  | transport + meters, every 50 ms while anyone is connected |
| `diag`    | live  | engine / platform statistics, every 1 s |

`patch` paths: `meta`, `patternList`, `pattern` (one pattern with all its sequences), `channels`,
`mixer`, `track` (one mixer track), `playlist`, `automation`, and indexed MIDI updates (below). Patches replace whole entities; they
are small (one pattern, one track) and make every message idempotent: applying the newest value of
an entity is always correct, whatever came before it.

`set` messages carry continuous controls — channel / track volume and pan, plugin and effect
parameters — instead of re-sending the whole channel list or track while a fader moves (`by` is
the client that moved it, so it can ignore its own echo while dragging). Structural changes (name,
routing, mute, plugin, effect order, ...) always produce a `patch`.

`st`: `{"t":"st","rev":R,"play":0|1|2|3,"mode":"pattern"|"song","tick":T,"pattern":id,"queued":id,
"bpm":128,"cpu":12.5,"voices":5,"cpuSkips":0,"ext":false,"rec":false,"m":[L0,R0,L1,R1,...],"cm":[...]}` — `play`: 0 stopped,
1 playing, 2 paused, 3 count-in; `m`: mixer track peaks (master first), `cm`: channel peaks; `cpuSkips`:
note-ons skipped by the CPU guard since boot (the browser marks the load meter while it grows).

`diag`: `cpu`, `load` (the same smoothed over ~50 ms), `workerWait` / `workerBusy` (two-core scheduling, % of the block), `renderUsAvg`, `renderUsMax`, `budgetUs`, `overruns`, `eventDrops`, `noteDrops`, `cpuGuardDrops`,
`nanResets`, `fxFailures`, `cmdOverflow`, plus platform fields (internal heap, PSRAM, I2S
underruns, Wi-Fi RSSI, uptime, firmware, partition, `parallelInstruments`, `dspStackFree`).
Each `plugins` entry also reports cumulative `parallelJobs` / `parallelDeclined` counts for the
ABI 1.3 host DSP service. `workerWait` / `workerBusy` describe engine-level scheduling; plugin
subjobs have these separate counters. A declined subjob uses the plugin's serial path.
With the hardware worker enabled, `cpu` measures elapsed render/deadline time across overlapping
jobs; each plugin reports its own cycles, so plugin shares need not sum to `cpu`.
Hardware diagnostics also report `psramLargest` (the largest contiguous free block), `resetReason`
(ESP-IDF's reset reason), and, when a flash core dump is available, `lastCrash`: task, panic reason,
ELF hash prefix (`elfSha256`), PC and backtrace addresses. This is the last recorded crash, which
can precede the current firmware; it is read once at startup, never on the audio thread.
`fileBufferBytes` is the capacity of active shared file buffers; `fileSpareBytes` is retained
unused capacity available for reuse. Both count allocated memory, including capacity headroom.
On QEMU and on Linux the timing fields describe
the emulator / the PC, never the ESP32-S3 (docs/PERFORMANCE.md).

## Revisions and consistency

- Every accepted mutation increments the project revision `rev`; every state message carries the
  revision after it. Acks carry `rev` too, so a client knows its own edit is included.
- The device applies operations strictly in arrival order (one control thread). Two clients
  editing the same entity: the later operation wins, and both see the result through the patch.
- **Stale edits** refer to an entity by id. An edit of something another client has deleted
  fails with `not_found` ("does not exist (stale edit)") instead of resurrecting it.
- **Undo** is client-side (`web/src/app/history.ts`): an undo sends the entity's previous full
  value (`notes.set`, `steps.set`, `clips.set`, ...). It restores what *this* client saw before its
  edit; concurrent edits of the same entity by another client in between are overwritten by it.
- **Backpressure.** Each client has a bounded send queue on the device (`web_server.cpp`:
  24 KB for live frames, 192 KB for state frames; `daw-host` allows 256 KB / 8 MB). A slow client
  loses `st` / `diag` frames first (only the newest matters). If a *state* frame does not fit, the
  device drops it, marks the client, and sends a fresh `welcome` as soon as its queue has drained:
  the client never continues with a silently wrong picture. A state frame larger than the queue
  (the `welcome` of a large project, such as an imported MIDI song) is queued alone once at most
  24 KB of live frames wait; before 2026-10-13 it was always dropped, and a browser could not load
  a project of more than about 190 KB of JSON.
- **Reconnect.** The browser reconnects with backoff (0.5 s × 1.6, at most 5 s) and starts over
  from the `welcome`. A disconnected client's held live notes are released by the device
  (no stuck notes when a phone locks its screen mid-chord).

## Operations (client → device)

Every operation is `{"t":"<op>", "id":N, ...}`. `id` is optional; with it the device answers
`ack` / `err`. Limits: 256 KB per message; fields are validated and clamped (BPM 20–300, steps
≤ 128 per row, notes ≤ 4096 per sequence, clips ≤ 2048, 32 channels, 16 inserts + master, 4 effects
and 4 sends per track).

| group | operations (main fields) |
|-------|--------------------------|
| session | `hello` (proto), `ping` (ts → ack with `echo` and the device's `uptimeMs`) |
| transport | `record` (on, quantize: input grid in ticks, 0 = as played; ack returns both); `play`, `stop`, `pause`, `toggle`; `bpm` (bpm); `timesig` (num, den); `mode` ("pattern"/"song"); `seek` (tick); `loop` (start, end, on); `metronome` (on, countIn) |
| patterns | `pattern.add` (name, color, select, length: 0 = auto), `pattern.dup`, `pattern.del`, `pattern.set` (name, color, length, swing), `pattern.select` (pattern, queue: while playing, switch when the current pattern reaches its end), `pattern.move` (index) |
| step sequencer | `steps.set` (pattern, channel, steps, count), `step.toggle` (pattern, channel, index, on, vel, accent), `seq.mode` (pianoRoll, convert: steps → notes) |
| piano roll | `notes.add`, `notes.update`, `notes.del` (ids), `notes.set` (whole row; undo) |
| channel rack | `channel.add` (plugin, name, color, preset, track), `channel.del`, `channel.set` (name, color, root, track, mute, solo, vol, pan), `channel.move`, `channel.plugin` (replace the instrument), `channel.param` (key, v), `channel.preset` (name) |
| mixer | `track.set` (name, color, route, mute, solo, vol, pan), `track.send` (slot, target, level), `fx.add` (track, plugin, preset, index), `fx.del`, `fx.move`, `fx.set` (enabled, mix), `fx.param` (key, v), `fx.preset` |
| playlist | `clip.add` (kind: pattern/automation, ref, track, start, len, offset), `clip.update` (clips: [{id, ...}]), `clip.del` (ids), `clip.split` (clip, at), `clips.set` (all clips; undo), `ptrack.set` (playlist track name, color, mute, solo; add), `marker` (add/move/del) |
| automation | `auto.add` (name, target, length, points), `auto.set`, `auto.del` |
| live input | `note` (channel, key, vel, on), `midi` (data: ≤ 1024 raw MIDI bytes, channel, ts), `panic` |
| user presets | `preset.save` (channel or fx, name), `preset.delete` (plugin, name); `channel.preset` / `fx.preset` take factory or user preset names |
| samples (ABI 1.1 resources) | `samples.list` (→ ack with `library`), `samples.delete` (name), `samples.presets` (name of a SoundFont → ack with `presets`: `[{name, bank, program}]` sorted, read from the file's PHDR records), `samples.probe` (`name`, `plugin`: the plugin's read-only parameters after loading that file in a temporary instance that never reaches the engine → ack with `values: {key: number}`; `not_found`, `bad_plugin` (no slot for the file), `bad_file`, `resources`; the Tracker plugin reports a module's `length`, `song_tempo`, `channels`, `format`, `orders`, `instruments`, `memory`, docs/TRACKER.md), `channel.resource` (channel, slot key, sample name; "" clears; SoundFont optionally accepts `bank`, `program` and `presets: [{bank, program}]`) |
| module cache | `samples.keepModule` (`name`: an existing module) → ack with `library`; delete all other module files, including current / saved project references, leaving WAV / SF2 untouched. Used for cached / library opens. Unknown or non-module names: `not_found`; playback: `playing`; an active streamed upload or flash failure: `storage`. |
| projects | `project.new` (template), `project.save` (slot, name), `project.load` (slot), `project.list`, `project.delete` (slot), `project.rename` (name, author) |

Lengths: patterns (`length`), notes (`len`, `start`) and clips (`start`, `len`) are at most 1000
bars of 4/4 (3,840,000 ticks; patterns and notes were limited to 64 bars before 2026-10-14, note
starts to 256 bars): a tracker song is one pattern holding one note (docs/TRACKER.md).

Patterns in `welcome` and `pattern` patches are compact like the stored document
(docs/STORAGE.md): rows without content are left out, `steps` may be missing (all off), notes may
have 5–9 fields (the rest default). The browser fills them in (`web/src/app/protocol.ts`).

Slots store the project document deflated (docs/STORAGE.md): `limits.projectBytes` is a slot's
capacity in deflated bytes, `limits.projectDocBytes` the largest document (before deflating) a
slot takes. Each entry of `projects` has `bytes` (the document) and `stored` (in flash). A save
that does not fit fails with code `storage` and both sizes in `msg`.
| diagnostics | `diag.tone` (kind: off, sine440, leftright, lowlevel, silence, click) |

Automation targets: `{"kind":"channelVol"|"channelPan"|"channelParam"|"trackVol"|"trackPan"|"fxParam"|"fxMix"|"sendLevel"|"tempo", ...ids}`.

`midi` input: running status, note-on velocity 0 = note-off, CC 64 sustain, CC 120/123 all
sound/notes off, pitch bend, program change, aftertouch; clock (0xF8) / start / stop / continue
drive the external-clock follower when enabled. Without `channel`, MIDI channel *n* plays channel
rack row *n*+1. `ts` (ms, sender's clock) improves the clock follower's tempo estimate.

## HTTP API

| method / path | auth | |
|---------------|------|--|
| `GET /` and assets | – | the browser application (gzip, embedded in the firmware) |
| `GET /api/status` | – | device, firmware, CPU, heap, clients (JSON) |
| `GET /api/project` | – | export the current project (`*.esp32daw.json`) |
| `POST /api/login` | same origin | `{"password":"..."}` → `{"token":"...","expiresIn":3600}`; wrong password: 401 after 1 s |
| `POST /api/project` | token | import (restore) a project document |
| `GET /api/settings` | token | Wi-Fi mode, network name, device name; `wifiPasswordSet` / `adminPasswordSet` flags — never the passwords |
| `POST /api/settings` | token, same origin | change any of `wifiMode` (AP / STA), `ssid`, `wifiPassword`, `hostname`, `adminPassword`; a new admin password revokes every token at once; the rest applies after a restart |
| `GET /api/samples` | – | the sample library (`list`, `total`, `free`, `largest`) |
| `POST /api/samples?name=<file>` | same origin | add / replace a WAV, SF2 or tracker module (MOD `M.K.` / `xCHN` / `xxCH`, XM, S3M, IT; the raw file as the body); refused while playing on the device; a SoundFont or module over 1 MB is written to flash as it arrives (no RAM copy), anything else needs that much free PSRAM (else 507) |
| `POST /api/samples?name=<file>&reclaim=modules` | same origin | single-song module cache: clear all other modules before upload, regardless of free space or project references; preserve SoundFonts / WAVs and project documents. A same-name old module may also be discarded if needed for staging. Ignored for non-module input. See docs/TRACKER.md. |
| `POST /api/ota` | token | firmware update (docs/SECURITY.md, docs/QEMU.md) |
| `POST /api/reboot` | token | restart |
| `POST /api/factory-reset` | token | erase projects and settings, restart |

Tokens go in `Authorization: Bearer <token>`. All POSTs require the same origin (or no `Origin`
header, i.e. not a browser page). With no admin password configured, privileged endpoints are
disabled.

`channel.resource` loads and validates the replacement before changing the channel. Allocation,
CRC or plugin-load failures return `err` code `resources`, leaving its previous resource and
instance intact. A file that exists in flash can still be too large for the available playback RAM.
For SoundFonts, `bank` / `program` selects the initial preset atomically. `presets` supplies all
planned presets for that font in this project, allowing one shared compact file from the first
binding. Without any of these optional fields a legacy client gets the whole font. Changing a
bound font's bank/program later prepares any missing samples before committing the parameter;
bank/program automation prepares the whole font before its lane is accepted.

## Versioning

`proto` is bumped for any incompatible change of message shapes or semantics; the device refuses
other versions with a clear error, and the browser application is always served by the same
firmware, so the two only disagree with a cached page (the reconnect then shows the error).
Project documents carry their own `format` / `schema` version (docs/STORAGE.md).


## Timed MIDI (ABI 1.4 firmware)

`server.limits.midiEvents` / `midiBytes` advertise the feature/capacity. `midi.set` takes
`pattern`, `channel`, `events: [[tick, order, status, data...], ...]`, optional `append: true`.
Notes stay editable notes, never hidden raw note-ons/offs. Validation and preset preparation
finish before mutation. An append preserves stable time/order; invalid input leaves the lane.
`notes.set` optionally accepts `midi` for atomic note+controller replacement/undo; omitting it
preserves the lane. Notes append optional on/off source-order fields (indices 10/11).

MIDI updates broadcast `patch/path: "midi"`, value `{pattern,channel,from,total,effLength,events}`.
Replace the lane tail from `from` with `events`, truncate to `total`. This is idempotent and
keeps chunk uploads from re-sending the whole growing lane; out-of-order appends send `from:0`.
All state updates retain the normal ordered/reliable queue and revision rules.
New event types/flags are delivered only to `DAW_CAP_MIDI_STREAM` plugins; old ABI plugins keep
their prior events/flags. Live pressure is routed too; channel-mode note-off commands affect
only their target instrument, not every channel.
