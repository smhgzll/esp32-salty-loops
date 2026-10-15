// SPDX-License-Identifier: GPL-3.0-or-later
// Project slots on NOR flash: round trips, A/B alternation, power loss at every stage, corruption.
#include <stddef.h>
#include <string.h>

#include <string>

#include "check.hpp"
#include "daw/model.hpp"
#include "daw/platform.hpp"
#include "daw/store.hpp"

using namespace daw;

static std::string payload(int n, char c) { return std::string((size_t)n, c); }
// bytes that do not deflate (slots store documents deflated): sizes in flash stay predictable
static std::string noise(int n, uint32_t seed)
{
    std::string s((size_t)n, '\0');
    for (auto &ch : s) {
        seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
        ch = (char)(seed >> 24);
    }
    return s;
}
// a project-like document: note arrays, repeating with small changes (deflates well)
static std::string notesJson(int notes)
{
    std::string s = "{\"notes\":[";
    for (int i = 0; i < notes; i++)
        s += (i ? "," : "") + std::string("[") + std::to_string(i + 1) + "," + std::to_string(i * 240 % 15360) + ",240," +
             std::to_string(36 + i * 7 % 48) + ",0." + std::to_string(5000 + i * 37 % 4999) + ",0,0,0.5,1,0]";
    return s + "]}";
}
// a version 1 half (written before slots were deflated): the document as it is
static void writeV1(MemFlash &f, uint32_t off, uint32_t seq, const std::string &doc, const char *name)
{
    struct {
        uint32_t magic;
        uint16_t version, size;
        uint32_t seq, len, crc, time;
        char name[32];
        uint32_t hcrc;
    } h{};
    static_assert(sizeof h == 60, "version 1 header");
    h.magic = 0x50574144u, h.version = 1, h.size = sizeof h, h.seq = seq, h.len = (uint32_t)doc.size();
    h.crc = crc32(doc.data(), doc.size()), h.time = 7;
    strncpy(h.name, name, sizeof h.name);
    h.hcrc = crc32(&h, offsetof(decltype(h), hcrc));
    CHECK(f.erase(off, 4096 * ((64 + (uint32_t)doc.size() + 4095) / 4096)));
    CHECK(f.write(off + 64, doc.data(), (uint32_t)doc.size()));
    CHECK(f.write(off, &h, sizeof h));
}

int main()
{
    // round trip, alternation between the halves, newest wins
    {
        MemFlash f(8 * 2 * 64 * 1024);
        ProjectStore st(f, 8);
        CHECK(st.maxPayload() == 64 * 1024 - 64);
        std::string out, err;
        CHECK(!st.load(1, out, nullptr, &err) && err == "empty slot");
        for (int i = 0; i < 5; i++) {
            CHECK(st.save(1, "song " + std::to_string(i), payload(1000 + i, (char)('a' + i)), 100 + i, &err));
            SlotInfo si;
            CHECK(st.load(1, out, &si, &err));
            CHECK(out == payload(1000 + i, (char)('a' + i)));
            CHECK(si.seq == (uint32_t)i + 1 && si.name == "song " + std::to_string(i) && si.time == (uint32_t)(100 + i));
        }
        CHECK(!st.info(2).valid);
        CHECK(st.info(1).valid && st.info(1).bytes == 1004 && st.info(1).stored < 100);  // deflated
        CHECK(st.remove(1));
        CHECK(!st.info(1).valid);
        // too large: refused, nothing written (bytes that do not deflate)
        CHECK(!st.save(3, "big", noise(70000, 1), 0, &err));
        CHECK(err.find("too large") != std::string::npos);
        CHECK(!st.info(3).valid);
        // a document several times larger than the slot fits when it deflates (a MIDI import)
        const std::string doc = notesJson(6000);  // 228 KB, about 45 KB deflated
        CHECK(doc.size() > 3 * st.maxPayload());
        CHECK(st.save(3, "midi", doc, 0, &err));
        SlotInfo si;
        CHECK(st.load(3, out, &si, &err) && out == doc);
        CHECK(si.bytes == doc.size() && si.stored <= st.maxPayload());
        // beyond the document limit: refused before anything is written
        CHECK(!st.save(4, "huge", payload((int)ProjectStore::kMaxDocument + 1, 'h'), 0, &err));
        CHECK(err.find("too large") != std::string::npos && !st.info(4).valid);
    }
    // version 1 halves (the document stored as it is) still load; the next save writes version 2
    // into the other half, and the newer one wins
    {
        MemFlash f(2 * 2 * 32 * 1024);
        ProjectStore st(f, 2);
        const uint32_t half = 32 * 1024;
        writeV1(f, 1 * 2 * half, 5, "{\"old\":true}", "old song");
        std::string out, err;
        SlotInfo si;
        CHECK(st.load(1, out, &si, &err) && out == "{\"old\":true}" && si.name == "old song" && si.seq == 5);
        CHECK(si.bytes == si.stored && si.bytes == 12);
        CHECK(st.save(1, "new song", notesJson(50), 8, &err));
        CHECK(st.load(1, out, &si, &err) && out == notesJson(50) && si.seq == 6 && si.name == "new song");
        // the version 1 copy is still the fallback when the new half is damaged
        f.data()[1 * 2 * half + half + 64 + 5] ^= 0x10;
        CHECK(st.load(1, out, &si, &err) && out == "{\"old\":true}");
    }
    // deflated halves: a stream that does not inflate to its recorded length is not loaded
    {
        MemFlash f(2 * 32 * 1024);
        ProjectStore st(f, 1);
        std::string out, err;
        CHECK(st.save(0, "a", notesJson(20), 1, &err));
        std::string packed;
        CHECK(platform::deflate("abc", 3, packed) && platform::inflate(packed.data(), packed.size(), 3, out) && out == "abc");
        CHECK(!platform::inflate(packed.data(), packed.size(), 4, out) && out.empty());                  // too short
        CHECK(!platform::inflate(packed.data(), packed.size(), 2, out));                                  // too long
        CHECK(!platform::inflate(packed.data(), packed.size() - 1, 3, out));                              // cut off
        CHECK(!platform::inflate("\xff\xff\xff\xff", 4, 3, out));                                      // not deflate
    }
    // power loss at every point of a save: the slot reads either the old or the new project,
    // never anything else
    {
        const std::string oldP = noise(5000, 2), newP = noise(6000, 3);
        int sawOld = 0, sawNew = 0;
        for (int64_t budget = 0; budget < 7000; budget += 97) {
            MemFlash f(2 * 2 * 32 * 1024);
            ProjectStore st(f, 2);
            std::string err, out;
            CHECK(st.save(0, "v1", oldP, 1, &err));
            f.failAfter(budget);
            st.save(0, "v2", newP, 2, &err);  // may fail anywhere
            f.failAfter(-1);
            SlotInfo si;
            CHECK(st.load(0, out, &si, &err));
            if (out == oldP && si.name == "v1")
                sawOld++;
            else if (out == newP && si.name == "v2")
                sawNew++;
            else
                CHECK(!"torn read");
            // and the slot keeps working afterwards
            CHECK(st.save(0, "v3", payload(100, '3'), 3, &err));
            CHECK(st.load(0, out, nullptr, &err) && out == payload(100, '3'));
        }
        CHECK(sawOld > 10 && sawNew > 0);
    }
    // a flipped payload bit falls back to the other copy
    {
        MemFlash f(2 * 32 * 1024);
        ProjectStore st(f, 1);
        std::string err, out;
        CHECK(st.save(0, "a", noise(300, 4), 1, &err));
        CHECK(st.save(0, "b", noise(300, 5), 2, &err));
        // the newest copy lives in half 1 (second save)
        f.data()[32 * 1024 + 64 + 10] ^= 0x01;  // half size = 64 KiB / 1 slot / 2
        SlotInfo si;
        CHECK(st.load(0, out, &si, &err));
        CHECK(out == noise(300, 4) && si.name == "a");
        // both corrupt: an error, not garbage
        f.data()[64 + 10] ^= 0x01;
        CHECK(!st.load(0, out, &si, &err));
        CHECK(err.find("corrupt") != std::string::npos);
        CHECK(out.empty());
    }
    // wear: alternating halves, one erase per 4 KiB per save
    {
        MemFlash f(2 * 2 * 32 * 1024);
        ProjectStore st(f, 2);
        std::string err;
        for (int i = 0; i < 10; i++)
            st.save(0, "w", noise(10000, 6), 0, &err);
        CHECK(f.eraseCount == 10 * 3);  // 64 + 10000 bytes (and deflate's few bytes) -> 3 sectors per save
    }
    // PCM: sign, left-justified 24-bit in 32-bit slots, L before R, clipping, dither bounds
    {
        CHECK(floatToS24(0.0f) == 0);
        CHECK(floatToS24(1.0f) == 8388607);
        CHECK(floatToS24(-1.0f) == -8388608);
        CHECK(floatToS24(2.0f) == 8388607);
        CHECK(floatToS24(0.5f) == 4194304);
        CHECK(floatToS24(NAN) == 0);
        float L[3] = {0.5f, -0.5f, 1.5f}, R[3] = {-0.25f, 0.0f, -3.0f};
        int32_t out[6];
        Dither d;
        toI2S32(L, R, out, 3, d, false);
        CHECK(out[0] == (int32_t)(4194304u << 8));
        CHECK(out[1] == (int32_t)((uint32_t)-2097152 << 8));
        CHECK(out[1] < 0);  // the sign is the slot's MSB
        CHECK(out[2] == (int32_t)((uint32_t)-4194304 << 8));
        CHECK(out[4] == (int32_t)0x7FFFFF00);
        CHECK(out[5] == (int32_t)0x80000000);
        for (int i = 0; i < 6; i++)
            CHECK((out[i] & 0xFF) == 0);  // low byte of the slot unused
        // dither: at most +-1 LSB, mean ~0
        float z[1024] = {}, q[1024];
        for (int i = 0; i < 1024; i++)
            q[i] = 0.25f;
        int32_t o[2048];
        toI2S32(q, z, o, 1024, d, true);
        long sum = 0;
        for (int i = 0; i < 1024; i++) {
            int32_t v = o[2 * i] >> 8;
            CHECK(v >= 2097152 - 1 && v <= 2097152 + 1);
            int32_t w = o[2 * i + 1] >> 8;
            CHECK(w >= -1 && w <= 1);
            sum += w;
        }
        CHECK(labs(sum) < 100);
    }
    TEST_MAIN_END("test_storage");
}
