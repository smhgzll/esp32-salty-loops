#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the Linux host: daw-host (server + ALSA + offline render) and the core tests (build-host/).
#   DAW_SANITIZE=1 scripts/build-host.sh   AddressSanitizer + UBSan build in build-host-asan/
source "$(dirname "$0")/common.sh"
[[ -f "$ROOT_DIR/plugins/generated/plugins.cmake" && -d "$ROOT_DIR/plugins/imported" ]] || python3 "$ROOT_DIR/scripts/import-plugins.py" --no-esp-check
read -r CCX CXXX < <(host_compilers)
B="$ROOT_DIR/build-host"; SAN=OFF
if [[ "${DAW_SANITIZE:-0}" == 1 ]]; then B="$ROOT_DIR/build-host-asan"; SAN=ON; fi
cmake -S "$ROOT_DIR/host" -B "$B" -G Ninja -DCMAKE_C_COMPILER="$CCX" -DCMAKE_CXX_COMPILER="$CXXX" -DDAW_SANITIZE=$SAN >/dev/null
cmake --build "$B"
echo "host build: $B/bin/daw-host, tests in $B/tests"
