#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Flash the hardware build (build-hw/, scripts/build-hardware.sh) over USB.
#   scripts/flash.sh [--port /dev/ttyACM0] [--app-only] [--erase-all] [--monitor]
# Default: bootloader + partition table + OTA data + app. Projects, presets, samples and settings
# partitions are not touched (--erase-all wipes the whole chip first: factory state).
source "$(dirname "$0")/common.sh"
load_local_env
PORT="${ESPPORT:-${DAW_SERIAL_PORT:-}}"; TARGET=flash; ERASE=0; MON=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port|-p) PORT="$2"; shift ;;
    --app-only) TARGET=app-flash ;;
    --erase-all) ERASE=1 ;;
    --monitor) MON=1 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[[ -f "$ROOT_DIR/build-hw/esp32_s3_daw.bin" ]] || { echo "no hardware build: run scripts/build-hardware.sh first" >&2; exit 1; }
if [[ -z "$PORT" ]]; then
  for p in /dev/ttyACM* /dev/ttyUSB*; do [[ -e "$p" ]] && { PORT="$p"; break; }; done
fi
[[ -n "$PORT" && -e "$PORT" ]] || { echo "no ESP32-S3 serial port found (connect the board's USB port, or pass --port)" >&2; exit 1; }
[[ -w "$PORT" ]] || { echo "$PORT is not writable: add yourself to its group (ls -l $PORT; sudo usermod -aG dialout \$USER), log in again" >&2; exit 1; }
grep -E "^CONFIG_DAW_I2S_(BCLK|WS|DOUT)_GPIO=" "$ROOT_DIR/build-hw/sdkconfig" | sed 's/^/flashing with pins: /'
source_idf
cd "$ROOT_DIR"
if [[ $ERASE == 1 ]]; then
  read -r -p "Erase the whole flash of the board on $PORT (projects, samples, settings)? [y/N] " a
  [[ "$a" == y || "$a" == Y ]] || exit 1
  idf.py -B build-hw -p "$PORT" erase-flash
fi
args=("$TARGET"); [[ $MON == 1 ]] && args+=(monitor)
idf.py -B build-hw -p "$PORT" "${args[@]}"
