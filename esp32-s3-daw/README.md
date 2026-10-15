# Salty Loops — DAW component

The firmware, browser interface and Linux host of [Salty Loops](../README.md), a micro-DAW for
ESP32-S3. The [repository guide](../README.md) covers the combined DAW + plugin SDK build,
hardware requirements, screenshot and current feature scope.

A standalone digital audio workstation that runs on an **ESP32-S3 N16R16** with a **PCM5102A** DAC.
The board makes the sound and keeps the project; any browser on its Wi-Fi — desktop, tablet,
phone — is the screen: an FL Studio–style workspace on a computer, FL Studio Mobile–style tabs on
a phone. Several browsers can work on the same song at once.

- Channel rack and step sequencer (polymeter, swing, accents, probability), piano roll, playlist
  with pattern and automation clips, mixer with 16 inserts, sends and effect chains.
- Instruments and effects are plugins built with the separate
  [esp32-daw-plugin-sdk](../esp32-daw-plugin-sdk): instruments Sine, 3xOSC, FM6, DrumLab, Acid303, MacroSynth, Sampler, SoundFont;
  Tracker plays MOD, XM, S3M and IT modules;
  effects Reverb, Delay, Chorus, Flanger, EQ, Filter, Compressor, Limiter, Distortion, Utility.
- A 6 MiB sample library with 16 MB flash, or 22 MiB with the 32 MB layout (upload WAVs,
  SoundFonts and tracker modules from the browser), power-safe project storage, export / import, browser firmware updates with
  automatic rollback, MIDI from Web MIDI or a Linux computer, MIDI file import (as a project or into
  the piano roll).
- The same C++ core runs on the board, in QEMU and on Linux (live ALSA audio, offline WAV render).

> **Status:** development version 0.1.0. Native tests and real-board measurements cover the
> engine, plugins, MIDI import, tracker playback and update paths. Coverage and limits are
> recorded in [PERFORMANCE.md](docs/PERFORMANCE.md) and [HARDWARE_TESTS.md](docs/HARDWARE_TESTS.md);
> these measurements do not establish analog output quality or long-duration stability.
> See [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Quick start (Linux host)

First build the plugins from the SDK in the parent repository, following [Build](../README.md#build).
The plugin packaging step requires the ESP32-S3 toolchain; running the Linux host requires no board.

```sh
scripts/bootstrap-nobara.sh      # optional Fedora/Nobara setup
scripts/doctor.sh                # what is installed / missing
scripts/build-web.sh && scripts/build-host.sh
build-host/bin/daw-host --web web/dist --audio alsa
# open http://127.0.0.1:8080/
```

`daw-host --render out.wav --mode song --seconds 30` renders the demo song offline.

## The firmware in QEMU

```sh
scripts/build-qemu.sh
scripts/run-qemu.sh              # the DAW at http://127.0.0.1:18080/  (Ctrl-A X quits)
scripts/test-qemu.sh             # self-test, render comparison, network e2e, OTA + rollback
```

## The board

Wiring (measured; do not replace with example-project pins):

| PCM5102A | ESP32-S3 | | PCM5102A | connect to |
|---|---|---|---|---|
| BCK | GPIO16 (adapter IO33) | | VIN, XSMT | 3V3 |
| LCK | GPIO6 (adapter IO34) | | GND, SCK, FMT, FLT, DEMP | GND |
| DIN | GPIO7 (adapter IO35) | | jumpers | 1 L, 2 L, 3 H, 4 L |

```sh
cp config/local.env.example config/local.env    # Wi-Fi and admin passwords — never committed
scripts/build-hardware.sh                       # prints the pins it used
scripts/flash.sh --monitor
```

Then join the `ESP32-DAW` access point (or the network you configured) and open
`http://192.168.4.1/` or `http://esp32daw.local/`.

## Scripts

| script | |
|---|---|
| `bootstrap-nobara.sh`, `doctor.sh` | set up / check the machine |
| `build-host.sh`, `test-host.sh` | Linux build; unit, e2e and MIDI-bridge tests |
| `build-web.sh`, `test-browser.sh` | browser app; Playwright tests (`--url` for a device / QEMU) |
| `build-qemu.sh`, `run-qemu.sh`, `test-qemu.sh` | QEMU firmware |
| `build-hardware.sh`, `flash.sh`, `monitor.sh` | the board |
| `test-all.sh` | everything this machine can run, one summary |
| `import-plugins.sh`, `sync-plugin-abi.sh` | plugins from the SDK |
| `midi-bridge.py` | Linux MIDI keyboard → DAW |

## Documentation

docs/ARCHITECTURE.md · AUDIO_ENGINE.md · DAW_FEATURES.md · UI_SPEC.md · PROTOCOL.md · MIDI.md ·
PLUGIN_INTEGRATION.md · STORAGE.md · SECURITY.md · QEMU.md · HARDWARE_TESTS.md · PERFORMANCE.md ·
LICENSES.md · ROADMAP.md — and PROGRESS.md, NEXT_STEPS.md, KNOWN_ISSUES.md.

## License

GPL-3.0-or-later, © 2026 smhgzll. Third-party components and their licenses: docs/LICENSES.md;
the device serves the complete notices at `/licenses.txt`.

## Performance examples

[4/6/8-track projects](examples/performance/README.md) include live grooves and dense overload
fixtures, with generated WAV/SF2 resources. [Measured performance](docs/PERFORMANCE.md) covers
the bundled plugins, model/algorithm/polyphony limits, RAM placement, UI profiling and the optional
32 kHz capacity profile. The normal build remains 44.1 kHz at 240 MHz.
