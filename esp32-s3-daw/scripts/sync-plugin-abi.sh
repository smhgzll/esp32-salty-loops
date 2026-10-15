#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copy the plugin ABI headers from an esp32-daw-plugin-sdk checkout into third_party/daw_plugin_abi
# (the only SDK files the DAW compiles against) and record the SDK commit and ABI version.
#   scripts/sync-plugin-abi.sh [path/to/esp32-daw-plugin-sdk]   (default: ../esp32-daw-plugin-sdk)
# Refuses an ABI major change: that needs a deliberate migration (docs/PLUGIN_INTEGRATION.md).
source "$(dirname "$0")/common.sh"
SDK="$(cd "${1:-$ROOT_DIR/../esp32-daw-plugin-sdk}" && pwd)"
DST="$ROOT_DIR/third_party/daw_plugin_abi"
[[ -f "$SDK/include/daw_plugin_api.h" ]] || { echo "no SDK at $SDK (include/daw_plugin_api.h missing)" >&2; exit 1; }
ver() { sed -n "s/^#define DAW_PLUGIN_API_$2 \+\([0-9]\+\).*/\1/p" "$1/daw_plugin_api.h"; }
old_major="$(ver "$DST" MAJOR)"; new_major="$(ver "$SDK/include" MAJOR)"; new_minor="$(ver "$SDK/include" MINOR)"
if [[ -n "$old_major" && "$old_major" != "$new_major" && "${FORCE_ABI_MAJOR:-0}" != 1 ]]; then
  echo "the SDK's plugin ABI major is $new_major, the DAW's $old_major: every plugin package must be" >&2
  echo "rebuilt and the host adapted. Re-run with FORCE_ABI_MAJOR=1 when that is the plan." >&2
  exit 1
fi
commit="$(git -C "$SDK" rev-parse --short HEAD 2>/dev/null || echo unknown)"
dirty="$(git -C "$SDK" status --porcelain include 2>/dev/null | head -1)"
[[ -n "$dirty" ]] && commit="$commit-dirty"
cp "$SDK"/include/daw_plugin_*.h "$DST/"
{
  echo "# Copy of esp32-daw-plugin-sdk/include (the plugin ABI headers, MIT), the only SDK files the DAW"
  echo "# uses. scripts/import-plugins.sh checks that every imported package's ABI major equals"
  echo "# DAW_PLUGIN_API_MAJOR here and its minor is not newer. Update with scripts/sync-plugin-abi.sh."
  echo "sdk_commit=$commit"
  echo "abi=$new_major.$new_minor"
} > "$DST/VERSION"
echo "plugin ABI $new_major.$new_minor from SDK $commit -> third_party/daw_plugin_abi"
git -C "$ROOT_DIR" status --short third_party/daw_plugin_abi || true
