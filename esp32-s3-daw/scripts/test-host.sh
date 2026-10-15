#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests: core unit tests, then the network end-to-end test against a daw-host instance.
# PASS / FAIL per suite; logs in build-host/test-logs/.
source "$(dirname "$0")/common.sh"
"$ROOT_DIR/scripts/build-host.sh" >/dev/null
B="${HOST_BUILD:-$ROOT_DIR/build-host}"
LOGS="$B/test-logs"; mkdir -p "$LOGS"
fail=0
if python3 "$ROOT_DIR/tests/web/test_wsclient.py" >"$LOGS/wsclient.log" 2>&1; then
  echo "PASS WebSocket test client (partial frames survive receive timeouts)"
else
  echo "FAIL WebSocket test client (log $LOGS/wsclient.log)"; fail=1
fi
for t in "$B"/tests/test_*; do
  n="$(basename "$t")"
  if "$t" >"$LOGS/$n.log" 2>&1; then echo "PASS $n ($(tail -1 "$LOGS/$n.log"))"; else echo "FAIL $n (log $LOGS/$n.log)"; tail -5 "$LOGS/$n.log"; fail=1; fi
done
# the browser UI's pure modules (MIDI file reader, arrangement): node's test runner
if command -v node >/dev/null 2>&1 && node -e 'const [a,b]=process.versions.node.split(".").map(Number); process.exit(a>22||(a===22&&b>=6)?0:1)'; then
  if (cd "$ROOT_DIR/web" && node --test --experimental-strip-types src/*/*.test.ts) >"$LOGS/web-unit.log" 2>&1; then
    echo "PASS web unit tests ($(grep -E '^# pass' "$LOGS/web-unit.log" | awk '{print $3}') tests)"
  else
    echo "FAIL web unit tests (log $LOGS/web-unit.log)"; grep -E '^not ok' "$LOGS/web-unit.log" | head; fail=1
  fi
else
  echo "SKIP web unit tests (needs node >= 22.6)"
fi
PORT="${HOST_TEST_PORT:-18190}"
if curl -s -o /dev/null "http://127.0.0.1:$PORT/api/status"; then
  echo "FAIL port $PORT is already in use (another daw-host?); set HOST_TEST_PORT" >&2; exit 1
fi
DATA="$(mktemp -d)"
"$B/bin/daw-host" --port "$PORT" --audio null --data "$DATA" --admin-password e2e-password >"$LOGS/daw-host.log" 2>&1 &
HPID=$!
for _ in $(seq 1 50); do curl -s "http://127.0.0.1:$PORT/api/status" >/dev/null 2>&1 && break; sleep 0.1; done
if python3 "$ROOT_DIR/tests/web/test_e2e.py" --connect "127.0.0.1:$PORT" --admin-password e2e-password >"$LOGS/e2e.log" 2>&1; then
  echo "PASS e2e against daw-host ($(grep -c '^PASS' "$LOGS/e2e.log") checks)"
else
  echo "FAIL e2e against daw-host (log $LOGS/e2e.log)"; grep -E '^FAIL|Error' "$LOGS/e2e.log" | head; fail=1
fi
if python3 "$ROOT_DIR/tests/web/test_midi_bridge.py" --connect "127.0.0.1:$PORT" >"$LOGS/midi-bridge.log" 2>&1; then
  echo "PASS MIDI bridge against daw-host ($(grep -c '^PASS' "$LOGS/midi-bridge.log") checks)"
else
  echo "FAIL MIDI bridge (log $LOGS/midi-bridge.log)"; grep -E '^FAIL|Error' "$LOGS/midi-bridge.log" | head; fail=1
fi
kill "$HPID" 2>/dev/null; wait "$HPID" 2>/dev/null; rm -rf "$DATA"
[[ $fail == 0 ]] && echo "host tests: PASS" || { echo "host tests: FAIL"; exit 1; }
