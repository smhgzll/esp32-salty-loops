#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Check the development environment: what can be built and tested here, and what is missing.
# Read-only; prints OK / WARN / MISSING per item and the command that fixes it.
source "$(dirname "$0")/common.sh"
set +e
miss=0
ok()   { printf '  OK       %-26s %s\n' "$1" "$2"; }
warn() { printf '  WARN     %-26s %s\n' "$1" "$2"; }
bad()  { printf '  MISSING  %-26s %s\n' "$1" "$2"; miss=1; }
have() { command -v "$1" >/dev/null 2>&1; }

echo "host build (daw-host, unit tests)"
if cc_pair="$(host_compilers 2>/dev/null)"; then ok "C/C++ compiler" "$cc_pair"; else bad "C/C++ compiler" "sudo dnf install gcc gcc-c++"; fi
have cmake && ok cmake "$(cmake --version | head -1)" || bad cmake "sudo dnf install cmake"
have ninja && ok ninja "$(ninja --version)" || warn ninja "optional (sudo dnf install ninja-build)"
[[ -f /usr/include/alsa/asoundlib.h ]] && ok "ALSA headers" "live audio in daw-host" || warn "ALSA headers" "daw-host builds without live audio (sudo dnf install alsa-lib-devel)"

echo "firmware (ESP-IDF v5.5.5)"
if p="$(find_idf)"; then
  v="$(sed -n 's/^set(IDF_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\))/\2/p' "$p/tools/cmake/version.cmake" | paste -sd.)"
  [[ "$v" == 5.5.5 ]] && ok ESP-IDF "$v at $p" || warn ESP-IDF "$v at $p (the project is tested with v5.5.5)"
else
  bad ESP-IDF "scripts/bootstrap-nobara.sh (or export IDF_PATH)"
fi
tc="$(ls -d "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin 2>/dev/null | tail -1)"
[[ -n "$tc" ]] && ok "xtensa toolchain" "$(basename "$(dirname "$(dirname "$tc")")")" || bad "xtensa toolchain" "\$IDF_PATH/install.sh esp32s3"
q="$(ls -d "$HOME"/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | tail -1)"
[[ -n "$q" ]] && ok "QEMU (xtensa)" "$(echo "$q" | sed 's#.*/qemu-xtensa/\([^/]*\)/.*#\1#')" || bad "QEMU (xtensa)" "python3 \$IDF_PATH/tools/idf_tools.py install qemu-xtensa"
if [[ -n "$q" ]] && ! "$q" --version >/dev/null 2>&1; then
  warn "QEMU libraries" "qemu-system-xtensa does not start: $("$q" --version 2>&1 | head -1) (sudo dnf install libgcrypt pixman SDL2 libslirp)"
fi
echo "browser UI"
have node && ok node "$(node --version)" || bad node "sudo dnf install nodejs npm"
[[ -d "$ROOT_DIR/web/node_modules" ]] && ok "web dependencies" "web/node_modules" || warn "web dependencies" "(cd web && npm ci)"
[[ -f "$ROOT_DIR/web/dist/index.html" ]] && ok "web build" "web/dist" || warn "web build" "scripts/build-web.sh (firmware embeds a fallback page without it)"
if python3 -c "import playwright" 2>/dev/null; then
  ls "$HOME"/.cache/ms-playwright/chromium* >/dev/null 2>&1 && ok "Playwright" "python + chromium" || warn "Playwright chromium" "python3 -m playwright install chromium"
else
  warn Playwright "browser tests skipped (pip install --user playwright; python3 -m playwright install chromium)"
fi

echo "plugins"
[[ -d "$ROOT_DIR/../esp32-daw-plugin-sdk" ]] && ok "plugin SDK checkout" "$(cd "$ROOT_DIR/../esp32-daw-plugin-sdk" && pwd)" || warn "plugin SDK checkout" "only needed to build new packages (plugins/packages has the current ones)"
n="$(ls "$ROOT_DIR"/plugins/packages/*.espplug 2>/dev/null | wc -l)"; [[ $n -gt 0 ]] && ok "plugin packages" "$n in plugins/packages" || bad "plugin packages" "scripts/import-plugins.sh --from-sdk"

echo "board"
ports=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null)
if [[ -n "$ports" ]]; then
  for p in $ports; do [[ -w "$p" ]] && ok "serial port" "$p" || warn "serial port" "$p not writable (sudo usermod -aG dialout \$USER; log in again)"; done
else
  warn "serial port" "no board connected: hardware tests stay UNVERIFIED (docs/HARDWARE_TESTS.md)"
fi
[[ -f "$ROOT_DIR/config/local.env" ]] && ok "config/local.env" "present (not printed)" || warn "config/local.env" "cp config/local.env.example config/local.env (Wi-Fi, admin password)"

echo "computer MIDI"
have aseqdump && ok "alsa-utils" "midi-bridge.py seq: inputs" || warn "alsa-utils" "sudo dnf install alsa-utils (sequencer inputs of scripts/midi-bridge.py)"
[[ $miss == 0 ]] && echo "doctor: nothing essential missing" || { echo "doctor: some essentials are missing (above)"; exit 1; }
