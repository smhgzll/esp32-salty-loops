#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# QEMU tests of the real firmware (docs/QEMU.md):
#   1. fresh 16 MB flash, multi-threaded TCG: boot, offline render through the plugins (compared with
#      the Linux build's numbers), live audio task, autosave + restart + restore, heap integrity
#   2. the same flash, single-threaded TCG (flash writes, KNOWN_ISSUES KI-2): the network end-to-end
#      test (tests/web/test_e2e.py) over the emulated Ethernet
# QEMU does not emulate I2S, Wi-Fi or real timing: those are hardware tests (docs/HARDWARE_TESTS.md).
#   3. the same flash: browser firmware update (tests/web/test_ota.py): refusals, an update that
#      confirms itself, and a forced unhealthy start that the bootloader rolls back
#   QEMU_TIMEOUT=s (default 400), QEMU_WEB_PORT (default 18080), QEMU_SKIP_OTA=1
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT_DIR/build-qemu"
LOG="$B/qemu-selftest.log"; LOG2="$B/qemu-e2e.log"
TIMEOUT="${QEMU_TIMEOUT:-400}"
export QEMU_WEB_PORT="${QEMU_WEB_PORT:-18080}"
"$ROOT_DIR/scripts/build-qemu.sh" >"$B.build.log" 2>&1 || { echo "QEMU test: FAIL (firmware build, $B.build.log)"; exit 1; }
"$ROOT_DIR/scripts/build-host.sh" >/dev/null 2>&1 || { echo "QEMU test: FAIL (host build)"; exit 1; }
HOST_RENDER="$("$ROOT_DIR/build-host/tests/test_render" 2>/dev/null | grep '^DAW_RENDER')"

# Phase 1 writes flash and restarts: single-threaded TCG by default (KNOWN_ISSUES KI-2: this QEMU's
# multi-threaded TCG can crash the emulator itself around flash writes). QEMU_SELFTEST_TCG=multi
# exercises real parallelism between the two cores instead.
run_selftest() {
  result=""
  rm -f "$LOG"
  QEMU_TCG_THREAD="${QEMU_SELFTEST_TCG:-single}" QEMU_LOG="$LOG" "$ROOT_DIR/scripts/run-qemu.sh" &
  QPID=$!
  for ((t = 0; t < TIMEOUT; t++)); do
    sleep 1
    grep -q "DAW_SELFTEST_PASS" "$LOG" 2>/dev/null && { result=PASS; break; }
    grep -qE "DAW_SELFTEST_FAIL|Guru Meditation|abort\(\) was called|Backtrace:" "$LOG" 2>/dev/null && { sleep 2; result=FAIL; break; }
    if ! kill -0 "$QPID" 2>/dev/null; then
      wait "$QPID"; rc=$?
      # killed by a signal (segfault...): the emulator crashed, not the firmware
      if (( rc > 128 )); then result=EMULATOR_CRASH; else result=FAIL; fi
      break
    fi
  done
  kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
  [[ -z "$result" ]] && result=TIMEOUT
}
run_selftest
if [[ "$result" == EMULATOR_CRASH ]]; then
  echo "note: qemu-system-xtensa itself crashed (KNOWN_ISSUES KI-2); retrying once on a fresh flash"
  run_selftest
fi
echo "---- self-test (log $LOG)"
sed 's/\x1b\[[0-9;]*m//g' "$LOG" | grep -E "selftest|DAW_|daw: |audio: |net: IP" | head -30

# cross-target render: the Xtensa build (QEMU) against the Linux build, within tolerances
QEMU_RENDER="$(grep '^DAW_RENDER' "$LOG" | head -1)"
python3 - "$HOST_RENDER" "$QEMU_RENDER" <<'PY' || result=FAIL
import sys
def parse(s):
    return {k: float(v) for k, v in (x.split("=") for x in s.split()[1:])} if s else {}
h, q = parse(sys.argv[1]), parse(sys.argv[2])
ok = bool(h) and bool(q) and all(abs(h[k] - q[k]) <= max(1e-6, 0.005 * abs(h[k])) for k in ("rmsL", "rmsR", "peak")) \
     and abs(h["nonzero"] - q["nonzero"]) <= 0.001 * h["frames"]
print(f"cross-target render: host {sys.argv[1][11:]}\n                     qemu {sys.argv[2][11:]}\n                     {'MATCH' if ok else 'MISMATCH'}")
sys.exit(0 if ok else 1)
PY

e2e=""
if [[ "$result" == PASS ]]; then
  rm -f "$LOG2"
  echo "---- network end-to-end against the firmware (single-threaded TCG, 127.0.0.1:$QEMU_WEB_PORT)"
  KEEP_FLASH=1 QEMU_TCG_THREAD=single QEMU_LOG="$LOG2" "$ROOT_DIR/scripts/run-qemu.sh" &
  QPID=$!
  for ((t = 0; t < 180; t++)); do
    sleep 1
    grep -q "DAW_QEMU_IDLE" "$LOG2" 2>/dev/null && grep -q "net: IP" "$LOG2" 2>/dev/null && break
  done
  if python3 "$ROOT_DIR/tests/web/test_e2e.py" --connect "127.0.0.1:$QEMU_WEB_PORT" --admin-password qemu-test-password \
       ${QEMU_E2E_ARGS:-}; then e2e=PASS; else e2e=FAIL; result=FAIL; fi
  kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
fi
ota=""
LOG3="$B/qemu-ota.log"
if [[ "$result" == PASS && "${QEMU_SKIP_OTA:-0}" != 1 ]]; then
  rm -f "$LOG3"
  echo "---- browser firmware update: refusals, update + self-confirmation, forced unhealthy start + rollback"
  KEEP_FLASH=1 QEMU_TCG_THREAD=single QEMU_LOG="$LOG3" "$ROOT_DIR/scripts/run-qemu.sh" &
  QPID=$!
  for ((t = 0; t < 180; t++)); do
    sleep 1
    grep -q "DAW_QEMU_IDLE" "$LOG3" 2>/dev/null && grep -q "net: IP" "$LOG3" 2>/dev/null && break
  done
  if python3 "$ROOT_DIR/tests/web/test_ota.py" --connect "127.0.0.1:$QEMU_WEB_PORT" --image "$B/esp32_s3_daw.bin" \
       --hw-image "$ROOT_DIR/build-hw/esp32_s3_daw.bin" --admin-password qemu-test-password --log "$LOG3"; then ota=PASS; else ota=FAIL; result=FAIL; fi
  kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
  sed 's/\x1b\[[0-9;]*m//g' "$LOG3" | grep -E "ota: |update|confirm|rolling back|rollback" | head -20
fi
for l in "$LOG" "${LOG2}" "$LOG3"; do
  if [[ -f "$l" ]] && grep -qE "DAW_SELFTEST_FAIL|Guru Meditation|abort\(\) was called|Backtrace:" "$l"; then
    echo "panic or self-test failure in $l"; result=FAIL
  fi
done
[[ -n "$e2e" ]] && echo "network e2e over QEMU Ethernet: $e2e"
[[ -n "$ota" ]] && echo "firmware update + rollback: $ota"
echo "QEMU test: $result"
[[ "$result" == PASS ]]
