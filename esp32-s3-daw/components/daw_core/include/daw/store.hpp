// SPDX-License-Identifier: GPL-3.0-or-later
// Transaction-safe project slots on raw NOR flash (ESP32-S3 partition, or a file / RAM on Linux).
//
// Each slot has two halves. A save erases the half holding the OLDER copy, writes the payload,
// then the 64-byte header (magic, sequence number, length, CRC32 of payload and header) LAST.
// A save interrupted at any point leaves an invalid header in that half, and load() returns the
// other half: the previous version. Slot 0 is the autosave.
// The payload is the project document deflated (header version 2); version 1 halves (the document
// as it is, written by firmware before 2026-10-13) still load.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace daw {

class FlashDevice {
public:
    static constexpr uint32_t kSector = 4096;
    virtual ~FlashDevice() = default;
    virtual uint32_t size() const = 0;
    virtual bool read(uint32_t off, void *dst, uint32_t len) = 0;
    virtual bool write(uint32_t off, const void *src, uint32_t len) = 0;  // NOR: can only clear bits
    virtual bool erase(uint32_t off, uint32_t len) = 0;                    // sector aligned -> 0xFF
};

// RAM flash with NOR semantics and power-loss injection (tests, host default)
class MemFlash : public FlashDevice {
public:
    explicit MemFlash(uint32_t size) : data_(size, 0xFF) {}
    uint32_t size() const override { return (uint32_t)data_.size(); }
    bool read(uint32_t off, void *dst, uint32_t len) override;
    bool write(uint32_t off, const void *src, uint32_t len) override;
    bool erase(uint32_t off, uint32_t len) override;
    // after this many more bytes written (or sectors erased) every operation fails ("power cut")
    void failAfter(int64_t bytes) { budget_ = bytes; }
    std::vector<uint8_t> &data() { return data_; }
    uint32_t eraseCount = 0;

private:
    bool spend(uint32_t n);
    std::vector<uint8_t> data_;
    int64_t budget_ = -1;
};

struct SlotInfo {
    bool valid = false;
    std::string name;
    uint32_t seq = 0;
    uint32_t bytes = 0;   // the project document
    uint32_t stored = 0;  // in flash (deflated)
    uint32_t time = 0;    // seconds (device clock / unix time on Linux)
};

class ProjectStore {
public:
    static constexpr uint32_t kHeader = 64;
    static constexpr uint32_t kMaxDocument = 2u << 20;  // the largest document a slot takes, before deflating
    ProjectStore(FlashDevice &dev, uint32_t slots);
    uint32_t slots() const { return slots_; }
    uint32_t maxPayload() const { return half_ - kHeader; }  // deflated bytes
    bool save(uint32_t slot, const std::string &name, const std::string &payload, uint32_t time, std::string *err);
    bool load(uint32_t slot, std::string &payload, SlotInfo *info, std::string *err);
    SlotInfo info(uint32_t slot);
    bool remove(uint32_t slot);
    struct Header;

private:
    bool readHeader(uint32_t off, Header &h);
    bool readHalf(uint32_t off, const Header &h, std::string &payload);
    FlashDevice &dev_;
    uint32_t slots_, half_;
};

// float -> I2S: interleaved 32-bit slots (left first), signed 24-bit PCM left-justified (bits 31..8,
// bits 7..0 zero), clipped, with optional TPDF dither of +-1 LSB at 24 bits
struct Dither {
    uint32_t s = 0x12345678u;
};
int32_t floatToS24(float x);
void toI2S32(const float *L, const float *R, int32_t *out, uint32_t frames, Dither &d, bool dither);

}  // namespace daw
