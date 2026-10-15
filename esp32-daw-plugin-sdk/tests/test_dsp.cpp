// SPDX-License-Identifier: MIT
// DSP helper accuracy: sine / exp2 approximations, oscillator pitch, envelope timing, filter
// response and stability, denormal handling.
#include <math.h>

#include <vector>

#include "check.hpp"
#include "dawsdk/dsp.hpp"
#include "../plugins/fm6/sine.hpp"

using namespace dawsdk::dsp;

static double measureFreq(const std::vector<float> &x, double sr)
{
    // rising zero crossings with linear interpolation, first to last
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = 1; i < x.size(); i++)
        if (x[i - 1] < 0 && x[i] >= 0) {
            double t = (i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0)
                first = t;
            last = t;
            n++;
        }
    return n > 1 ? (n - 1) * sr / (last - first) : 0;
}

int main()
{
    // sinTurns against libm
    double maxErr = 0;
    for (int i = 0; i < 100000; i++) {
        double p = i / 100000.0;
        maxErr = fmax(maxErr, fabs(sinTurns((float)p) - sin(2 * M_PI * p)));
    }
    CHECK(maxErr < 1e-5);
    // FM's shared lookup meets the same sine accuracy without polynomial work
    // for each of the six operators. Check every quadrant and table boundary.
    double lookupErr = 0, polynomialDifference = 0;
    for (int i = 0; i <= 100000; ++i) {
        const float phase = (float)i / 100000.0f;
        const float y = fm6dsp::sineTurns(phase);
        lookupErr = fmax(lookupErr, fabs(y - sin(2 * M_PI * (double)phase)));
        polynomialDifference = fmax(polynomialDifference, fabs(y - sinTurnsUnit(phase)));
    }
    CHECK(lookupErr < 1e-5);
    CHECK(polynomialDifference < 5e-6);
    CHECK(fm6dsp::sineTurns(0) == 0 && fm6dsp::sineTurns(.5f) == 0 && fm6dsp::sineTurns(1) == 0);
    CHECK(fm6dsp::sineTurns(.25f) == -fm6dsp::sineTurns(.75f));
    double multiTurnError = 0;
    for (int i = 0; i <= 100000; ++i) {
        const float phase = (float)i * (24.0f / 100000.0f) - 12.0f;
        multiTurnError = fmax(multiTurnError, fabs(fm6dsp::sineTurns(phase) - sin(2 * M_PI * (double)phase)));
    }
    CHECK(multiTurnError < 1e-5);
    // exp2 / midiToHz
    for (float x = -20; x <= 20; x += 0.37f) {
        CHECK_NEAR(fastExp2(x) / exp2(x), 1.0, 2e-4);
        CHECK_NEAR(fastLog2(fastExp2(x)), x, 3e-4);
    }
    for (double x = 1e-6; x < 1e6; x *= 1.37)
        CHECK_NEAR(fastLog2((float)x), log2(x), 6e-5);
    CHECK_NEAR(gainToDb(0.5f), -6.0206, 1e-3);
    CHECK_NEAR(midiToHz(69), 440.0, 0.05);
    CHECK_NEAR(midiToHz(60), 261.6256, 0.05);
    CHECK_NEAR(dbToGain(-6.0206f), 0.5, 1e-3);

    // oscillator pitch accuracy, every waveform, several notes (+-0.1 %)
    const float sr = 44100;
    Rng rng;
    for (Wave w : {Wave::Sine, Wave::Triangle, Wave::Saw, Wave::Square}) {
        for (float key : {33.0f, 57.0f, 69.0f, 93.0f}) {
            Osc o;
            o.wave = w;
            o.setFreq(midiToHz(key), sr);
            std::vector<float> x(44100);
            for (auto &s : x)
                s = o.next(rng);
            double f = measureFreq(x, sr);
            double want = 440.0 * pow(2.0, (key - 69) / 12.0);
            if (w == Wave::Saw)  // a saw crosses zero once per cycle, mid-ramp
                CHECK_NEAR(f / want, 1.0, 1e-3);
            else
                CHECK_NEAR(f / want, 1.0, 1e-3);
        }
    }
    // PolyBLEP saw stays bounded
    {
        Osc o;
        o.wave = Wave::Saw;
        o.setFreq(5000, sr);
        float pk = 0;
        for (int i = 0; i < 44100; i++)
            pk = fmaxf(pk, fabsf(o.next(rng)));
        CHECK(pk < 1.3f);
    }
    // ADSR timing: attack 10 ms reaches 1.0 after ~441 samples; release ends
    {
        Adsr e;
        e.set(0.010f, 0.1f, 0.5f, 0.05f, sr);
        e.gate(true);
        int n = 0;
        while (e.stage == Adsr::Attack && n < 100000) {
            e.next();
            n++;
        }
        CHECK(abs(n - 441) <= 2);
        for (int i = 0; i < 44100; i++)
            e.next();
        CHECK_NEAR(e.level, 0.5, 1e-3);
        e.gate(false);
        n = 0;
        while (e.active() && n < 1000000) {
            e.next();
            n++;
        }
        CHECK(!e.active());
        CHECK(n < (int)(0.05f * sr * 2.5f));
    }
    // SVF low pass: -3 dB-ish at cutoff (res 0 -> k=2: Butterworth-like Q 0.5, -6 dB), strong
    // attenuation two octaves up, unity at DC, stable at max resonance
    {
        auto gainAt = [&](float hz, float cut, float res) {
            Svf f;
            f.set(cut, res, sr);
            Osc o;
            o.wave = Wave::Sine;
            o.setFreq(hz, sr);
            double e = 0;
            for (int i = 0; i < 44100; i++) {
                float y = f.process(o.next(rng), Svf::Low);
                if (i > 22050)
                    e += y * y;
            }
            return sqrt(e / 22049) * sqrt(2.0);
        };
        CHECK_NEAR(gainAt(50, 1000, 0), 1.0, 0.02);
        CHECK(gainAt(4000, 1000, 0) < 0.08);
        CHECK(gainAt(1000, 1000, 0) < 0.6);
        CHECK(gainAt(1000, 1000, 0.9f) > 2.0);  // resonance peak
        Svf f;
        f.set(18000, 1.0f, sr);
        float pk = 0;
        for (int i = 0; i < 44100; i++)
            pk = fmaxf(pk, fabsf(f.process(i == 0 ? 1.0f : 0.0f, Svf::Low)));
        CHECK(finite(pk) && pk < 100);
    }
    // biquad peak +6 dB at centre
    {
        Biquad b;
        b.set(Biquad::Peak, 1000, 1.0f, 6.0f, sr);
        Osc o;
        o.wave = Wave::Sine;
        o.setFreq(1000, sr);
        double e = 0;
        for (int i = 0; i < 44100; i++) {
            float y = b.process(o.next(rng));
            if (i > 22050)
                e += y * y;
        }
        CHECK_NEAR(20 * log10(sqrt(e / 22049) * sqrt(2.0)), 6.0, 0.1);
    }
    // sanitize flushes denormals / NaN
    CHECK(sanitize(1e-30f) == 0.0f);
    CHECK(sanitize(NAN) == 0.0f);
    CHECK(sanitize(0.5f) == 0.5f);
    // soft clip bounded and transparent near 0
    CHECK(fabsf(softClip(100.0f)) <= 1.0f);
    CHECK_NEAR(softClip(0.01f), 0.01, 1e-4);
    // pan law
    {
        float l, r;
        panGains(0, l, r);
        CHECK(l == 1.0f && r == 1.0f);
        panGains(-1, l, r);
        CHECK(l == 1.0f && r == 0.0f);
    }
    TEST_MAIN_END("test_dsp");
}
