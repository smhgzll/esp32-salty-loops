/*
Copyright (c) 2020 Electrosmith, Corp, Emilie Gillet

Use of this source code is governed by an MIT-style
license that can be found in the LICENSE file or at
https://opensource.org/licenses/MIT.
*/

/** Helpful defines, functions, and other utilities for use in/with daisysp modules.
*/
#pragma once
#ifndef DSY_CORE_DSP
#define DSY_CORE_DSP
#include <cassert>
#include <cstdint>
#include <random>
#include <cmath>

/** PIs
*/
#define PI_F 3.1415927410125732421875f
#define TWOPI_F (2.0f * PI_F)
#define HALFPI_F (PI_F * 0.5f)
#define DSY_MIN(in, mn) (in < mn ? in : mn)
#define DSY_MAX(in, mx) (in > mx ? in : mx)
#define DSY_CLAMP(in, mn, mx) (DSY_MIN(DSY_MAX(in, mn), mx))
#define DSY_COUNTOF(_arr) (sizeof(_arr) / sizeof(_arr[0]))

namespace daisysp
{
//Avoids division for random floats. e.g. rand() * kRandFrac
static constexpr float kRandFrac = 1.f / (float)RAND_MAX;

// esp32-daw-plugin-sdk modification: a per-object deterministic generator. Classes that called
// libc rand() hold one of these as a member named `rand`, so rand() inside them resolves to it:
// no global state shared between plugin instances, identical output for identical input, and no
// libc call from the audio thread. Same range as RAND_MAX (0 .. 2^31-1).
// esp32-daw-plugin-sdk modification: the drum models were ported from Plaits, whose resonators
// take a quality factor Q. This Svf's damping is 2 * (1 - res^0.25) = 1 / Q, so
// res = (1 - 1 / (2 Q))^4. (Passing Q-derived values straight to SetRes clamps them to 1: no
// damping, the resonator rings forever and the decay setting has no effect.)
inline float PlaitsQToSvfRes(float q)
{
    float d = 1.0f / (q < 0.51f ? 0.51f : q);
    float r = 1.0f - 0.5f * d;
    return r * r * r * r;
}

// esp32-daw-plugin-sdk modification: sines without a libm call on the audio path (sinf costs a
// call and range reduction on the ESP32-S3).
// sin(pi * x) for 0 <= x <= 0.25: odd Taylor polynomial to t^9, within 2e-9 of sin on [0, pi/4],
// below a float's rounding of the result.
inline float SinPiQuarter(float x)
{
    const float t = PI_F * x, t2 = t * t;
    return t * (1.0f + t2 * (-1.0f / 6.0f + t2 * (1.0f / 120.0f + t2 * (-1.0f / 5040.0f + t2 * (1.0f / 362880.0f)))));
}
// sin(pi / 2 * r) for 0 <= r <= 1: odd Taylor polynomial to t^13, within 6e-10 of sin on [0, pi/2].
inline float SinHalfPi(float r)
{
    const float t = HALFPI_F * r, t2 = t * t;
    return t
           * (1.0f
              + t2
                    * (-1.0f / 6.0f
                       + t2 * (1.0f / 120.0f + t2 * (-1.0f / 5040.0f + t2 * (1.0f / 362880.0f + t2 * (-1.0f / 39916800.0f + t2 * (1.0f / 6227020800.0f)))))));
}
// sin(2 pi p) for any p, by quarter wave.
inline float SinTwoPi(float p)
{
    p -= static_cast<float>(static_cast<int32_t>(p));
    if(p < 0.0f)
        p += 1.0f;
    const float q = p * 4.0f;
    int         k = static_cast<int>(q);
    float       r = q - static_cast<float>(k);
    switch(k & 3)
    {
        case 0: return SinHalfPi(r);
        case 1: return SinHalfPi(1.0f - r);
        case 2: return -SinHalfPi(r);
        default: return -SinHalfPi(1.0f - r);
    }
}

// esp32-daw-plugin-sdk modification: a / b on the audio path without the ESP32-S3's __divsf3
// call (about 25 dependent FPU instructions behind a windowed call): a reciprocal seed from the
// float's bits (within 12 %) refined by three Newton-Raphson steps, within an ulp or two of the
// quotient. Only for normal, nonzero denominators (every call site divides by at least 0.5).
// Exact division in host builds, so host renders are unchanged.
inline float FastDiv(float a, float b)
{
#ifdef __XTENSA__
    union
    {
        float    f;
        uint32_t u;
    } v;
    v.f              = b;
    const uint32_t s = v.u & 0x80000000u;
    v.u              = (0x7EF311C7u - (v.u & 0x7FFFFFFFu)) | s;
    float r          = v.f;
    r                = r * (2.0f - b * r);
    r                = r * (2.0f - b * r);
    r                = r * (2.0f - b * r);
    return a * r;
#else
    return a / b;
#endif
}

struct LocalRand
{
    uint32_t s = 0x2545F491u;
    inline int operator()()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (int)(s >> 1);
    }
};

//Convert from semitones to other units. e.g. 2 ^ (kOneTwelfth * x)
static constexpr float kOneTwelfth = 1.f / 12.f;

/** efficient floating point min/max
c/o stephen mccaul
*/
inline float fmax(float a, float b)
{
    float r;
#ifdef __arm__
    asm("vmaxnm.f32 %[d], %[n], %[m]" : [d] "=t"(r) : [n] "t"(a), [m] "t"(b) :);
#else
    r = (a > b) ? a : b;
#endif // __arm__
    return r;
}

inline float fmin(float a, float b)
{
    float r;
#ifdef __arm__
    asm("vminnm.f32 %[d], %[n], %[m]" : [d] "=t"(r) : [n] "t"(a), [m] "t"(b) :);
#else
    r = (a < b) ? a : b;
#endif // __arm__
    return r;
}

/** quick fp clamp
*/
inline float fclamp(float in, float min, float max)
{
    return fmin(fmax(in, min), max);
}

/** From Musicdsp.org "Fast power and root estimates for 32bit floats)
Original code by Stefan Stenzel
These are approximations
*/
inline float fastpower(float f, int n)
{
    long *lp, l;
    lp = (long *)(&f);
    l  = *lp;
    l -= 0x3F800000;
    l <<= (n - 1);
    l += 0x3F800000;
    *lp = l;
    return f;
}

inline float fastroot(float f, int n)
{
    long *lp, l;
    lp = (long *)(&f);
    l  = *lp;
    l -= 0x3F800000;
    l >>= (n - 1);
    l += 0x3F800000;
    *lp = l;
    return f;
}

/** Significantly more efficient than fmodf(x, 1.0f) for calculating
 *  the decimal part of a floating point value.
 */
inline float fastmod1f(float x)
{
    return x - floorf(x);
}

/** From http://openaudio.blogspot.com/2017/02/faster-log10-and-pow.html
No approximation, pow10f(x) gives a 90% speed increase over powf(10.f, x)
*/
inline float pow10f(float f)
{
    return expf(2.302585092994046f * f);
}

/* Original code for fastlog2f by Dr. Paul Beckmann from the ARM community forum, adapted from the CMSIS-DSP library
About 25% performance increase over std::log10f
*/
inline float fastlog2f(float f)
{
    float frac;
    int   exp;
    frac = frexpf(fabsf(f), &exp);
    f    = 1.23149591368684f;
    f *= frac;
    f += -4.11852516267426f;
    f *= frac;
    f += 6.02197014179219f;
    f *= frac;
    f += -3.13396450166353f;
    f += exp;
    return (f);
}

inline float fastlog10f(float f)
{
    return fastlog2f(f) * 0.3010299956639812f;
}

/** Midi to frequency helper
*/
inline float mtof(float m)
{
    return powf(2, (m - 69.0f) / 12.0f) * 440.0f;
}


/** one pole lpf
out is passed by reference, and must be retained between
calls to properly filter the signal
coeff can be calculated:
coeff = 1.0 / (time * sample_rate) ; where time is in seconds
*/
inline void fonepole(float &out, float in, float coeff)
{
    out += coeff * (in - out);
}

/** Curves to use with the fmap function */
enum class Mapping
{
    LINEAR,
    EXP,
    LOG,
};

/** Maps a float between a specified range, using a specified curve.
 *
 *  \param in a value between 0 to 1 that will be mapped to the new range.
 *  \param min the new minimum value
 *  \param max the new maxmimum value
 *  \param curve a Mapping Value to adjust the response curve of the transformation
 *               defaults to Linear. @see Mapping
 *
 *  When using the log curve min and max, must be greater than zero.
 *
 *  \retval returns the transformed float within the new range.
*/
inline float
fmap(float in, float min, float max, Mapping curve = Mapping::LINEAR)
{
    switch(curve)
    {
        case Mapping::EXP:
            return fclamp(min + (in * in) * (max - min), min, max);
        case Mapping::LOG:
        {
            const float a = 1.f / log10f(max / min);
            return fclamp(min * powf(10, in / a), min, max);
        }
        case Mapping::LINEAR:
        default: return fclamp(min + in * (max - min), min, max);
    }
}

/** Simple 3-point median filter
c/o stephen mccaul
*/
template <typename T>
T median(T a, T b, T c)
{
    return (b < a) ? (b < c) ? (c < a) ? c : a : b
                   : (a < c) ? (c < b) ? c : b : a;
}

/** Ported from pichenettes/eurorack/plaits/dsp/oscillator/oscillator.h
*/
inline float ThisBlepSample(float t)
{
    return 0.5f * t * t;
}

/** Ported from pichenettes/eurorack/plaits/dsp/oscillator/oscillator.h
*/
inline float NextBlepSample(float t)
{
    t = 1.0f - t;
    return -0.5f * t * t;
}

/** Ported from pichenettes/eurorack/plaits/dsp/oscillator/oscillator.h
*/
inline float NextIntegratedBlepSample(float t)
{
    const float t1 = 0.5f * t;
    const float t2 = t1 * t1;
    const float t4 = t2 * t2;
    return 0.1875f - t1 + 1.5f * t2 - t4;
}

/** Ported from pichenettes/eurorack/plaits/dsp/oscillator/oscillator.h
*/
inline float ThisIntegratedBlepSample(float t)
{
    return NextIntegratedBlepSample(1.0f - t);
}

/** Soft Limiting function ported extracted from pichenettes/stmlib */
inline float SoftLimit(float x)
{
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

/** Soft Clipping function extracted from pichenettes/stmlib */
inline float SoftClip(float x)
{
    if(x < -3.0f)
        return -1.0f;
    else if(x > 3.0f)
        return 1.0f;
    else
        return SoftLimit(x);
}

/** Quick check for Invalid float values (NaN, Inf, out of range)
 ** \param x value passed by reference, replaced by y if invalid.
 ** \param y value to replace x if invalidity is found.
 **
 ** When DEBUG is true in the build, this will halt
 ** execution for tracing the reason for the invalidity. */
inline void TestFloat(float &x, float y = 0.f)
{
    if(!std::isnormal(x) && x != 0)
    {
#if defined(__arm__) && defined(DEBUG)
        asm("bkpt 255");
#else
        x = y;
#endif
    }
}

/** Based on soft saturate from:
[musicdsp.org](musicdsp.org/en/latest/Effects/42-soft-saturation.html)
Bram de Jong (2002-01-17)
This still needs to be tested/fixed. Definitely does some weird stuff
described as:
x < a:
     f(x) = x
x > a:
     f(x) = a + (x-a)/(1+((x-a)/(1-a))^2)
x > 1:
     f(x) = (a + 1)/2
*/
inline float soft_saturate(float in, float thresh)
{
    bool  flip;
    float val, out;
    //val = fabsf(in);
    out  = 0.f;
    flip = in < 0.0f;
    val  = flip ? -in : in;
    if(val < thresh)
    {
        out = in;
    }
    else if(val > 1.0f)
    {
        out = (thresh + 1.0f) / 2.0f;
        if(flip)
            out *= -1.0f;
    }
    else if(val > thresh)
    {
        float temp;
        temp = (val - thresh) / (1 - thresh);
        out  = thresh + (val - thresh) / (1.0f + (temp * temp));
        if(flip)
            out *= -1.0f;
    }
    return out;
    //    return val < thresh
    //               ? val
    //               : val > 1.0f
    //                     ? (thresh + 1.0f) / 2.0f
    //                     : thresh
    //                           + (val - thresh)
    //                                 / (1.0f
    //                                    + (((val - thresh) / (1.0f - thresh))
    //                                       * ((val - thresh) / (1.0f - thresh))));
}
constexpr bool is_power2(uint32_t x)
{
    return ((x - 1) & x) == 0;
}

/** Prior to C++14 constexpr functions were required to be a single return statement.
 *  So this clause guards against that behavior to allow the library, and this function
 *  to continue to work with C++11.
 *  The function itself is not currently (12 May 2021) used within the library itself.
 */
#if __cplusplus <= 201103L
inline uint32_t get_next_power2(uint32_t x)
#else
constexpr uint32_t get_next_power2(uint32_t x)
#endif
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;

    assert(is_power2(x));
    return x;
}

} // namespace daisysp
#endif

#ifdef DSY_CUSTOM_DSP
#include "custom_dsp.h"
#endif
