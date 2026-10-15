// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/store.hpp"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <algorithm>

#include "daw/model.hpp"
#include "daw/platform.hpp"

namespace daw {

// ---- MemFlash -----------------------------------------------------------------------------

bool MemFlash::spend(uint32_t n)
{
    if (budget_ < 0)
        return true;
    if (budget_ < (int64_t)n) {
        budget_ = 0;
        return false;
    }
    budget_ -= n;
    return true;
}

bool MemFlash::read(uint32_t off, void *dst, uint32_t len)
{
    if ((uint64_t)off + len > data_.size())
        return false;
    memcpy(dst, data_.data() + off, len);
    return true;
}

bool MemFlash::write(uint32_t off, const void *src, uint32_t len)
{
    if ((uint64_t)off + len > data_.size())
        return false;
    const uint8_t *s = static_cast<const uint8_t *>(src);
    for (uint32_t i = 0; i < len; i++) {
        if (!spend(1))
            return false;  // power cut mid-write: the bytes before stay written
        data_[off + i] &= s[i];
    }
    return true;
}

bool MemFlash::erase(uint32_t off, uint32_t len)
{
    if (off % kSector || len % kSector || (uint64_t)off + len > data_.size())
        return false;
    for (uint32_t a = off; a < off + len; a += kSector) {
        if (!spend(64))  // an erase costs "64 bytes" of the fault budget
            return false;
        memset(data_.data() + a, 0xFF, kSector);
        eraseCount++;
    }
    return true;
}

// ---- ProjectStore -------------------------------------------------------------------------

// Version 1: the document as it is. Version 2: the document deflated (platform::deflate), its
// length before deflating in `raw`. Both 4-byte aligned, CRC32 of all bytes before `hcrc` last.
struct HeaderV1 {
    uint32_t magic;    // 'DAWP'
    uint16_t version;  // 1
    uint16_t size;     // sizeof(HeaderV1)
    uint32_t seq;
    uint32_t len;
    uint32_t crc;      // payload
    uint32_t time;
    char name[32];
    uint32_t hcrc;     // all bytes above
};
struct HeaderV2 {
    uint32_t magic;    // 'DAWP'
    uint16_t version;  // 2
    uint16_t size;     // sizeof(HeaderV2)
    uint32_t seq;
    uint32_t len;      // stored (deflated) bytes
    uint32_t crc;      // of the stored bytes
    uint32_t time;
    char name[32];
    uint32_t raw;      // the document's length
    uint32_t hcrc;     // all bytes above
};
static_assert(sizeof(HeaderV1) <= ProjectStore::kHeader && sizeof(HeaderV2) <= ProjectStore::kHeader, "headers fit");
static_assert(offsetof(HeaderV1, name) == offsetof(HeaderV2, name), "common prefix");
static constexpr uint32_t kMagic = 0x50574144u;  // "DAWP" little endian

// either version, as read
struct ProjectStore::Header {
    uint32_t seq, len, crc, time, raw;
    bool deflated;
    char name[32];
};

ProjectStore::ProjectStore(FlashDevice &dev, uint32_t slots) : dev_(dev), slots_(slots ? slots : 1)
{
    half_ = (dev.size() / slots_ / 2) / FlashDevice::kSector * FlashDevice::kSector;
}

bool ProjectStore::readHeader(uint32_t off, Header &h)
{
    HeaderV2 b;  // the larger one; a version 1 header ends before `raw`
    if (!dev_.read(off, &b, sizeof b) || b.magic != kMagic)
        return false;
    if (b.version == 1 && b.size == sizeof(HeaderV1)) {
        HeaderV1 v1;
        memcpy(&v1, &b, sizeof v1);
        if (crc32(&v1, offsetof(HeaderV1, hcrc)) != v1.hcrc)
            return false;
        h.raw = v1.len;
        h.deflated = false;
    } else if (b.version == 2 && b.size == sizeof(HeaderV2)) {
        if (crc32(&b, offsetof(HeaderV2, hcrc)) != b.hcrc || b.raw > kMaxDocument)
            return false;
        h.raw = b.raw;
        h.deflated = true;
    } else {
        return false;
    }
    if (b.len > half_ - kHeader)
        return false;
    h.seq = b.seq;
    h.len = b.len;
    h.crc = b.crc;
    h.time = b.time;
    memcpy(h.name, b.name, sizeof h.name);
    return true;
}

// a half's document: its stored bytes, checked against the CRC, inflated when deflated
bool ProjectStore::readHalf(uint32_t off, const Header &h, std::string &payload)
{
    std::string stored(h.len, '\0');
    if (!dev_.read(off + kHeader, stored.data(), h.len) || crc32(stored.data(), stored.size()) != h.crc)
        return false;
    if (!h.deflated) {
        payload.swap(stored);
        return true;
    }
    return platform::inflate(stored.data(), stored.size(), h.raw, payload);
}

SlotInfo ProjectStore::info(uint32_t slot)
{
    SlotInfo best;
    if (slot >= slots_)
        return best;
    for (int k = 0; k < 2; k++) {
        Header h;
        const uint32_t off = (slot * 2 + (uint32_t)k) * half_;
        if (!readHeader(off, h))
            continue;
        if (!best.valid || h.seq > best.seq) {
            best.valid = true;
            best.seq = h.seq;
            best.bytes = h.raw;
            best.stored = h.len;
            best.time = h.time;
            best.name.assign(h.name, strnlen(h.name, sizeof h.name));
        }
    }
    return best;
}

bool ProjectStore::load(uint32_t slot, std::string &payload, SlotInfo *info, std::string *err)
{
    if (slot >= slots_) {
        if (err)
            *err = "no such slot";
        return false;
    }
    // newest valid half first; a half whose payload CRC fails falls back to the other one
    Header hs[2];
    bool ok[2];
    for (int k = 0; k < 2; k++)
        ok[k] = readHeader((slot * 2 + (uint32_t)k) * half_, hs[k]);
    int order[2] = {0, 1};
    if (ok[1] && (!ok[0] || hs[1].seq > hs[0].seq)) {
        order[0] = 1;
        order[1] = 0;
    }
    for (int k : order) {
        if (!ok[k] || !readHalf((slot * 2 + (uint32_t)k) * half_, hs[k], payload))
            continue;
        if (info) {
            info->valid = true;
            info->seq = hs[k].seq;
            info->bytes = hs[k].raw;
            info->stored = hs[k].len;
            info->time = hs[k].time;
            info->name.assign(hs[k].name, strnlen(hs[k].name, sizeof hs[k].name));
        }
        return true;
    }
    if (err)
        *err = ok[0] || ok[1] ? "stored project is corrupt (CRC)" : "empty slot";
    payload.clear();
    return false;
}

bool ProjectStore::save(uint32_t slot, const std::string &name, const std::string &payload, uint32_t time, std::string *err)
{
    if (slot >= slots_) {
        if (err)
            *err = "no such slot";
        return false;
    }
    if (payload.size() > kMaxDocument) {
        if (err)
            *err = "project too large (" + std::to_string(payload.size() / 1024) + " KB; at most " + std::to_string(kMaxDocument / 1024) + " KB)";
        return false;
    }
    std::string packed;
    if (!platform::deflate(payload.data(), payload.size(), packed)) {
        if (err)
            *err = "out of memory compressing the project";
        return false;
    }
    if (packed.size() > maxPayload()) {
        if (err)
            *err = "project too large for a slot (" + std::to_string(payload.size() / 1024) + " KB, " + std::to_string(packed.size() / 1024) +
                   " KB compressed; a slot holds " + std::to_string(maxPayload() / 1024) + " KB)";
        return false;
    }
    Header h0, h1;
    const bool v0 = readHeader(slot * 2 * half_, h0), v1 = readHeader((slot * 2 + 1) * half_, h1);
    // write into the half that is invalid, or holds the older copy
    uint32_t k = !v0 ? 0 : (!v1 ? 1 : (h0.seq <= h1.seq ? 0 : 1));
    const uint32_t seq = std::max(v0 ? h0.seq : 0u, v1 ? h1.seq : 0u) + 1;
    const uint32_t off = (slot * 2 + k) * half_;
    const uint32_t eraseLen = (kHeader + (uint32_t)packed.size() + FlashDevice::kSector - 1) / FlashDevice::kSector * FlashDevice::kSector;
    if (!dev_.erase(off, std::min(eraseLen, half_)) || !dev_.write(off + kHeader, packed.data(), (uint32_t)packed.size())) {
        if (err)
            *err = "flash write failed";
        return false;
    }
    HeaderV2 h{};
    h.magic = kMagic;
    h.version = 2;
    h.size = sizeof(HeaderV2);
    h.seq = seq;
    h.len = (uint32_t)packed.size();
    h.crc = crc32(packed.data(), packed.size());
    h.time = time;
    memcpy(h.name, name.c_str(), std::min(name.size(), sizeof h.name));
    h.raw = (uint32_t)payload.size();
    h.hcrc = crc32(&h, offsetof(HeaderV2, hcrc));
    if (!dev_.write(off, &h, sizeof h)) {
        if (err)
            *err = "flash write failed (header)";
        return false;
    }
    // read back
    std::string check;
    SlotInfo si;
    if (!load(slot, check, &si, nullptr) || si.seq != seq || check != payload) {
        if (err)
            *err = "verification after write failed";
        return false;
    }
    return true;
}

bool ProjectStore::remove(uint32_t slot)
{
    if (slot >= slots_)
        return false;
    // invalidating the headers is enough; erase the first sector of each half
    return dev_.erase(slot * 2 * half_, FlashDevice::kSector) && dev_.erase((slot * 2 + 1) * half_, FlashDevice::kSector);
}

// ---- PCM ----------------------------------------------------------------------------------

int32_t floatToS24(float x)
{
    if (!(x == x))
        return 0;
    const float s = x * 8388608.0f;
    if (s >= 8388607.0f)
        return 8388607;
    if (s <= -8388608.0f)
        return -8388608;
    return (int32_t)lrintf(s);
}

void toI2S32(const float *L, const float *R, int32_t *out, uint32_t frames, Dither &d, bool dither)
{
    for (uint32_t i = 0; i < frames; i++) {
        float l = L[i], r = R[i];
        if (dither) {  // TPDF: sum of two uniform values in [-0.5, 0.5) LSB
            uint32_t a, b;
            d.s ^= d.s << 13; d.s ^= d.s >> 17; d.s ^= d.s << 5; a = d.s;
            d.s ^= d.s << 13; d.s ^= d.s >> 17; d.s ^= d.s << 5; b = d.s;
            const float lsb = 1.0f / 8388608.0f;
            l += ((float)(a >> 8) - (float)(b >> 8)) * (lsb / 16777216.0f);
            d.s ^= d.s << 13; d.s ^= d.s >> 17; d.s ^= d.s << 5; a = d.s;
            d.s ^= d.s << 13; d.s ^= d.s >> 17; d.s ^= d.s << 5; b = d.s;
            r += ((float)(a >> 8) - (float)(b >> 8)) * (lsb / 16777216.0f);
        }
        // shift as unsigned: left-justified 24-bit sample in a 32-bit slot
        out[2 * i] = (int32_t)((uint32_t)floatToS24(l) << 8);
        out[2 * i + 1] = (int32_t)((uint32_t)floatToS24(r) << 8);
    }
}

}  // namespace daw
