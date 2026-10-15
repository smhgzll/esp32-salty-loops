// SPDX-License-Identifier: GPL-3.0-or-later
// Audio output of the firmware: I2S master TX to the PCM5102A (hardware), or a null backend paced
// by the system clock (QEMU, or no pins configured). The engine renders float blocks; write()
// converts them (24-bit left-justified in 32-bit slots, optional TPDF dither) and blocks until
// the DMA has room, which paces the audio task.
#pragma once

#include <stdint.h>

#include <atomic>

namespace daw {

struct AudioOutConfig {
    uint32_t sampleRate = 44100;
    int bclk = 16, ws = 6, dout = 7;
    uint32_t dmaDesc = 4, dmaFrames = 256;
    bool i2s = true;    // false: null backend
    bool dither = true;
};

struct AudioOutStats {
    std::atomic<uint32_t> underruns{0};   // the DMA ran dry and sent silence (auto-clear)
    std::atomic<uint32_t> writes{0};
    std::atomic<uint32_t> writeUsMax{0};
    std::atomic<uint32_t> late{0};        // null backend: blocks behind the clock
    std::atomic<uint64_t> frames{0};
};

enum class AudioKind : uint8_t { None, I2S, Null };

bool audioOutInit(const AudioOutConfig &cfg);
AudioKind audioOutKind();
void audioOutWrite(const float *L, const float *R, uint32_t frames);
AudioOutStats &audioOutStats();
// FNV-1a over the 24-bit samples written so far (deterministic render checks, QEMU self-test)
uint64_t audioOutHash();
void audioOutHashReset();

}  // namespace daw
