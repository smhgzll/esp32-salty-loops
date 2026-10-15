#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the QEMU-profile firmware (null audio, OpenCores Ethernet, boot self-test) into build-qemu/.
source "$(dirname "$0")/common.sh"
source_idf
prepare_firmware_inputs
refresh_sdkconfig "$ROOT_DIR/build-qemu" "$ROOT_DIR/sdkconfig.defaults" "$ROOT_DIR/config/sdkconfig.qemu.defaults"
cd "$ROOT_DIR"
idf.py -B build-qemu -DIDF_TARGET=esp32s3 -DSDKCONFIG="$ROOT_DIR/build-qemu/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;config/sdkconfig.qemu.defaults" build
python3 "$ROOT_DIR/scripts/report-size.py" build-qemu || true
