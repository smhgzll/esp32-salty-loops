// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal ProTracker module ("M.K.", 4 channels): one pattern of 64 rows at speed 6 / 125 BPM
// (7.68 s, 16 quarter notes at 125 BPM), a note on channel 1 every 4 rows, one looped 32-byte saw.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
inline std::vector<uint8_t> moduleFixture(const char *magic = "M.K.")
{
    std::vector<uint8_t> b(1084, 0);
    memcpy(b.data(), "fixture", 7);
    uint8_t *s = b.data() + 20;  // sample 1 of 31 (30 bytes each)
    memcpy(s, "saw", 3);
    s[22] = 0, s[23] = 16;       // length: 16 words
    s[25] = 64;                  // volume
    s[28] = 0, s[29] = 16;       // loop: 16 words from 0
    for (int i = 1; i < 31; i++)
        b[20 + 30 * i + 29] = 1;  // empty samples: loop length 1 word
    b[950] = 1;                   // one order: pattern 0
    b[951] = 127;
    memcpy(b.data() + 1080, magic, 4);
    uint8_t pat[64][4][4] = {};
    pat[0][3][2] = 0xF, pat[0][3][3] = 6;     // speed 6
    pat[0][2][2] = 0xF, pat[0][2][3] = 0x7D;  // 125 BPM
    for (int r = 0; r < 64; r += 4) {
        const int period = (r / 4) % 2 ? 339 : 428;  // E-2 / C-2
        pat[r][0][0] = (uint8_t)(period >> 8), pat[r][0][1] = (uint8_t)period, pat[r][0][2] = 0x10;
    }
    b.insert(b.end(), &pat[0][0][0], &pat[0][0][0] + sizeof pat);
    for (int i = 0; i < 32; i++)
        b.push_back((uint8_t)(int8_t)(i * 4 - 64));
    return b;
}
