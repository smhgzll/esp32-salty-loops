// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/sf2_info.hpp"

#include <string.h>

#include <algorithm>

namespace daw {

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool sf2Presets(const Sf2Reader &read, uint32_t size, std::vector<Sf2Preset> &out, std::string *err)
{
    out.clear();
    auto fail = [&](const char *m) {
        if (err)
            *err = m;
        out.clear();
        return false;
    };
    uint8_t h[12];
    if (size < 12 || !read(0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "sfbk", 4))
        return fail("not a SoundFont 2 file");
    const uint64_t end = std::min<uint64_t>(size, 8ull + le32(h + 4));
    // top level: LIST INFO, LIST sdta (the samples: skipped), LIST pdta
    for (uint64_t off = 12; off + 12 <= end;) {
        if (!read((uint32_t)off, h, 12))
            return fail("truncated SoundFont");
        const uint64_t len = le32(h + 4), next = off + 8 + len + (len & 1);
        if (!memcmp(h, "LIST", 4) && !memcmp(h + 8, "pdta", 4)) {
            const uint64_t lend = std::min<uint64_t>(end, off + 8 + len);
            for (uint64_t s = off + 12; s + 8 <= lend;) {
                if (!read((uint32_t)s, h, 8))
                    return fail("truncated SoundFont");
                const uint64_t slen = le32(h + 4);
                if (!memcmp(h, "phdr", 4)) {
                    constexpr uint32_t kRec = 38;
                    if (slen % kRec || slen < 2 * kRec || s + 8 + slen > lend)
                        return fail("damaged preset headers (phdr)");
                    const uint32_t n = std::min<uint32_t>((uint32_t)(slen / kRec) - 1, kMaxSf2Presets);  // the last is "EOP"
                    uint8_t r[kRec];
                    for (uint32_t i = 0; i < n; i++) {
                        if (!read((uint32_t)(s + 8 + (uint64_t)i * kRec), r, kRec))
                            return fail("truncated SoundFont");
                        Sf2Preset p;
                        for (int k = 0; k < 20 && r[k]; k++)
                            p.name += r[k] >= 0x20 && r[k] < 0x7f ? (char)r[k] : '?';
                        while (!p.name.empty() && p.name.back() == ' ')
                            p.name.pop_back();
                        p.program = le16(r + 20);
                        p.bank = le16(r + 22);
                        if (p.program > 127 || p.bank > 16383)  // outside what the player can select
                            continue;
                        out.push_back(p);
                    }
                    std::stable_sort(out.begin(), out.end(), [](const Sf2Preset &a, const Sf2Preset &b) {
                        return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
                    });
                    return true;
                }
                s += 8 + slen + (slen & 1);
            }
            return fail("no preset headers in the SoundFont");
        }
        off = next;
    }
    return fail("no preset data (pdta) in the SoundFont");
}

}  // namespace daw
