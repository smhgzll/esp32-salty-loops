#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screenshots of the browser DAW at the reference viewports (tests/browser/out/*.png).
   tests/browser/screens.py --url http://127.0.0.1:18190/"""
import argparse
import os
import sys

from playwright.sync_api import sync_playwright

VIEWPORTS = [
    ("desktop-1920x1080", 1920, 1080, False),
    ("laptop-1366x768", 1366, 768, False),
    ("tablet-landscape-1180x820", 1180, 820, True),
    ("phone-portrait-390x844", 390, 844, True),
    ("phone-landscape-844x390", 844, 390, True),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:18190/")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "out"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    with sync_playwright() as p:
        b = p.chromium.launch()
        for name, w, h, touch in VIEWPORTS:
            ctx = b.new_context(viewport={"width": w, "height": h}, has_touch=touch, is_mobile=touch and w < 1000, device_scale_factor=1)
            page = ctx.new_page()
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto(a.url)
            page.wait_for_selector(".shell, .mshell", timeout=20000)
            page.wait_for_timeout(800)
            page.screenshot(path=os.path.join(a.out, f"{name}.png"))
            print(f"{name}: {'errors ' + '; '.join(errors) if errors else 'ok'}")
            ctx.close()
        b.close()


if __name__ == "__main__":
    sys.exit(main())
