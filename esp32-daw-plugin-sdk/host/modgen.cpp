// SPDX-License-Identifier: MIT
// Minimal tracker modules for tests: the same two-pattern song written as Protracker MOD,
// FastTracker 2 XM, Scream Tracker 3 S3M and Impulse Tracker IT (see makeTestModule in host.hpp).
#include <string.h>

#include <string>
#include <vector>

#include "dawsdk_host/host.hpp"

namespace dawhost {
namespace {

// one event of the test song; note: semitones from the module's middle C (-99: none)
struct Ev {
    int row, ch, note;
    char fx;  // 'A' speed, 'T' tempo, 'C' pattern break, 0 none
    int param;
};

constexpr int kNone = -99;

// pattern 0: speed 6, 125 BPM; pattern 1: 150 BPM, break after row 32 (the song ends there)
std::vector<Ev> pattern(int p)
{
    std::vector<Ev> ev;
    static const int arp[4] = {0, 4, 7, 12};
    if (p == 0) {
        ev.push_back({0, 3, kNone, 'A', 6});
        ev.push_back({0, 2, kNone, 'T', 125});
    } else {
        ev.push_back({0, 2, kNone, 'T', 150});
        ev.push_back({32, 3, kNone, 'C', 0});
    }
    const int last = p == 0 ? 63 : 32;
    for (int r = 0; r <= last; r += 4)
        ev.push_back({r, 0, arp[(r / 4) % 4], 0, 0});
    for (int r = 2; r <= last; r += 8)
        ev.push_back({r, 1, -5, 0, 0});
    return ev;
}

// 32 frames of a looped 8-bit saw
std::vector<int8_t> wave()
{
    std::vector<int8_t> w(32);
    for (int i = 0; i < 32; i++)
        w[i] = (int8_t)(i * 4 - 64);
    return w;
}

struct Writer {
    std::vector<uint8_t> b;
    void u8(int x) { b.push_back((uint8_t)x); }
    void u16le(uint32_t x) { u8(x & 0xFF), u8(x >> 8 & 0xFF); }
    void u32le(uint32_t x) { u16le(x & 0xFFFF), u16le(x >> 16); }
    void u16be(uint32_t x) { u8(x >> 8 & 0xFF), u8(x & 0xFF); }
    void str(const char *s, size_t n)
    {
        for (size_t i = 0; i < n; i++)
            u8(i < strlen(s) ? s[i] : 0);
    }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void align(size_t a)
    {
        while (b.size() % a)
            u8(0);
    }
    void put16le(size_t at, uint32_t x) { b[at] = (uint8_t)x, b[at + 1] = (uint8_t)(x >> 8); }
    void put32le(size_t at, uint32_t x) { put16le(at, x & 0xFFFF), put16le(at + 2, x >> 16); }
};

std::vector<uint8_t> mod()
{
    static const int periods[] = {570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226, 214};
    Writer w;
    w.str("daw test mod", 20);
    for (int s = 0; s < 31; s++) {
        w.str(s == 0 ? "saw" : "", 22);
        w.u16be(s == 0 ? 16 : 0);  // length in words
        w.u8(0);                   // finetune
        w.u8(s == 0 ? 64 : 0);
        w.u16be(0);
        w.u16be(s == 0 ? 16 : 1);  // loop length in words (1: no loop)
    }
    w.u8(2);
    w.u8(127);
    w.u8(0), w.u8(1);
    w.zeros(126);
    w.str("M.K.", 4);
    for (int p = 0; p < 2; p++) {
        uint8_t cells[64][4][4] = {};
        for (const Ev &e : pattern(p)) {
            uint8_t *c = cells[e.row][e.ch];
            if (e.note != kNone) {
                const int per = periods[e.note + 5];
                c[0] = (uint8_t)(per >> 8);  // sample 1: high nibble 0
                c[1] = (uint8_t)per;
                c[2] = 0x10;
            }
            if (e.fx) {
                c[2] |= e.fx == 'C' ? 0xD : 0xF;
                c[3] = (uint8_t)e.param;
            }
        }
        w.b.insert(w.b.end(), &cells[0][0][0], &cells[0][0][0] + sizeof cells);
    }
    for (int8_t x : wave())
        w.u8((uint8_t)x);
    return w.b;
}

std::vector<uint8_t> xm()
{
    Writer w;
    w.str("Extended Module: ", 17);
    w.str("daw test xm", 20);
    w.u8(0x1A);
    w.str("dawsdk", 20);
    w.u16le(0x0104);
    w.u32le(276);
    w.u16le(2);    // song length
    w.u16le(0);    // restart
    w.u16le(4);    // channels
    w.u16le(2);    // patterns
    w.u16le(1);    // instruments
    w.u16le(1);    // linear frequencies
    w.u16le(6);    // speed
    w.u16le(125);  // BPM
    w.u8(0), w.u8(1);
    w.zeros(254);
    for (int p = 0; p < 2; p++) {
        uint8_t cells[64][4][5] = {};
        for (const Ev &e : pattern(p)) {
            uint8_t *c = cells[e.row][e.ch];
            if (e.note != kNone) {
                c[0] = (uint8_t)(49 + e.note);  // C-4
                c[1] = 1;
            }
            if (e.fx) {
                c[3] = e.fx == 'C' ? 0xD : 0xF;
                c[4] = (uint8_t)e.param;
            }
        }
        w.u32le(9);
        w.u8(0);
        w.u16le(64);
        w.u16le(sizeof cells);
        w.b.insert(w.b.end(), &cells[0][0][0], &cells[0][0][0] + sizeof cells);
    }
    w.u32le(263);
    w.str("saw", 22);
    w.u8(0);
    w.u16le(1);
    w.u32le(40);
    w.zeros(96 + 48 + 48);       // keymap, envelopes
    w.zeros(2 + 6 + 2);          // point counts, sustain / loop points, envelope types
    w.zeros(4);                  // vibrato
    w.u16le(0);                  // fadeout
    w.zeros(22);
    const auto s = wave();
    w.u32le((uint32_t)s.size());
    w.u32le(0);
    w.u32le((uint32_t)s.size());
    w.u8(64);
    w.u8(0);
    w.u8(1);     // forward loop, 8-bit
    w.u8(128);   // pan
    w.u8(0);     // relative note
    w.u8(0);
    w.str("saw", 22);
    int8_t prev = 0;
    for (int8_t x : s) {
        w.u8((uint8_t)(x - prev));  // delta coded
        prev = x;
    }
    return w.b;
}

std::vector<uint8_t> s3m()
{
    Writer w;
    w.str("daw test s3m", 28);
    w.u8(0x1A);
    w.u8(16);
    w.u16le(0);
    w.u16le(4);       // orders
    w.u16le(1);       // instruments
    w.u16le(2);       // patterns
    w.u16le(0);       // flags
    w.u16le(0x1320);  // Scream Tracker 3.20
    w.u16le(2);       // unsigned samples
    w.str("SCRM", 4);
    w.u8(64);         // global volume
    w.u8(6);          // speed
    w.u8(125);        // tempo
    w.u8(0xB0);       // stereo, master volume 48
    w.u8(0);
    w.u8(0);          // no pan table
    w.zeros(8);
    w.u16le(0);
    const uint8_t chans[4] = {0, 8, 1, 9};
    for (int c = 0; c < 32; c++)
        w.u8(c < 4 ? chans[c] : 255);
    w.u8(0), w.u8(1), w.u8(255), w.u8(255);
    const size_t ptrs = w.b.size();
    w.zeros(2 + 4);
    w.align(16);
    const size_t ins = w.b.size();
    w.zeros(80);
    w.put16le(ptrs, (uint32_t)(ins / 16));
    for (int p = 0; p < 2; p++) {
        w.align(16);
        const size_t at = w.b.size();
        w.put16le(ptrs + 2 + 2 * p, (uint32_t)(at / 16));
        w.u16le(0);
        const auto ev = pattern(p);
        for (int r = 0; r < 64; r++) {
            for (int c = 0; c < 4; c++) {
                int note = kNone;
                char fx = 0;
                int prm = 0;
                for (const Ev &e : ev)
                    if (e.row == r && e.ch == c) {
                        if (e.note != kNone)
                            note = e.note;
                        if (e.fx)
                            fx = e.fx, prm = e.param;
                    }
                if (note == kNone && !fx)
                    continue;
                w.u8(c | (note != kNone ? 0x20 : 0) | (fx ? 0x80 : 0));
                if (note != kNone) {
                    const int n = 48 + note;  // C-4
                    w.u8((n / 12) << 4 | n % 12);
                    w.u8(1);
                }
                if (fx) {
                    w.u8(fx == 'A' ? 1 : fx == 'T' ? 20 : 3);
                    w.u8(prm);
                }
            }
            w.u8(0);
        }
        w.put16le(at, (uint32_t)(w.b.size() - at));
    }
    w.align(16);
    const size_t smp = w.b.size();
    for (int8_t x : wave())
        w.u8((uint8_t)(x + 128));
    // the instrument, now that the sample's place is known
    Writer i;
    i.u8(1);
    i.str("SAW.SMP", 12);
    i.u8((int)(smp / 16 >> 16));
    i.u16le((uint32_t)(smp / 16 & 0xFFFF));
    i.u32le(32);
    i.u32le(0);
    i.u32le(32);
    i.u8(64);
    i.u8(0);
    i.u8(0);
    i.u8(1);  // loop
    i.u32le(8363);
    i.zeros(12);
    i.str("saw", 28);
    i.str("SCRS", 4);
    memcpy(&w.b[ins], i.b.data(), 80);
    return w.b;
}

std::vector<uint8_t> it()
{
    Writer w;
    w.str("IMPM", 4);
    w.str("daw test it", 26);
    w.u16le(0x1004);
    w.u16le(3);       // orders
    w.u16le(0);       // instruments
    w.u16le(1);       // samples
    w.u16le(2);       // patterns
    w.u16le(0x0214);  // created with
    w.u16le(0x0200);  // compatible with
    w.u16le(1 | 8);   // stereo, linear slides
    w.u16le(0);
    w.u8(128);        // global volume
    w.u8(48);         // mix volume
    w.u8(6);          // speed
    w.u8(125);        // tempo
    w.u8(128);        // separation
    w.u8(0);
    w.u16le(0);
    w.u32le(0);
    w.u32le(0);
    for (int c = 0; c < 64; c++)
        w.u8(c < 4 ? 32 : 32 | 128);
    for (int c = 0; c < 64; c++)
        w.u8(64);
    w.u8(0), w.u8(1), w.u8(255);
    const size_t ptrs = w.b.size();
    w.zeros(4 * 3);
    const size_t sh = w.b.size();
    w.put32le(ptrs, (uint32_t)sh);
    w.zeros(80);
    for (int p = 0; p < 2; p++) {
        const size_t at = w.b.size();
        w.put32le(ptrs + 4 + 4 * p, (uint32_t)at);
        w.u16le(0);
        w.u16le(64);
        w.zeros(4);
        const auto ev = pattern(p);
        for (int r = 0; r < 64; r++) {
            for (int c = 0; c < 4; c++) {
                int note = kNone;
                char fx = 0;
                int prm = 0;
                for (const Ev &e : ev)
                    if (e.row == r && e.ch == c) {
                        if (e.note != kNone)
                            note = e.note;
                        if (e.fx)
                            fx = e.fx, prm = e.param;
                    }
                if (note == kNone && !fx)
                    continue;
                w.u8((c + 1) | 0x80);
                w.u8((note != kNone ? 1 | 2 : 0) | (fx ? 8 : 0));
                if (note != kNone) {
                    w.u8(60 + note);  // C-5
                    w.u8(1);
                }
                if (fx) {
                    w.u8(fx == 'A' ? 1 : fx == 'T' ? 20 : 3);
                    w.u8(prm);
                }
            }
            w.u8(0);
        }
        w.put16le(at, (uint32_t)(w.b.size() - at - 8));
    }
    const size_t smp = w.b.size();
    for (int8_t x : wave())
        w.u8((uint8_t)x);
    Writer s;
    s.str("IMPS", 4);
    s.str("SAW.SMP", 12);
    s.u8(0);
    s.u8(64);         // global volume
    s.u8(1 | 16);     // sample present, loop
    s.u8(64);
    s.str("saw", 26);
    s.u8(1);          // signed
    s.u8(32);
    s.u32le(32);
    s.u32le(0);
    s.u32le(32);
    s.u32le(8363);
    s.u32le(0);
    s.u32le(0);
    s.u32le((uint32_t)smp);
    s.zeros(4);
    memcpy(&w.b[sh], s.b.data(), 80);
    return w.b;
}

// "it-dense": 16 channels playing four samples at once (8-bit saw, 16-bit square, a ping-pong 8-bit
// triangle and a one-shot 8-bit decay that ends inside ticks), notes every third row, changing
// volumes; speed 3 at 125 BPM, one pattern (3.84 s). Many voices on several samples: what the
// Tracker splits between two cores.
std::vector<uint8_t> itDense()
{
    constexpr int kCh = 16, kSmp = 4;
    Writer w;
    w.str("IMPM", 4);
    w.str("daw test it dense", 26);
    w.u16le(0x1004);
    w.u16le(2);       // orders
    w.u16le(0);       // instruments
    w.u16le(kSmp);    // samples
    w.u16le(1);       // patterns
    w.u16le(0x0214);
    w.u16le(0x0200);
    w.u16le(1 | 8);   // stereo, linear slides
    w.u16le(0);
    w.u8(128);
    w.u8(48);
    w.u8(3);          // speed
    w.u8(125);        // tempo
    w.u8(128);
    w.u8(0);
    w.u16le(0);
    w.u32le(0);
    w.u32le(0);
    for (int c = 0; c < 64; c++)
        w.u8(c < kCh ? c * 4 : 32 | 128);  // pans across the field
    for (int c = 0; c < 64; c++)
        w.u8(64);
    w.u8(0), w.u8(255);
    const size_t ptrs = w.b.size();
    w.zeros(4 * (kSmp + 1));
    size_t sh[kSmp];
    for (int i = 0; i < kSmp; i++) {
        sh[i] = w.b.size();
        w.put32le(ptrs + 4 * i, (uint32_t)sh[i]);
        w.zeros(80);
    }
    const size_t at = w.b.size();
    w.put32le(ptrs + 4 * kSmp, (uint32_t)at);
    w.u16le(0);
    w.u16le(64);
    w.zeros(4);
    for (int r = 0; r < 64; r++) {
        for (int c = 0; c < kCh; c++) {
            if ((r + c) % 3)
                continue;
            w.u8((c + 1) | 0x80);
            w.u8(1 | 2 | 4);
            w.u8(48 + (r * 7 + c * 5) % 24);
            w.u8(c % kSmp + 1);
            w.u8(16 + (r * 13 + c * 7) % 48);
        }
        w.u8(0);
    }
    w.put16le(at, (uint32_t)(w.b.size() - at - 8));
    // name, flags (1 present, 2 16-bit, 16 loop, 64 ping-pong), length, loop
    struct Smp { const char *name; int flags, len, ls, le; };
    const Smp smps[kSmp] = {{"saw", 1 | 16, 32, 0, 32}, {"square", 1 | 2 | 16, 64, 0, 64},
                            {"triangle", 1 | 16 | 64, 48, 8, 48}, {"decay", 1, 300, 0, 0}};
    for (int i = 0; i < kSmp; i++) {
        const size_t data = w.b.size();
        for (int k = 0; k < smps[i].len; k++) {
            int v = i == 0 ? k * 4 - 64 : i == 1 ? (k < 32 ? 12000 : -12000)
                  : i == 2 ? (k < 24 ? k * 10 - 120 : 360 - k * 10) : ((k * 37) % 61 - 30) * (300 - k) / 75;
            if (smps[i].flags & 2)
                w.u16le((uint16_t)(int16_t)v);
            else
                w.u8((uint8_t)(int8_t)v);
        }
        Writer s;
        s.str("IMPS", 4);
        s.str("", 12);
        s.u8(0);
        s.u8(64);
        s.u8(smps[i].flags);
        s.u8(64);
        s.str(smps[i].name, 26);
        s.u8(1);  // signed
        s.u8(32);
        s.u32le((uint32_t)smps[i].len);
        s.u32le((uint32_t)smps[i].ls);
        s.u32le((uint32_t)smps[i].le);
        s.u32le(8363);
        s.u32le(0);
        s.u32le(0);
        s.u32le((uint32_t)data);
        s.zeros(4);
        memcpy(&w.b[sh[i]], s.b.data(), 80);
    }
    return w.b;
}

}  // namespace

std::vector<uint8_t> makeTestModule(const std::string &format)
{
    if (format == "it-dense")
        return itDense();
    if (format == "xm")
        return xm();
    if (format == "s3m")
        return s3m();
    if (format == "it")
        return it();
    return mod();
}

}  // namespace dawhost
