# SPDX-License-Identifier: GPL-3.0-or-later
"""A minimal SoundFont 2 for tests: one preset -> one instrument -> one looped 16-bit sine."""
import math
import struct


def _chunk(cid, body):
    return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def _list(ltype, body):
    return _chunk(b"LIST", ltype + body)


def make_sf2(freq=261.6256, root=60, rate=44100, seconds=0.5, extra_presets=()):
    """extra_presets: (name, bank, program) entries without zones (they list, but play nothing)"""
    period = round(rate / freq)
    frames = max(period * 4, int(seconds * rate) // period * period)
    smpl = b"".join(struct.pack("<h", int(16000 * math.sin(2 * math.pi * i / period))) for i in range(frames)) + b"\0\0" * 46
    info = _chunk(b"ifil", struct.pack("<HH", 2, 1)) + _chunk(b"INAM", b"esp32-daw test font\0") + _chunk(b"isng", b"EMU8000\0")
    n20 = lambda s: s.encode()[:19].ljust(20, b"\0")
    phdr = n20("Test Sine") + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0)
    for name, bank, program in extra_presets:
        phdr += n20(name) + struct.pack("<HHHIII", program, bank, 1, 0, 0, 0)
    phdr += n20("EOP") + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0)
    pbag = struct.pack("<HHHH", 0, 0, 1, 0)
    pgen = struct.pack("<HHHH", 41, 0, 0, 0)
    inst = n20("Sine") + struct.pack("<H", 0) + n20("EOI") + struct.pack("<H", 1)
    ibag = struct.pack("<HHHH", 0, 0, 3, 0)
    igen = struct.pack("<HHHHHHHH", 54, 1, 58, root, 53, 0, 0, 0)
    shdr = n20("sine") + struct.pack("<IIIIIBbHH", 0, frames, 0, frames, rate, root, 0, 0, 1) + n20("EOS") + struct.pack("<IIIIIBbHH", 0, 0, 0, 0, 0, 0, 0, 0, 0)
    pdta = b"".join(_chunk(c, b) for c, b in ((b"phdr", phdr), (b"pbag", pbag), (b"pmod", b"\0" * 10), (b"pgen", pgen), (b"inst", inst),
                                            (b"ibag", ibag), (b"imod", b"\0" * 10), (b"igen", igen), (b"shdr", shdr)))
    return _chunk(b"RIFF", b"sfbk" + _list(b"INFO", info) + _list(b"sdta", _chunk(b"smpl", smpl)) + _list(b"pdta", pdta))
