#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Third-party notices for what a build distributes: served as /licenses.txt by the firmware (and by
daw-host), linked from the browser's device page.

  scripts/gen-notices.py --target firmware|host [--out FILE]

Collects: this DAW (GPL-3.0-or-later), every imported plugin package (its manifest's license and
provenance, and the license texts it ships in licenses/), cJSON, the browser application's runtime
dependencies (web/node_modules/<dep>/LICENSE), and for firmware builds ESP-IDF (LICENSE and its
COPYRIGHT summary of bundled components) and managed components (mDNS).
"""
import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RULE = "=" * 78


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().rstrip() + "\n"
    except OSError:
        return None


def find_idf():
    for c in (os.environ.get("IDF_PATH"), read(os.path.join(ROOT, ".idf_path"))):
        if c and os.path.isfile(os.path.join(c.strip(), "LICENSE")):
            return c.strip()
    env = os.path.expanduser("~/.espressif/idf-env.json")
    if os.path.isfile(env):
        for v in json.load(open(env)).get("idfInstalled", {}).values():
            if os.path.isfile(os.path.join(v.get("path", ""), "LICENSE")):
                return v["path"]
    return None


def section(out, title, body):
    out.append(f"{RULE}\n{title}\n{RULE}\n\n{body.rstrip()}\n\n")


def generate(target):
    out = []
    out.append("ESP32-S3 DAW: licenses and notices\n\n"
               "This device runs software from several authors under several licenses. The texts below\n"
               "are the notices their licenses require to accompany copies of the software.\n\n")
    section(out, "esp32-s3-daw (firmware, Linux host, browser application)",
            "Copyright (C) 2026 smhgzll\nLicensed under the GNU General Public License, version 3 or later.\n"
            "The full license text is at the end of this file.")

    imported = os.path.join(ROOT, "plugins", "imported")
    for mf in sorted(glob.glob(os.path.join(imported, "*", "manifest.json"))):
        d = os.path.dirname(mf)
        m = json.load(open(mf))
        body = [f"{m.get('name')} {m.get('version')} by {m.get('vendor')}, license {m.get('license')}",
                f"Plugin id {m.get('id')}, linked into this build from its .espplug package."]
        for p in m.get("provenance", []):
            line = f"- {p.get('component')}: {p.get('origin')}"
            if p.get("author"):
                line += f", {p['author']}"
            if p.get("commit"):
                line += f", commit {p['commit']}"
            line += f" ({p.get('license')})"
            if p.get("modifications"):
                line += f"\n  modified: {p['modifications']}"
            body.append(line)
        for lf in m.get("licenseFiles", []):
            text = read(os.path.join(d, lf))
            if text:
                body.append(f"\n--- {lf} ---\n{text}")
        section(out, f"Plugin: {m.get('name')} ({m.get('id')})", "\n".join(body))

    cj = read(os.path.join(ROOT, "third_party", "cJSON", "LICENSE"))
    if cj:
        section(out, "cJSON", cj)

    pkg = json.load(open(os.path.join(ROOT, "web", "package.json")))
    for dep, ver in sorted(pkg.get("dependencies", {}).items()):
        text = read(os.path.join(ROOT, "web", "node_modules", dep, "LICENSE"))
        section(out, f"{dep} {ver} (browser application)", text or f"{dep}: license text not found at build time (see https://www.npmjs.com/package/{dep})")

    if target == "firmware":
        idf = find_idf()
        if idf:
            section(out, "ESP-IDF (Espressif IoT Development Framework)", read(os.path.join(idf, "LICENSE")) or "")
            cr = read(os.path.join(idf, "docs", "en", "COPYRIGHT.rst"))
            if cr:
                section(out, "ESP-IDF: copyrights and licenses of bundled components (docs/en/COPYRIGHT.rst)", cr)
        else:
            section(out, "ESP-IDF", "Apache License 2.0, Copyright Espressif Systems; see https://github.com/espressif/esp-idf")
        for lic in sorted(glob.glob(os.path.join(ROOT, "managed_components", "*", "LICENSE"))):
            name = os.path.basename(os.path.dirname(lic)).replace("__", "/")
            section(out, f"ESP component {name}", read(lic))
    else:
        section(out, "Linux host", "daw-host links the system's ALSA library (alsa-lib, LGPL-2.1-or-later) dynamically.")

    gpl = read(os.path.join(ROOT, "LICENSE"))
    if gpl:
        section(out, "GNU General Public License, version 3", gpl)
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", choices=("firmware", "host"), default="firmware")
    ap.add_argument("--out")
    a = ap.parse_args()
    text = generate(a.target)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
