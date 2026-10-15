#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Cross-compile the plugin libraries for the ESP32-S3 with ESP-IDF's toolchain (build-esp32/lib).
source "$(dirname "$0")/common.sh"
source_idf
BUILD="$SDK_DIR/build-esp32"
cmake -S "$SDK_DIR" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SDK_DIR/cmake/toolchain-esp32s3.cmake" >/dev/null
cmake --build "$BUILD"
xtensa-esp32s3-elf-size -t "$BUILD"/lib/*.a | tail -1 | awk '{print "esp32s3 plugin code+rodata total: " $1+$2 " bytes"}'
# The S3's FPU is single precision: double arithmetic and double libm calls are emulated in
# software (tens to hundreds of cycles each). Conversions alone (e.g. of the ABI's double tempo,
# once per block) are fine; arithmetic in a plugin is a performance bug unless it is off the audio path.
for lib in "$BUILD"/lib/libdaw_plugin_*.a; do
  d="$(xtensa-esp32s3-elf-nm -u "$lib" | awk '{print $2}' | grep -E '^__(add|sub|mul|div)df3$|^(sin|cos|tan|exp|exp2|log|log2|log10|pow|sqrt|tanh|fmod)$' | tr '\n' ' ' || true)"
  if [[ -n "$d" ]]; then
    name="$(basename "$lib" .a | sed 's/^libdaw_plugin_//')"
    # a plugin may record why its remaining double math is off the per-sample path ("doubleMath" in plugin.json)
    why="$(python3 -c "import json,glob,sys
for p in glob.glob('$SDK_DIR/plugins/*/plugin.json')+glob.glob('$SDK_DIR/effects/*/plugin.json')+glob.glob('$SDK_DIR/examples/*/plugin.json'):
    m=json.load(open(p))
    if m.get('name')=='$name': print(m.get('doubleMath',''))" 2>/dev/null)"
    if [[ -n "$why" ]]; then
      echo "note: $name uses double-precision math, reviewed: $why"
    else
      echo "warning: $name uses double-precision math (software on the ESP32-S3): $d"
    fi
  fi
done
echo "esp32s3 build: $BUILD/lib"
