# SPDX-License-Identifier: GPL-3.0-or-later
"""A minimal ProTracker module for tests ("M.K.", 4 channels): one pattern of 64 rows at speed 6 /
125 BPM (7.68 s), a note on channel 1 every 4 rows, one looped 32-byte saw. Same song as
tests/core/module_fixture.hpp."""
import struct


def make_mod(magic=b"M.K."):
    b = bytearray(1084)
    b[0:7] = b"fixture"
    s = 20  # sample 1 of 31 (30 bytes each)
    b[s:s + 3] = b"saw"
    b[s + 22:s + 24] = struct.pack(">H", 16)  # length in words
    b[s + 25] = 64                             # volume
    b[s + 28:s + 30] = struct.pack(">H", 16)  # loop length in words
    for i in range(1, 31):
        b[20 + 30 * i + 29] = 1                # empty samples: loop length 1 word
    b[950], b[951] = 1, 127                    # one order: pattern 0
    b[1080:1084] = magic
    pat = bytearray(64 * 4 * 4)
    cell = lambda r, c: (r * 4 + c) * 4
    pat[cell(0, 3) + 2], pat[cell(0, 3) + 3] = 0x0F, 6      # speed 6
    pat[cell(0, 2) + 2], pat[cell(0, 2) + 3] = 0x0F, 0x7D   # 125 BPM
    for r in range(0, 64, 4):
        period = 339 if (r // 4) % 2 else 428
        o = cell(r, 0)
        pat[o], pat[o + 1], pat[o + 2] = period >> 8, period & 255, 0x10
    return bytes(b) + bytes(pat) + bytes((i * 4 - 64) & 255 for i in range(32))
