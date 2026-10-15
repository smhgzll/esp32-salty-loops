# Security model

The DAW is a local device: an instrument on a home / studio network or on its own Wi-Fi access
point, used from browsers next to it. The model protects against **other web pages** driving the
device through a user's browser, **other people on the same network** changing the device itself
(firmware, factory reset, project restore), and **untrusted native code**. It does not try to be
an internet-facing server.

## What is protected, and how

| threat | protection | where |
|--------|-----------|-------|
| A web page elsewhere drives the device through the user's browser (CSRF, cross-site WebSocket) | Every WebSocket upgrade and every POST whose `Origin` header is not `http://<Host>` is refused. Non-browser clients (no `Origin`) are allowed: they cannot be triggered by a web page. | `sameOrigin()` in `components/daw_webserver/web_server.cpp`, `host/http_server.cpp` |
| Someone on the network replaces the firmware, erases the device, changes its settings or overwrites the project with an import | `POST /api/ota`, `/api/factory-reset`, `/api/reboot`, `/api/project`, `/api/settings` need a bearer token from `POST /api/login` with the admin password. Tokens: 128 random bits, 1 hour, 4 at most (the oldest is replaced). Password comparison in constant time; a wrong password costs 1 s. No admin password configured → these operations are disabled. | `onLogin()`, `authorized()` |
| A wrong or hostile firmware image | The OTA handler checks the image *before* writing: ESP image magic, chip = ESP32-S3, project name, firmware profile (a QEMU build is refused on hardware and the reverse). `esp_ota_end` verifies segments + SHA-256. The new image must confirm itself after a healthy start (audio running, control plane booted) or the bootloader rolls back. | `components/daw_webserver/ota.cpp`, `healthTask` in `main/app_main.cpp` |
| Untrusted native plugin code | **No runtime loading of native code.** Plugins are compiled into the firmware from `.espplug` packages at build time; the package importer checks the ABI version, that each library exports exactly its one entry symbol and references nothing outside an allowlist (mem*/str* basics, libm, compiler helpers): no allocation, stdio, threads, RTOS or ESP-IDF APIs. The browser can only *describe* plugin controls; the UI renders them from parameter descriptors, never from plugin-provided markup or script. | `scripts/import-plugins.py`, `docs/PLUGIN_INTEGRATION.md` |
| Resource exhaustion from clients | Bounded everything: 4 WebSocket clients, 256 KB per message, 1024 bytes per MIDI message, request bodies (login 512 B, project import 512 KB, OTA = slot size), per-client send queues (24 KB live / 192 KB state) — slow clients lose meter frames, then get resynchronised instead of growing memory. The engine command queue is bounded; refused commands are counted and retried. | `web_server.cpp`, `session.cpp` |
| Secrets in the repository | Wi-Fi and admin passwords live in `config/local.env` (gitignored), turned into `config/sdkconfig.local.defaults` (gitignored) at build time. The build refuses the template's placeholder passwords. No credentials in tracked files; QEMU builds use a fixed test password that only exists in emulation. | `scripts/build-hardware.sh`, `.gitignore` |
| An open access point | Never: with no Wi-Fi password of ≥ 8 characters configured, every boot generates a random WPA2 password and prints it on the serial console. | `main/app_main.cpp` |

## Deliberate limits (know them)

- **Plain HTTP.** Passwords and tokens cross the network unencrypted. On the device's own access
  point (WPA2) that is the Wi-Fi encryption; on a shared network, anyone who can sniff it can
  capture the admin token. TLS on the ESP32-S3 is possible (esp_https_server) but costs tens of KB of
  internal RAM per connection (mbedTLS buffers) and needs certificate handling that browsers accept for a local IP;
  it is on the roadmap, not implemented.
- **Editing is not authenticated.** Any same-origin browser on the network can open the DAW and
  edit, play, save, load and delete project slots — like walking up to an instrument. Only the
  device-level operations above need the admin password.
- **Project export is not authenticated** (`GET /api/project`): anyone who can reach the device
  can download the current project.
- **No rate limit across connections** beyond the 1 s delay per wrong password (four parallel
  attempts per second at most, bounded by the httpd's socket limit).
- **No secure boot / flash encryption** in the default configuration. Someone with physical access
  can read or replace the firmware and the NVS (Wi-Fi password, admin password) over USB. Enabling
  them is an ESP-IDF configuration step that is irreversible on a chip; see ESP-IDF's security
  guide before doing it on a board you want to keep reflashable.

## Configuration changes

Device settings (Wi-Fi mode, network name, Wi-Fi password, device name, admin password) start
from the build (`config/local.env`) and can be changed from the browser's device page:
`GET` / `POST /api/settings`, both behind the admin token, the POST also same-origin only. The
passwords are write-only (the device reports only whether one is set). The admin password can be
changed but not removed from the browser (8–64 characters); changing it revokes every login token
immediately. Changes are stored in NVS; Wi-Fi and the device name apply after a restart, and a
device that cannot join the configured network opens its own access point again. A factory reset
returns to the build's settings.

## Reporting

This is a personal project (owner: smhgzll). Report security problems privately to the repository
owner rather than in a public issue.
