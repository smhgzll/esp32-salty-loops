// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library on raw NOR flash (the "samples" partition: 6 MB, 22 MB on 32 MB boards; a
// file or RAM on Linux).
//
// A file occupies a run of whole 4 KB sectors: a 64-byte header, then its bytes. A save erases the
// run, writes the bytes, then the header LAST, so a power cut leaves either nothing (the run reads as
// free space) or the complete file. Deleting clears the header's magic (one small write). Replacing
// a name writes the new file first and deletes the old one afterwards; if power fails in between,
// the scan keeps the copy with the higher sequence number and deletes the other.
// Free space is the sectors no valid header covers; files are placed first-fit (no compaction:
// largestFree() can be smaller than freeBytes() after many deletes).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "daw/store.hpp"

namespace daw {

enum class SampleType : uint8_t { Unknown = 0, Wav = 1, SoundFont = 2, Preset = 3, Module = 4 };

struct SampleInfo {
    std::string name;
    uint32_t size = 0;
    SampleType type = SampleType::Unknown;
    uint32_t seq = 0;
    uint32_t sector = 0;  // first sector of the run
    uint32_t crc = 0;
};

// what the bytes are (RIFF WAVE / RIFF sfbk / a tracker module the Tracker plugin's libxmp-lite
// reads: IT "IMPM", XM "Extended Module: ", S3M "SCRM" at 44, MOD "M.K." / "xCHN" / "xxCH" at
// 1080), or Unknown. A MOD needs its first 1084 bytes to be recognised.
SampleType sniffSample(const uint8_t *data, size_t size);
const char *sampleTypeName(SampleType t);  // "wav", "sf2", "module", "?"
// a usable stored name: letters, digits, space . _ - ( ) +, at most 39 bytes; "" if nothing is left
std::string sanitizeSampleName(const std::string &name);

class SampleStore {
public:
    static constexpr uint32_t kHeader = 64;
    static constexpr uint32_t kMaxName = 39;
    explicit SampleStore(FlashDevice &dev);

    void mount();  // scan the flash (and finish an interrupted replace)
    const std::vector<SampleInfo> &list() const { return files_; }
    const SampleInfo *find(const std::string &name) const;
    bool put(const std::string &name, const uint8_t *data, uint32_t size, std::string *err);  // WAV / SF2 / module only
    bool putRaw(const std::string &name, SampleType type, const uint8_t *data, uint32_t size, std::string *err);
    // A file written in pieces (an upload larger than free RAM): beginPut with its size and first
    // bytes (at least 12: its type), appendPut the rest in order, finishPut writes the header last.
    // Until then the run is reserved and no file exists (a power cut leaves free space); abortPut,
    // or another beginPut, gives the run back. Sectors are erased as the bytes reach them.
    bool beginPut(const std::string &name, uint32_t size, const uint8_t *head, uint32_t headLen, std::string *err);
    bool appendPut(const uint8_t *data, uint32_t len, std::string *err);
    bool finishPut(std::string *name, std::string *err);
    void abortPut();
    bool putting() const { return pending_.active; }
    bool read(const std::string &name, std::vector<uint8_t> &out, std::string *err);  // checks the CRC
    bool readInto(const std::string &name, uint8_t *out, size_t capacity, std::string *err);  // caller-owned buffer, CRC checked
    bool verify(const std::string &name, std::string *err);  // streamed CRC, no whole-file allocation
    // part of a file (no CRC check: for parsing headers of large files without reading them whole)
    bool readRange(const std::string &name, uint32_t offset, void *buf, uint32_t len);
    bool remove(const std::string &name);
    uint32_t totalBytes() const { return dev_.size(); }
    uint32_t freeBytes() const;
    uint32_t largestFree() const;  // the biggest file that fits right now (header included)

    struct Header;

private:
    bool readHeader(uint32_t sector, Header &h);
    bool commit(const std::string &name, SampleType type, uint32_t sector, uint32_t sectors, uint32_t size, uint32_t crc, std::string *err);
    struct Pending {
        bool active = false;
        std::string name;
        SampleType type = SampleType::Unknown;
        uint32_t sector = 0, sectors = 0, size = 0, written = 0, crc = 0, erased = 0;  // erased: bytes of the run
    } pending_;
    bool erase(uint32_t sector);  // clear the magic of the header at this sector
    int findRun(uint32_t sectors) const;
    uint32_t sectorsFor(uint32_t size) const { return (kHeader + size + FlashDevice::kSector - 1) / FlashDevice::kSector; }
    std::vector<bool> used_;  // per sector
    FlashDevice &dev_;
    std::vector<SampleInfo> files_;
    uint32_t seq_ = 0;
};

}  // namespace daw
