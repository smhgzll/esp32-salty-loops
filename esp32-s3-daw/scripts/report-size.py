#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the flash budget after a firmware build: image size vs its OTA slot, every partition."""
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    build = os.path.join(ROOT, sys.argv[1] if len(sys.argv) > 1 else "build-hw")
    image = os.path.join(build, "esp32_s3_daw.bin")
    flash_mb = 16
    table = "config/partitions.csv"
    sdkconfig = os.path.join(build, "sdkconfig")
    if os.path.isfile(sdkconfig):
        with open(sdkconfig) as f:
            text = f.read()
        match = re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE="(\d+)MB"$', text, re.MULTILINE)
        if match:
            flash_mb = int(match.group(1))
        match = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"$', text, re.MULTILINE)
        if match:
            table = match.group(1)
    flash_bytes = flash_mb * 1024 * 1024
    parts = []
    for row in csv.reader(open(os.path.join(ROOT, table))):
        if not row or row[0].strip().startswith("#"):
            continue
        name, ptype, sub, off, size = [x.strip() for x in row[:5]]
        parts.append((name, ptype, sub, int(off, 0), int(size, 0)))
    end = 0
    for name, ptype, sub, off, size in parts:
        if off < end:
            raise SystemExit(f"partition {name} overlaps the previous one")
        end = off + size
    if end > flash_bytes:
        raise SystemExit(f"partitions end at {end:#x}, beyond {flash_mb} MB")
    slot = next(p[4] for p in parts if p[0] == "ota_0")
    if os.path.isfile(image):
        n = os.path.getsize(image)
        print(f"firmware image: {n} bytes = {100.0 * n / slot:.1f} % of the {slot // 1024} KB OTA slot ({(slot - n) // 1024} KB free)")
    print(f"flash map ({flash_mb} MB, {table}):")
    for name, ptype, sub, off, size in parts:
        print(f"  {name:10s} {off:#09x} {size // 1024:6d} KB")
    print(f"  (end {end:#x}, {(flash_bytes - end) // 1024} KB unallocated)")


if __name__ == "__main__":
    main()
