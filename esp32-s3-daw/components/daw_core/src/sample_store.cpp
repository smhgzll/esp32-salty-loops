// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/sample_store.hpp"

#include <string.h>

#include <algorithm>

#include "daw/model.hpp"  // crc32
#include "daw/platform.hpp"

namespace daw {

struct SampleStore::Header {
    uint32_t magic;  // 'DAWS'
    uint16_t version;
    uint16_t type;   // SampleType
    uint32_t seq;
    uint32_t size;
    uint32_t crc;    // of the file's bytes
    char name[kMaxName + 1];
    uint32_t hcrc;   // of all bytes above
};
static_assert(sizeof(SampleStore::Header) <= SampleStore::kHeader, "the header fits");
static constexpr uint32_t kMagic = 0x53574144u;  // "DAWS" little endian

SampleType sniffSample(const uint8_t *d, size_t n)
{
    if (n >= 12 && !memcmp(d, "RIFF", 4)) {
        if (!memcmp(d + 8, "WAVE", 4))
            return SampleType::Wav;
        if (!memcmp(d + 8, "sfbk", 4))
            return SampleType::SoundFont;
    }
    if ((n >= 4 && !memcmp(d, "IMPM", 4)) || (n >= 17 && !memcmp(d, "Extended Module: ", 17)) ||
        (n >= 48 && !memcmp(d + 44, "SCRM", 4)))
        return SampleType::Module;
    if (n >= 1084) {  // the MOD signatures libxmp-lite accepts
        const uint8_t *m = d + 1080;
        auto digit = [](uint8_t c) { return c >= '0' && c <= '9'; };
        if (!memcmp(m, "M.K.", 4) || (digit(m[0]) && m[0] != '0' && !memcmp(m + 1, "CHN", 3)))
            return SampleType::Module;
        if (digit(m[0]) && digit(m[1]) && !memcmp(m + 2, "CH", 2)) {
            const int ch = (m[0] - '0') * 10 + (m[1] - '0');
            if (ch > 0 && ch <= 32)
                return SampleType::Module;
        }
    }
    return SampleType::Unknown;
}

const char *sampleTypeName(SampleType t)
{
    switch (t) {
    case SampleType::Wav: return "wav";
    case SampleType::SoundFont: return "sf2";
    case SampleType::Module: return "module";
    default: return "?";
    }
}

std::string sanitizeSampleName(const std::string &in)
{
    std::string out;
    for (char ch : in) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '.' ||
                        ch == '_' || ch == '-' || ch == '(' || ch == ')' || ch == '+';
        out += ok ? ch : '_';
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.'))  // no hidden / blank names
        out.erase(out.begin());
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    if (out.size() > SampleStore::kMaxName) {  // keep the extension
        const size_t dot = out.rfind('.');
        const std::string ext = dot != std::string::npos && out.size() - dot <= 6 ? out.substr(dot) : "";
        out = out.substr(0, SampleStore::kMaxName - ext.size()) + ext;
    }
    return out;
}

SampleStore::SampleStore(FlashDevice &dev) : dev_(dev) {}

bool SampleStore::readHeader(uint32_t sector, Header &h)
{
    uint8_t buf[kHeader];
    if (!dev_.read(sector * FlashDevice::kSector, buf, kHeader))
        return false;
    memcpy(&h, buf, sizeof h);
    if (h.magic != kMagic || h.version != 1 || h.size == 0)
        return false;
    if (crc32(&h, offsetof(Header, hcrc)) != h.hcrc)
        return false;
    if (h.name[kMaxName] != 0 || !h.name[0])
        return false;
    const uint64_t end = (uint64_t)sector * FlashDevice::kSector + kHeader + h.size;
    return end <= dev_.size();
}

void SampleStore::mount()
{
    const uint32_t sectors = dev_.size() / FlashDevice::kSector;
    used_.assign(sectors, false);
    files_.clear();
    seq_ = 0;
    for (uint32_t s = 0; s < sectors;) {
        Header h;
        if (!readHeader(s, h)) {
            s++;
            continue;
        }
        SampleInfo f;
        f.name = h.name;
        f.size = h.size;
        f.type = (SampleType)h.type;
        f.seq = h.seq;
        f.sector = s;
        f.crc = h.crc;
        const uint32_t n = sectorsFor(h.size);
        for (uint32_t k = 0; k < n && s + k < sectors; k++)
            used_[s + k] = true;
        files_.push_back(f);
        seq_ = std::max(seq_, h.seq);
        s += n;
    }
    // an interrupted replace: two files with one name; the newer one wins
    std::sort(files_.begin(), files_.end(), [](const SampleInfo &a, const SampleInfo &b) { return a.name != b.name ? a.name < b.name : a.seq > b.seq; });
    for (size_t i = 1; i < files_.size();) {
        if (files_[i].name == files_[i - 1].name) {
            erase(files_[i].sector);
            for (uint32_t k = 0; k < sectorsFor(files_[i].size); k++)
                used_[files_[i].sector + k] = false;
            files_.erase(files_.begin() + (long)i);
        } else
            i++;
    }
}

const SampleInfo *SampleStore::find(const std::string &name) const
{
    for (const auto &f : files_)
        if (f.name == name)
            return &f;
    return nullptr;
}

int SampleStore::findRun(uint32_t n) const
{
    uint32_t run = 0;
    for (uint32_t s = 0; s < used_.size(); s++) {
        run = used_[s] ? 0 : run + 1;
        if (run == n)
            return (int)(s + 1 - n);
    }
    return -1;
}

uint32_t SampleStore::freeBytes() const
{
    return (uint32_t)std::count(used_.begin(), used_.end(), false) * FlashDevice::kSector;
}

uint32_t SampleStore::largestFree() const
{
    uint32_t run = 0, best = 0;
    for (bool u : used_) {
        run = u ? 0 : run + 1;
        best = std::max(best, run);
    }
    return best ? best * FlashDevice::kSector - kHeader : 0;
}

bool SampleStore::put(const std::string &rawName, const uint8_t *data, uint32_t size, std::string *err)
{
    const SampleType type = sniffSample(data, size);
    if (type == SampleType::Unknown) {
        if (err)
            *err = "not a WAV, SoundFont (sf2) or tracker module (mod, xm, s3m, it) file";
        return false;
    }
    return putRaw(rawName, type, data, size, err);
}

bool SampleStore::putRaw(const std::string &rawName, SampleType type, const uint8_t *data, uint32_t size, std::string *err)
{
    auto fail = [&](const std::string &m) {
        if (err)
            *err = m;
        return false;
    };
    const std::string name = sanitizeSampleName(rawName);
    if (name.empty())
        return fail("invalid file name");
    if (size == 0)
        return fail("empty file");
    const uint32_t n = sectorsFor(size);
    const int at = findRun(n);
    if (at < 0)
        return fail("not enough free space in the sample library (" + std::to_string(size / 1024) + " KB needed, " +
                    std::to_string(largestFree() / 1024) + " KB free in one piece)");
    const uint32_t off = (uint32_t)at * FlashDevice::kSector;
    if (!dev_.erase(off, n * FlashDevice::kSector))
        return fail("flash erase failed");
    if (!dev_.write(off + kHeader, data, size))
        return fail("flash write failed");
    return commit(name, type, (uint32_t)at, n, size, crc32(data, size), err);
}

// the header last: the file now exists; then the previous file of that name goes
bool SampleStore::commit(const std::string &name, SampleType type, uint32_t at, uint32_t n, uint32_t size, uint32_t crc, std::string *err)
{
    Header h{};
    h.magic = kMagic;
    h.version = 1;
    h.type = (uint16_t)type;
    h.seq = ++seq_;
    h.size = size;
    h.crc = crc;
    memcpy(h.name, name.c_str(), name.size() + 1);
    h.hcrc = crc32(&h, offsetof(Header, hcrc));
    uint8_t hb[kHeader];
    memset(hb, 0xFF, sizeof hb);
    memcpy(hb, &h, sizeof h);
    if (!dev_.write(at * FlashDevice::kSector, hb, kHeader)) {
        if (err)
            *err = "flash write failed";
        return false;
    }
    for (uint32_t k = 0; k < n; k++)
        used_[at + k] = true;
    for (size_t i = 0; i < files_.size(); i++)
        if (files_[i].name == name) {
            erase(files_[i].sector);
            for (uint32_t k = 0; k < sectorsFor(files_[i].size); k++)
                used_[files_[i].sector + k] = false;
            files_.erase(files_.begin() + (long)i);
            break;
        }
    files_.push_back({name, size, type, h.seq, at, crc});
    std::sort(files_.begin(), files_.end(), [](const SampleInfo &a, const SampleInfo &b) { return a.name < b.name; });
    return true;
}

bool SampleStore::beginPut(const std::string &rawName, uint32_t size, const uint8_t *head, uint32_t headLen, std::string *err)
{
    abortPut();
    auto fail = [&](const std::string &m) {
        if (err)
            *err = m;
        return false;
    };
    const SampleType type = sniffSample(head, headLen);
    if (type == SampleType::Unknown)
        return fail("not a WAV, SoundFont (sf2) or tracker module (mod, xm, s3m, it) file");
    const std::string name = sanitizeSampleName(rawName);
    if (name.empty())
        return fail("invalid file name");
    if (size == 0 || headLen > size)
        return fail("empty file");
    const uint32_t n = sectorsFor(size);
    const int at = findRun(n);
    if (at < 0)
        return fail("not enough free space in the sample library (" + std::to_string(size / 1024) + " KB needed, " +
                    std::to_string(largestFree() / 1024) + " KB free in one piece)");
    for (uint32_t k = 0; k < n; k++)
        used_[(uint32_t)at + k] = true;  // reserved until finishPut / abortPut
    pending_ = Pending{true, name, type, (uint32_t)at, n, size, 0, 0, 0};
    return appendPut(head, headLen, err);
}

bool SampleStore::appendPut(const uint8_t *data, uint32_t len, std::string *err)
{
    auto fail = [&](const std::string &m) {
        abortPut();
        if (err)
            *err = m;
        return false;
    };
    if (!pending_.active)
        return fail("no upload in progress");
    if (len > pending_.size - pending_.written)
        return fail("more bytes than announced");
    const uint32_t off = pending_.sector * FlashDevice::kSector;
    const uint32_t end = kHeader + pending_.written + len;  // within the run
    if (end > pending_.erased) {
        // ahead to the next 64 KB boundary (one block erase instead of 16 sector erases)
        constexpr uint32_t kBlock = 64 * 1024;
        const uint32_t to = std::min(pending_.sectors * FlashDevice::kSector, (off + end + kBlock - 1) / kBlock * kBlock - off);
        if (!dev_.erase(off + pending_.erased, to - pending_.erased))
            return fail("flash erase failed");
        pending_.erased = to;
    }
    if (len && !dev_.write(off + kHeader + pending_.written, data, len))
        return fail("flash write failed");
    pending_.crc = crc32(data, len, pending_.crc);
    pending_.written += len;
    return true;
}

bool SampleStore::finishPut(std::string *name, std::string *err)
{
    if (!pending_.active || pending_.written != pending_.size) {
        abortPut();
        if (err)
            *err = "the upload ended early";
        return false;
    }
    const Pending p = pending_;
    pending_.active = false;
    for (uint32_t k = 0; k < p.sectors; k++)
        used_[p.sector + k] = false;  // commit marks them again
    if (name)
        *name = p.name;
    return commit(p.name, p.type, p.sector, p.sectors, p.size, p.crc, err);
}

void SampleStore::abortPut()
{
    if (!pending_.active)
        return;
    for (uint32_t k = 0; k < pending_.sectors; k++)
        used_[pending_.sector + k] = false;
    pending_.active = false;
}

bool SampleStore::read(const std::string &name, std::vector<uint8_t> &out, std::string *err)
{
    const SampleInfo *f = find(name);
    if (!f) {
        if (err)
            *err = "no sample " + name;
        return false;
    }
    Header h;
    if (!readHeader(f->sector, h)) {
        if (err)
            *err = "sample header unreadable";
        return false;
    }
    const auto mi = platform::memInfo();
    if (h.size > out.capacity() && mi.psramTotal && h.size > mi.psramLargest) {
        if (err)
            *err = "not enough contiguous PSRAM to read sample " + name;
        return false;
    }
    out.resize(h.size);
    if (!readInto(name, out.data(), out.size(), err)) {
        out.clear();
        return false;
    }
    return true;
}

bool SampleStore::readInto(const std::string &name, uint8_t *out, size_t capacity, std::string *err)
{
    const SampleInfo *f = find(name);
    Header h;
    if (!f || !readHeader(f->sector, h)) {
        if (err)
            *err = "sample " + name + " unavailable";
        return false;
    }
    if (!out || capacity < h.size) {
        if (err)
            *err = "sample read buffer is too small";
        return false;
    }
    if (!dev_.read(f->sector * FlashDevice::kSector + kHeader, out, h.size) || crc32(out, h.size) != h.crc) {
        if (err)
            *err = "sample " + name + " is damaged (CRC)";
        return false;
    }
    return true;
}

bool SampleStore::verify(const std::string &name, std::string *err)
{
    const SampleInfo *f = find(name); Header h;
    if (!f || !readHeader(f->sector, h)) { if (err) *err = "sample unavailable"; return false; }
    uint8_t buf[4096]; uint32_t crc = 0;
    for (uint32_t off = 0; off < h.size;) {
        const uint32_t n = std::min<uint32_t>(sizeof buf, h.size-off);
        if (!dev_.read(f->sector*FlashDevice::kSector+kHeader+off, buf, n)) { if (err) *err = "sample read failed"; return false; }
        crc = crc32(buf, n, crc); off += n;
    }
    if (crc != h.crc) { if (err) *err = "sample " + name + " is damaged (CRC)"; return false; }
    return true;
}

bool SampleStore::readRange(const std::string &name, uint32_t offset, void *buf, uint32_t len)
{
    const SampleInfo *f = find(name);
    if (!f || offset > f->size || len > f->size - offset)
        return false;
    return dev_.read(f->sector * FlashDevice::kSector + kHeader + offset, buf, len);
}

bool SampleStore::erase(uint32_t sector)
{
    const uint32_t zero = 0;  // NOR: clearing bits needs no erase
    return dev_.write(sector * FlashDevice::kSector, &zero, sizeof zero);
}

bool SampleStore::remove(const std::string &name)
{
    for (size_t i = 0; i < files_.size(); i++)
        if (files_[i].name == name) {
            if (!erase(files_[i].sector))
                return false;
            for (uint32_t k = 0; k < sectorsFor(files_[i].size); k++)
                used_[files_[i].sector + k] = false;
            files_.erase(files_.begin() + (long)i);
            return true;
        }
    return false;
}

}  // namespace daw
