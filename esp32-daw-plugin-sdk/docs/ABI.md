# Plugin ABI, version 1.4

Headers: `include/daw_plugin_api.h` (descriptor, process), `daw_plugin_events.h`,
`daw_plugin_params.h`, `daw_plugin_host_api.h`. Plain C11; usable from C and C++.

## Entry point and identity

A plugin library exports exactly one symbol, `const daw_plugin_descriptor_t *<entry>(void)`; the
name is in the package manifest. The descriptor and everything it points to is immutable static
data. `id` is a lower-case reverse-DNS string that never changes for the life of the plugin
(projects reference it). `version` is semantic (`x.y.z`).

## Versioning

`DAW_PLUGIN_API_MAJOR.MINOR` = 1.4 (1.1 added resources, 1.2 note chase, 1.3 optional host-owned
parallel DSP jobs; 1.4 timed MIDI; below). A host
accepts a plugin when the majors are equal and the plugin's minor is <= the host's
(`daw_plugin_abi_compatible()`). Minor versions only append fields to the end of structs (readers
check `struct_size` before touching new fields) or add an opt-in capability: a plugin receives the
new events or flags only after declaring it, so a plugin built for 1.1 sees exactly the 1.1
behaviour. Any other change is a new major version.

## Threads and call order

```text
create -> [set_param / load_state / set_resource]* -> activate -> process* (audio thread) -> deactivate -> destroy
                                                                    deactivate -> [set_resource]* -> activate ...
```

| Function | Thread | May allocate | Notes |
| --- | --- | --- | --- |
| create / destroy | control | yes (host API only) | create returns NULL and frees everything on failure |
| activate / deactivate | control | yes | activate resets DSP state |
| set_param / get_param | control, between process calls | no | values are plain (min..max), clamp them |
| save_state / load_state | control | no | only with `DAW_CAP_STATE_BLOB` |
| process | audio [RT] | **no** | bounded time, no locks, no I/O, no logging |
| active_voices | audio or control | no | diagnostics |
| set_resource (1.1) | control, not activated | yes | see Resources |

The host never calls two functions of one instance concurrently.

### Optional parallel DSP jobs (ABI 1.3)

The trailing `daw_host_api_t::parallel` service accepts two independent, bounded DSP callbacks
and their arguments. Check `struct_size` through the end of that field and the pointer before
calling it. A return of **1** means both callbacks have completed; **0** means neither callback
was invoked, so the plugin must execute both serially. The host may decline when its worker is
busy, on nested calls or when no worker exists. Callbacks may run concurrently: keep their
mutable state and output regions separate and their shared inputs read-only.

The host owns the persistent worker and its completion wait; plugins create no threads and
perform no allocation, logging, I/O or application locking in their jobs. Arguments remain valid
until the service returns. This is explicit parallel work inside the current `process()` call,
not concurrent calls to an instance's ordinary ABI entry points. Hosts without the service keep
the serial DSP path. Reverb uses it for its independent left/right delay networks, Tracker for
the voices of a mixer part (two jobs with separate tick buffers, added after both finish).

## process()

`daw_process_t` gives `frames` (1..max_block), stereo `outputs` (and `inputs` for effects; they may
alias, so read a sample before writing it), the block's events sorted by `offset`, the transport
state, the host's monotonic sample counter and the musical position. Events take effect at their
sample offset ("sample-accurate"). Parameter changes arrive as `DAW_EV_PARAM` events; smooth them
when they would click.

## Events (`daw_event_t`, 32 bytes)

| Type | Payload | Meaning |
| --- | --- | --- |
| NOTE_ON | key, velocity 0..1, note_id, initial pan / pitch, flags, slide_ms | start a note |
| NOTE_OFF | key, note_id, release velocity | end it (match by note_id when >= 0, else key) |
| NOTE_EXPR | note_id, key, expr, value | change one note's pan / pitch / volume / cutoff / resonance |
| PARAM | id, value | parameter change at this offset |
| CC, PITCH_BEND, CHANNEL_PRESSURE, PROGRAM, SUSTAIN | channel, number, value | MIDI-style controls |
| ALL_NOTES_OFF | — | release all voices (tails continue) |
| ALL_SOUND_OFF | — | silence now |

Note flags: `SLIDE` (glide from the sounding note; see below), `ACCENT`, `LEGATO` (do not retrigger
envelopes), `PREVIEW` (an editor audition).

### Per-note expression semantics

A plugin declares what it honours with capabilities. `DAW_CAP_NOTE_PAN` / `NOTE_PITCH` / ... mean the
plugin applies the NOTE_ON's initial value **to that voice only**; NOTE_EXPR changes during the
note are honoured by plugins that track voices by note id (all bundled polyphonic synths do).
A plugin without the capability ignores the value — the DAW greys the corresponding piano-roll
lane out instead of faking it with channel-wide pitch bend or pan.

### Slide

`DAW_NOTE_FLAG_SLIDE` + `DAW_CAP_SLIDE`: the newest **held** voice glides to the new key without
retriggering its envelopes, over `slide_ms` (0 = the plugin's glide setting). If no voice is held,
the plugin starts a new voice gliding from the last key played. The DAW's sequencer keeps a note
held until the slide note that follows it starts (legato), the way a 303 pattern does. A
monophonic engine (Acid303) slides its single voice; a polyphonic one (3xOSC) slides the newest
voice. Without the capability the flag is ignored and the UI disables slide.

### Note chase (ABI 1.2)

A sequencer normally starts a note only at its beginning: when the transport starts, jumps or loops
back into the middle of a long note, an instrument stays silent until the next note. A plugin
playing whole songs or long recordings declares `DAW_CAP_NOTE_CHASE`; the host then sends it a
NOTE_ON with `DAW_NOTE_FLAG_CHASE` for every note the position lands inside, at the sample where
playback resumes, with `u.note.chase_mbeats` = how much of the note has already passed, in 1/1000
quarter notes (the field shares its place with `slide_ms`; a chased note never slides). Its NOTE_OFF
comes where the note ends, as usual. Before a chase the host ends the plugin's held notes
(NOTE_OFF), so a plugin never sees a stale note continue across the jump. Without the capability
nothing changes: no chase events, no early note-offs. Hosts older than 1.2 never chase.

## Parameters

`daw_param_desc_t`: stable numeric `id` (never reused), stable string `key` (used in project files),
display name, group, unit, type (float / int / bool / enum), UI scale (linear / log / dB /
bipolar), min / max / default, flags (`AUTOMATABLE`, `HIDDEN`, `READONLY`, `NO_SMOOTH`), enum labels.
With the C++ adapter (`dawsdk::Plugin`) ids are the indices 0..N-1.

Presets (`daw_preset_desc_t`): name, category, and one value per parameter in descriptor order
(`values == NULL` means "all defaults"). The host applies a preset through `set_param`.

## Memory

All memory comes from `host->alloc(ctx, size, align, flags)`: `DAW_MEM_FAST` (internal SRAM on the
ESP32-S3: hot state) or `DAW_MEM_LARGE` (PSRAM: delay lines, tables, samples), optionally
`DAW_MEM_ZERO`. The descriptor's `mem_fast_bytes` / `mem_large_bytes` are the estimates the DAW
budgets with; `dawplug validate` warns when create() exceeds them.

## Resources (ABI 1.1)

Samples and sound banks are data the host owns, not plugin code. A plugin lists **resource slots**
(`resource_slot_count`, `resource_slots`: a stable `key` stored in projects, a display `name`, the
accepted `types`, a file-name filter for the UI) and implements `set_resource(inst, slot, res)`.

- `DAW_RES_AUDIO`: the host decodes the file (WAV: PCM 8/16/24/32-bit, float 32; mono or stereo)
  into float channels and passes `channels`, `frames`, `sample_rate`, `root_key` (from the WAV
  `smpl` chunk, −1 if none) and the file's loop points.
- `DAW_RES_FILE`: the raw bytes (`data`, `size`), e.g. a SoundFont the plugin parses itself.

`set_resource` runs on the control thread while the instance is not activated (between create and
activate, or after deactivate). The plugin may allocate and pre-compute there; it must not keep
pointers into a previous resource afterwards. `res == NULL` empties the slot. The host keeps the
resource and its memory valid until the slot is set again or `destroy()` returns. A plugin
returns `DAW_ERR_ARG` for a resource it cannot use (wrong type, channel count, corrupt file) and
must stay silent and safe with empty slots. Descriptors check: `daw_plugin_has_resources()`.

In C++: implement `int32_t setResource(uint32_t slot, const daw_resource_t *res)` and add
`DAWSDK_RESOURCES(P, count, slots)` after `DAWSDK_CALLBACKS(P)` in the descriptor.

## Ownership summary

| Object | Owner | Lifetime |
| --- | --- | --- |
| descriptor, params, presets, strings | plugin | static |
| `daw_host_api_t` | host | create() .. destroy() returns |
| instance | plugin | create() .. destroy() |
| `daw_process_t`, events, audio buffers | host | one process() call |
| state buffers | caller | one save/load call |
| `daw_resource_t` and its memory | host | set_resource() .. the slot is set again or destroy() returns |


## Timed MIDI (1.4, opt in)

`DAW_CAP_MIDI_STREAM` opts into `DAW_NOTE_FLAG_MIDI`, `DAW_EV_POLY_PRESSURE` (midi.number=key),
`DAW_EV_MIDI_BYTES` (u.bytes.data/size, borrowed only for process), and `DAW_EV_MIDI_RESET`. `DAW_EV_CHANNEL_PITCH` carries an absolute semitone offset for the
C++ adapter's RPN/tuning result, keeping PITCH_BEND normalized to -1..1.
The event stays 32 bytes on both targets; existing enum values, payloads and struct offsets stay
unchanged. Hosts suppress new events/flags for older/non-opting plugins. PROGRAM continues to
use midi.index; CC/pressure use midi.number/value. Events retain same-sample source order.

The C++ adapter applies gain/pan/balance, soft pedal and common RPN tuning/bend only after MIDI
opt-in events arrive. Unflagged legacy renders retain their old arithmetic. `nativeMidi()=true`
lets an engine handle the stream itself (SoundFont); unmapped CC/RPN/device commands remain
opaque, not arbitrary parameter mappings. The DAW import computes pedal gates for engines
without native pedals; the adapter does not introduce a pending-note/voice ceiling.

SoundFont prepares its private per-voice region/controller state in LARGE memory on resource
binding and frees it on detach. Program changes never load resources or allocate. SysEx global
parsing is bounded (packets <=256 bytes); long vendor dumps remain host data without an audio
thread scan. Notes keep IDs through preset changes, sustain, stealing and all-sound-off.
