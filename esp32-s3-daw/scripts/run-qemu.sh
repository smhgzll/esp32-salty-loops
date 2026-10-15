#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run the QEMU-profile firmware in Espressif's QEMU: machine esp32s3, 16 MB flash, 16 MB PSRAM
# (quad mode, KNOWN_ISSUES KI-3), OpenCores Ethernet with the web server forwarded to the host.
#   scripts/run-qemu.sh                 interactive console (Ctrl-A X quits)
#   KEEP_FLASH=1                        keep build-qemu/flash_image.bin exactly as the last run left it
#   KEEP_DATA=1                         this build's bootloader / partition table / app, but the last
#                                       run's NVS, otadata and projects (an update by cable)
#   QEMU_LOG=file                       non-interactive: console to the file, stdin from /dev/null (KI-1)
#   QEMU_WEB_PORT=18080 (default)       the DAW at http://127.0.0.1:18080/
#   QEMU_TCG_THREAD=multi (default)     "single" for flash-writing tests (KI-2)
source "$(dirname "$0")/common.sh"
source_idf
B="$ROOT_DIR/build-qemu"
[[ -f "$B/flash_args" ]] || "$ROOT_DIR/scripts/build-qemu.sh"
cd "$B"
if [[ "${KEEP_DATA:-0}" == 1 && -f flash_image.bin ]]; then
  python3 "$ROOT_DIR/scripts/qemu-flash-update.py" "$B" flash_image.bin >/dev/null
elif [[ "${KEEP_FLASH:-0}" != 1 || ! -f flash_image.bin ]]; then
  python -m esptool --chip esp32s3 merge_bin --fill-flash-size 16MB -o flash_image.bin @flash_args >/dev/null
fi
QEMU=(qemu-system-xtensa -nographic -machine esp32s3 -m 16M -accel "tcg,thread=${QEMU_TCG_THREAD:-multi}"
      -drive file=flash_image.bin,if=mtd,format=raw
      -nic "user,model=open_eth,hostfwd=tcp:127.0.0.1:${QEMU_WEB_PORT:-18080}-:80")
if [[ -n "${QEMU_LOG:-}" ]]; then
  exec "${QEMU[@]}" < /dev/null > "$QEMU_LOG" 2>&1
fi
exec "${QEMU[@]}"
