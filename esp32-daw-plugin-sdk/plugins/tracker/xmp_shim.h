/* SPDX-License-Identifier: MIT
 * Tracker plugin: the C library libxmp needs, without the C library.
 *
 * Force-included (-include) into every libxmp source and the glue. A plugin must not reference
 * malloc, stdio or other libc state (tools/package.py rejects such archives), so the names libxmp
 * calls are redirected here, so its sources need no changes for them:
 *   - malloc / calloc / realloc / free: the host's allocator (daw_host_api_t), set per control call
 *     with dawxmp_set_host(); a call without a host (the audio thread) fails instead of allocating
 *   - file I/O (fopen ... stat): fails; modules are only loaded from memory
 *   - snprintf / vsnprintf: a small formatter for the loaders' tracker names (%s %c %d %u %x)
 *   - isprint / isdigit, strncpy / strncmp, errno, time(): local versions
 */
#ifndef DAWXMP_SHIM_H
#define DAWXMP_SHIM_H

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daw_host_api;
/* the allocator for the following libxmp calls (control thread); NULL: every allocation fails */
void dawxmp_set_host(const struct daw_host_api *host, unsigned flags);
/* bytes currently allocated through the shim (all instances) */
size_t dawxmp_allocated(void);

void *dawxmp_malloc(size_t n);
void *dawxmp_calloc(size_t n, size_t size);
void *dawxmp_calloc_fast(size_t n, size_t size);  /* hot mixer voices, control thread */
void *dawxmp_realloc(void *p, size_t n);
void dawxmp_free(void *p);

int dawxmp_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int dawxmp_snprintf(char *buf, size_t size, const char *fmt, ...);
char *dawxmp_strncpy(char *dst, const char *src, size_t n);
int dawxmp_strncmp(const char *a, const char *b, size_t n);

FILE *dawxmp_fopen(const char *path, const char *mode);
int dawxmp_fclose(FILE *f);
size_t dawxmp_fread(void *buf, size_t size, size_t n, FILE *f);
int dawxmp_fseek(FILE *f, long off, int whence);
long dawxmp_ftell(FILE *f);
int dawxmp_feof(FILE *f);
int dawxmp_ferror(FILE *f);
int dawxmp_fgetc(FILE *f);
int dawxmp_stat(const char *path, struct stat *st);
time_t dawxmp_time(time_t *t);
extern int dawxmp_errno;

static inline int dawxmp_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int dawxmp_isprint(int c) { return c >= 0x20 && c < 0x7f; }

#ifdef __cplusplus
}
#endif

#ifndef DAWXMP_SHIM_IMPL
#define malloc dawxmp_malloc
#define calloc dawxmp_calloc
#define realloc dawxmp_realloc
#define free dawxmp_free
#undef snprintf
#define snprintf dawxmp_snprintf
#undef vsnprintf
#define vsnprintf dawxmp_vsnprintf
#define strncpy dawxmp_strncpy
#define strncmp dawxmp_strncmp
#define fopen dawxmp_fopen
#define fclose dawxmp_fclose
#define fread dawxmp_fread
#define fseek dawxmp_fseek
#define ftell dawxmp_ftell
#undef feof
#define feof dawxmp_feof
#undef ferror
#define ferror dawxmp_ferror
#undef fgetc
#define fgetc dawxmp_fgetc
#define stat(p, b) dawxmp_stat(p, b)
#define time(t) dawxmp_time(t)
#undef errno
#define errno dawxmp_errno
#undef isdigit
#define isdigit(c) dawxmp_isdigit(c)
#undef isprint
#define isprint(c) dawxmp_isprint(c)
#endif

#endif
