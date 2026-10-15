#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Browser tests: builds the web UI, serves it from a fresh daw-host and runs the Playwright suites
# (layout screenshots at five viewports, then real pointer / touch editing checked on the device).
#   scripts/test-browser.sh            against a local daw-host (started here)
#   scripts/test-browser.sh --url URL  against a running device / QEMU (e.g. http://127.0.0.1:18080/)
# Needs: node + npm (web/), python3 with playwright and its chromium (pip install playwright;
# python3 -m playwright install chromium).
source "$(dirname "$0")/common.sh"
URL=""
[[ "${1:-}" == "--url" ]] && URL="${2:?--url needs a value}"
OUT="$ROOT_DIR/tests/browser/out"; mkdir -p "$OUT"
HPID=""
if [[ -z "$URL" ]]; then
  "$ROOT_DIR/scripts/build-web.sh" >/dev/null
  "$ROOT_DIR/scripts/build-host.sh" >/dev/null
  PORT="${BROWSER_TEST_PORT:-18191}"
  if curl -s -o /dev/null "http://127.0.0.1:$PORT/api/status"; then
    echo "FAIL port $PORT is already in use; set BROWSER_TEST_PORT" >&2; exit 1
  fi
  DATA="$(mktemp -d)"
  "$ROOT_DIR/build-host/bin/daw-host" --port "$PORT" --audio null --web "$ROOT_DIR/web/dist" --data "$DATA" \
    --admin-password browser-test --test-hooks >"$OUT/daw-host.log" 2>&1 &
  HPID=$!
  trap 'kill $HPID 2>/dev/null; wait $HPID 2>/dev/null; rm -rf "$DATA"' EXIT
  for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/api/status" && break; sleep 0.1; done
  URL="http://127.0.0.1:$PORT/"
fi
fail=0
python3 "$ROOT_DIR/tests/browser/screens.py" --url "$URL" --out "$OUT" || fail=1
python3 "$ROOT_DIR/tests/browser/test_ui.py" --url "$URL" --out "$OUT" || fail=1
echo "screenshots: $OUT"
exit $fail
