#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build and run every host test; renders reference WAVs to build-host/renders. Logs: build-host/test.log
source "$(dirname "$0")/common.sh"
"$SDK_DIR/scripts/build-host.sh" >/dev/null
B="$SDK_DIR/build-host"
LOG="$B/test.log"
: > "$LOG"
fail=0
for t in test_abi test_dsp test_plugins; do
  if "$B/bin/$t" >>"$LOG" 2>&1; then echo "PASS $t"; else echo "FAIL $t"; fail=1; fi
done
if "$B/bin/dawplug" validate all >>"$LOG" 2>&1; then echo "PASS validate all"; else echo "FAIL validate all"; fail=1; fi
mkdir -p "$B/renders"
while read -r id _; do
  short="${id##*.}"
  "$B/bin/dawplug" render "$id" --seconds 4 --out "$B/renders/$short.wav" >>"$LOG" 2>&1 || { echo "FAIL render $id"; fail=1; }
done < <("$B/bin/dawplug" list)
echo "reference renders: $B/renders/*.wav"
grep -E "FAIL|CHECK" "$LOG" | head -40 || true
[[ $fail == 0 ]] && echo "SDK host tests: PASS" || { echo "SDK host tests: FAIL (log $LOG)"; exit 1; }
