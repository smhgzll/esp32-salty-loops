#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Shared helpers for the project scripts (sourced).
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ROOT_DIR

# ESP-IDF v5.5.x: $IDF_PATH, else .idf_path (scripts/bootstrap-nobara.sh), else ~/.espressif/idf-env.json
find_idf() {
  if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then echo "$IDF_PATH"; return 0; fi
  if [[ -f "$ROOT_DIR/.idf_path" ]]; then
    local p; p="$(cat "$ROOT_DIR/.idf_path")"; [[ -f "$p/export.sh" ]] && { echo "$p"; return 0; }
  fi
  if [[ -f "$HOME/.espressif/idf-env.json" ]]; then
    python3 - "$HOME/.espressif/idf-env.json" <<'PY' && return 0
import json, os, sys
for v in json.load(open(sys.argv[1])).get("idfInstalled", {}).values():
    if os.path.isfile(os.path.join(v.get("path", ""), "export.sh")):
        print(v["path"]); sys.exit(0)
sys.exit(1)
PY
  fi
  for c in "$HOME/esp/esp-idf" "$HOME/esp/v5.5.5/esp-idf"; do [[ -f "$c/export.sh" ]] && { echo "$c"; return 0; }; done
  return 1
}

source_idf() {
  local p
  p="$(find_idf)" || { echo "ESP-IDF v5.5.5 not found: set IDF_PATH or run scripts/bootstrap-nobara.sh" >&2; exit 1; }
  export IDF_PATH="$p"
  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh" >/dev/null 2>&1
}

load_local_env() {
  if [[ -f "$ROOT_DIR/config/local.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "$ROOT_DIR/config/local.env"
    set +a
  fi
}

host_compilers() {
  local c x
  for c in "${DAW_HOST_CC:-}" "${CC:-}" gcc clang cc; do [[ -n "$c" ]] && command -v "$c" >/dev/null 2>&1 && break; c=""; done
  for x in "${DAW_HOST_CXX:-}" "${CXX:-}" g++ clang++ c++; do [[ -n "$x" ]] && command -v "$x" >/dev/null 2>&1 && break; x=""; done
  [[ -n "$c" && -n "$x" ]] || { echo "no host C/C++ compiler (sudo dnf install gcc gcc-c++)" >&2; return 1; }
  echo "$c $x"
}

# ESP-IDF only applies sdkconfig.defaults when it creates an sdkconfig: regenerate the build's
# sdkconfig whenever the tracked defaults (or the local overlay) change.
refresh_sdkconfig() {
  local build_dir="$1"; shift
  local sum
  sum="$(cat "$@" "$ROOT_DIR/main/Kconfig.projbuild" 2>/dev/null | sha256sum | cut -d' ' -f1)"
  if [[ -f "$build_dir/sdkconfig" && "$(cat "$build_dir/.defaults.sha256" 2>/dev/null)" != "$sum" ]]; then
    echo "note: sdkconfig defaults changed: regenerating $build_dir/sdkconfig"
    rm -f "$build_dir/sdkconfig"
  fi
  mkdir -p "$build_dir"
  echo "$sum" > "$build_dir/.defaults.sha256"
}

# before any firmware build: plugins imported, web assets embedded
prepare_firmware_inputs() {
  [[ -f "$ROOT_DIR/plugins/generated/plugins.cmake" && -d "$ROOT_DIR/plugins/imported" ]] || python3 "$ROOT_DIR/scripts/import-plugins.py"
  # the browser UI: rebuilt when a source is newer than the bundle (needs node; otherwise the
  # existing web/dist, or embed-web.py's fallback page, is embedded)
  if command -v npx >/dev/null 2>&1 && { [[ ! -f "$ROOT_DIR/web/dist/index.html" ]] ||
      [[ -n "$(find "$ROOT_DIR/web/src" "$ROOT_DIR/web/index.html" "$ROOT_DIR/web/package.json" -newer "$ROOT_DIR/web/dist/index.html" -print -quit)" ]]; }; then
    "$ROOT_DIR/scripts/build-web.sh" >/dev/null
  fi
  python3 "$ROOT_DIR/scripts/embed-web.py"
}
