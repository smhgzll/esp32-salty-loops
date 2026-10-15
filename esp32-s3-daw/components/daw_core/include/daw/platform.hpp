// SPDX-License-Identifier: GPL-3.0-or-later
// esp32-s3-daw: the services the portable core needs from its platform (ESP32-S3 or Linux).
// Implemented in components/daw_hal (ESP-IDF) and host/platform_host.cpp. The core never includes
// ESP-IDF or POSIX headers itself.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace daw::platform {

enum class Mem : uint8_t {
    Fast,   // internal SRAM: hot DSP state, audio buffers
    Large,  // PSRAM on the ESP32-S3: delay lines, project model, JSON, snapshots
};

void *alloc(size_t size, size_t align, Mem cls);  // nullptr on failure; never from the audio thread
void free(void *p);

uint64_t micros();          // monotonic
uint32_t cycles();          // CPU cycle counter (wraps); cheap enough for the audio thread
uint32_t cyclesPerMicro();  // 240 on the ESP32-S3 at 240 MHz

enum class Log : uint8_t { Error, Warn, Info, Debug };
void log(Log level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

struct MemInfo {
    size_t internalFree, internalMinFree, internalTotal;
    size_t psramFree, psramMinFree, psramTotal;
    size_t psramLargest = 0;
};
MemInfo memInfo();

// random bytes for tokens / ids (hardware RNG on the ESP32-S3, getrandom on Linux)
void randomBytes(void *dst, size_t n);

// cJSON allocations that failed so far (PSRAM exhausted): a document written meanwhile is
// incomplete (cJSON leaves out what it could not allocate)
uint32_t jsonAllocFailures();

// raw DEFLATE streams (RFC 1951) for the project slots: the ESP32-S3 ROM's miniz on the device,
// zlib on Linux. deflate() fails only without memory; inflate() succeeds only when `in` is one
// complete stream of exactly `rawLen` bytes. Control thread only (they allocate).
bool deflate(const void *in, size_t len, std::string &out);
bool inflate(const void *in, size_t len, size_t rawLen, std::string &out);

}  // namespace daw::platform

#define DAW_LOGE(tag, ...) ::daw::platform::log(::daw::platform::Log::Error, tag, __VA_ARGS__)
#define DAW_LOGW(tag, ...) ::daw::platform::log(::daw::platform::Log::Warn, tag, __VA_ARGS__)
#define DAW_LOGI(tag, ...) ::daw::platform::log(::daw::platform::Log::Info, tag, __VA_ARGS__)
