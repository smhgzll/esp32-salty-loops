#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Import .espplug packages (esp32-daw-plugin-sdk format 1) into this DAW build.

  scripts/import-plugins.py [packages...]     (default: plugins/packages/*.espplug)

For every package: verify the manifest, ABI compatibility with third_party/daw_plugin_abi, file
checksums, the two target libraries, that the entry point is the only global symbol and that the
libraries reference no forbidden symbols (allocation, stdio, threads, RTOS). Then extract to
plugins/imported/<id>/ and generate:

  plugins/generated/daw_plugin_registry.c   the entry-point table the firmware / host link
  plugins/generated/plugins.cmake           DAW_PLUGIN_LIBS_ESP32S3 / DAW_PLUGIN_LIBS_HOST
  plugins/generated/catalog.json            id, version, type, license, checksum per plugin
  plugins/plugins.lock.json                 what is imported (commit this with the packages)

The packages are native code linked into the firmware: import only packages you trust.
"""
import argparse
import glob
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABI_HEADER = os.path.join(ROOT, "third_party", "daw_plugin_abi", "daw_plugin_api.h")
ARCHS = ("esp32s3", "host-x86_64-linux")
ALLOWED = re.compile(
    r"^(memcpy|memmove|memset|memcmp|strlen|strcmp|"
    r"(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|exp2|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|"
    r"fabs|floor|ceil|round|lround|lrint|rint|trunc|fmod|remainder|fmin|fmax|hypot|copysign|ldexp|frexp|modf|"
    r"nearbyint|sincos)f?|__[a-z0-9_]+|_GLOBAL_OFFSET_TABLE_)$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{2,95}$")


def abi_version():
    text = open(ABI_HEADER).read()
    major = int(re.search(r"#define DAW_PLUGIN_API_MAJOR (\d+)", text).group(1))
    minor = int(re.search(r"#define DAW_PLUGIN_API_MINOR (\d+)", text).group(1))
    return major, minor


def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    for cand in glob.glob(os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/" + name)):
        return cand
    return None


def symbols(nm, lib, defined):
    args = [nm, "-g", "--defined-only", lib] if defined else [nm, "-u", "-g", lib]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    res = set()
    for line in out.splitlines():
        parts = line.split()
        if defined and len(parts) == 3 and parts[1] in "TDRBV":
            res.add(parts[2])
        elif not defined and len(parts) >= 2 and parts[-2] in ("U", "w"):
            res.add(parts[-1])
    return res


def fail(pkg, msg):
    raise SystemExit(f"import: {os.path.basename(pkg)}: {msg}")


def read_package(path):
    with tarfile.open(path, "r:gz") as tar:
        members = {m.name: m for m in tar.getmembers()}
        for name, m in members.items():
            if not m.isfile() or name.startswith("/") or ".." in name.split("/"):
                fail(path, f"unsafe archive member {name}")
        if "manifest.json" not in members:
            fail(path, "no manifest.json")
        files = {name: tar.extractfile(m).read() for name, m in members.items()}
    return json.loads(files["manifest.json"]), files


def check(path, manifest, files, abi, nm_tools, strict_esp):
    for key in ("id", "name", "version", "type", "entry", "libName", "apiVersion", "files", "license"):
        if key not in manifest:
            fail(path, f"manifest field '{key}' missing")
    if manifest.get("format") != 1:
        fail(path, f"unsupported package format {manifest.get('format')}")
    if not ID_RE.match(manifest["id"]):
        fail(path, f"invalid id {manifest['id']}")
    if manifest["type"] not in ("instrument", "effect"):
        fail(path, "type must be instrument or effect")
    req = manifest.get("requires", {})
    if req.get("abiMajor", manifest["apiVersion"]) != abi[0] or req.get("abiMinor", 0) > abi[1]:
        fail(path, f"built for ABI {req.get('abiMajor')}.{req.get('abiMinor')}, this DAW has {abi[0]}.{abi[1]}")
    listed = manifest["files"]
    for name, data in files.items():
        if name == "manifest.json":
            continue
        if name not in listed:
            fail(path, f"{name} not listed in the manifest")
        if hashlib.sha256(data).hexdigest() != listed[name]["sha256"]:
            fail(path, f"checksum mismatch for {name}")
    for name in listed:
        if name not in files:
            fail(path, f"{name} listed but missing")
    for arch in ARCHS:
        lib = f"lib/{arch}/lib{manifest['libName']}.a"
        if lib not in files:
            fail(path, f"no {arch} library")
        nm = nm_tools.get(arch)
        if not nm:
            if arch == "esp32s3" and strict_esp:
                fail(path, "xtensa-esp32s3-elf-nm not found (source ESP-IDF's export.sh) or pass --no-esp-check")
            print(f"warning: {manifest['id']}: {arch} symbols not checked (no nm)", file=sys.stderr)
            continue
        tmp = os.path.join(ROOT, "build-import.tmp.a")
        open(tmp, "wb").write(files[lib])
        try:
            exported = symbols(nm, tmp, True)
            undefined = symbols(nm, tmp, False)
        finally:
            os.remove(tmp)
        if exported != {manifest["entry"]}:
            fail(path, f"{arch}: library must export exactly {manifest['entry']}, exports {sorted(exported)[:6]}")
        bad = sorted(s for s in undefined if not ALLOWED.match(s))
        if bad:
            fail(path, f"{arch}: forbidden external symbols {bad}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("packages", nargs="*")
    ap.add_argument("--no-esp-check", action="store_true", help="skip the xtensa symbol checks (no ESP-IDF here)")
    a = ap.parse_args()
    pkgs = a.packages or sorted(glob.glob(os.path.join(ROOT, "plugins", "packages", "*.espplug")))
    if not pkgs:
        raise SystemExit("import: no packages (build them in esp32-daw-plugin-sdk: scripts/package.sh)")
    abi = abi_version()
    nm_tools = {"host-x86_64-linux": find_tool("nm"), "esp32s3": find_tool("xtensa-esp32s3-elf-nm")}
    imported_dir = os.path.join(ROOT, "plugins", "imported")
    gen_dir = os.path.join(ROOT, "plugins", "generated")
    pkg_dir = os.path.join(ROOT, "plugins", "packages")
    os.makedirs(gen_dir, exist_ok=True)
    os.makedirs(pkg_dir, exist_ok=True)
    seen = {}
    for path in pkgs:
        manifest, files = read_package(path)
        check(path, manifest, files, abi, nm_tools, not a.no_esp_check)
        pid = manifest["id"]
        if pid in seen:
            fail(path, f"duplicate plugin id {pid} (also in {seen[pid]['package']})")
        if any(m["entry"] == manifest["entry"] for m in seen.values()):
            fail(path, f"entry symbol {manifest['entry']} already used by another package")
        dest = os.path.join(imported_dir, pid)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        for name, data in files.items():
            out = os.path.join(dest, name)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, "wb").write(data)
        pkg_copy = os.path.join(pkg_dir, os.path.basename(path))
        if os.path.abspath(path) != os.path.abspath(pkg_copy):
            shutil.copyfile(path, pkg_copy)
        seen[pid] = {"package": os.path.basename(path), "version": manifest["version"], "entry": manifest["entry"],
                     "libName": manifest["libName"], "type": manifest["type"], "license": manifest["license"],
                     "sha256": hashlib.sha256(open(path, "rb").read()).hexdigest(), "name": manifest["name"]}
        print(f"imported {pid} {manifest['version']} ({manifest['type']}, {manifest['license']})")

    # remove imports that are no longer selected
    for d in os.listdir(imported_dir) if os.path.isdir(imported_dir) else []:
        if d not in seen and os.path.isdir(os.path.join(imported_dir, d)):
            shutil.rmtree(os.path.join(imported_dir, d))

    ids = sorted(seen)
    with open(os.path.join(gen_dir, "daw_plugin_registry.c"), "w") as f:
        f.write("/* generated by scripts/import-plugins.py: do not edit */\n#include <stddef.h>\n\n#include \"daw_plugin_api.h\"\n\n")
        for pid in ids:
            f.write(f"extern const daw_plugin_descriptor_t *{seen[pid]['entry']}(void); /* {pid} {seen[pid]['version']} */\n")
        f.write("\nconst daw_plugin_entry_fn daw_plugin_entries[] = {\n")
        for pid in ids:
            f.write(f"    {seen[pid]['entry']},\n")
        if not ids:
            f.write("    0,\n")
        f.write("};\n")
        f.write(f"const size_t daw_plugin_entry_count = {len(ids)};\n")
    with open(os.path.join(gen_dir, "plugins.cmake"), "w") as f:
        f.write("# generated by scripts/import-plugins.py: do not edit\n")
        for arch, var in (("esp32s3", "DAW_PLUGIN_LIBS_ESP32S3"), ("host-x86_64-linux", "DAW_PLUGIN_LIBS_HOST")):
            libs = " ".join(f"${{DAW_ROOT}}/plugins/imported/{pid}/lib/{arch}/lib{seen[pid]['libName']}.a" for pid in ids)
            f.write(f"set({var} {libs})\n")
    catalog = [{"id": pid, **{k: seen[pid][k] for k in ("name", "version", "type", "license", "sha256")}} for pid in ids]
    json.dump(catalog, open(os.path.join(gen_dir, "catalog.json"), "w"), indent=2)
    lock = {"abi": f"{abi[0]}.{abi[1]}", "plugins": {pid: {k: seen[pid][k] for k in ("package", "version", "sha256", "license")} for pid in ids}}
    json.dump(lock, open(os.path.join(ROOT, "plugins", "plugins.lock.json"), "w"), indent=2)
    print(f"registry: {len(ids)} plugins -> plugins/generated/")


if __name__ == "__main__":
    main()
