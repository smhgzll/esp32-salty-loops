#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Every automated test this computer can run, with one summary at the end:
#   host       core unit tests, network e2e and MIDI bridge against daw-host
#   browser    layout screenshots + pointer/touch editing (Playwright), skipped without Playwright
#   qemu       the real firmware in QEMU: self-test, render comparison, network e2e
#   hardware   firmware build for the board (build only; the board tests are manual:
#              docs/HARDWARE_TESTS.md, UNVERIFIED unless run on a board)
#   scripts/test-all.sh [--skip qemu,browser,...]
source "$(dirname "$0")/common.sh"
set +e
SKIP=",${2:-},"; [[ "${1:-}" == --skip ]] || SKIP=",,"
declare -A R
run() {
  local name="$1"; shift
  if [[ "$SKIP" == *",$name,"* ]]; then R[$name]="skipped (--skip)"; return; fi
  echo "==== $name"
  if "$@"; then R[$name]=PASS; else R[$name]=FAIL; fi
}
run host "$ROOT_DIR/scripts/test-host.sh"
if python3 -c "import playwright" 2>/dev/null; then
  run browser "$ROOT_DIR/scripts/test-browser.sh"
else
  R[browser]="skipped (no Playwright: scripts/doctor.sh)"
fi
run qemu "$ROOT_DIR/scripts/test-qemu.sh"
run hardware-build "$ROOT_DIR/scripts/build-hardware.sh"
echo "==== summary"
fail=0
for k in host browser qemu hardware-build; do
  printf '  %-15s %s\n' "$k" "${R[$k]}"
  [[ "${R[$k]}" == FAIL ]] && fail=1
done
echo "  hardware tests  UNVERIFIED here (docs/HARDWARE_TESTS.md needs the board)"
exit $fail
