#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build host + ESP32-S3 libraries and pack every plugin (or the ones named) as dist/<id>-<version>.espplug
#   scripts/package.sh [plugin-dir-name ...]
source "$(dirname "$0")/common.sh"
"$SDK_DIR/scripts/build-host.sh" >/dev/null
"$SDK_DIR/scripts/build-esp32.sh" >/dev/null
source_idf
python3 "$SDK_DIR/tools/package.py" --sdk "$SDK_DIR" --host-build "$SDK_DIR/build-host" \
  --esp-build "$SDK_DIR/build-esp32" --out "$SDK_DIR/dist" "$@"
