# QEMU

Espressif's QEMU (`qemu-system-xtensa`, `esp_develop_9.2.2_20260417`, installed by ESP-IDF's
`idf_tools.py install qemu-xtensa`) runs the real Xtensa firmware: the same compiler, the same
ESP-IDF, FreeRTOS on two cores, flash with partitions, PSRAM, NVS, the HTTP / WebSocket server,
OTA and the bootloader's rollback. It is the closest thing to the board without the board.

## What QEMU covers — and what it does not

| covered | not covered |
|---|---|
| boot, partitions, NVS, PSRAM allocation, both cores, tasks | I2S / the DAC: the QEMU profile uses a null audio backend paced by `esp_timer` |
| the engine and all plugins compiled for Xtensa; offline render statistics identical to the Linux build | real CPU time: emulated time is not ESP32 time (never reported as such) |
| project storage on emulated flash, autosave, restart, restore | flash timing and its cache stalls |
| network: HTTP, WebSocket, protocol, multiple clients, backpressure (OpenCores Ethernet) | Wi-Fi, mDNS |
| browser UI served by the firmware (the Playwright suite passes against it) | octal PSRAM (quad mode instead, KI-3) |
| OTA: refusals, verification, update, self-confirmation, rollback | USB, GPIO |

## Profile

`config/sdkconfig.qemu.defaults` on top of `sdkconfig.defaults`: `CONFIG_DAW_QEMU_BUILD`, quad PSRAM,
OpenCores Ethernet instead of Wi-Fi, no mDNS, the boot self-test, admin password
`qemu-test-password` (emulation only), firmware version `0.1.0+qemu`. The `+qemu` suffix is the
firmware *profile*: the OTA handler refuses a QEMU image on hardware and a hardware image in QEMU.

## Commands

```sh
scripts/build-qemu.sh            # build-qemu/, also rebuilds web/ when its sources changed
scripts/run-qemu.sh              # interactive console (Ctrl-A X quits); DAW at http://127.0.0.1:18080/
scripts/test-qemu.sh             # the automated tests below
scripts/test-browser.sh --url http://127.0.0.1:18080/   # browser suite against a running QEMU
```

`run-qemu.sh` options: `KEEP_FLASH=1` (reuse the flash image exactly as the last run left it),
`KEEP_DATA=1` (this build's bootloader / table / app, but keep NVS, otadata and projects — like a
cable update), `QEMU_LOG=file` (non-interactive, KI-1), `QEMU_WEB_PORT`, `QEMU_TCG_THREAD=single|multi`.

## `scripts/test-qemu.sh`

1. **Self-test** (fresh 16 MB flash, single-threaded TCG): boot, offline render of the demo
   project through every plugin (`DAW_RENDER ...` statistics), live audio task running, marker
   project saved by the autosave, **restart**, marker restored, heap integrity. Then the
   **cross-target comparison**: the same render on Linux must give the same RMS / peak / non-zero
   counts (it does, exactly).
2. **Network end-to-end** (same flash): `tests/web/test_e2e.py` over the emulated Ethernet —
   handshake, origin checks, invalid input, edits from two clients, stale edits, transport,
   live notes and their release on disconnect, MIDI bursts, a reconnect storm at the client limit,
   a slow client, save / load, login, export / import.
3. **Firmware update** (same flash): `tests/web/test_ota.py` — refusals before anything is written
   (no login, cross-origin, random bytes, the hardware image), a corrupted image that fails
   SHA-256 verification without a restart, a real update that boots from the other slot and
   confirms itself, then a forced unhealthy start (QEMU-only test hook
   `POST /api/test/fail-next-health`, token-protected, absent from hardware builds) that the
   bootloader rolls back. The project survives all of it.

Last full run (2026-10-11): all three phases PASS; render MATCH.

## Known QEMU limitations (KNOWN_ISSUES.md)

- **KI-1**: QEMU's console reads stdin; non-interactive runs must redirect it (`QEMU_LOG`).
- **KI-2**: this QEMU's multi-threaded TCG occasionally crashes the *emulator* (host segfault)
  around flash writes; tests use single-threaded TCG, and the self-test retries once on an
  emulator crash (never on a firmware failure).
- **KI-3**: octal PSRAM data accesses crash QEMU 9.2.2; the QEMU profile uses quad mode (same size
  and memory map).
