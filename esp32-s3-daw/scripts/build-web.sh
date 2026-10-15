#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the browser UI (web/dist): type check, then the Vite production bundle.
# The firmware embeds web/dist (scripts/embed-web.py runs from the IDF build); daw-host serves it with --web.
source "$(dirname "$0")/common.sh"
cd "$ROOT_DIR/web"
[[ -d node_modules ]] || npm ci --no-audit --no-fund
npx tsc --noEmit
npx vite build
python3 "$ROOT_DIR/scripts/gen-notices.py" --target host --out "$ROOT_DIR/web/dist/licenses.txt"
