#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Shared helpers for the SDK scripts (sourced).
set -euo pipefail
SDK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export SDK_DIR

# a host C/C++ compiler that exists ($CC may name one that is not installed)
host_compilers() {
  local c x
  for c in "${DAWSDK_HOST_CC:-}" "${CC:-}" gcc clang cc; do
    [[ -n "$c" ]] && command -v "$c" >/dev/null 2>&1 && break
    c=""
  done
  for x in "${DAWSDK_HOST_CXX:-}" "${CXX:-}" g++ clang++ c++; do
    [[ -n "$x" ]] && command -v "$x" >/dev/null 2>&1 && break
    x=""
  done
  [[ -n "$c" && -n "$x" ]] || { echo "no host C/C++ compiler (sudo dnf install gcc gcc-c++)" >&2; return 1; }
  echo "$c $x"
}

# ESP-IDF location: $IDF_PATH, else .idf_path written by scripts/bootstrap.sh
find_idf() {
  if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then echo "$IDF_PATH"; return 0; fi
  if [[ -f "$SDK_DIR/.idf_path" ]]; then
    local p; p="$(cat "$SDK_DIR/.idf_path")"
    [[ -f "$p/export.sh" ]] && { echo "$p"; return 0; }
  fi
  return 1
}

source_idf() {
  local p
  p="$(find_idf)" || { echo "ESP-IDF not found: set IDF_PATH or run scripts/bootstrap.sh" >&2; exit 1; }
  export IDF_PATH="$p"
  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh" >/dev/null
}
