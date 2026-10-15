# esp32-daw-plugin-sdk

Instrument and effect plugin SDK for **[Salty Loops](../README.md)**, an ESP32-S3 micro-DAW
([DAW component](../esp32-s3-daw)). Both components are included in this repository.
Author / owner: [smhgzll](https://github.com/smhgzll). License: MIT (SDK); every plugin states its own.

The SDK defines a versioned C ABI, a header-only C++ helper layer, a
Linux test host, conformance checks, benchmarks, cross-compilation for the ESP32-S3 and a package
format (`.espplug`) that the DAW imports at **firmware build time**.

```bash
./scripts/bootstrap.sh     # check tools, locate ESP-IDF v5.5.x (needed for the ESP32 build only)
./scripts/build-host.sh    # plugins + test host + tests for Linux (build-host/)
./scripts/test-host.sh     # unit tests, conformance of every plugin, reference WAV renders
./scripts/benchmark.sh     # host benchmarks (relative; not ESP32 numbers)
./scripts/build-esp32.sh   # cross-compile the plugin libraries with ESP-IDF's xtensa GCC
./scripts/package.sh       # dist/<id>-<version>.espplug for every plugin
```

Then in the DAW: `scripts/import-plugins.sh ../esp32-daw-plugin-sdk/dist/*.espplug` and rebuild
the firmware. Uploading a package to a running device does **not** make its code executable:
static integration is the supported path (docs/PLUGIN_FORMAT.md; dynamic loading is a future
experiment).

## Plugins

| Id | Kind | Engine | Status |
| --- | --- | --- | --- |
| `org.smhgzll.sine` | instrument | original | reference plugin: per-note pan / pitch |
| `org.smhgzll.3xosc` | instrument | original | 3 oscillators, SVF + envelopes, LFO, glide / slide, 8 voices |
| `org.smhgzll.drumlab` | instrument | DaisySP drum models (MIT) + original clap | kit or single-piece mode, 8 pieces |
| `org.smhgzll.fm6` | instrument | original | six-operator FM, the DX7's 32 algorithms, feedback, per-operator ADSR, 8 voices |
| `org.smhgzll.sampler` | instrument | original | plays a WAV across the keyboard (ABI 1.1 resource): loop, reverse, one-shot, ADSR, filter, 16 voices |
| `org.smhgzll.soundfont` | instrument | TinySoundFont (MIT), voice loop ported to float | plays .sf2 instruments (ABI 1.1 file resource), bank / program, 24 voices |
| `org.smhgzll.tracker` | instrument | libxmp 4.7.4 lite (MIT), patched: scan hook, exact tick lengths, mixing in parts, on two cores | plays MOD / XM / S3M / IT songs (ABI 1.1 file resource): a note plays the song, note chase (ABI 1.2) continues it where the transport starts, follows the project tempo; channel range, stereo separation, interpolation |
| `org.smhgzll.acid303` | instrument | Open303 filter / envelopes (MIT) + original oscillator | mono TB-303 bass: accent, slide, last-note priority |
| `org.smhgzll.macrosynth` | instrument | Braids macro oscillator (MIT), instance-owned RNG patch | the 48 Braids models with Timbre / Color, ADSR, 4 voices at 2× rate |
| `org.smhgzll.fx.utility` | effect | original | gain, balance, width, mono, swap, polarity |
| `org.smhgzll.fx.filter` | effect | original | SVF LP/BP/HP/notch, drive, mix |
| `org.smhgzll.fx.delay` | effect | original | stereo / ping-pong, tempo sync, damping (PSRAM lines) |
| `org.smhgzll.fx.limiter` | effect | original | 1.5 ms look-ahead brickwall |
| `org.smhgzll.fx.reverb` | effect | original (comb / all-pass network after the public-domain Freeverb design) | size, damping, pre-delay, low cut, width, freeze; lines in PSRAM |
| `org.smhgzll.fx.chorus` | effect | original | 1–3 voices, cubic-interpolated, stereo spread, tone |
| `org.smhgzll.fx.flanger` | effect | original | ± feedback (soft-limited), stereo spread |
| `org.smhgzll.fx.eq` | effect | original (RBJ biquads) | low / high cut, two shelves, two peaks |
| `org.smhgzll.fx.compressor` | effect | original | stereo-linked, soft knee, makeup, parallel mix |
| `org.smhgzll.fx.distortion` | effect | original | ADAA soft / hard clip, wavefolder, bit crusher |
| `org.smhgzll.example.tremolo` | effect | original, plain C | example of the raw C ABI |

Planned (docs/ROADMAP in the DAW): OPL3 (Nuked-OPL3, LGPL-2.1), optional.

## Layout

```text
include/            the C ABI (daw_plugin_api.h, _events.h, _params.h, _host_api.h)
core/include/dawsdk C++ helpers: plugin.hpp (adapter), dsp.hpp, voices.hpp
host/               native test host (allocation accounting, offline render, WAV)
tools/              dawplug CLI (list / info / render / bench / validate), package.py
plugins/ effects/   the plugins, each with plugin.json (+ PROVENANCE.md, licenses)
examples/           plain-C example
third_party/        vendored upstream code (pinned, patched, see docs/LICENSES.md)
tests/              ABI layout, DSP accuracy, plugin behaviour
cmake/              daw_add_plugin(), ESP32-S3 toolchain file
```

Documentation: [SDK](docs/SDK.md) · [ABI](docs/ABI.md) · [Building](docs/BUILDING.md) ·
[Package format](docs/PLUGIN_FORMAT.md) · [Real-time rules](docs/REALTIME_RULES.md) ·
[Performance](docs/PERFORMANCE.md) · [Licenses](docs/LICENSES.md)
