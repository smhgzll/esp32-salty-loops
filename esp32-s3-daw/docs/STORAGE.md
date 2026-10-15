# Storage

ESP32-S3 N16R16: 16 MB flash, 16 MB PSRAM. This document covers the flash layout, how projects are
saved without ever leaving a half-written file behind, the project document format and its
versioning, and the rules that keep flash work away from the audio.

## Flash layout (`config/partitions.csv`)

| partition | offset | size | use |
|-----------|--------|------|-----|
| nvs | 0x9000 | 24 KB | device settings (Wi-Fi, hostname, admin password), boot counter, self-test state |
| otadata | 0xF000 | 8 KB | which OTA slot boots, image states (rollback) |
| phy_init | 0x11000 | 4 KB | RF calibration |
| ota_0 | 0x20000 | 3 MB | firmware slot A |
| ota_1 | 0x320000 | 3 MB | firmware slot B (browser updates alternate between the two) |
| projects | 0x620000 | 2 MB | 8 project slots × 256 KB; slot 0 = autosave |
| presets | 0x820000 | 512 KB | user presets: one small JSON file per preset, in the same power-safe store as the samples |
| samples | 0x8A0000 | 6 MB | the sample library (WAV, SoundFont, tracker module), see below |
| stage | 0xEA0000 | 1 MB | reserved: full-device backups |
| diag | 0xFA0000 | 320 KB | reserved: diagnostics log |
| coredump | 0xFF0000 | 64 KB | panic core dump |

On 32 MB boards (`DAW_FLASH_SIZE_MB=32`, the tested board) `scripts/build-hardware.sh` selects
`config/partitions-32mb.csv`: the same table up to the sample library, which keeps its offset and
grows to **22 MB** (0x8A0000–0x1EA0000), with stage / diag / coredump moved to the end of the flash.
The library's files stay valid across that change (the scan reads more space); the table itself
needs one USB flash (`scripts/flash.sh`: bootloader, table, OTA data, app; data partitions kept).
On the board on 2026-10-13 three SoundFonts (6.0 + 6.3 + 3.2 MB) were stored, the second one
across the 16 MB address boundary.

The firmware is ~0.8 MB (QEMU profile) / ~1.2 MB (hardware, with Wi-Fi) of a 3 MB slot
(`scripts/report-size.py` prints the numbers after every build). A firmware update never touches
the data partitions; `--erase-all` in `scripts/flash.sh` and the factory reset do.

## Project slots: power-safe saves (`components/daw_core/src/store.cpp`)

Each 256 KB slot has two 128 KB halves. A save:

1. deflates the project JSON (raw DEFLATE, RFC 1951: the ESP32-S3 ROM's miniz `tdefl` on the
   device, zlib on Linux; imported MIDI songs shrink 5–11 times),
2. picks the half holding the **older** valid copy (or an invalid one),
3. erases it, writes the payload (the deflated document),
4. writes the header **last** (64-byte region): magic `DAWP`, header version 2 and size, sequence
   number (one more than the other half's), payload length, CRC32 of the payload, time, name
   (32 bytes), the document's length before deflating, CRC32 of all header bytes before it,
5. reads the half back and inflates it (`tinfl`) to check it is the same document.

Version 1 headers (the document stored as it is, written before 2026-10-13) still load; the next
save of that slot writes version 2 into its other half. A firmware from before then sees version 2
halves as empty (it never misreads them). The deflate code is in the chip's ROM: the firmware
links no compression library (`components/daw_hal/platform_esp.cpp`; the compressor's ~160 KB
state lives in PSRAM only while it runs).

`load()` validates both halves (magic, lengths, both CRCs) and returns the valid one with the higher
sequence number. A power cut at any point of a save leaves the half being written without a valid
header, so the previous version loads. `tests/core/test_storage.cpp` cuts the power after every
possible byte / sector of a save and checks this (2397 checks), plus NOR semantics (writes can only
clear bits), torn headers, bit flips in payload and header, sequence wrap and full slots.

Limits: a deflated payload ≤ 128 KB − 64 B per slot, a document ≤ 2 MB before deflating (the
demo project is ~15 KB; an imported pop song of 5,000 notes 220–350 KB, 26–53 KB deflated). The
session refuses a save that does not fit with a clear error, which gives both sizes. The browser
sees the limits as `server.limits.projectBytes` (deflated) and `projectDocBytes`; the MIDI import
estimates the deflated size conservatively (a quarter of the JSON).

## The sample library (`components/daw_core/src/sample_store.cpp`)

The `samples` partition holds uploaded files (WAV, SoundFont `.sf2`, tracker modules: header type
4, the MOD / XM / S3M / IT signatures libxmp-lite reads, docs/TRACKER.md; content-sniffed, other
files are refused). Each file takes a run of whole sectors: a 64-byte header (magic, version, type,
sequence number, size, CRC32 of the bytes, name ≤ 39 characters, header CRC) and then the bytes.
A save erases the run, writes the bytes, and the header **last**: a power cut leaves no file or
the complete file. Deleting clears the header's magic (one 4-byte write). Replacing a name writes
the new file before deleting the old one; if power fails between the two, the scan at the next
boot keeps the higher sequence number. Placement is first-fit; there is no compaction, so after
many deletions the largest file that fits (`largest` in the listing) can be smaller than the free
total. Module opening uses a single-song cache (`reclaim=modules` on upload, `samples.keepModule`
for cached opens): all other module files are cleared, including current / saved project
references, irrespective of free space. SoundFonts / WAVs and project documents stay. Saving a
project does not retain its module file against a subsequent module open; reimport it when needed.
A same-name module replacement keeps its old copy when staging fits, but can discard it to make
room; this opt-in module path therefore allows the old song to be lost after an interrupted upload.
Ordinary uploads retain the transaction-safe replacement above. No file relocation is required.
Library entries expose their stored `crc`, letting the browser reuse identical modules without
writes (same name and size alone were insufficient).
Reads verify the CRC. A SoundFont or module larger than 1 MB is uploaded in pieces (`beginPut` /
`appendPut` / `finishPut`): the run is reserved, sectors are erased in 64 KB blocks as the bytes
reach them, the CRC runs along, and the header comes last, so a GM font of 6 MB+ needs no free
PSRAM; an interrupted ordinary upload leaves free space and keeps an older file of the same name. Other
uploads arrive whole in PSRAM and are refused (507) when it lacks room. Tests: `tests/core/test_samples.cpp` cuts the power at every step of
a new save and of a replace (92 checks).

WAV files are decoded (PCM 8/16/24/32-bit, float 32, mono / stereo, `smpl` root key and loop)
into float memory in PSRAM when a plugin instance needs them; instances using the same file share
one decoded copy, which lives until the last of them is destroyed. A 6 MB library of 16-bit stereo
audio decodes to about 12 MB of floats: the PSRAM budget, not the flash, is the practical limit for
how much can be loaded at once.

Projects store a plugin's resources by **name** (`"resources": {"sample": "kick.wav"}`); a project
whose file is not in the library loads, keeps the reference, and the plugin stays silent
(`missingResources` tells the browser). Uploading a file under that name brings it back.

Since 2026-10-14, project loads and planned MIDI imports build a compact SoundFont in PSRAM with
only the project's selected presets and their sample dependencies (`sf2_subset.cpp`). The loader
reads bounded tables (≤ 1 MiB), follows instrument/sample references and stereo links, merges
overlapping PCM ranges, and relocates sample addresses. Global zones, generators and modulators
are retained. The original font in flash and its full preset menu are unchanged. Channels with
the same font and selection share the compact PCM file; each channel indexes that selected
preset set. A 6.3 MB Phoenix font with 15 Alphaville channels takes about 2.1 MB including the
shared file allocation, per-channel indices and voices, rather than 11.3 MB with the whole font.

Source CRC verification still reads the entire file, in 4 KiB chunks; it does not buffer the whole
file. Only selected PCM is then copied into the compact file. MIDI imports send all intended
presets for each font with their first binding, so later channels reuse one allocation. Stored
projects derive the selection from all their channels. A manual bank/program change prepares a
replacement on the control thread and uses the normal instrument crossfade. Bank/program
automation uses the whole font so that audio-thread changes never read storage. Legacy
`channel.resource` requests without `bank`, `program` and `presets` also use the whole font;
refresh an older browser page to get selective MIDI imports.

Reusable file buffers survive project changes. A buffer is overwritten only after its last
resource owner has gone, including plugins awaiting audio-thread retirement. Excessively large
unused whole-font buffers are discarded before a subset allocation; suitable buffers can be
reused with at most 1 MiB excess capacity. Loads use checked allocations. Insufficient total or
contiguous PSRAM returns a `resources` error; a failed `channel.resource` keeps the old binding
and plugin. The 2 MiB project / protocol reserve remains enforced for new allocations.

## User presets (`components/daw_core/src/preset_library.cpp`)

"Save as preset" stores `{"plugin", "name", "params"}` (parameters by key) as one file in the
`presets` partition, using the sample library's power-safe store. File names are the plugin id's
CRC32 (8 hex digits), `_` and the preset name (≤ 30 characters), so presets of different plugins
can share a name. The library is indexed in RAM at boot. Loading a user preset applies the keys
it has and leaves the others as they are, so presets survive plugin versions with new parameters.
Factory preset names cannot be reused. Saving and deleting are refused while playing on the device.

## When flash is written — and when not

On the ESP32-S3, erasing or writing flash disables the flash cache: every task executing from
flash, including the audio task on core 1, stalls until the operation finishes (a sector erase is
tens of milliseconds; a 128 KB save is longer than a second). Therefore:

- the **audio path never touches storage**: the engine has no file or flash API at all;
- the **autosave** (slot 0) runs on the control task only while the transport is **stopped**, at
  least 10 s after the last change;
- **saving and deleting** a project slot, **uploading and deleting samples** are refused while
  playing on the device
  (`err` code `playing`, "stop playback first"); the Linux host has no such restriction;
- **firmware updates, reboot, factory reset** stop the transport first (`beforeFlashWork`);
- project **loading** only reads flash (no cache disable) and is allowed any time.

Buffers handed to flash writes come from internal RAM (writes sourced from PSRAM are slower and,
with the cache disabled, unsafe): the OTA handler uses a static internal buffer, the project store
copies its data through a 4 KB internal bounce buffer (`components/daw_esp_storage/partition_flash.cpp`).

## Project document

A project is one JSON document (`components/daw_core/src/project_json.cpp`), the same on every
target, also used for export / import (`GET`/`POST /api/project`, files named `*.esp32daw.json`):

```json
{"format":"esp32-daw-project","schema":1,"app":{"name":"esp32-s3-daw","version":"0.1.0"},"nextId":40,
 "meta":{"name":"...","author":"...","bpm":128,"timeSig":[4,4],"currentPattern":11,...},
 "channels":[{"id":1,"name":"Kick","plugin":{"id":"org.smhgzll.drumlab","version":"0.1.0",
              "params":{"piece":0,"decay":0.5,...},"preset":"..."},"vol":0.78,"pan":0,"track":1,...}],
 "patterns":[{"id":11,"name":"Beat","seqs":[{"channel":1,"steps":[[on,vel,pan,key,accent,prob],...],
              "notes":[[id,start,len,key,vel,pan,pitch,rel,prob,flags],...]}]}],
 "mixer":[{"index":0,"name":"Master","vol":0.8,"fx":[...],"sends":[...]},...],
 "playlist":{"tracks":[...],"clips":[...],"markers":[...]},
 "automation":[...]}
```

- **Patterns are written compactly** (2026-10-13): a channel's row with nothing in it is left out
  (the model makes it again when needed), a row's `steps` are left out when all are off with
  default values, and notes leave out trailing fields that hold their defaults (pan 0, pitch 0,
  release 0.5, probability 1, flags 0; at least `[id,start,len,key,vel]`). Earlier firmware
  reads these documents too. Steps and notes are written as JSON text rather than one cJSON node
  per number: an imported song of 216 patterns × 15 channels no longer needs megabytes of PSRAM
  to be written.
- **Plugin parameters are stored by key**, not by index, with the plugin's id and version: a plugin
  can add parameters later, and a project still loads (new parameters take their defaults).
- **Missing plugins are preserved.** A project that uses a plugin this firmware does not contain
  loads anyway: the channel / effect is kept verbatim (its full JSON), marked `missing`, silent,
  and written back unchanged on the next save. A firmware that has the plugin again plays it.
- **Sample references** are kept the same way until the sample storage milestone.
- **Versioning.** `schema` is the document version (`Project::kSchema`, currently 2; legacy exports stay 1). Loading runs
  the migrations from the document's schema up to the current one, one step at a time
  (`migrate()` in `project_json.cpp`; schema 1 defaults remain valid in schema 2). A document from a *newer* schema is refused
  with a clear message instead of being half-understood. Unknown fields are ignored.
- **Validation.** Imports are parsed and validated completely in RAM (ids, references, ranges,
  limits: 32 channels, 256 patterns, 4096 notes per sequence, 2048 clips, ...) before anything
  replaces the running project; an invalid document changes nothing.

## Settings (NVS, namespace `daw`)

`set`, `mode`, `ssid`, `pass`, `host`, `admin` (when `set` is present they override the build's
values from `config/local.env`; the browser's device settings write them), `boots` (boot counter), `phase` (QEMU self-test), `tfh` (QEMU-only test flag). The factory
reset erases the namespace and every project slot.


### MIDI projects (schema 2)

A sequence may have `midi: [[tick, order, status, data...], ...]`: every non-note channel
message and opaque SysEx/system/meta bytes. Notes append optional `midiOnOrder, midiOffOrder`
after their ten legacy fields. Both use per-tick source order; zero is ordinary DAW ordering.
Empty MIDI arrays/default order fields are omitted. Only projects using these fields write
schema 2; schema-1 projects load and export with their existing shape. New MIDI projects are
refused by older firmware rather than silently losing data. Decoding rejects malformed events
transactionally (65,536 messages / 512 KiB raw bytes per sequence); failed JSON duplication also
leaves the old project intact. These limits are independent of slot compression and available RAM.
