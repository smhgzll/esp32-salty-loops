#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Browser firmware update (POST /api/ota) against the QEMU firmware: refusals before anything is
written, a real update into the other OTA slot that confirms itself after a healthy start, and an
update whose start is made unhealthy (QEMU-only test hook) that the bootloader must roll back.
The project partition must survive all of it.

  tests/web/test_ota.py --connect 127.0.0.1:18080 --image build-qemu/esp32_s3_daw.bin \
      [--hw-image build-hw/esp32_s3_daw.bin] --admin-password qemu-test-password
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from wsclient import http_req  # noqa: E402

FAILS = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def status(host, port, timeout=5):
    try:
        st, body = http_req(host, port, "GET", "/api/status", timeout=timeout)
        return json.loads(body) if st == 200 else None
    except (OSError, ValueError):
        return None


def login(host, port, pw):
    st, body = http_req(host, port, "POST", "/api/login", body=json.dumps({"password": pw}))
    return json.loads(body)["token"] if st == 200 else ""


def upload(host, port, data, token=None, origin=None):
    h = {"Content-Type": "application/octet-stream"}
    if token:
        h["Authorization"] = "Bearer " + token
    if origin:
        h["Origin"] = origin
    try:
        st, body = http_req(host, port, "POST", "/api/ota", body=data, headers=h, timeout=600)
    except OSError as e:
        return 0, str(e)
    try:
        return st, json.loads(body).get("error", "") if st != 200 else body.decode()
    except ValueError:
        return st, body.decode(errors="replace")


def watch_boots(host, port, boot0, until, timeout):
    """status samples after a restart until until(samples) or the timeout: [(boot, partition, otaState)]"""
    seen, end = [], time.time() + timeout
    while time.time() < end:
        s = status(host, port, 3)
        if s and s.get("boot", 0) > boot0:
            t = (s["boot"], s.get("partition"), s.get("otaState"))
            if not seen or seen[-1] != t:
                seen.append(t)
                print(f"      boot {t[0]}: {t[1]} {t[2]}", flush=True)
            if until(seen):
                break
        time.sleep(1.0)
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--connect", default="127.0.0.1:18080")
    ap.add_argument("--image", required=True)
    ap.add_argument("--hw-image")
    ap.add_argument("--admin-password", default="qemu-test-password")
    ap.add_argument("--boot-timeout", type=float, default=240)
    ap.add_argument("--log", help="the device's serial log (QEMU_LOG), to confirm what happened inside")
    a = ap.parse_args()
    host, port = a.connect.rsplit(":", 1)
    port = int(port)
    image = open(a.image, "rb").read()

    s0 = status(host, port)
    check(s0 is not None, "device answers")
    if not s0:
        return 1
    print(f"      boot {s0['boot']}: {s0['partition']} {s0.get('otaState')} ({s0['firmware']})")
    project0 = json.loads(http_req(host, port, "GET", "/api/project")[1])["meta"]["name"]

    # ---- refused before anything is written --------------------------------------------------------
    st, _ = upload(host, port, image)
    check(st == 401, f"update without a login -> 401 ({st})")
    token = login(host, port, a.admin_password)
    check(bool(token), "login")
    st, _ = upload(host, port, image, token, origin="http://evil.example")
    check(st == 403, f"cross-origin update -> 403 ({st})")
    st, why = upload(host, port, os.urandom(8192), token)
    check(st == 400 and "not an ESP application image" in why, f"random bytes refused ({st} {why})")
    if a.hw_image and os.path.exists(a.hw_image):
        st, why = upload(host, port, open(a.hw_image, "rb").read(), token)
        check(st == 400 and "hardware firmware refused under QEMU" in why, f"the hardware build is refused under QEMU ({st} {why})")
    else:
        print("SKIP  hardware image refusal (no --hw-image; scripts/build-hardware.sh)")
    bad = bytearray(image)
    mid = len(bad) // 2
    bad[mid:mid + 64] = bytes(b ^ 0xA5 for b in bad[mid:mid + 64])
    t0 = time.time()
    st, why = upload(host, port, bytes(bad), token)
    check(st == 400 and "verification failed" in why, f"a corrupted image fails verification and is not booted ({st} {why}, {time.time() - t0:.0f} s)")
    s1 = status(host, port)
    check(s1 and s1["boot"] == s0["boot"] and s1["partition"] == s0["partition"], "still running the same firmware, no restart")

    # ---- a real update ------------------------------------------------------------------------------
    t0 = time.time()
    st, body = upload(host, port, image, token)
    check(st == 200 and "restart" in body, f"update accepted ({len(image)} bytes in {time.time() - t0:.0f} s of wall time)")
    other = "ota_1" if s0["partition"] == "ota_0" else "ota_0"
    seen = watch_boots(host, port, s0["boot"], lambda v: v[-1][2] == "valid", a.boot_timeout)
    check(any(x[1] == other for x in seen), f"booted the new image from {other}")
    check(bool(seen) and seen[-1][1] == other and seen[-1][2] == "valid", "the new image confirmed itself after a healthy start")
    s2 = status(host, port)

    # ---- an unhealthy update: the bootloader rolls back ----------------------------------------------
    token = login(host, port, a.admin_password)
    st, _ = http_req(host, port, "POST", "/api/test/fail-next-health", body=b"", headers={"Authorization": "Bearer " + token})
    check(st == 200, f"test hook armed ({st})")
    st, body = upload(host, port, image, token)
    check(st == 200, "second update accepted")
    # the unhealthy start is short (about 4 s): it may be seen in passing, but the proof is two restarts
    # (boot counter) ending on the confirmed image, plus the device's own log (--log)
    seen = watch_boots(host, port, s2["boot"], lambda v: v[-1][0] >= s2["boot"] + 2 and v[-1][2] == "valid", a.boot_timeout)
    check(bool(seen) and seen[-1][0] == s2["boot"] + 2, f"two restarts: the unhealthy start, then the rollback ({[x[0] for x in seen]})")
    check(bool(seen) and seen[-1][1] == other and seen[-1][2] == "valid", f"rolled back to the confirmed image on {other}")
    if a.log:
        time.sleep(1)
        text = open(a.log, errors="replace").read()
        check(f"new firmware on {s0['partition']}" in text and "unhealthy start (test hook): rolling back" in text,
              f"the device log shows the start from {s0['partition']} and the rollback")

    project = json.loads(http_req(host, port, "GET", "/api/project")[1])["meta"]["name"]
    check(project == project0, f"the project survived three restarts and two updates ('{project}')")
    print(f"ota: {'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
