// SPDX-License-Identifier: MIT
// Quarter-wave lookup for the FM operators. Linear interpolation keeps absolute
// sine error below 1e-5, the SDK polynomial's accuracy contract, with 1 KB shared
// read-only data. On Xtensa the table uses internal RAM, away from flash/PSRAM traffic.
#pragma once
#include "dawsdk/dsp.hpp"

namespace fm6dsp {
struct QuarterSine {
    float value[257] = {};
    constexpr QuarterSine()
    {
        for (unsigned i = 0; i <= 256; ++i) {
            const float t = (float)i * (1.0f / 1024.0f) * dawsdk::dsp::kTwoPi;
            const float t2 = t * t;
            value[i] = t * (1.0f + t2 * (-0.16666667f + t2 *
                (0.0083333310f + t2 * (-0.00019840874f + t2 * 2.7525562e-6f))));
        }
    }
};
#ifdef __XTENSA__
__attribute__((section(".dram1.daw_fm6_sine")))
#endif
inline constexpr QuarterSine sineTable{};

// Wrap in table coordinates once: integer masking handles both negative and
// multi-turn modulation, avoiding a separate floating-point phase normalization.
inline float sineTurns(float phase)
{
    const float pos = phase * 1024.0f;
    int32_t index = (int32_t)pos;
    float fraction = pos - (float)index;
    if (fraction < 0) { --index; fraction += 1.0f; }
    const unsigned cell = (uint32_t)index & 1023u, quadrant = cell >> 8;
    const unsigned a = (quadrant & 1u) ? 256u - (cell & 255u) : cell & 255u;
    const unsigned b = (quadrant & 1u) ? a - 1u : a + 1u;
    const float y = sineTable.value[a] + (sineTable.value[b] - sineTable.value[a]) * fraction;
    return (quadrant & 2u) ? -y : y;
}
}  // namespace fm6dsp
