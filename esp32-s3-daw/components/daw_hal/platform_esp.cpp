// SPDX-License-Identifier: GPL-3.0-or-later
// daw::platform on the ESP32-S3 (deflate: the ROM's miniz), plus the C++ heap policy of the firmware: every `new` (the project
// document, std containers, protocol strings) goes to PSRAM first, so internal SRAM stays for the
// audio engine (platform::alloc(Fast)), DMA, Wi-Fi and the stacks.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <new>

#include "cJSON.h"
#include "daw/platform.hpp"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "miniz.h"  // the ROM's tdefl / tinfl (esp_rom)
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

namespace daw::platform {

void *alloc(size_t size, size_t align, Mem cls)
{
    if (align < 4)
        align = 4;
    const uint32_t caps = cls == Mem::Fast ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *p = heap_caps_aligned_alloc(align, size ? size : 1, caps);
    if (!p && cls == Mem::Large)  // no PSRAM (or full): internal as a last resort
        p = heap_caps_aligned_alloc(align, size ? size : 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

void free(void *p) { heap_caps_free(p); }

uint64_t micros() { return (uint64_t)esp_timer_get_time(); }
uint32_t cycles() { return (uint32_t)esp_cpu_get_cycle_count(); }
uint32_t cyclesPerMicro() { return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ; }

void log(Log level, const char *tag, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    switch (level) {
    case Log::Error: ESP_LOGE(tag, "%s", msg); break;
    case Log::Warn: ESP_LOGW(tag, "%s", msg); break;
    case Log::Info: ESP_LOGI(tag, "%s", msg); break;
    default: ESP_LOGD(tag, "%s", msg); break;
    }
}

MemInfo memInfo()
{
    MemInfo m;
    m.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    m.internalMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    m.internalTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    m.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m.psramMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    m.psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    m.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return m;
}

void randomBytes(void *dst, size_t n) { esp_fill_random(dst, n); }

// Project slots: raw DEFLATE with the ROM's miniz. The compressor (about 160 KB) and the
// decompressor (11 KB, too large for a task stack) live in PSRAM only while they work.
static mz_bool appendOut(const void *buf, int len, void *user)
{
    static_cast<std::string *>(user)->append(static_cast<const char *>(buf), (size_t)len);
    return MZ_TRUE;
}

bool deflate(const void *in, size_t len, std::string &out)
{
    auto *c = static_cast<tdefl_compressor *>(heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!c)
        return false;
    out.clear();
    out.reserve(len / 4 + 64);
    // no TDEFL_WRITE_ZLIB_HEADER flag: a raw stream, like zlib's windowBits -15 on Linux
    const bool ok = tdefl_init(c, appendOut, &out, TDEFL_DEFAULT_MAX_PROBES) == TDEFL_STATUS_OKAY &&
                    tdefl_compress_buffer(c, in, len, TDEFL_FINISH) == TDEFL_STATUS_DONE;
    heap_caps_free(c);
    if (!ok)
        out.clear();
    return ok;
}

bool inflate(const void *in, size_t len, size_t rawLen, std::string &out)
{
    auto *d = static_cast<tinfl_decompressor *>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!d)
        return false;
    out.assign(rawLen, '\0');
    tinfl_init(d);
    size_t inBytes = len, outBytes = rawLen;
    auto *dst = reinterpret_cast<mz_uint8 *>(out.data());
    // all of the input at once (no TINFL_FLAG_HAS_MORE_INPUT), into one buffer of rawLen bytes
    const tinfl_status st = tinfl_decompress(d, static_cast<const mz_uint8 *>(in), &inBytes, dst, dst, &outBytes,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(d);
    const bool ok = st == TINFL_STATUS_DONE && outBytes == rawLen && inBytes == len;
    if (!ok)
        out.clear();
    return ok;
}

}  // namespace daw::platform

// ---- C++ heap: PSRAM first ----------------------------------------------------------------

static void *psramFirst(size_t n)
{
    void *p = heap_caps_malloc_prefer(n ? n : 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p)
        abort();  // no exceptions: out of memory is fatal (reported by the panic handler)
    return p;
}

void *operator new(size_t n) { return psramFirst(n); }
void *operator new[](size_t n) { return psramFirst(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept
{
    return heap_caps_malloc_prefer(n ? n : 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void *operator new[](size_t n, const std::nothrow_t &t) noexcept { return operator new(n, t); }
void operator delete(void *p) noexcept { heap_caps_free(p); }
void operator delete[](void *p) noexcept { heap_caps_free(p); }
void operator delete(void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete[](void *p, size_t) noexcept { heap_caps_free(p); }

// cJSON trees (project files, protocol messages) in PSRAM too
static std::atomic<uint32_t> s_jsonFailures{0};
// PSRAM only: a large document parsed while PSRAM is short (a 300 KB MIDI import next to the
// previous song's SoundFont channels) took internal RAM down to 28 bytes, which Wi-Fi and the
// stacks need. Without PSRAM the document fails to parse or write, with a message.
static void *jsonMalloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        s_jsonFailures.fetch_add(1, std::memory_order_relaxed);
    return p;
}
uint32_t daw::platform::jsonAllocFailures() { return s_jsonFailures.load(std::memory_order_relaxed); }
extern "C" void daw_platform_init(void)
{
    cJSON_Hooks hooks = {jsonMalloc, heap_caps_free};
    cJSON_InitHooks(&hooks);
}
