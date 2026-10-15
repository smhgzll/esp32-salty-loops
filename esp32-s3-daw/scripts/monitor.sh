#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Serial console of the board (decodes panics with build-hw's ELF). Ctrl+] quits.
#   scripts/monitor.sh [--port /dev/ttyACM0]
source "$(dirname "$0")/common.sh"
load_local_env
PORT="${ESPPORT:-${DAW_SERIAL_PORT:-}}"
[[ "${1:-}" == --port || "${1:-}" == -p ]] && PORT="$2"
if [[ -z "$PORT" ]]; then
  for p in /dev/ttyACM* /dev/ttyUSB*; do [[ -e "$p" ]] && { PORT="$p"; break; }; done
fi
[[ -n "$PORT" && -e "$PORT" ]] || { echo "no ESP32-S3 serial port found (pass --port)" >&2; exit 1; }
source_idf
cd "$ROOT_DIR"
exec idf.py -B build-hw -p "$PORT" monitor
