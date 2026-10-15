#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standard MIDI Files for the tests and examples/midi (Python standard library only).

"Salty Groove": 16 bars in A minor, drums / bass / keys / lead, a tempo change and three markers,
written as format 1 (a track per part) or format 0 (one track, a MIDI channel per part). An
original composition of this project. `groove()` also returns every note it wrote, so a test can
check what an import made of them.

  tests/web/midigen.py --examples examples/midi     write groove-format0.mid and groove-format1.mid
"""
import argparse
import os
import struct

DIV = 480  # ticks per quarter note in these files
BAR = DIV * 4


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.insert(0, (v & 0x7F) | 0x80)
        v >>= 7
    return bytes(out)


def meta(t, kind, body):
    return (t, 0, bytes([0xFF, kind]) + vlq(len(body)) + body)


def track_bytes(events):
    """events: (tick, order, raw bytes); sorted by tick (note offs before note ons at a tick)"""
    out = b""
    last = 0
    for t, _, raw in sorted(events, key=lambda e: (e[0], e[1])):
        out += vlq(t - last) + raw
        last = t
    out += b"\x00\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(out)) + out


def smf(fmt, tracks, division=DIV):
    return b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), division) + b"".join(track_bytes(t) for t in tracks)


CHORDS = [(45, [57, 60, 64]), (41, [53, 57, 60]), (48, [55, 60, 64]), (43, [55, 59, 62])]  # Am F C G: bass root, voicing
LEAD = [  # (bar offset in the 8-bar lead section, beat, length in beats, key)
    (0, 0, 1.5, 69), (0, 1.5, 0.5, 72), (0, 2, 2, 76),
    (1, 0, 1, 74), (1, 1, 1, 72), (1, 2, 2, 69),
    (2, 0, 1.5, 67), (2, 1.5, 0.5, 69), (2, 2, 2, 72),
    (3, 0, 3, 71), (3, 3, 1, 67),
    (4, 0, 1.5, 69), (4, 1.5, 0.5, 72), (4, 2, 2, 76),
    (5, 0, 1, 79), (5, 1, 1, 76), (5, 2, 2, 74),
    (6, 0, 1, 72), (6, 1, 1, 74), (6, 2, 2, 76),
    (7, 0, 4, 69),
]
PARTS = [  # name, MIDI channel, GM program (None = drums), pan (CC 10), volume (CC 7)
    ("Drums", 9, None, 64, 100),
    ("Bass", 1, 33, 64, 100),
    ("Keys", 2, 4, 40, 96),
    ("Lead", 3, 81, 88, 110),
]


def groove_notes():
    """{part name: [(start, length, key, velocity)]} in DIV ticks"""
    n = {name: [] for name, *_ in PARTS}
    e = DIV // 2  # an eighth
    for bar in range(16):
        b = bar * BAR
        root, chord = CHORDS[bar % 4]
        d = n["Drums"]
        for k in (0, 2):
            d.append((b + k * DIV, e, 36, 112))
        if bar % 2 == 1:
            d.append((b + 2 * DIV + e, e, 36, 96))
        for k in (1, 3):
            d.append((b + k * DIV, e, 38, 104))
        for k in range(8):
            if not (bar % 4 == 3 and k == 7):
                d.append((b + k * e, e // 2, 42, 84 if k % 2 == 0 else 62))
        if bar % 4 == 3:
            d.append((b + 7 * e, e, 46, 90))
        if bar in (0, 8):
            d.append((b, DIV, 49, 110))  # crash (DrumLab plays it as its open hat)
        for k in range(8):
            key = root + 12 if k == 7 else root
            n["Bass"].append((b + k * e, e - 20, key, 100 if k % 2 == 0 else 84))
        for key in chord:
            n["Keys"].append((b, BAR - 10, key, 76))
    for bar_off, beat, length, key in LEAD:
        n["Lead"].append((8 * BAR + bar_off * BAR + int(beat * DIV), int(length * DIV) - 10, key, 98))
    return n


def groove(fmt=1):
    """(file bytes, {part name: notes}) for format 0 or 1"""
    notes = groove_notes()
    conductor = [
        meta(0, 0x03, b"Salty Groove"),
        meta(0, 0x58, bytes([4, 2, 24, 8])),
        meta(0, 0x51, (500000 * 120 // 112).to_bytes(3, "big")),  # 112 BPM
        meta(8 * BAR, 0x51, (500000 * 120 // 116).to_bytes(3, "big")),  # 116 BPM from bar 9
        meta(0, 0x06, b"Intro"),
        meta(4 * BAR, 0x06, b"Groove"),
        meta(8 * BAR, 0x06, b"Lead"),
    ]
    tracks = []
    for name, ch, program, pan, vol in PARTS:
        ev = [meta(0, 0x03, name.encode())] if fmt == 1 else []
        ev.append((0, 1, bytes([0xB0 | ch, 7, vol])))
        ev.append((0, 1, bytes([0xB0 | ch, 10, pan])))
        if program is not None:
            ev.append((0, 1, bytes([0xC0 | ch, program])))
        for start, length, key, vel in notes[name]:
            ev.append((start, 2, bytes([0x90 | ch, key, vel])))
            ev.append((start + length, 0, bytes([0x80 | ch, key, 64])))
        tracks.append(ev)
    if fmt == 1:
        data = smf(1, [conductor] + tracks)
    else:
        data = smf(0, [conductor + [e for t in tracks for e in t]])
    return data, notes


def guitars():
    """(file bytes, {part name: notes}): a distortion guitar riff (GM 30, power chords) and an
    overdriven lead (GM 29), 4 bars, format 1"""
    riff, lead = [], []
    for bar in range(4):
        b = bar * BAR
        for k in range(8):
            root = (40, 40, 43, 45)[bar] + (0 if k < 6 else 2)
            for key in (root, root + 7, root + 12):
                riff.append((b + k * (DIV // 2), DIV // 2 - 30, key, 110))
        lead.append((b, DIV * 2, 64 + bar * 2, 100))
        lead.append((b + DIV * 2, DIV * 2 - 30, 67 + bar * 2, 96))
    notes = {"Rhythm": riff, "Lead": lead}
    tracks = [[meta(0, 0x03, b"Riff"), meta(0, 0x51, (500000).to_bytes(3, "big"))]]
    for name, ch, program in (("Rhythm", 0, 30), ("Lead", 1, 29)):
        ev = [meta(0, 0x03, name.encode()), (0, 1, bytes([0xC0 | ch, program]))]
        for start, length, key, vel in notes[name]:
            ev.append((start, 2, bytes([0x90 | ch, key, vel])))
            ev.append((start + length, 0, bytes([0x80 | ch, key, 64])))
        tracks.append(ev)
    return smf(1, tracks), notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--examples", help="directory to write groove-format0.mid and groove-format1.mid into")
    a = ap.parse_args()
    if a.examples:
        for fmt in (0, 1):
            path = os.path.join(a.examples, f"groove-format{fmt}.mid")
            open(path, "wb").write(groove(fmt)[0])
            print(path)


if __name__ == "__main__":
    main()
