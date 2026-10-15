#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Check the SDK's build dependencies and locate ESP-IDF v5.5.x (records it in .idf_path).
# Installs nothing system-wide; prints the commands to run instead.
source "$(dirname "$0")/common.sh"
ok=1
for t in cmake ninja python3 tar; do
  if command -v "$t" >/dev/null; then echo "ok    $t: $(command -v "$t")"; else echo "MISSING $t"; ok=0; fi
done
if cc="$(host_compilers)"; then echo "ok    host compilers: $cc"; else ok=0; fi
idf=""
if p="$(find_idf)"; then idf="$p"; fi
if [[ -z "$idf" && -f "$HOME/.espressif/idf-env.json" ]]; then
  idf="$(python3 - "$HOME/.espressif/idf-env.json" <<'PY'
import json, sys, os
d = json.load(open(sys.argv[1]))
for k, v in d.get("idfInstalled", {}).items():
    p = v.get("path", "")
    if os.path.isfile(os.path.join(p, "export.sh")):
        print(p); break
PY
)"
fi
for cand in "$HOME/esp/esp-idf" "$HOME/esp/v5.5.5/esp-idf"; do
  [[ -z "$idf" && -f "$cand/export.sh" ]] && idf="$cand"
done
if [[ -n "$idf" ]]; then
  ver="$(cat "$idf/version.txt" 2>/dev/null || git -C "$idf" describe --tags 2>/dev/null || echo unknown)"
  echo "ok    ESP-IDF $ver at $idf"
  echo "$idf" > "$SDK_DIR/.idf_path"
  [[ "$ver" == v5.5* ]] || echo "warn  ESP-IDF v5.5.5 is the tested version"
else
  echo "MISSING ESP-IDF (needed for scripts/build-esp32.sh only):"
  echo "  git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf && ~/esp/esp-idf/install.sh esp32s3"
fi
[[ $ok == 1 ]] && echo "bootstrap: OK" || { echo "bootstrap: missing tools (sudo dnf install cmake ninja-build gcc gcc-c++ python3)"; exit 1; }
