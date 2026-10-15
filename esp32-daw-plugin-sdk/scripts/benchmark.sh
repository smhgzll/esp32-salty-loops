#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host benchmarks of every plugin (JSON lines in build-host/bench.jsonl). These are THIS machine's
# numbers: they rank plugins against each other but say nothing absolute about the ESP32-S3
# (measure there with the DAW's diagnostics; docs/PERFORMANCE.md).
source "$(dirname "$0")/common.sh"
"$SDK_DIR/scripts/build-host.sh" >/dev/null
B="$SDK_DIR/build-host"
: > "$B/bench.jsonl"
while read -r id _ kind _; do
  if [[ "$kind" == effect ]]; then voices="0"; else voices="1 4 8"; fi
  for v in $voices; do
    "$B/bin/dawplug" bench "$id" --voices "$v" --seconds "${BENCH_SECONDS:-5}"
    "$B/bin/dawplug" bench "$id" --voices "$v" --seconds 2 --json >> "$B/bench.jsonl"
  done
done < <("$B/bin/dawplug" list)
echo "results: $B/bench.jsonl"
