# Plugin integration

Instruments and effects are **separate projects** built with
[esp32-daw-plugin-sdk](../../esp32-daw-plugin-sdk) (its own repository, MIT). The DAW never
includes plugin source; it imports **packages** and links their libraries at firmware build time.

```
esp32-daw-plugin-sdk                         esp32-s3-daw
  plugins/<name>/*.cpp ── build (host + esp32s3) ──► dist/<id>-<ver>.espplug
                                                         │ scripts/import-plugins.sh
                                                         ▼
                                    plugins/packages/*.espplug   (committed)
                                    plugins/plugins.lock.json    (committed: ids, versions, sha256)
                                    plugins/imported/<id>/       (extracted, gitignored)
                                    plugins/generated/           (registry, cmake list, catalog)
                                                         │ firmware / daw-host build
                                                         ▼
                                    linked into the image; listed in the browser's plugin catalog
```

## The contract

- **C ABI 1.4** (1.1: resources; 1.2: note chase; 1.3: optional host-owned parallel DSP jobs; 1.4: opt-in timed MIDI; `third_party/daw_plugin_abi/`, a copy of the SDK's `include/`, updated only by
  `scripts/sync-plugin-abi.sh`, which refuses a major-version change). A descriptor with
  `create / destroy / activate / deactivate / process / set_param / get_param / save_state /
  load_state / active_voices`; 32-byte events with sample offsets; parameters with stable keys,
  ranges, scales, units, groups; presets; capability flags (polyphony, slide, accent, per-note
  pan / pitch, sustain, pitch bend, drum kit, tail, ...).
- **Compatibility**: a package's ABI major must equal the DAW's, its minor must not be newer.
  `PluginRegistry::compatible()` checks every linked descriptor again at start-up.
- **Memory** comes only from the host (`daw_host_api_t::alloc`): `DAW_MEM_FAST` → internal RAM,
  `DAW_MEM_LARGE` → PSRAM; FAST falls back to LARGE with a warning when internal RAM is short. All
  allocation happens in `create` / `activate` on the control thread; per-instance accounting is
  visible on the device page. FAST allocation also falls back if the request would leave less
  than 32 KiB of internal RAM at allocation time (including temporary probes and replacement
  instances). This check does not reserve that memory against later allocations by other tasks.
- **Real time**: `process()` runs on the audio thread and must not allocate, lock, log or do I/O.
  ABI 1.3 adds an optional size-checked `host->parallel` service: two independent DSP callbacks
  complete before it returns 1; returning 0 invokes neither, and the plugin uses its serial path.
  On hardware it shares the existing DSP task and declines nested calls or a busy worker, so it
  cannot overwrite an engine job. No task or scratch allocation occurs during rendering.

## What the importer checks (`scripts/import-plugins.py`)

For every package, before anything is linked:

1. archive safety (no absolute paths, no `..`), a manifest, format version;
2. ABI compatibility with `third_party/daw_plugin_abi`;
3. SHA-256 of every file listed in the manifest;
4. both target libraries present (`host-x86_64-linux`, `esp32s3`);
5. the entry point is the **only** global symbol (`nm`), so plugins cannot collide with each other
   or with the DAW;
6. undefined symbols only from an **allowlist**: `memcpy`-family and `strlen` / `strcmp`, libm
   (float and double), compiler helpers (`__*`). Anything else — allocation, `new`, stdio, threads,
   RTOS, ESP-IDF, exceptions — rejects the package;
7. unique ids and entry symbols across all packages.

Then it extracts to `plugins/imported/<id>/`, writes the registry (`daw_plugin_registry.c`), the
CMake library list, the catalog, and the lock file. The license texts each package ships
(`licenses/`) end up in the device's `/licenses.txt` (`scripts/gen-notices.py`).

**Packages are native code linked into the firmware: import only packages you trust.** The checks
above catch mistakes and accidental real-time violations, not malice. There is no runtime loading
of native code (docs/SECURITY.md).

## Adding or updating a plugin

```sh
cd ../esp32-daw-plugin-sdk
scripts/test-host.sh                 # behaviour tests + `dawplug validate` for every plugin
scripts/package.sh <name>            # → dist/<id>-<version>.espplug
cd ../esp32-s3-daw
python3 scripts/import-plugins.py ../esp32-daw-plugin-sdk/dist/<id>-<version>.espplug [others...]
#   (the import set is exactly the packages given: list every plugin the build should contain)
# or rebuild and import the SDK's whole set:
scripts/import-plugins.sh --from-sdk
scripts/test-host.sh && scripts/test-qemu.sh
git add plugins/packages plugins/plugins.lock.json plugins/generated
```

## Resources (ABI 1.1)

Plugins with resource slots (the Sampler) get files from the device's sample library: the session
reads the file (CRC-checked), decodes WAVs into float buffers in PSRAM (shared between instances of
the same file) or passes raw bytes (`.sf2`), and calls `set_resource()` on a fresh instance before
activating it; changing a channel's sample builds a new instance and the engine crossfades. The
instance keeps its resources alive until it is destroyed (after the engine has let go of it).

## Projects and plugin versions

Projects store a plugin by **id + version** and its parameters by **key**. A newer plugin version
with more parameters loads old projects (new parameters take defaults); removed keys are ignored.
A project whose plugin is not in this firmware keeps that channel / effect verbatim and silent
(`missing`) and writes it back unchanged (docs/STORAGE.md).

## Linked plugins (0.1.0): 17

All of them do their audio-path math in single precision (the S3 has no double-precision FPU);
the SDK's `build-esp32.sh` warns when a plugin references software double arithmetic.


| id | type | notes |
|---|---|---|
| `org.smhgzll.sine` | instrument | reference sine synth (ABI conformance) |
| `org.smhgzll.3xosc` | instrument | three oscillators, filter, envelopes, glide / slide, 40 parameters |
| `org.smhgzll.drumlab` | instrument | 8 analog drum models (DaisySP, MIT, pinned + patched; PROVENANCE in the package) |
| `org.smhgzll.fm6` | instrument | six-operator FM: the DX7's 32 algorithm routings, feedback, per-operator ratio / level / ADSR / velocity, vibrato; 4 voices by default (up to 8) |
| `org.smhgzll.sampler` | instrument | plays a file from the sample library (ABI 1.1 resource slot "sample"): root key, loop, reverse, one-shot, ADSR, filter, 16 voices |
| `org.smhgzll.soundfont` | instrument | plays .sf2 files from the sample library (TinySoundFont, MIT, voice loop ported to float; file resource slot "soundfont"), bank / program, 24 voices |
| `org.smhgzll.tracker` | instrument | plays MOD / XM / S3M / IT songs from the sample library (libxmp 4.7.4 lite, MIT, patched: scan hook, exact tick lengths, mixing in parts on two cores; file resource slot "module"); note chase (ABI 1.2), follows the project tempo; docs/TRACKER.md |
| `org.smhgzll.macrosynth` | instrument | the 48 macro-oscillator models of Mutable Instruments Braids (MIT, pinned, unmodified) with Timbre / Color, ADSR; 2× rate + halfband decimator; 2 voices by default (up to 4) |
| `org.smhgzll.acid303` | instrument | mono TB-303 bass: Open303's ladder filter, envelopes, accent and slide (MIT, pinned, ported to float); step accents and piano-roll slides map to the 303's |
| `org.smhgzll.fx.delay` | effect | stereo / ping-pong delay, tempo sync, damping (delay lines in PSRAM) |
| `org.smhgzll.fx.filter` | effect | state-variable filter: low / band / high pass, notch, drive |
| `org.smhgzll.fx.limiter` | effect | look-ahead brickwall limiter, stereo linked |
| `org.smhgzll.fx.utility` | effect | gain, balance, stereo width, mono, channel swap, polarity |
| `org.smhgzll.fx.reverb` | effect | comb / all-pass room (after the public-domain Freeverb design): size, damping, pre-delay, low cut, width, freeze; 137 KB of PSRAM |
| `org.smhgzll.fx.chorus` | effect | 1–3 voices, stereo spread, tone |
| `org.smhgzll.fx.flanger` | effect | ± feedback (soft-limited), stereo spread |
| `org.smhgzll.fx.eq` | effect | low / high cut, two shelves, two peaking bands |
| `org.smhgzll.fx.compressor` | effect | stereo-linked, soft knee, makeup, parallel mix |
| `org.smhgzll.fx.distortion` | effect | anti-aliased (ADAA) soft / hard clipping, wavefolder, bit crusher |

Planned: KNOWN_ISSUES.md KI-13, ROADMAP.md.

## Dynamic loading (M11, experimental, not implemented)

Loading `.espplug` ELF objects at run time (e.g. espressif/elf_loader) would let users add
plugins without a firmware build. It is deliberately **not** done: a native ELF loader is not a
sandbox — a loaded plugin has the same access as the firmware (memory, flash, Wi-Fi). If it is ever
added it will be off by default, accept only signed packages, run the same symbol checks on the
device, and be documented as trusting the signer completely.
