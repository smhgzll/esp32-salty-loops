/* SPDX-License-Identifier: MIT
 * Tracker plugin: implementations behind xmp_shim.h (see there). */
#define DAWXMP_SHIM_IMPL
#include "xmp_shim.h"

#include <stdint.h>

#include "daw_plugin_host_api.h"

int dawxmp_errno;

static const daw_host_api_t *s_host;
static unsigned s_flags = DAW_MEM_LARGE;
static size_t s_allocated;

void dawxmp_set_host(const struct daw_host_api *host, unsigned flags)
{
    s_host = host;
    s_flags = flags;
}

size_t dawxmp_allocated(void) { return s_allocated; }

/* every block starts with its owner and size, so free() / realloc() need no current host */
typedef struct {
    const daw_host_api_t *host;
    size_t size;
} Head;
#define HEAD 16u /* keeps the 16-byte alignment of the host's block */
_Static_assert(sizeof(Head) <= HEAD, "block header fits");

static void *block(size_t n, unsigned extra, unsigned mem)
{
    if (!s_host || n > (size_t)0x7FFFFFFF - HEAD)
        return NULL;
    uint8_t *b = (uint8_t *)s_host->alloc(s_host->host_ctx, n + HEAD, 16, mem | extra);
    if (!b)
        return NULL;
    Head *h = (Head *)b;
    h->host = s_host;
    h->size = n;
    s_allocated += n;
    return b + HEAD;
}

void *dawxmp_malloc(size_t n) { return block(n ? n : 1, 0, s_flags); }

void *dawxmp_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    const size_t total = n * size;
    return block(total ? total : 1, DAW_MEM_ZERO, s_flags);
}

void *dawxmp_calloc_fast(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return NULL;
    const size_t total = n * size;
    return block(total ? total : 1, DAW_MEM_ZERO, DAW_MEM_FAST);
}

void dawxmp_free(void *p)
{
    if (!p)
        return;
    Head *h = (Head *)((uint8_t *)p - HEAD);
    s_allocated -= h->size;
    h->host->free(h->host->host_ctx, h);
}

void *dawxmp_realloc(void *p, size_t n)
{
    if (!p)
        return dawxmp_malloc(n);
    if (!n) {
        dawxmp_free(p);
        return NULL;
    }
    const Head *h = (const Head *)((uint8_t *)p - HEAD);
    if (n <= h->size)
        return p;  /* shrinking keeps the block (libxmp only trims scan tables this way) */
    void *q = dawxmp_malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, h->size);
    dawxmp_free(p);
    return q;
}

char *dawxmp_strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

int dawxmp_strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
    return 0;
}

/* ---- formatting: flags '-' '0', width, precision (strings), %s %c %d %i %u %x %X %% ---- */
typedef struct {
    char *buf;
    size_t size, len;
} Out;

static void put(Out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void field(Out *o, const char *s, size_t n, int width, int left, char pad)
{
    int fill = width > (int)n ? width - (int)n : 0;
    if (!left)
        while (fill-- > 0)
            put(o, pad);
    for (size_t i = 0; i < n; i++)
        put(o, s[i]);
    if (left)
        while (fill-- > 0)
            put(o, ' ');
}

int dawxmp_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    Out o = {buf, size, 0};
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            put(&o, *f);
            continue;
        }
        f++;
        int left = 0, zero = 0, width = 0, prec = -1;
        for (; *f == '-' || *f == '0'; f++)
            *f == '-' ? (left = 1) : (zero = 1);
        for (; *f >= '0' && *f <= '9'; f++)
            width = width * 10 + (*f - '0');
        if (*f == '.')
            for (prec = 0, f++; *f >= '0' && *f <= '9'; f++)
                prec = prec * 10 + (*f - '0');
        while (*f == 'l' || *f == 'h')
            f++;
        char num[24];
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t n = 0;
            while (s[n] && (prec < 0 || n < (size_t)prec))
                n++;
            field(&o, s, n, width, left, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            field(&o, &c, 1, width, left, ' ');
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X': {
            const int sign = *f == 'd' || *f == 'i';
            const unsigned base = (*f == 'x' || *f == 'X') ? 16u : 10u;
            const char *digits = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            unsigned v;
            int neg = 0;
            if (sign) {
                int x = va_arg(ap, int);
                neg = x < 0;
                v = neg ? 0u - (unsigned)x : (unsigned)x;
            } else {
                v = va_arg(ap, unsigned);
            }
            size_t n = sizeof(num);
            do {
                num[--n] = digits[v % base];
                v /= base;
            } while (v);
            if (neg) {
                if (zero && !left) {  /* "-007": the sign before the zeros */
                    put(&o, '-');
                    width = width > 0 ? width - 1 : 0;
                } else {
                    num[--n] = '-';
                }
            }
            field(&o, num + n, sizeof(num) - n, width, left, zero && !left ? '0' : ' ');
            break;
        }
        case '%': put(&o, '%'); break;
        case 0: f--; break;
        default: break;  /* unsupported conversion: dropped */
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int dawxmp_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = dawxmp_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* no file system: libxmp's path-based loaders fail cleanly */
FILE *dawxmp_fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    dawxmp_errno = ENOENT;
    return NULL;
}
int dawxmp_fclose(FILE *f) { (void)f; return EOF; }
size_t dawxmp_fread(void *buf, size_t size, size_t n, FILE *f)
{
    (void)buf; (void)size; (void)n; (void)f;
    return 0;
}
int dawxmp_fseek(FILE *f, long off, int whence)
{
    (void)f; (void)off; (void)whence;
    return -1;
}
long dawxmp_ftell(FILE *f) { (void)f; return -1; }
int dawxmp_feof(FILE *f) { (void)f; return 1; }
int dawxmp_ferror(FILE *f) { (void)f; return 1; }
int dawxmp_fgetc(FILE *f) { (void)f; return EOF; }
int dawxmp_stat(const char *path, struct stat *st)
{
    (void)path;
    (void)st;
    dawxmp_errno = ENOENT;
    return -1;
}
time_t dawxmp_time(time_t *t)
{
    if (t)
        *t = 0;
    return 0;  /* libxmp seeds its random generator with the time: a fixed seed, reproducible renders */
}
