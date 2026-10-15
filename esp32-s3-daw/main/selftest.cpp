// SPDX-License-Identifier: GPL-3.0-or-later
// Boot self-test (CONFIG_DAW_SELFTEST, on by default in the QEMU profile). Output markers are parsed
// by scripts/test-qemu.sh. Two boots: phase 1 stores a marker project and restarts; phase 2 checks
// that the autosave brought it back.
#include "selftest.hpp"

#include <math.h>
#include <string.h>

#include <new>

#include "audio_out.hpp"
#include "control.hpp"
#include "daw/project_json.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace daw::selftest {

static const char *TAG = "selftest";

// the same render as tests/core/test_render.cpp on Linux
bool offlineRender()
{
    void *mem = heap_caps_malloc(sizeof(Engine), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mem)
        return false;
    Engine *eng = new (mem) Engine(Engine::Config{44100, 256});
    bool ok = eng->ok();
    {
        Session s(*eng, Session::Config{});
        s.start(defaultProject());
        EngineCmd mode{EngineCmd::SetMode, 1};
        eng->post(mode);
        eng->post(EngineCmd{EngineCmd::Play});
        float L[256], R[256];
        double eL = 0, eR = 0, peak = 0;
        uint32_t nonzero = 0;
        const int blocks = 44100 * 8 / 256;  // 8 s of the demo song
        const int64_t t0 = esp_timer_get_time();
        for (int b = 0; b < blocks; b++) {
            eng->render(L, R, 256);
            for (int i = 0; i < 256; i++) {
                eL += (double)L[i] * L[i];
                eR += (double)R[i] * R[i];
                peak = fmax(peak, fmax(fabs(L[i]), fabs(R[i])));
                nonzero += (L[i] != 0 || R[i] != 0);
            }
            if (b % 64 == 0) {
                s.tick((uint32_t)b);
                vTaskDelay(1);  // let the idle task feed the watchdog
            }
        }
        const double us = (double)(esp_timer_get_time() - t0);
        const double n = (double)blocks * 256;
        printf("DAW_RENDER frames=%u rmsL=%.6f rmsR=%.6f peak=%.6f nonzero=%u\n", (unsigned)n, sqrt(eL / n), sqrt(eR / n), peak,
               (unsigned)nonzero);
        // (the time is QEMU's, not the chip's: informational only)
        ESP_LOGI(TAG, "offline render of 8 s took %.1f s of emulated time", us / 1e6);
        ok = ok && nonzero > n / 2 && peak > 0.05;
        s.tick(1000000);
    }
    // the session's destructor retired the instances; free what the engine handed back
    eng->~Engine();
    heap_caps_free(mem);
    return ok;
}

static uint8_t phaseGet()
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("selftest", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "phase", &v);
        nvs_close(h);
    }
    return v;
}

static void phaseSet(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("selftest", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "phase", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

struct Ctx {
    Session *s;
    Engine *e;
};

static void fail(const char *why)
{
    printf("DAW_SELFTEST_FAIL %s\n", why);
    vTaskDelete(nullptr);
}

static void task(void *arg)
{
    Ctx *c = static_cast<Ctx *>(arg);
    while (!control::booted())
        vTaskDelay(pdMS_TO_TICKS(50));
    // the live engine renders: blocks advance through the audio task
    const uint32_t b0 = c->e->status().blocks.load();
    vTaskDelay(pdMS_TO_TICKS(1500));
    const uint32_t b1 = c->e->status().blocks.load();
    printf("selftest: audio blocks %u -> %u (%s backend)\n", (unsigned)b0, (unsigned)b1,
           audioOutKind() == AudioKind::I2S ? "I2S" : "null");
    if (b1 <= b0 + 10)
        fail("the audio task does not render");
    const uint8_t phase = phaseGet();
    if (phase == 0) {
        // phase 1: a marker project through the session, saved by the autosave, then a restart
        std::string err;
        bool ok = false;
        control::call(
            [&] {
                Project p = defaultProject();
                p.name = "selftest-marker";
                p.bpm = 111;
                const std::string text = projectToText(p);
                ok = c->s->importProject(text.data(), text.size(), &err);
                c->s->stopForMaintenance();  // forces the autosave
            },
            10000);
        if (!ok)
            fail(("import: " + err).c_str());
        phaseSet(1);
        printf("selftest: phase 1 done (marker project autosaved), restarting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    // phase 2: the autosave restored the marker
    std::string name;
    double bpm = 0;
    control::call(
        [&] {
            name = c->s->project().name;
            bpm = c->s->project().bpm;
        },
        5000);
    printf("selftest: restored project \"%s\" at %.1f BPM\n", name.c_str(), bpm);
    if (name != "selftest-marker" || bpm != 111.0)
        fail("the autosave did not restore the project across the restart");
    if (!heap_caps_check_integrity_all(true))
        fail("heap corruption");
    phaseSet(2);  // later boots of the same flash stay idle (web tests)
    printf("selftest: heap internal free %u (min %u), PSRAM free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("DAW_SELFTEST_PASS\n");
    printf("DAW_QEMU_IDLE\n");
    vTaskDelete(nullptr);
}

void startLive(Session *s, Engine *e)
{
    if (phaseGet() >= 2) {
        printf("DAW_QEMU_IDLE\n");
        return;
    }
    static Ctx c;
    c = Ctx{s, e};
    xTaskCreatePinnedToCore(task, "selftest", 8192, &c, 4, nullptr, 0);
}

}  // namespace daw::selftest
