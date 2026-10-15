#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the bundled plugins from the sibling SDK and import them into the DAW.
# The plain-C SDK example is intentionally an optional, separate import.
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_SDK_DIR="$REPO_DIR/esp32-daw-plugin-sdk"
PLUGIN_DAW_DIR="$REPO_DIR/esp32-s3-daw"

plugin_name_list="$(python3 - "$PLUGIN_SDK_DIR" <<'PY'
import json
import sys
from pathlib import Path

sdk = Path(sys.argv[1])
for manifest in sorted(sdk.glob('plugins/*/plugin.json')) + sorted(sdk.glob('effects/*/plugin.json')):
    print(json.loads(manifest.read_text())['name'])
PY
)"
if [[ -z "$plugin_name_list" ]]; then
  echo "No bundled plugin manifests found in esp32-daw-plugin-sdk." >&2
  exit 1
fi
mapfile -t plugin_names <<< "$plugin_name_list"
"$PLUGIN_SDK_DIR/scripts/package.sh" "${plugin_names[@]}"

plugin_package_list="$(python3 - "$PLUGIN_SDK_DIR" <<'PY'
import json
import subprocess
import sys
from pathlib import Path

sdk = Path(sys.argv[1])
for manifest in sorted(sdk.glob('plugins/*/plugin.json')) + sorted(sdk.glob('effects/*/plugin.json')):
    meta = json.loads(manifest.read_text())
    descriptor = json.loads(subprocess.check_output([sdk / 'build-host/bin/dawplug', 'info', meta['id']]))
    print(sdk / 'dist' / f"{meta['id']}-{descriptor['version']}.espplug")
PY
)"
mapfile -t plugin_packages <<< "$plugin_package_list"
"$PLUGIN_DAW_DIR/scripts/import-plugins.sh" "${plugin_packages[@]}"
