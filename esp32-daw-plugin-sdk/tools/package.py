#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack plugins as .espplug archives (docs/PLUGIN_FORMAT.md).

An .espplug is a gzip'd tar with:
  manifest.json                       descriptor-derived metadata + packaging metadata + checksums
  lib/<arch>/libdaw_plugin_<name>.a   one static library per target architecture
  licenses/...                        license texts of the plugin and its third-party code
  PROVENANCE.md                       upstream sources, commits, modifications (optional)

The manifest's parameter / capability data is generated from the compiled descriptor
(`dawplug info`), never written by hand, so it cannot drift from the code.
"""
import argparse
import hashlib
import io
import json
import os
import platform
import re
import subprocess
import sys
import tarfile
import time

FORMAT_VERSION = 1
SDK_VERSION = "0.1.0"

# A plugin library may only reference these external symbols: math, memory copies, compiler
# runtime helpers. Anything else (malloc, operator new, printf, threads, RTOS, exceptions,
# thread-safe static guards) breaks the real-time contract or the host-owned memory model.
ALLOWED = re.compile(
    r"^(memcpy|memmove|memset|memcmp|strlen|strcmp|"
    r"(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|exp2|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|"
    r"fabs|floor|ceil|round|lround|lrint|rint|trunc|fmod|remainder|fmin|fmax|hypot|copysign|ldexp|frexp|modf|"
    r"nearbyint|sincos)f?|"
    r"__[a-z0-9_]+|_GLOBAL_OFFSET_TABLE_|"
    r"_ZSt21ios_base_Init_dummy)$"  # never expected; kept explicit so a hit is visible
)
FORBIDDEN_HINT = re.compile(r"malloc|calloc|realloc|free|_Znw|_Zna|_Zdl|_Zda|printf|puts|fopen|pthread|"
                            r"__cxa_throw|__cxa_guard|heap_caps|vTask|xQueue|xSemaphore|esp_")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def undefined_symbols(nm, lib):
    out = subprocess.run([nm, "-u", "-g", lib], check=True, capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in ("U", "w"):
            syms.add(parts[-1])
    return syms


def defined_globals(nm, lib):
    out = subprocess.run([nm, "-g", "--defined-only", lib], check=True, capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "D", "R", "B"):
            syms.add(parts[2])
    return syms


def tool_version(cmd):
    try:
        return subprocess.run(cmd + ["--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception:
        return "unknown"


def find_plugin_dirs(sdk):
    dirs = {}
    for top in ("plugins", "effects", "examples"):
        base = os.path.join(sdk, top)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            pj = os.path.join(base, name, "plugin.json")
            if os.path.isfile(pj):
                meta = json.load(open(pj))
                dirs[meta["name"]] = (os.path.join(base, name), meta)
    return dirs


def pack(sdk, name, pdir, meta, host_build, esp_build, out_dir, nm_host, nm_esp):
    dawplug = os.path.join(host_build, "bin", "dawplug")
    desc = json.loads(subprocess.run([dawplug, "info", meta["id"]], check=True, capture_output=True, text=True).stdout)
    if desc["id"] != meta["id"]:
        raise SystemExit(f"{name}: plugin.json id {meta['id']} != descriptor id {desc['id']}")
    libs = {
        "host-x86_64-linux": (os.path.join(host_build, "lib", f"libdaw_plugin_{name}.a"), nm_host),
        "esp32s3": (os.path.join(esp_build, "lib", f"libdaw_plugin_{name}.a"), nm_esp),
    }
    files = {}
    targets = []
    for arch, (lib, nm) in libs.items():
        if not os.path.isfile(lib):
            raise SystemExit(f"{name}: missing {arch} library {lib}")
        bad = sorted(s for s in undefined_symbols(nm, lib) if not ALLOWED.match(s))
        if bad:
            raise SystemExit(f"{name} ({arch}): forbidden external symbols {bad}"
                             " (plugins may not allocate, print, use threads or exceptions; see docs/REALTIME_RULES.md)")
        exported = defined_globals(nm, lib)
        if meta["entry"] not in exported:
            raise SystemExit(f"{name} ({arch}): entry {meta['entry']} not exported")
        stray = sorted(s for s in exported if s != meta["entry"])
        if stray:
            # any other global could collide with another plugin in the same firmware; the SDK's
            # build (cmake/DawPlugin.cmake) localizes everything but the entry point
            raise SystemExit(f"{name} ({arch}): exports symbols besides {meta['entry']}: {stray[:8]}")
        files[f"lib/{arch}/libdaw_plugin_{name}.a"] = lib
        targets.append(arch)
    lic_files = []
    # the plugin's own license text: its directory's LICENSE, else the SDK's when the plugin uses
    # the SDK's license (a binary distribution must carry the notice: firmware notices page)
    own = os.path.join(pdir, "LICENSE")
    sdk_license = os.path.join(sdk, "LICENSE")
    if not os.path.isfile(own) and desc.get("license") == "MIT" and os.path.isfile(sdk_license):
        own = sdk_license
    if os.path.isfile(own):
        files["licenses/LICENSE"] = own
        lic_files.append("licenses/LICENSE")
    elif desc.get("vendor") == "smhgzll":
        raise SystemExit(f"{name}: no LICENSE for license {desc.get('license')}")
    for lf in meta.get("licenseFiles", []):
        src = os.path.join(pdir, lf)
        dst = f"licenses/{os.path.basename(lf)}"
        files[dst] = src
        lic_files.append(dst)
    prov = os.path.join(pdir, "PROVENANCE.md")
    if os.path.isfile(prov):
        files["PROVENANCE.md"] = prov

    manifest = dict(desc)
    manifest.pop("arch", None)
    manifest.update({
        "format": FORMAT_VERSION,
        "libName": f"daw_plugin_{name}",
        "entry": meta["entry"],
        "targets": targets,
        "requires": {"abiMajor": desc["apiVersion"], "abiMinor": desc["apiMinor"], "sdk": SDK_VERSION},
        "licenseFiles": lic_files,
        "provenance": meta.get("provenance", []),
        "dependencies": meta.get("dependencies", []),
        "ui": meta.get("ui", {"editor": "generic"}),
        "build": {
            "sdkVersion": SDK_VERSION,
            "hostCompiler": tool_version([os.environ.get("CXX", "g++")]),
            "espCompiler": tool_version(["xtensa-esp32s3-elf-g++"]),
            "builtOn": platform.platform(),
        },
        "files": {k: {"sha256": sha256(v), "size": os.path.getsize(v)} for k, v in sorted(files.items())},
    })
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"{desc['id']}-{desc['version']}.espplug")
    # reproducible archive: fixed mtime / owner, sorted entries
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH", "1700000000"))
    with tarfile.open(out, "w:gz", format=tarfile.PAX_FORMAT) as tar:
        def add_bytes(arcname, data):
            ti = tarfile.TarInfo(arcname)
            ti.size = len(data)
            ti.mtime = mtime
            ti.mode = 0o644
            tar.addfile(ti, io.BytesIO(data))
        add_bytes("manifest.json", (json.dumps(manifest, indent=2, sort_keys=False) + "\n").encode())
        for arc, src in sorted(files.items()):
            add_bytes(arc, open(src, "rb").read())
    print(f"packed {out} ({os.path.getsize(out)} bytes, targets {', '.join(targets)})")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--host-build", required=True)
    ap.add_argument("--esp-build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    dirs = find_plugin_dirs(a.sdk)
    names = a.names or list(dirs)
    for n in names:
        if n not in dirs:
            raise SystemExit(f"unknown plugin {n}; known: {', '.join(dirs)}")
        pack(a.sdk, n, dirs[n][0], dirs[n][1], a.host_build, a.esp_build, a.out, "nm", "xtensa-esp32s3-elf-nm")


if __name__ == "__main__":
    main()
