// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/sf2_subset.hpp"
#include <algorithm>
#include <array>
#include <string.h>

namespace daw {
namespace {
uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
void w16(uint8_t *p, uint32_t x) { p[0] = x; p[1] = x >> 8; }
void w32(uint8_t *p, uint32_t x) { for (int i = 0; i < 4; i++) p[i] = x >> (8 * i); }
using Bytes = std::vector<uint8_t>;
void append(Bytes &v, const uint8_t *p, size_t n) { v.insert(v.end(), p, p + n); }
void header(Bytes &v, const char *tag, uint32_t n) {
    const size_t at = v.size(); v.resize(at + 8); memcpy(v.data() + at, tag, 4); w32(v.data() + at + 4, n);
}
void chunk(Bytes &v, const char *tag, const Bytes &b) {
    header(v, tag, (uint32_t)b.size()); append(v, b.data(), b.size()); if (b.size() & 1) v.push_back(0);
}
struct Range { uint32_t lo, hi, base; };
constexpr const char *names[] = {"phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"};
constexpr uint32_t record[] = {38, 4, 10, 4, 22, 4, 10, 4, 46};
}

bool sf2Subset(const Sf2Reader &read, uint32_t size, const std::vector<Sf2Preset> &wanted,
               Sf2Subset &out, std::string *err)
{
    out = {};
    auto fail = [&](const char *s) { if (err) *err = s; out = {}; return false; };
    uint8_t h[12];
    if (size < 12 || !read(0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "sfbk", 4))
        return fail("not a SoundFont 2 file");
    const uint64_t end = 8ull + u32(h + 4);
    if (end > size || end < 12) return fail("truncated SoundFont");
    std::array<Bytes, 9> t;
    uint32_t pcmAt = 0, pcmSize = 0;
    size_t metadata = 0;
    for (uint64_t p = 12; p + 8 <= end;) {
        if (!read((uint32_t)p, h, 8)) return fail("SoundFont read failed");
        const uint32_t n = u32(h + 4);
        if (p + 8 + n > end) return fail("invalid SoundFont chunk size");
        if (!memcmp(h, "LIST", 4) && n >= 4) {
            if (!read((uint32_t)p + 8, h + 8, 4)) return fail("SoundFont read failed");
            const bool pdta = !memcmp(h + 8, "pdta", 4), sdta = !memcmp(h + 8, "sdta", 4);
            if (pdta || sdta) for (uint64_t q = p + 12; q + 8 <= p + 8 + n;) {
                uint8_t sh[8]; if (!read((uint32_t)q, sh, 8)) return fail("SoundFont read failed");
                const uint32_t m = u32(sh + 4);
                if (q + 8 + m > p + 8 + n) return fail("invalid SoundFont subchunk");
                if (sdta && !memcmp(sh, "smpl", 4)) {
                    if (pcmSize || m % 2) return fail("invalid PCM16 sample pool");
                    pcmAt = (uint32_t)q + 8; pcmSize = m;
                }
                if (pdta) for (size_t i = 0; i < t.size(); i++) if (!memcmp(sh, names[i], 4)) {
                    if (!t[i].empty() || !m || m % record[i] || m > (1u << 20) || metadata + m > (1u << 20))
                        return fail("invalid or excessive SoundFont metadata");
                    t[i].resize(m); metadata += m;
                    if (!read((uint32_t)q + 8, t[i].data(), m)) return fail("SoundFont read failed");
                }
                q += 8ull + m + (m & 1);
            }
        }
        p += 8ull + n + (n & 1);
    }
    for (const auto &b : t) if (b.empty()) return fail("incomplete SoundFont metadata");
    const uint32_t np = (uint32_t)t[0].size()/38, ni = (uint32_t)t[4].size()/22, ns = (uint32_t)t[8].size()/46;
    if (np < 2 || ni < 2 || ns < 2 || !pcmSize) return fail("incomplete SoundFont");
    std::vector<uint32_t> picks;
    uint32_t first = 0;
    for (uint32_t i = 1; i + 1 < np; i++) {
        const uint8_t *a = &t[0][i*38], *b = &t[0][first*38];
        if (u16(a+22) < u16(b+22) || (u16(a+22) == u16(b+22) && u16(a+20) < u16(b+20))) first = i;
    }
    for (const auto &w : wanted) {
        uint32_t hit = first;
        for (uint32_t i = 0; i + 1 < np; i++) {
            const uint8_t *p = &t[0][i*38];
            if (u16(p+22) == w.bank && u16(p+20) == w.program) { hit = i; break; }
        }
        picks.push_back(hit);
    }
    if (picks.empty()) picks.push_back(first);
    std::sort(picks.begin(), picks.end()); picks.erase(std::unique(picks.begin(), picks.end()), picks.end());
    Bytes ph, pb, pm, pg;
    std::vector<bool> instruments(ni, false), samples(ns, false);
    uint64_t pad = 0;
    auto offsets = [&](const uint8_t *g) {
        const uint16_t op = u16(g); const int64_t amount = (int16_t)u16(g+2);
        if (op <= 3 || op == 4 || op == 12 || op == 45 || op == 50)
            pad += (uint64_t)(amount < 0 ? -amount : amount) * ((op <= 3) ? 1u : 32768u);
    };
    // Preserve each preset's global/local zones and modulators. Instrument tables keep their
    // original indexes; only reachable sample headers are relocated into the packed pool.
    for (uint32_t i : picks) {
        const uint8_t *p = &t[0][i*38]; const uint32_t a = u16(p+24), b = u16(p+38+24);
        if (a > b || b >= t[1].size()/4 || pb.size()/4 > 65535) return fail("invalid preset bags");
        const size_t at = ph.size(); append(ph, p, 38); w16(ph.data()+at+24, (uint32_t)pb.size()/4);
        for (uint32_t j = a; j < b; j++) {
            const uint8_t *bag = &t[1][j*4]; const uint32_t ga = u16(bag), gb = u16(bag+4), ma = u16(bag+2), mb = u16(bag+6);
            if (ga > gb || gb >= t[3].size()/4 || ma > mb || mb >= t[2].size()/10 || pg.size()/4 > 65535 || pm.size()/10 > 65535)
                return fail("invalid preset generators");
            uint8_t nb[4]; w16(nb, (uint32_t)pg.size()/4); w16(nb+2, (uint32_t)pm.size()/10); append(pb, nb, 4);
            for (uint32_t k = ga; k < gb; k++) {
                const uint8_t *g = &t[3][k*4]; append(pg, g, 4); offsets(g);
                if (u16(g) == 41) { const uint32_t id = u16(g+2); if (id+1 >= ni) return fail("invalid instrument reference"); instruments[id] = true; }
            }
            append(pm, &t[2][ma*10], (mb-ma)*10);
        }
    }
    if (pb.size()/4 > 65535 || pg.size()/4 > 65535 || pm.size()/10 > 65535) return fail("too many preset zones");
    uint8_t term[38]{}; memcpy(term, "EOP", 3); w16(term+24, (uint32_t)pb.size()/4); append(ph, term, 38);
    uint8_t bag[4]; w16(bag, (uint32_t)pg.size()/4); w16(bag+2, (uint32_t)pm.size()/10); append(pb, bag, 4);
    uint8_t zero[10]{}; append(pg, zero, 4); append(pm, zero, 10);
    for (uint32_t i = 0; i + 1 < ni; i++) if (instruments[i]) {
        const uint32_t a = u16(&t[4][i*22+20]), b = u16(&t[4][(i+1)*22+20]);
        if (a > b || b >= t[5].size()/4) return fail("invalid instrument bags");
        for (uint32_t j = a; j < b; j++) {
            const uint32_t ga = u16(&t[5][j*4]), gb = u16(&t[5][(j+1)*4]);
            if (ga > gb || gb >= t[7].size()/4) return fail("invalid instrument generators");
            for (uint32_t k = ga; k < gb; k++) {
                const uint8_t *g = &t[7][k*4]; offsets(g);
                if (u16(g) == 53) { const uint32_t id = u16(g+2); if (id+1 >= ns) return fail("invalid sample reference"); samples[id] = true; }
            }
        }
    }
    // Stereo-linked samples can refer to one another; follow the closure without recursion.
    bool added = true;
    while (added) {
        added = false;
        for (uint32_t i = 0; i + 1 < ns; i++) if (samples[i] && (u16(&t[8][i*46+44]) & 6)) {
            const uint32_t link = u16(&t[8][i*46+42]);
            if (link+1 >= ns) return fail("invalid stereo sample link");
            if (!samples[link]) { samples[link] = true; added = true; }
        }
    }
    const uint32_t count = pcmSize/2;
    std::vector<Range> ranges;
    for (uint32_t i = 0; i + 1 < ns; i++) if (samples[i]) {
        const uint8_t *s = &t[8][i*46]; const uint32_t start = u32(s+20), endSample = u32(s+24), ls = u32(s+28), le = u32(s+32);
        if (start > endSample || endSample > count || ls > count || le > count) return fail("invalid sample bounds");
        const uint32_t lo = le || ls ? std::min({start, ls, le}) : start;
        const uint32_t hi = le || ls ? std::max({endSample, ls, le}) : endSample;
        ranges.push_back({(uint32_t)(lo > pad ? lo-pad : 0), (uint32_t)std::min<uint64_t>(count, (uint64_t)hi+pad+46), 0});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.lo < b.lo; });
    std::vector<Range> merged;
    for (const auto &r : ranges) {
        if (merged.empty() || r.lo > merged.back().hi) merged.push_back(r);
        else merged.back().hi = std::max(merged.back().hi, r.hi);
    }
    uint32_t packed = 0;
    for (auto &r : merged) { r.base = packed; packed += r.hi-r.lo; out.samples.push_back({pcmAt+r.lo*2, (r.hi-r.lo)*2}); }
    // A preset with no zones is legal. TSF requires at least one PCM16 value in its container.
    if (!packed) { packed = 1; out.samples.push_back({pcmAt, 2}); }
    for (uint32_t i = 0; i + 1 < ns; i++) {
        uint8_t *s = &t[8][i*46];
        if (!samples[i]) { memset(s+20, 0, 16); continue; }
        const bool noLoop = !u32(s+28) && !u32(s+32);
        for (int k = 20; k <= 32; k += 4) {
            if (noLoop && k >= 28) continue;
            const uint32_t pos = u32(s+k); bool found = false;
            for (const auto &r : merged) if (pos >= r.lo && pos <= r.hi) { w32(s+k, r.base+pos-r.lo); found = true; break; }
            if (!found) return fail("sample relocation failed");
        }
    }
    t[0] = std::move(ph); t[1] = std::move(pb); t[2] = std::move(pm); t[3] = std::move(pg);
    Bytes pdta;
    for (size_t i = 0; i < t.size(); i++) chunk(pdta, names[i], t[i]);
    header(out.suffix, "LIST", 4+(uint32_t)pdta.size()); append(out.suffix, (const uint8_t *)"pdta", 4); append(out.suffix, pdta.data(), pdta.size());
    out.pcmBytes = packed*2;
    // Minimal INFO is sufficient; original bank/program/name records stay intact in pdta.
    Bytes info, version{2,0,1,0}, engine{'E','M','U','8','0','0','0',0}, title{'S','e','l','e','c','t','e','d',0,0};
    chunk(info, "ifil", version); chunk(info, "isng", engine); chunk(info, "INAM", title);
    header(out.prefix, "RIFF", 0); append(out.prefix, (const uint8_t *)"sfbk", 4);
    header(out.prefix, "LIST", 4+(uint32_t)info.size()); append(out.prefix, (const uint8_t *)"INFO", 4); append(out.prefix, info.data(), info.size());
    header(out.prefix, "LIST", 12+out.pcmBytes); append(out.prefix, (const uint8_t *)"sdta", 4); header(out.prefix, "smpl", out.pcmBytes);
    w32(out.prefix.data()+4, out.bytes()-8);
    return true;
}

bool Sf2Subset::copy(const Sf2Reader &read, uint8_t *out, size_t capacity, std::string *err) const
{
    if (!out || capacity < bytes()) { if (err) *err = "SoundFont subset buffer too small"; return false; }
    memcpy(out, prefix.data(), prefix.size()); size_t at = prefix.size();
    for (const auto &s : samples) {
        if (!read(s.source, out+at, s.bytes)) { if (err) *err = "SoundFont sample read failed"; return false; }
        at += s.bytes;
    }
    memcpy(out+at, suffix.data(), suffix.size()); return true;
}
}  // namespace daw
