#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/midi-bridge.py against a running DAW: raw MIDI bytes on the bridge's stdin must sound
on the DAW (voices in the status frames) and stop again; the aseqdump text parser must rebuild
the right MIDI bytes.

  tests/web/test_midi_bridge.py --connect 127.0.0.1:18190
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from wsclient import WS  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BRIDGE = os.path.join(ROOT, "scripts", "midi-bridge.py")
FAILS = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def max_voices(ws, seconds):
    end, best = time.time() + seconds, 0
    while time.time() < end:
        try:
            m = ws.recv(max(0.05, end - time.time()))
        except (TimeoutError, OSError):
            break
        if m.get("t") == "st":
            best = max(best, m.get("voices", 0))
    return best


def last_voices(ws, seconds):
    end, last = time.time() + seconds, None
    while time.time() < end:
        try:
            m = ws.recv(max(0.05, end - time.time()))
        except (TimeoutError, OSError):
            break
        if m.get("t") == "st":
            last = m.get("voices", 0)
    return last


def parser_checks():
    spec = importlib.util.spec_from_file_location("midi_bridge", BRIDGE)
    mb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mb)
    cases = [
        (" 20:0   Note on                 0, note 60, velocity 100", "903c64"),
        (" 20:0   Note off                3, note 61, velocity 0", "833d00"),
        (" 20:0   Control change          0, controller 64, value 127", "b0407f"),
        (" 20:0   Program change          9, program 5", "c905"),
        (" 20:0   Pitch bend              0, value -8192", "e00000"),
        (" 20:0   Pitch bend              1, value 0", "e10040"),
        (" 20:0   Pitch bend              0, value 8191", "e07f7f"),
        (" 20:0   Channel aftertouch      2, value 33", "d221"),
        (" 20:0   Polyphonic aftertouch   0, note 60, value 5", "a03c05"),
        (" 20:0   Clock", "f8"),
        (" 20:0   Start", "fa"),
        (" 20:0   Stop", "fc"),
        ("Waiting for data. Press Ctrl+C to end.", ""),
        ("Source  Event                  Ch  Data", ""),
    ]
    bad = [(l, want, mb.SeqInput.to_bytes(l).hex()) for l, want in cases if mb.SeqInput.to_bytes(l).hex() != want]
    check(not bad, f"aseqdump lines become the right MIDI bytes ({len(cases)} cases){' ' + str(bad) if bad else ''}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--connect", default="127.0.0.1:18190")
    a = ap.parse_args()
    host, port = a.connect.rsplit(":", 1)
    parser_checks()

    obs = WS(host, int(port))
    wel = obs.hello()
    sine = next(c["id"] for c in wel["project"]["channels"] if c["plugin"]["id"] == "org.smhgzll.sine")
    base = last_voices(obs, 0.6) or 0

    p = subprocess.Popen([sys.executable, BRIDGE, "--input", "-", "--host", host, "--port", port, "--channel", str(sine), "--linger", "3"],
                         stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(1.0)  # connected
    # three notes plus active sensing (dropped by the bridge) and running status
    p.stdin.write(bytes([0x90, 60, 100, 0xFE, 64, 100, 67, 100]))
    p.stdin.flush()
    held = max_voices(obs, 1.0)
    check(held >= base + 3, f"three notes from the bridge sound on the DAW ({held} voices, {base} before)")
    p.stdin.write(bytes([0x80, 60, 0, 0x90, 64, 0, 67, 0]))  # note off + note-on-velocity-0 offs
    p.stdin.flush()
    after = last_voices(obs, 1.5)
    check(after is not None and after <= base, f"note offs from the bridge release them ({after} voices)")
    # a held note is released when the bridge goes away (its connection closes)
    p.stdin.write(bytes([0x90, 72, 90]))
    p.stdin.flush()
    check(max_voices(obs, 0.8) >= base + 1, "a held note sounds")
    p.stdin.close()
    p.wait(timeout=15)
    err = p.stderr.read()
    check(p.returncode == 0, f"the bridge exits cleanly when stdin ends ({err.decode().strip().splitlines()[-1:]})")
    check((last_voices(obs, 2.0) or 0) <= base, "the DAW released the bridge's held note when it disconnected")
    obs.close()
    print(f"midi-bridge: {'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
