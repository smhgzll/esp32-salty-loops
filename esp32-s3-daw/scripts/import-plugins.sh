#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Import plugin packages (default: plugins/packages/*.espplug), or build them from a sibling SDK
# checkout first: scripts/import-plugins.sh --from-sdk [../esp32-daw-plugin-sdk]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ "${1:-}" == --from-sdk ]]; then
  SDK="${2:-$ROOT/../esp32-daw-plugin-sdk}"
  "$SDK/scripts/package.sh" example_tremolo $(cd "$SDK" && python3 -c "import json,glob;print(' '.join(json.load(open(p))['name'] for p in sorted(glob.glob('plugins/*/plugin.json')+glob.glob('effects/*/plugin.json'))))")
  rm -f "$ROOT"/plugins/packages/*.espplug
  set -- "$SDK"/dist/*.espplug
fi
python3 "$ROOT/scripts/import-plugins.py" "$@"
