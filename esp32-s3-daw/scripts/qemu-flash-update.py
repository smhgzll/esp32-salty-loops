#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a new build (bootloader, partition table, application) into an existing 16 MB QEMU flash
image without touching the data partitions (NVS, otadata, projects): a firmware update by cable
that keeps the device's state. Used by scripts/run-qemu.sh with KEEP_DATA=1."""
import os
import sys


def main():
    build, image = sys.argv[1], sys.argv[2]
    args = open(os.path.join(build, "flash_args")).read().split()
    pairs = [(int(args[i], 16), args[i + 1]) for i in range(len(args)) if args[i].startswith("0x") and i + 1 < len(args)]
    flash = bytearray(open(image, "rb").read())
    for off, path in pairs:
        if "ota_data" in path:
            continue  # keep the OTA selection
        data = open(os.path.join(build, path), "rb").read()
        flash[off:off + len(data)] = data
        print(f"  {off:#08x} {path} ({len(data)} bytes)")
    open(image, "wb").write(flash)


if __name__ == "__main__":
    main()
