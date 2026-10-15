// SPDX-License-Identifier: MIT
// esp32-daw-plugin-sdk: small real-time DSP building blocks (header only, float32, no allocation).
// Written for the ESP32-S3's single-precision FPU: no double math, no libm calls per sample
// except where noted.
#pragma once

#include <math.h>
#include <stdint.h>

namespace dawsdk {
inline namespace v1 {
namespace dsp {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline bool silent(const float *l, const float *r, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (l[i] != 0.0f || r[i] != 0.0f) return false;
    return true;
}

// flush tiny values (denormals cost hundreds of cycles on many FPUs) and NaN/Inf to zero
inline float sanitize(float x)
{
    float a = x < 0 ? -x : x;
    return (a > 1e-15f && a < 1e15f) ? x : 0.0f;
}
inline bool finite(float x) { return x == x && x < 3e38f && x > -3e38f; }

// a / b without the ESP32-S3's __divsf3 call (about 25 dependent FPU instructions behind a call):
// a reciprocal seed from the float's bits (within 12 %) refined by three Newton-Raphson steps,
// within an ulp or two of the quotient. b must be a normal, nonzero number (|b| well above 1e-30).
// Exact division in host builds.
inline float fastDiv(float a, float b)
{
#ifdef __XTENSA__
    union {
        float f;
        uint32_t u;
    } v;
    v.f = b;
    const uint32_t s = v.u & 0x80000000u;
    v.u = (0x7EF311C7u - (v.u & 0x7FFFFFFFu)) | s;
    float r = v.f;
    r = r * (2.0f - b * r);
    r = r * (2.0f - b * r);
    r = r * (2.0f - b * r);
    return a * r;
#else
    return a / b;
#endif
}

// 2^x, accurate to ~1e-4 relative over the musical range; no libm
inline float fastExp2(float x)
{
    if (x < -60.0f)
        return 0.0f;
    if (x > 60.0f)
        x = 60.0f;
    int32_t i = (int32_t)(x < 0 ? x - 1.0f : x);  // floor
    float f = x - (float)i;
    // minimax-ish polynomial for 2^f on [0,1)
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    union {
        uint32_t u;
        float v;
    } c;
    c.u = (uint32_t)(i + 127) << 23;
    return p * c.v;
}

// log2(x) for x > 0, |err| < 3e-5 (0.0002 dB); no libm. log2(m) on the mantissa's octave [1,2) is
// (m - 1) * q(m) with a least-squares quartic q, exact at m = 1 (continuous across octaves).
inline float fastLog2(float x)
{
    union {
        float v;
        uint32_t u;
    } c{x > 1e-30f ? x : 1e-30f};
    const float e = (float)(int32_t)((c.u >> 23) & 255) - 127.0f;
    c.u = (c.u & 0x007FFFFFu) | 0x3F800000u;  // mantissa in [1, 2)
    const float m = c.v;
    const float q = 2.806238044f + m * (-2.306336122f + m * (1.274003126f + m * (-0.377964779f + m * 0.045885527f)));
    return e + (m - 1.0f) * q;
}
inline float gainToDb(float g) { return 6.02059991f * fastLog2(g); }

inline float midiToHz(float key) { return 440.0f * fastExp2((key - 69.0f) * (1.0f / 12.0f)); }
inline float dbToGain(float db) { return fastExp2(db * 0.16609640474f); }  // 10^(db/20)

// sin(2*pi*phase), phase in [0,1): degree-9 odd polynomial after quadrant folding; |err| < 4e-6
inline float sinTurnsUnit(float x)
{
    // fold to [-0.25, 0.25] turns
    if (x > 0.75f)
        x -= 1.0f;
    else if (x > 0.25f)
        x = 0.5f - x;
    float t = x * kTwoPi;  // [-pi/2, pi/2]
    float t2 = t * t;
    return t * (1.0f + t2 * (-0.16666667f + t2 * (0.0083333310f + t2 * (-0.00019840874f + t2 * 2.7525562e-6f))));
}
inline float sinTurns(float phase)
{
    float x = phase - (float)(int32_t)phase;
    if (x < 0) x += 1.0f;
    return sinTurnsUnit(x);
}

// Exact floor for bounded audio phase/delay coordinates, without a libm call on Xtensa.
inline int32_t floorInt(float x)
{
    int32_t i = (int32_t)x;
    return x < (float)i ? i - 1 : i;
}

// soft clipper: tanh-like rational approximation, exact 0 and slope 1 at 0, |y| <= 1
inline float softClip(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// tan approximation for filter prewarping, w in [0, ~1.5]
inline float fastTan(float w)
{
    float w2 = w * w;
    return w * (1.0f + w2 * (0.33333333f + w2 * (0.13333334f + w2 * (0.053968254f + w2 * 0.021869488f))));
}

struct Rng {
    uint32_t s = 0x9E3779B9u;
    void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float bipolar() { return (float)(int32_t)next() * (1.0f / 2147483648.0f); }  // [-1,1)
    float unipolar() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }       // [0,1)
};

// one-pole parameter smoother (avoids zipper noise)
struct Smoother {
    float value = 0, target = 0, coef = 0.001f;
    void setTime(float seconds, float sr) { coef = seconds <= 0 ? 1.0f : 1.0f - expf(-1.0f / (seconds * sr)); }
    void snap(float v) { value = target = v; }
    float next()
    {
        value += (target - value) * coef;
        return value;
    }
    bool settled() const { float d = target - value; return d < 1e-6f && d > -1e-6f; }
};

// band-limited oscillator (PolyBLEP saw / pulse, naive triangle and sine)
enum class Wave : uint8_t { Sine = 0, Triangle, Saw, Square, Noise };

inline float polyBlep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

struct Osc {
    float phase = 0;  // [0,1)
    float inc = 0;    // cycles per sample
    float pw = 0.5f;  // pulse width
    Wave wave = Wave::Saw;

    void setFreq(float hz, float sr)
    {
        inc = hz / sr;
        if (inc > 0.45f)
            inc = 0.45f;
        if (inc < 0)
            inc = 0;
    }
    float next(Rng &rng)
    {
        float t = phase, out;
        switch (wave) {
        case Wave::Sine: out = sinTurnsUnit(t); break;
        case Wave::Triangle: out = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t; break;
        case Wave::Saw: out = 2.0f * t - 1.0f - polyBlep(t, inc); break;
        case Wave::Square: {
            out = t < pw ? 1.0f : -1.0f;
            out += polyBlep(t, inc);
            float t2 = t - pw + 1.0f;
            t2 -= (float)(int32_t)t2;
            out -= polyBlep(t2, inc);
            break;
        }
        default: out = rng.bipolar(); break;
        }
        phase += inc;
        if (phase >= 1.0f)
            phase -= 1.0f;
        return out;
    }
};

// ADSR, linear attack, exponential decay / release (times in seconds, sustain 0..1)
struct Adsr {
    enum Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Idle;
    float level = 0;
    float aInc = 1, dCoef = 0, rCoef = 0, sustain = 1;

    void set(float a, float d, float s, float r, float sr)
    {
        aInc = a <= 0.0005f ? 1.0f : 1.0f / (a * sr);
        dCoef = d <= 0.0005f ? 1.0f : 1.0f - expf(-4.6f / (d * sr));  // ~-40 dB after d
        rCoef = r <= 0.0005f ? 1.0f : 1.0f - expf(-4.6f / (r * sr));
        sustain = clamp(s, 0.0f, 1.0f);
    }
    void gate(bool on)
    {
        if (on)
            stage = Attack;
        else if (stage != Idle)
            stage = Release;
    }
    void kill() { stage = Idle; level = 0; }
    bool active() const { return stage != Idle; }
    float next()
    {
        switch (stage) {
        case Attack:
            level += aInc;
            if (level >= 1.0f) {
                level = 1.0f;
                stage = Decay;
            }
            break;
        case Decay:
            level += (sustain - level) * dCoef;
            if (level - sustain < 1e-4f) {
                level = sustain;
                stage = Sustain;
            }
            break;
        case Sustain: level = sustain; break;
        case Release:
            level -= level * rCoef;
            if (level < 1e-4f) {
                level = 0;
                stage = Idle;
            }
            break;
        default: break;
        }
        return level;
    }
};

// Topology-preserving-transform state variable filter (Zavalishin / Simper)
struct Svf {
    enum Mode : uint8_t { Low = 0, Band, High, Notch };
    float ic1 = 0, ic2 = 0;
    float g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
    float lastHz = -1, lastRes = -1, lastSr = 0;

    void set(float hz, float res01, float sr)
    {
        if (hz == lastHz && res01 == lastRes && sr == lastSr) return;
        lastHz = hz;
        lastRes = res01;
        lastSr = sr;
        float w = kPi * clamp(hz, 10.0f, sr * 0.45f) / sr;
        g = w < 1.2f ? fastTan(w) : tanf(w);
        k = 2.0f - 1.96f * clamp(res01, 0.0f, 1.0f);  // res 1 -> k 0.04 (near self-oscillation)
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0; }
    float process(float v0, Mode m)
    {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = sanitize(2.0f * v1 - ic1);
        ic2 = sanitize(2.0f * v2 - ic2);
        switch (m) {
        case Low: return v2;
        case Band: return v1;
        case High: return v0 - k * v1 - v2;
        default: return v0 - k * v1;
        }
    }
};

// biquad (RBJ cookbook), transposed direct form II
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    enum Type : uint8_t { LowPass, HighPass, Peak, LowShelf, HighShelf };
    void set(Type t, float hz, float q, float gainDb, float sr)
    {
        float w0 = kTwoPi * clamp(hz, 10.0f, sr * 0.49f) / sr;
        float cw = cosf(w0), sw = sinf(w0);
        float alpha = sw / (2.0f * (q < 0.05f ? 0.05f : q));
        float A = powf(10.0f, gainDb / 40.0f);
        float B0, B1, B2, A0, A1, A2;
        switch (t) {
        case LowPass: B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = B0; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
        case HighPass: B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = B0; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
        case Peak: B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A; A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A; break;
        case LowShelf: {
            float sa = 2 * sqrtf(A) * alpha;
            B0 = A * ((A + 1) - (A - 1) * cw + sa); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - sa);
            A0 = (A + 1) + (A - 1) * cw + sa; A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - sa;
            break;
        }
        default: {
            float sa = 2 * sqrtf(A) * alpha;
            B0 = A * ((A + 1) + (A - 1) * cw + sa); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - sa);
            A0 = (A + 1) - (A - 1) * cw + sa; A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - sa;
            break;
        }
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void reset() { z1 = z2 = 0; }
    float process(float x)
    {
        float y = b0 * x + z1;
        z1 = sanitize(b1 * x - a1 * y + z2);
        z2 = sanitize(b2 * x - a2 * y);
        return y;
    }
};

// balance pan law, p in [-1,1]: centre is unity on both sides, the far side fades linearly
// (the same law as the DAW's mixer, so a voice panned by a note and a channel pan agree)
inline void panGains(float p, float &l, float &r)
{
    p = clamp(p, -1.0f, 1.0f);
    l = p > 0 ? 1.0f - p : 1.0f;
    r = p < 0 ? 1.0f + p : 1.0f;
}

}  // namespace dsp
}  // namespace v1
}  // namespace dawsdk
