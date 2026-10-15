# Architecture

A standalone DAW: the ESP32-S3 makes all the sound and owns the project; browsers (desktop, tablet,
phone) are the screens and controllers, connected over Wi-Fi. The same C++ core runs on three
targets, so most of the system is tested on a PC before it ever reaches the board.

```
                       browsers (Preact UI, canvas editors)            Linux computer
                        │  HTTP: app, export/import, OTA                │ scripts/midi-bridge.py
                        │  WebSocket: protocol v1 (docs/PROTOCOL.md)    │ (ALSA MIDI → WebSocket)
 ───────────────────────┼───────────────────────────────────────────────┼─────────────────────
  core 0                ▼                                               ▼
   httpd task (prio 5) ── components/daw_webserver: sockets, per-client send FIFOs, auth, OTA
        │ queue (48 msgs)
   control task "daw_ctl" (prio 6) ── main/control.cpp → daw::Session (components/daw_core)
        │   project document, validation, revisions, undo-friendly patches, autosave,
        │   plugin instantiation (PSRAM), compile → immutable PlayModel / MixModel
        │ SPSC command queue (256)              ▲ retire queue (256) + atomics (EngineStatus)
  core 1 ▼                                       │
   audio task "daw_audio" (prio 20) ── daw::Engine::render(): sequencer, plugins, mixer
        │   256-frame blocks, never allocates / locks / logs / touches flash or the document
        ▼
   components/daw_hal/audio_out.cpp ── I2S0 DMA (4 × 256) → PCM5102A   (null backend in QEMU)
```

## Repository layout

| path | what |
|------|------|
| `components/daw_core/` | portable core: engine, sequencer compiler, model, project JSON, store, MIDI, session. No ESP-IDF includes; `platform.hpp` is the only seam (allocation classes, clock, logging) |
| `components/daw_hal/` | ESP32-S3 platform: allocation (internal / PSRAM), I2S output, pins |
| `components/daw_esp_storage/` | flash partitions as `FlashDevice`, NVS settings |
| `components/daw_webserver/` | Wi-Fi / Ethernet, HTTP + WebSocket server, OTA, embedded web assets |
| `components/daw_plugins/` | links the imported plugin libraries + the generated registry |
| `main/` | `app_main` (boot, tasks, health check), control task, QEMU self-test, Kconfig |
| `host/` | Linux target: POSIX HTTP/WebSocket server, ALSA / null audio, offline WAV render (`daw-host`) |
| `web/` | browser application (TypeScript, Preact, Vite) |
| `plugins/` | `.espplug` packages (from the SDK), lock file, generated registry |
| `third_party/` | plugin ABI headers (copy from the SDK), cJSON |
| `tests/` | core unit tests, network e2e, MIDI bridge, OTA, browser (Playwright) |
| `config/` | partition table, per-profile sdkconfig defaults, `local.env.example` |
| `scripts/` | build / test / flash / doctor / bootstrap (see README.md) |

### Why one `daw_core` component instead of one component per subsystem

The brief suggested `daw_audio`, `daw_transport`, `daw_sequencer`, `daw_patterns`, `daw_playlist`,
`daw_midi`, `daw_mixer`, `daw_fx_host`, `daw_plugin_host`, `daw_storage`, `daw_protocol`,
`daw_diagnostics`, `daw_ota`. The portable subsystems are tightly coupled through the project model
and the engine's command set, and must build unchanged for Linux; one component with one file per
subsystem keeps a single CMake target for the host build and the firmware, while the ESP-specific
parts stay in their own components:

| suggested | here |
|---|---|
| daw_audio, daw_transport, daw_mixer, daw_fx_host | `daw_core/src/engine.cpp` (+ `daw_hal/audio_out.cpp`) |
| daw_sequencer, daw_patterns, daw_playlist | `daw_core/src/compile.cpp`, `model.cpp` |
| daw_plugin_host | `daw_core/src/plugin_host.cpp` |
| daw_midi | `daw_core/src/midi.cpp` |
| daw_storage | `daw_core/src/store.cpp`, `project_json.cpp`, `daw_esp_storage/` |
| daw_protocol, daw_diagnostics | `daw_core/src/session.cpp` |
| daw_webserver, daw_ota | `daw_webserver/` (`web_server.cpp`, `ota.cpp`, `net.cpp`) |

## Targets

| | Linux host (`daw-host`) | QEMU (`build-qemu`) | ESP32-S3 (`build-hw`) |
|---|---|---|---|
| core | same sources | same sources, Xtensa | same sources, Xtensa |
| audio | ALSA or null; offline WAV render | null backend paced by `esp_timer` | I2S0 → PCM5102A |
| network | POSIX sockets | OpenCores Ethernet (user-mode, port 18080) | Wi-Fi AP / STA, mDNS |
| storage | file-backed / RAM `MemFlash` | emulated 16 MB flash | 16 MB flash |
| PSRAM | – | 16 MB, quad mode (KI-3) | 16 MB octal |
| used for | development, unit + e2e + browser tests, reference renders | firmware logic, boot, storage, network, OTA + rollback | sound, real timing, Wi-Fi (UNVERIFIED so far) |

The cross-target render check runs the same 8-second project through the Linux build and the
Xtensa build in QEMU and compares RMS / peak / non-zero counts: they match exactly.

## Threads and ownership

- **The session owns the project.** Only the control task reads or writes the document. HTTP
  handlers that need it (`/api/project`, `/api/status`) run their work *on* the control task
  (`control::call`) and wait.
- **The engine owns the audio state.** The control task sends `EngineCmd`s (POD, 32 bytes) through
  a wait-free SPSC queue. Large data (compiled patterns / song, mixer graph, plugin instances,
  effect chains) is built on the control task and handed over **by pointer**; the engine swaps
  pointers at block boundaries and returns the old objects through the retire queue for the control
  task to free. Nothing is freed or allocated on the audio thread.
- **Status flows back through atomics** (`EngineStatus`): transport position, meters, CPU, voice
  count, error counters. The session samples them every 50 ms for the `st` message.
- A full command queue never blocks the control task: the command goes to a backlog and is retried
  on the next tick (`cmdOverflow` counts it).

## Memory

| what | where | why |
|------|-------|-----|
| engine buffers (17 tracks × 2 × 256 floats, event pool), audio task stack, I2S DMA | internal RAM | touched every block; PSRAM latency would cost CPU |
| plugin instances, `DAW_MEM_FAST` requests (filters, voice state) | internal RAM | hot DSP state |
| plugin `DAW_MEM_LARGE` (delay lines, wavetables) | PSRAM (internal if PSRAM is missing) | size |
| project document, compiled models, JSON, strings, WebSocket queues | PSRAM (global `operator new` prefers PSRAM) | keeps ~100 KB of internal RAM free for Wi-Fi / lwIP / httpd |
| web application | flash (embedded, gzipped, ~48 KB) | served straight from flash |

Internal heap after boot in QEMU: ~97–117 KB free, minimum ~64 KB (`heapInternal`, `heapInternalMin`
in `/api/status`). Hardware with Wi-Fi will have less: measure (docs/HARDWARE_TESTS.md H3).

## Plugins

Plugins are separate projects built with **esp32-daw-plugin-sdk** against a versioned C ABI
(`third_party/daw_plugin_abi`). Each becomes one relocatable object per target with a single
exported descriptor symbol, packaged as `.espplug`, imported into `plugins/` with ABI and symbol
checks, and linked into the firmware at build time. The engine talks to plugins only through the
descriptor (create / activate / process with sample-accurate events / parameters / state).
Details: docs/PLUGIN_INTEGRATION.md.

## Failure containment

- A plugin producing NaN / Inf: its output block is muted, an effect is taken out of the chain
  (`fxFailures`), the master has a safety clamp (`nanResets`).
- An overfull block event pool or note table: events dropped / oldest note ended, counted.
- A slow browser: loses meter frames, then gets a full resync; never unbounded memory.
- A disconnected browser: its held notes are released.
- A bad firmware update: refused before writing, or verified after writing, or rolled back after
  an unhealthy start.
- A power cut while saving: the previous copy of the slot loads.
