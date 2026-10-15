// SPDX-License-Identifier: MIT
// Minimal test macros: CHECK records a failure and continues; main() returns non-zero on failure.
#pragma once
#include <math.h>
#include <stdio.h>

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                              \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        g_checks++;                                                                                    \
        double _a = (a), _b = (b);                                                                     \
        if (!(fabs(_a - _b) <= (tol))) {                                                               \
            g_failures++;                                                                              \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.9g, %s = %.9g (tol %.3g)\n", __FILE__, \
                    __LINE__, #a, _a, #b, _b, (double)(tol));                                          \
        }                                                                                              \
    } while (0)
#define TEST_MAIN_END(name)                                                                 \
    do {                                                                                    \
        printf("%s: %s (%d checks, %d failed)\n", g_failures ? "FAIL" : "PASS", name,    \
               g_checks, g_failures);                                                       \
        return g_failures ? 1 : 0;                                                          \
    } while (0)
