// SPDX-License-Identifier: GPL-3.0-or-later
// daw::platform on Linux (host build): heap, clocks, logging, random, deflate (zlib).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>
#include <zlib.h>

#include "daw/platform.hpp"

namespace daw::platform {

static int g_logLevel = (int)Log::Info;
void setLogLevel(Log l) { g_logLevel = (int)l; }

void *alloc(size_t size, size_t align, Mem)
{
    if (align < sizeof(void *))
        align = sizeof(void *);
    void *p = nullptr;
    if (posix_memalign(&p, align, size ? size : 1) != 0)
        return nullptr;
    return p;
}

void free(void *p) { ::free(p); }

uint64_t micros()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint32_t cycles()  // nanoseconds stand in for cycles on the host
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec);
}

uint32_t cyclesPerMicro() { return 1000; }

void log(Log level, const char *tag, const char *fmt, ...)
{
    if ((int)level > g_logLevel)
        return;
    static const char lv[] = {'E', 'W', 'I', 'D'};
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%c (%llu) %s: %s\n", lv[(int)level & 3], (unsigned long long)(micros() / 1000), tag, msg);
}

MemInfo memInfo() { return MemInfo{0, 0, 0, 0, 0, 0}; }

void randomBytes(void *dst, size_t n)
{
    uint8_t *p = static_cast<uint8_t *>(dst);
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r <= 0)
            break;
        p += r;
        n -= (size_t)r;
    }
}

uint32_t jsonAllocFailures() { return 0; }  // cJSON uses malloc here (Linux overcommits)

bool deflate(const void *in, size_t len, std::string &out)
{
    z_stream z{};
    if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)  // -15: raw, no zlib header
        return false;
    out.resize(deflateBound(&z, len));
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)len;
    z.next_out = (Bytef *)out.data();
    z.avail_out = (uInt)out.size();
    const bool ok = ::deflate(&z, Z_FINISH) == Z_STREAM_END;
    out.resize(ok ? z.total_out : 0);
    deflateEnd(&z);
    return ok;
}

bool inflate(const void *in, size_t len, size_t rawLen, std::string &out)
{
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK)
        return false;
    out.assign(rawLen, '\0');
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)len;
    z.next_out = (Bytef *)out.data();
    z.avail_out = (uInt)rawLen;
    // the whole stream, all of the input, exactly rawLen bytes
    const bool ok = ::inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == rawLen && z.avail_in == 0;
    inflateEnd(&z);
    if (!ok)
        out.clear();
    return ok;
}

}  // namespace daw::platform
