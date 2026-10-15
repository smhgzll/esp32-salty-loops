#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the plugins, the test host (dawplug) and the tests for this Linux machine.
source "$(dirname "$0")/common.sh"
read -r CCX CXXX < <(host_compilers)
BUILD="${BUILD_DIR:-$SDK_DIR/build-host}"
cmake -S "$SDK_DIR" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
  -DCMAKE_C_COMPILER="$CCX" -DCMAKE_CXX_COMPILER="$CXXX" >/dev/null
cmake --build "$BUILD"
echo "host build: $BUILD (tools in $BUILD/bin, plugin libraries in $BUILD/lib)"
