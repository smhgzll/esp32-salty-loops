// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library (power-safe blob store on NOR flash) and the WAV decoder.
#include <math.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "check.hpp"
#include "daw/model.hpp"
#include "module_fixture.hpp"
#include "daw/audio_file.hpp"
#include "daw/platform.hpp"
#include "daw/sample_store.hpp"
#include "daw/sf2_info.hpp"

using namespace daw;

static void put16(std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t)x), v.push_back((uint8_t)(x >> 8)); }
static void put32(std::vector<uint8_t> &v, uint32_t x) { put16(v, x & 0xFFFF), put16(v, x >> 16); }
static void tag(std::vector<uint8_t> &v, const char *t) { v.insert(v.end(), t, t + 4); }

// a WAV file: `frames` frames of a ramp (channel c: value * (c ? -1 : 1)), in the given format
static std::vector<uint8_t> makeWav(uint16_t fmt, uint16_t bits, uint16_t chans, uint32_t rate, uint32_t frames, int root = -1,
                                    uint32_t loopS = 0, uint32_t loopE = 0, bool extensible = false)
{
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < frames; i++)
        for (uint16_t c = 0; c < chans; c++) {
            const double x = ((double)i / frames * 1.6 - 0.8) * (c ? -1 : 1);
            if (fmt == 3) {
                float f = (float)x;
                uint32_t u;
                memcpy(&u, &f, 4);
                put32(data, u);
            } else if (bits == 8)
                data.push_back((uint8_t)lrint(x * 127 + 128));
            else if (bits == 16)
                put16(data, (uint16_t)(int16_t)lrint(x * 32767));
            else if (bits == 24) {
                const int32_t v = (int32_t)lrint(x * 8388607);
                data.push_back((uint8_t)v), data.push_back((uint8_t)(v >> 8)), data.push_back((uint8_t)(v >> 16));
            } else
                put32(data, (uint32_t)(int32_t)lrint(x * 2147483647.0));
        }
    std::vector<uint8_t> w;
    tag(w, "RIFF");
    put32(w, 0);
    tag(w, "WAVE");
    tag(w, "fmt ");
    put32(w, extensible ? 40 : 16);
    put16(w, extensible ? 0xFFFE : fmt);
    put16(w, chans);
    put32(w, rate);
    put32(w, rate * chans * bits / 8);
    put16(w, chans * bits / 8);
    put16(w, bits);
    if (extensible) {
        put16(w, 22), put16(w, bits), put32(w, 0);
        put16(w, fmt);
        for (int i = 0; i < 14; i++)
            w.push_back(0);
    }
    if (root >= 0) {
        tag(w, "smpl");
        put32(w, 36 + 24);
        for (int i = 0; i < 3; i++)
            put32(w, 0);
        put32(w, (uint32_t)root);
        for (int i = 0; i < 3; i++)
            put32(w, 0);
        put32(w, loopE ? 1 : 0);
        put32(w, 0);
        put32(w, 0), put32(w, 0), put32(w, loopS), put32(w, loopE ? loopE - 1 : 0), put32(w, 0), put32(w, 0);
    }
    tag(w, "data");
    put32(w, (uint32_t)data.size());
    w.insert(w.end(), data.begin(), data.end());
    const uint32_t riff = (uint32_t)w.size() - 8;
    memcpy(w.data() + 4, &riff, 4);
    return w;
}

static std::vector<uint8_t> blob(size_t n, uint8_t seed)
{
    auto w = makeWav(1, 16, 1, 44100, (uint32_t)(n / 2));
    for (size_t i = 44; i < w.size(); i++)
        w[i] = (uint8_t)(i * 131 + seed);
    return w;
}

// a SoundFont skeleton: INFO, a small sdta, and a pdta whose phdr holds these presets (+ EOP)
struct TestPreset {
    const char *name;
    uint16_t program, bank;
};
static std::vector<uint8_t> makeSf2(const std::vector<TestPreset> &presets, int phdrExtra = 0)
{
    auto chunk = [](std::vector<uint8_t> &v, const char *id, const std::vector<uint8_t> &body) {
        tag(v, id);
        put32(v, (uint32_t)body.size());
        v.insert(v.end(), body.begin(), body.end());
        if (body.size() & 1)
            v.push_back(0);
    };
    auto list = [&](std::vector<uint8_t> &v, const char *type, const std::vector<uint8_t> &body) {
        std::vector<uint8_t> b;
        tag(b, type);
        b.insert(b.end(), body.begin(), body.end());
        chunk(v, "LIST", b);
    };
    std::vector<uint8_t> info, sdta, smpl(1001, 0x55), phdr, pdta, all, file;
    chunk(info, "ifil", {2, 0, 1, 0});
    chunk(info, "INAM", {'t', 'e', 's', 't', 0});  // odd length: padded
    chunk(sdta, "smpl", smpl);
    auto rec = [&](const char *name, uint16_t prog, uint16_t bank) {
        char n[20] = {};
        strncpy(n, name, 20);
        phdr.insert(phdr.end(), n, n + 20);
        put16(phdr, prog);
        put16(phdr, bank);
        put16(phdr, 0);
        put32(phdr, 0);
        put32(phdr, 0);
        put32(phdr, 0);
    };
    for (auto &p : presets)
        rec(p.name, p.program, p.bank);
    rec("EOP", 0, 0);
    for (int i = 0; i < phdrExtra; i++)
        phdr.push_back(0);
    chunk(pdta, "phdr", phdr);
    chunk(pdta, "pbag", std::vector<uint8_t>(4 * (presets.size() + 1), 0));
    tag(all, "sfbk");
    list(all, "INFO", info);
    list(all, "sdta", sdta);
    list(all, "pdta", pdta);
    chunk(file, "RIFF", all);
    return file;
}
static bool presetsOf(const std::vector<uint8_t> &f, std::vector<Sf2Preset> &out, uint32_t size = 0xFFFFFFFF)
{
    const uint32_t n = size == 0xFFFFFFFF ? (uint32_t)f.size() : size;
    return sf2Presets(
        [&](uint32_t off, void *buf, uint32_t len) {
            if (off > n || len > n - off)
                return false;
            memcpy(buf, f.data() + off, len);
            return true;
        },
        n, out, nullptr);
}

int main()
{
    // names and types
    CHECK(sanitizeSampleName("../Kick 01.wav") == "_Kick 01.wav");  // leading dots removed, slash replaced
    CHECK(sanitizeSampleName("  ") == "");
    CHECK(sanitizeSampleName(std::string(80, 'a') + ".wav").size() == SampleStore::kMaxName);
    CHECK(sanitizeSampleName(std::string(80, 'a') + ".wav").substr(SampleStore::kMaxName - 4) == ".wav");
    {
        auto w = makeWav(1, 16, 1, 44100, 10);
        CHECK(sniffSample(w.data(), w.size()) == SampleType::Wav);
        std::vector<uint8_t> sf = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 's', 'f', 'b', 'k'};
        CHECK(sniffSample(sf.data(), sf.size()) == SampleType::SoundFont);
        std::vector<uint8_t> junk(100, 7);
        CHECK(sniffSample(junk.data(), junk.size()) == SampleType::Unknown);
        // tracker modules the Tracker plugin (libxmp-lite) reads; other MOD signatures are not
        for (const char *m : {"M.K.", "6CHN", "16CH", "32CH"}) {
            auto mod = moduleFixture(m);
            CHECK(sniffSample(mod.data(), mod.size()) == SampleType::Module);
        }
        for (const char *m : {"FLT4", "M!K!", "0CHN", "00CH", "33CH"}) {
            auto mod = moduleFixture(m);
            CHECK(sniffSample(mod.data(), mod.size()) == SampleType::Unknown);
        }
        auto shortMod = moduleFixture();
        CHECK(sniffSample(shortMod.data(), 1083) == SampleType::Unknown);  // the signature is at 1080
        std::vector<uint8_t> xm(64, 0), s3m(64, 0), it(64, 0);
        memcpy(xm.data(), "Extended Module: ", 17);
        memcpy(s3m.data() + 44, "SCRM", 4);
        memcpy(it.data(), "IMPM", 4);
        CHECK(sniffSample(xm.data(), xm.size()) == SampleType::Module);
        CHECK(sniffSample(s3m.data(), s3m.size()) == SampleType::Module);
        CHECK(sniffSample(it.data(), it.size()) == SampleType::Module);
        CHECK(!strcmp(sampleTypeName(SampleType::Module), "module"));
    }
    // put / read / list / remove / free space / remount
    {
        MemFlash flash(64 * 4096);
        SampleStore st(flash);
        st.mount();
        CHECK(st.list().empty() && st.freeBytes() == 64 * 4096);
        auto a = blob(10000, 1), b = blob(30000, 2);
        std::string err;
        CHECK(st.put("kick.wav", a.data(), (uint32_t)a.size(), &err));
        CHECK(st.put("pad.wav", b.data(), (uint32_t)b.size(), &err));
        CHECK(st.list().size() == 2);
        CHECK(st.freeBytes() == (64 - 3 - 8) * 4096);  // 10000 + 64 -> 3 sectors, 30000 + 64 -> 8
        std::vector<uint8_t> out;
        CHECK(st.read("pad.wav", out, &err) && out == b);
        CHECK(st.verify("pad.wav", &err));
        CHECK(st.find("pad.wav")->crc == crc32(b.data(), b.size()));
        std::vector<uint8_t> direct(b.size(), 0);
        CHECK(st.readInto("pad.wav", direct.data(), direct.size(), &err) && direct == b);
        CHECK(!st.readInto("pad.wav", direct.data(), direct.size() - 1, &err));
        CHECK(!st.readInto("pad.wav", nullptr, direct.size(), &err));
        auto owned = LoadedResource::FileBytes{static_cast<uint8_t *>(platform::alloc(b.size(), 16, platform::Mem::Large)), platform::free};
        CHECK(owned && st.readInto("pad.wav", owned.get(), b.size(), &err));
        auto resource = LoadedResource::file("pad.wav", std::move(owned), (uint32_t)b.size());
        CHECK(resource && resource->bytes() == b.size() && resource->resource()->size == b.size());
        CHECK(resource && !memcmp(resource->resource()->data, b.data(), b.size()));
        CHECK(!st.put("x.bin", (const uint8_t *)"hello world!", 12, &err) && err.find("not a WAV") != std::string::npos);
        SampleStore again(flash);
        again.mount();
        CHECK(again.list().size() == 2 && again.find("kick.wav") && again.find("kick.wav")->size == a.size());
        CHECK(again.remove("kick.wav") && !again.find("kick.wav"));
        CHECK(again.freeBytes() == (64 - 8) * 4096);
        SampleStore third(flash);
        third.mount();
        CHECK(third.list().size() == 1);
        // replace: the new bytes under the old name, the old copy's space comes back
        auto b2 = blob(5000, 9);
        CHECK(third.put("pad.wav", b2.data(), (uint32_t)b2.size(), &err));
        CHECK(third.list().size() == 1 && third.read("pad.wav", out, &err) && out == b2);
        CHECK(third.freeBytes() == (64 - 2) * 4096);
        // full: a clear refusal; a bit flip in the data is caught by the CRC
        auto huge = blob(70 * 4096, 3);
        CHECK(!third.put("huge.wav", huge.data(), (uint32_t)huge.size(), &err) && err.find("free space") != std::string::npos);
        flash.data()[third.find("pad.wav")->sector * 4096 + 64 + 100] ^= 0x10;
        CHECK(!third.read("pad.wav", out, &err) && err.find("CRC") != std::string::npos);
        CHECK(!third.verify("pad.wav", &err) && err.find("CRC") != std::string::npos);
        CHECK(!third.readInto("pad.wav", direct.data(), direct.size(), &err) && err.find("CRC") != std::string::npos);
    }
    // power cuts at every point of a new file's save: afterwards it is complete or absent, and the
    // files already stored are untouched
    {
        auto keep = blob(6000, 4), add = blob(9000, 5);
        MemFlash probe(32 * 4096);
        SampleStore ps(probe);
        ps.mount();
        std::string err;
        ps.put("keep.wav", keep.data(), (uint32_t)keep.size(), &err);
        const auto base = probe.data();
        int complete = 0, absent = 0, bad = 0;
        for (int64_t budget = 0; budget < 9000 + 64 + 3 * 64 + 64; budget += 17) {
            MemFlash f(32 * 4096);
            f.data() = base;
            SampleStore s(f);
            s.mount();
            f.failAfter(budget);
            s.put("add.wav", add.data(), (uint32_t)add.size(), &err);
            f.failAfter(-1);
            SampleStore r(f);  // reboot
            r.mount();
            std::vector<uint8_t> out;
            if (!r.read("keep.wav", out, &err) || out != keep)
                bad++;
            if (r.find("add.wav")) {
                if (r.read("add.wav", out, &err) && out == add)
                    complete++;
                else
                    bad++;
            } else
                absent++;
        }
        CHECK(bad == 0);
        CHECK(complete > 0 && absent > 0);
    }
    // power cuts while replacing a file: exactly one copy survives, old or new, never damaged
    {
        auto v1 = blob(8000, 6), v2 = blob(7000, 7);
        MemFlash probe(32 * 4096);
        SampleStore ps(probe);
        ps.mount();
        std::string err;
        ps.put("loop.wav", v1.data(), (uint32_t)v1.size(), &err);
        const auto base = probe.data();
        int oldv = 0, newv = 0, bad = 0;
        for (int64_t budget = 0; budget < 7000 + 64 + 2 * 64 + 64 + 8; budget += 13) {
            MemFlash f(32 * 4096);
            f.data() = base;
            SampleStore s(f);
            s.mount();
            f.failAfter(budget);
            s.put("loop.wav", v2.data(), (uint32_t)v2.size(), &err);
            f.failAfter(-1);
            SampleStore r(f);
            r.mount();
            std::vector<uint8_t> out;
            if (r.list().size() != 1 || !r.read("loop.wav", out, &err))
                bad++;
            else if (out == v1)
                oldv++;
            else if (out == v2)
                newv++;
            else
                bad++;
        }
        CHECK(bad == 0 && oldv > 0 && newv > 0);
    }
    // WAV decoding: every supported format gives the same ramp
    {
        struct F {
            uint16_t fmt, bits;
            float tol;
        } fs[] = {{1, 8, 1.0f / 100}, {1, 16, 1e-4f}, {1, 24, 1e-6f}, {1, 32, 1e-6f}, {3, 32, 1e-7f}};
        for (auto f : fs)
            for (uint16_t ch : {1, 2})
                for (bool ext : {false, true}) {
                    auto w = makeWav(f.fmt, f.bits, ch, 48000, 1000, -1, 0, 0, ext);
                    DecodedAudio a;
                    std::string err;
                    CHECK(decodeWav(w.data(), w.size(), a, &err));
                    CHECK(a.channels == ch && a.rate == 48000 && a.frames() == 1000);
                    bool ok = true;
                    for (uint32_t i = 0; i < 1000; i += 37)
                        for (uint32_t c = 0; c < ch; c++)
                            ok &= fabs(a.ch[c][i] - ((double)i / 1000 * 1.6 - 0.8) * (c ? -1 : 1)) < f.tol;
                    CHECK(ok);
                }
        auto w = makeWav(1, 16, 1, 44100, 2000, 64, 100, 1500);
        DecodedAudio a;
        std::string err;
        CHECK(decodeWav(w.data(), w.size(), a, &err) && a.rootKey == 64 && a.loopStart == 100 && a.loopEnd == 1500);
        auto res = LoadedResource::audio("x.wav", std::move(a));
        CHECK(res->resource()->type == DAW_RES_AUDIO && res->resource()->frames == 2000 && res->resource()->root_key == 64);
        CHECK(res->resource()->channels[0] && res->resource()->channel_count == 1);
        // refusals: not a WAV, a compressed format, a truncated file stays inside its buffer
        std::vector<uint8_t> junk(64, 0);
        CHECK(!decodeWav(junk.data(), junk.size(), a, &err));
        auto adpcm = makeWav(2, 4, 1, 44100, 100);
        CHECK(!decodeWav(adpcm.data(), adpcm.size(), a, &err) && err.find("unsupported") != std::string::npos);
        auto cut = makeWav(1, 16, 2, 44100, 1000);
        cut.resize(cut.size() / 2);
        CHECK(decodeWav(cut.data(), cut.size(), a, &err) && a.frames() < 1000 && a.frames() > 400);
    }
    // SoundFont preset lists (PHDR), read piecewise
    {
        const char ctl[] = {'B', 'a', 'd', 1, 'N', 'a', 'm', 'e', 0};
        auto f = makeSf2({{"Piano  ", 0, 0}, {"Standard Kit", 0, 128}, {"Strings", 48, 0}, {ctl, 5, 1}, {"Out of range", 200, 0}});
        std::vector<Sf2Preset> ps;
        CHECK(presetsOf(f, ps));
        CHECK(ps.size() == 4);  // program 200 cannot be selected: left out
        if (ps.size() == 4) {
            CHECK(ps[0].name == "Piano" && ps[0].bank == 0 && ps[0].program == 0);  // trailing spaces trimmed
            CHECK(ps[1].name == "Strings" && ps[1].program == 48);                 // sorted by bank, program
            CHECK(ps[2].name == "Bad?Name" && ps[2].bank == 1 && ps[2].program == 5);
            CHECK(ps[3].name == "Standard Kit" && ps[3].bank == 128);
        }
        CHECK(!presetsOf(makeSf2({{"A", 0, 0}}, 5), ps) && ps.empty());  // phdr not a multiple of 38 bytes
        CHECK(!presetsOf(f, ps, (uint32_t)f.size() - 60));               // truncated
        auto w = makeWav(1, 16, 1, 44100, 10);
        CHECK(!presetsOf(w, ps));
        std::vector<uint8_t> noPdta = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 's', 'f', 'b', 'k'};
        CHECK(!presetsOf(noPdta, ps));
        std::vector<uint8_t> huge = f;  // a chunk size pointing far past the end
        huge[4] = huge[5] = huge[6] = 0xFF;
        CHECK(presetsOf(huge, ps) && ps.size() == 4);
        // through the sample library, without reading the whole file
        MemFlash fl(64 * 4096);
        SampleStore st(fl);
        st.mount();
        CHECK(st.put("font.sf2", f.data(), (uint32_t)f.size(), nullptr));
        uint8_t b4[4];
        CHECK(st.readRange("font.sf2", 0, b4, 4) && !memcmp(b4, "RIFF", 4));
        CHECK(!st.readRange("font.sf2", (uint32_t)f.size() - 2, b4, 4));
        CHECK(!st.readRange("nope.sf2", 0, b4, 4));
        CHECK(sf2Presets([&](uint32_t o, void *b, uint32_t l) { return st.readRange("font.sf2", o, b, l); }, (uint32_t)f.size(), ps, nullptr) &&
              ps.size() == 4);
    }
    // a file written in pieces (an upload streamed into the library) is the same file as one put
    // whole; until finishPut nothing exists and the run stays reserved; a power cut or an abort
    // leaves free space; a name is replaced only when the new file is complete
    {
        auto f = makeSf2({{"Piano", 0, 0}, {"Kit", 0, 128}});
        f.resize(70000, 0x5A);  // larger than one 64 KB erase block, not a multiple of a piece
        MemFlash whole(64 * 4096), parts(64 * 4096);
        SampleStore a(whole), b(parts);
        a.mount();
        b.mount();
        std::string err, name;
        CHECK(a.put("gm.sf2", f.data(), (uint32_t)f.size(), &err));
        CHECK(b.beginPut("gm.sf2", (uint32_t)f.size(), f.data(), 4096, &err) && b.putting());
        CHECK(b.find("gm.sf2") == nullptr && b.freeBytes() < 64 * 4096 - 69 * 1024);  // reserved, not listed
        for (size_t at = 4096; at < f.size(); at += 5000)
            CHECK(b.appendPut(f.data() + at, (uint32_t)std::min<size_t>(5000, f.size() - at), &err));
        CHECK(b.finishPut(&name, &err) && name == "gm.sf2" && !b.putting());
        CHECK(whole.data() == parts.data());  // byte for byte the same flash
        std::vector<uint8_t> back;
        CHECK(b.read("gm.sf2", back, &err) && back == f);
        SampleStore c(parts);
        c.mount();
        CHECK(c.find("gm.sf2") && c.find("gm.sf2")->size == f.size());
        // more bytes than announced, too few, not a SoundFont / WAV
        CHECK(b.beginPut("x.sf2", 100, f.data(), 12, &err) && !b.appendPut(f.data(), 200, &err) && !b.putting());
        CHECK(b.beginPut("x.sf2", 100, f.data(), 12, &err) && !b.finishPut(&name, &err) && b.find("x.sf2") == nullptr);
        std::vector<uint8_t> junk(64, 1);
        CHECK(!b.beginPut("x.sf2", 64, junk.data(), 64, &err));
        // a tracker module in pieces (its first piece holds the MOD signature at 1080)
        auto mod = moduleFixture();
        mod.resize(9000, 0x11);
        CHECK(b.beginPut("song.mod", (uint32_t)mod.size(), mod.data(), 2048, &err));
        CHECK(b.appendPut(mod.data() + 2048, (uint32_t)mod.size() - 2048, &err));
        CHECK(b.finishPut(&name, &err) && name == "song.mod" && b.find("song.mod")->type == SampleType::Module);
        CHECK(b.remove("song.mod"));
        // replacing gm.sf2 in pieces: an abort keeps the old file and frees the run
        const uint32_t free0 = b.freeBytes();
        auto g = f;
        g[5000] ^= 0xFF;
        CHECK(b.beginPut("gm.sf2", (uint32_t)g.size(), g.data(), 4096, &err));
        CHECK(b.appendPut(g.data() + 4096, 20000, &err));
        b.abortPut();
        CHECK(b.freeBytes() == free0 && b.read("gm.sf2", back, &err) && back == f);
        // a power cut in the middle: after a remount the old file is there, the run is free
        CHECK(b.beginPut("gm.sf2", (uint32_t)g.size(), g.data(), 4096, &err));
        CHECK(b.appendPut(g.data() + 4096, 30000, &err));
        SampleStore d(parts);
        d.mount();
        CHECK(d.freeBytes() == free0 && d.read("gm.sf2", back, &err) && back == f);
        // and completed: the new file replaces the old one
        CHECK(d.beginPut("gm.sf2", (uint32_t)g.size(), g.data(), 4096, &err));
        CHECK(d.appendPut(g.data() + 4096, (uint32_t)g.size() - 4096, &err) && d.finishPut(&name, &err));
        CHECK(d.read("gm.sf2", back, &err) && back == g && d.list().size() == 1 && d.freeBytes() == free0);
    }
    TEST_MAIN_END("test_samples");
}
