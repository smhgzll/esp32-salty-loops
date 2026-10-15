// SPDX-License-Identifier: GPL-3.0-or-later
// The deterministic reference render (the same as the firmware self-test, main/selftest.cpp):
// 8 s of the demo song through the real plugins. Prints DAW_RENDER for scripts/test-qemu.sh, which
// compares the ESP32-S3 (QEMU) numbers with these. They are not bit-identical: libm differs between
// newlib and glibc, so the comparison uses tolerances. Also checks determinism on one platform.
#include <math.h>
#include <stdio.h>

#include "check.hpp"
#include "daw/session.hpp"

using namespace daw;

struct Stats {
    double rmsL, rmsR, peak;
    uint32_t nonzero, frames;
};

static Stats renderDemo()
{
    Engine eng(Engine::Config{44100, 256});
    Session s(eng, Session::Config{});
    s.start(defaultProject());
    eng.post(EngineCmd{EngineCmd::SetMode, 1});
    eng.post(EngineCmd{EngineCmd::Play});
    float L[256], R[256];
    double eL = 0, eR = 0, peak = 0;
    uint32_t nonzero = 0;
    const int blocks = 44100 * 8 / 256;
    for (int b = 0; b < blocks; b++) {
        eng.render(L, R, 256);
        for (int i = 0; i < 256; i++) {
            eL += (double)L[i] * L[i];
            eR += (double)R[i] * R[i];
            peak = fmax(peak, fmax(fabs(L[i]), fabs(R[i])));
            nonzero += (L[i] != 0 || R[i] != 0);
        }
        if (b % 64 == 0)
            s.tick((uint32_t)b);
    }
    const double n = (double)blocks * 256;
    return {sqrt(eL / n), sqrt(eR / n), peak, nonzero, (uint32_t)n};
}

int main()
{
    Stats a = renderDemo(), b = renderDemo();
    printf("DAW_RENDER frames=%u rmsL=%.6f rmsR=%.6f peak=%.6f nonzero=%u\n", a.frames, a.rmsL, a.rmsR, a.peak, a.nonzero);
    CHECK(a.rmsL == b.rmsL && a.rmsR == b.rmsR && a.peak == b.peak);  // deterministic
    CHECK(a.nonzero > a.frames / 2);
    CHECK(a.peak > 0.05 && a.peak <= 1.0);
    TEST_MAIN_END("test_render");
}
