// SPDX-License-Identifier: GPL-3.0-or-later
// esp32-s3-daw firmware entry: wires the portable DAW core to the ESP32-S3.
//   core 1: audio task (configured priority): Engine::render -> I2S (or the null backend under QEMU)
//   core 0: one instrument worker, control task, HTTP / WebSocket, Wi-Fi / Ethernet, lwIP
#include <stdio.h>
#include <string.h>

#include <new>
#include <string>

#include "audio_out.hpp"
#include "audio_worker.hpp"
#include "cJSON.h"
#include "control.hpp"
#include "daw/engine.hpp"
#include "daw/plugin_host.hpp"
#include "daw/session.hpp"
#include "daw_pins.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_storage.hpp"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "soc/rtc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "daw/settings_update.hpp"
#include "selftest.hpp"
#include "web_server.hpp"

// (a disabled Kconfig bool is not defined at all: 0 / 1 macros for use in expressions)
#if CONFIG_DAW_QEMU_BUILD
#define DAW_QEMU 1
#else
#define DAW_QEMU 0
#endif
#if CONFIG_DAW_DITHER
#define DAW_DITHER 1
#else
#define DAW_DITHER 0
#endif

extern "C" void daw_platform_init(void);

using namespace daw;

static const char *TAG = "daw";
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
static esp_core_dump_summary_t *s_crash;
static char s_panicReason[128];
#endif
static Engine *s_engine;
static Session *s_session;
static ProjectStore *s_store;
static PartitionFlash *s_projects;
static PartitionFlash *s_sampleFlash;
static SampleStore *s_samples;
static PartitionFlash *s_presetFlash;
static SampleStore *s_presetStore;
static PresetLibrary *s_presets;
static TaskHandle_t s_audioTask;

static void audioTask(void *)
{
    const uint32_t frames = CONFIG_DAW_BLOCK_FRAMES;
    static float L[256], R[256];  // internal RAM (static .bss)
    for (;;) {
        s_engine->render(L, R, frames);
        audioOutWrite(L, R, frames);  // blocks on the DMA: this paces the loop
    }
}

static void platformDiag(cJSON *d)
{
    cJSON_AddNumberToObject(d, "heapInternal", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "heapInternalMin", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "heapInternalLargest", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(d, "psramFree", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(d, "psramLargest", (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(d, "psramTotal", (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(d, "resetReason", esp_reset_reason());
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (s_crash) {
        cJSON *c = cJSON_AddObjectToObject(d, "lastCrash");
        cJSON_AddStringToObject(c, "task", s_crash->exc_task);
        cJSON_AddStringToObject(c, "reason", s_panicReason);
        cJSON_AddStringToObject(c, "elfSha256", reinterpret_cast<const char *>(s_crash->app_elf_sha256));
        cJSON_AddNumberToObject(c, "pc", s_crash->exc_pc);
        cJSON *bt = cJSON_AddArrayToObject(c, "backtrace");
        for (uint32_t i = 0; i < s_crash->exc_bt_info.depth && i < 16; i++)
            cJSON_AddItemToArray(bt, cJSON_CreateNumber(s_crash->exc_bt_info.bt[i]));
    }
#endif
    cJSON_AddNumberToObject(d, "cpuMHz", esp_clk_cpu_freq() / 1000000);
    // Probe the driver's actual divider support without changing a running clock.
    rtc_cpu_freq_config_t clockConfig;
    cJSON *clocks = cJSON_AddObjectToObject(d, "cpuClockSupport");
    for (uint32_t mhz : {240u, 280u, 300u, 320u}) {
        char key[12];
        snprintf(key, sizeof key, "%u", (unsigned)mhz);
        cJSON_AddBoolToObject(clocks, key, rtc_clk_cpu_freq_mhz_to_config(mhz, &clockConfig));
    }
    AudioOutStats &a = audioOutStats();
    cJSON_AddNumberToObject(d, "underruns", a.underruns.load());
    cJSON_AddNumberToObject(d, "audioLate", a.late.load());
    cJSON_AddNumberToObject(d, "audioWriteUsMax", a.writeUsMax.load());
    cJSON_AddStringToObject(d, "audioBackend", audioOutKind() == AudioKind::I2S ? "i2s" : "null");
    cJSON_AddNumberToObject(d, "uptimeMs", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(d, "boot", bootCounter());
    cJSON_AddNumberToObject(d, "wsDropped", web::droppedFrames());
    cJSON_AddNumberToObject(d, "wsClients", web::clientCount());
    if (s_audioTask)
        cJSON_AddNumberToObject(d, "audioStackFree", uxTaskGetStackHighWaterMark(s_audioTask));
    cJSON_AddBoolToObject(d, "parallelInstruments", s_engine && s_engine->config().worker);
    cJSON_AddNumberToObject(d, "dspStackFree", audioWorkerStackFree());
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(d, "partition", run ? run->label : "?");
    esp_ota_img_states_t st;
    const char *ota = "factory";
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK)
        ota = st == ESP_OTA_IMG_PENDING_VERIFY ? "pending-verify" : st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_NEW ? "new" : "other";
    cJSON_AddStringToObject(d, "otaState", ota);
}

// a new image confirms itself once audio and the web server have run for a few seconds
static void healthTask(void *)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "new firmware on %s: confirming after a healthy start", run->label);
    const uint32_t b0 = s_engine->status().blocks.load();
    vTaskDelay(pdMS_TO_TICKS(4000));
    // QEMU test hook (POST /api/test/fail-next-health): this start counts as unhealthy
    const bool forcedFail = DAW_QEMU && settingsTakeFlag("tfh");
    if (!forcedFail && control::booted() && s_engine->status().blocks.load() > b0 + 100) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware confirmed (rollback cancelled)");
    } else {
        ESP_LOGE(TAG, "unhealthy start%s: rolling back", forcedFail ? " (test hook)" : "");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
    daw_platform_init();
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    s_crash = static_cast<esp_core_dump_summary_t *>(heap_caps_calloc(1, sizeof(*s_crash), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_crash && esp_core_dump_get_summary(s_crash) != ESP_OK) {
        heap_caps_free(s_crash);
        s_crash = nullptr;
    }
    if (s_crash)
        esp_core_dump_get_panic_reason(s_panicReason, sizeof s_panicReason);
#endif
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "esp32-s3-daw %s (%s), IDF %s, %d cores, flash %u MB, PSRAM %u MB, boot #%u", app->version,
             DAW_QEMU ? "QEMU profile" : "hardware profile", app->idf_ver, chip.cores, (unsigned)(flash >> 20),
             (unsigned)(esp_psram_get_size() >> 20), (unsigned)bootCounter());
    ESP_LOGI(TAG, "plugins: %u linked", (unsigned)PluginRegistry::all().size());

#if CONFIG_DAW_SELFTEST
    {
        const bool ok = selftest::offlineRender();
        printf("selftest: offline render %s\n", ok ? "ok" : "FAILED");
        if (!ok)
            printf("DAW_SELFTEST_FAIL offline render\n");
    }
#endif

    // audio output first: the PCM5102A gets a clean clock before anything plays
    AudioOutConfig ac;
    ac.sampleRate = CONFIG_DAW_SAMPLE_RATE;
    ac.bclk = DAW_PIN_BCLK;
    ac.ws = DAW_PIN_WS;
    ac.dout = DAW_PIN_DOUT;
    ac.dmaDesc = CONFIG_DAW_I2S_DMA_DESC;
    ac.dmaFrames = CONFIG_DAW_I2S_DMA_FRAMES;
    ac.i2s = !DAW_QEMU;
    ac.dither = DAW_DITHER;
    audioOutInit(ac);

    // engine in internal RAM (it is read every sample); the session and project in PSRAM (new)
    void *mem = heap_caps_malloc(sizeof(Engine), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Engine::Worker *worker = nullptr;
#if CONFIG_DAW_PARALLEL_INSTRUMENTS
    worker = audioWorkerStart(1 - CONFIG_DAW_AUDIO_CORE, CONFIG_DAW_AUDIO_PRIORITY,
                              CONFIG_DAW_BLOCK_FRAMES * 1000000u / CONFIG_DAW_SAMPLE_RATE);
#endif
    if (worker) PluginInstance::setParallelService(audioWorkerParallel);
    // instrument jobs under 2.5 % of the block stay on the audio core (measured, docs/PERFORMANCE.md)
    s_engine = new (mem) Engine(Engine::Config{CONFIG_DAW_SAMPLE_RATE, CONFIG_DAW_BLOCK_FRAMES, DAW_QEMU ? 0u : (uint32_t)CONFIG_DAW_CPU_GUARD_PERMILLE, worker, 25});
    if (!s_engine->ok())
        ESP_LOGE(TAG, "engine buffers could not be allocated in internal RAM");

    s_projects = new PartitionFlash("projects");
    if (s_projects->ok())
        s_store = new ProjectStore(*s_projects, 8);
    else
        ESP_LOGE(TAG, "no \"projects\" partition: projects cannot be saved");
    s_sampleFlash = new PartitionFlash("samples");
    if (s_sampleFlash->ok()) {
        s_samples = new SampleStore(*s_sampleFlash);
        s_samples->mount();
        ESP_LOGI(TAG, "sample library: %u files, %u KB free", (unsigned)s_samples->list().size(), (unsigned)(s_samples->freeBytes() / 1024));
    } else
        ESP_LOGE(TAG, "no \"samples\" partition: no sample library");
    s_presetFlash = new PartitionFlash("presets");
    if (s_presetFlash->ok()) {
        s_presetStore = new SampleStore(*s_presetFlash);
        s_presets = new PresetLibrary(*s_presetStore);
        s_presets->mount();
        ESP_LOGI(TAG, "user presets: %u", (unsigned)s_presets->list().size());
    }
    DeviceSettings settings = settingsLoad();
    Session::Config sc;
    sc.deviceName = settings.hostname;
    sc.firmware = app->version;
    sc.target = DAW_QEMU ? "qemu" : "esp32s3";
    sc.store = s_store;
    sc.samples = s_samples;
    sc.presets = s_presets;
    sc.flashWritesStallAudio = true;
    sc.cpuAdmitPermille = DAW_QEMU ? 0u : (uint32_t)CONFIG_DAW_CPU_ADMIT_PERMILLE;  // QEMU timing is not real
    sc.platformDiag = platformDiag;
    sc.waitForAudio = [] { vTaskDelay(1); };
    s_session = new Session(*s_engine, sc);
    control::start(s_session);

    xTaskCreatePinnedToCore(audioTask, "daw_audio", 6144, nullptr, CONFIG_DAW_AUDIO_PRIORITY, &s_audioTask, CONFIG_DAW_AUDIO_CORE);

    // network and web
    if (settings.wifiMode != "STA" && settings.password.size() < 8 && !DAW_QEMU) {
        // never an open access point: a random password for this boot, shown on the serial console
        static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789";
        settings.password.clear();
        for (int i = 0; i < 10; i++)
            settings.password += abc[esp_random() % (sizeof abc - 1)];
        ESP_LOGW(TAG, "no Wi-Fi password configured (config/local.env): this boot's AP password is \"%s\"", settings.password.c_str());
    }
    web::netStart(settings.wifiMode.c_str(), settings.ssid.c_str(), settings.password.c_str(), settings.hostname.c_str());
    web::Callbacks cb;
    cb.onOpen = [](int c) { control::postOpen(c); };
    cb.onClose = [](int c) { control::postClose(c); };
    cb.onText = [](int c, char *d, size_t n) { return control::postText(c, d, n); };
    cb.status = [] {
        std::string out = "{}";
        control::call([&] { out = s_session->statusJson(); }, 3000);
        return out;
    };
    cb.exportProject = [](std::string &out) { return control::call([&] { out = s_session->exportProject(); }, 5000); };
    cb.importProject = [](std::string &in, std::string &err) {
        bool ok = false;
        control::call([&] { ok = s_session->importProject(in.data(), in.size(), &err); }, 10000);
        return ok;
    };
    cb.samplesJson = [] {
        std::string out = "{\"list\":[]}";
        control::call([&] { out = s_session->samplesJson(); }, 3000);
        return out;
    };
    cb.importSample = [](const std::string &name, const std::string &data, std::string &err, bool reclaimModules) {
        bool ok = false;
        if (!control::call([&] { ok = s_session->importSample(name, (const uint8_t *)data.data(), data.size(), &err, reclaimModules); }, 120000))
            err = "busy";
        return ok;
    };
    cb.sampleBegin = [](const std::string &name, size_t size, const std::string &head, std::string &err, bool reclaimModules) {
        bool ok = false;
        if (!control::call([&] { ok = s_session->importSampleBegin(name, size, (const uint8_t *)head.data(), head.size(), &err, reclaimModules); }, 30000))
            err = "busy";
        return ok;
    };
    cb.sampleAppend = [](const std::string &piece, std::string &err) {
        bool ok = false;
        if (!control::call([&] { ok = s_session->importSampleAppend((const uint8_t *)piece.data(), piece.size(), &err); }, 30000))
            err = "busy";
        return ok;
    };
    cb.sampleFinish = [](std::string &err) {
        bool ok = false;
        if (!control::call([&] { ok = s_session->importSampleFinish(&err); }, 30000))
            err = "busy";
        return ok;
    };
    cb.sampleAbort = [] { control::call([] { s_session->importSampleAbort(); }, 30000); };
    cb.settingsJson = [] {
        const DeviceSettings s = settingsLoad();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "wifiMode", s.wifiMode.c_str());
        cJSON_AddStringToObject(o, "ssid", s.ssid.c_str());
        cJSON_AddStringToObject(o, "hostname", s.hostname.c_str());
        cJSON_AddBoolToObject(o, "wifiPasswordSet", s.password.size() >= 8);
        cJSON_AddBoolToObject(o, "adminPasswordSet", !s.adminPassword.empty());
        cJSON_AddBoolToObject(o, "changedOnDevice", s.fromNvs);
        char *t = cJSON_PrintUnformatted(o);
        std::string out = t ? t : "{}";
        cJSON_free(t);
        cJSON_Delete(o);
        return out;
    };
    cb.saveSettings = [](const std::string &body, std::string &err) {
        SettingsUpdate u;
        if (!parseSettingsUpdate(body.data(), body.size(), u, &err))
            return false;
        DeviceSettings s = settingsLoad();
        if (u.wifiMode)
            s.wifiMode = *u.wifiMode;
        if (u.ssid)
            s.ssid = *u.ssid;
        if (u.wifiPassword)
            s.password = *u.wifiPassword;
        if (u.hostname)
            s.hostname = *u.hostname;
        if (u.adminPassword)
            s.adminPassword = *u.adminPassword;
        if (!settingsSave(s)) {
            err = "the settings could not be written";
            return false;
        }
        if (u.adminPassword)
            web::setAdminPassword(*u.adminPassword);  // now; Wi-Fi and the name apply after a restart
        ESP_LOGI(TAG, "device settings changed from the browser (Wi-Fi / name apply after a restart)");
        return true;
    };
    cb.beforeFlashWork = [] { control::call([] { s_session->stopForMaintenance(); }, 5000); };
    cb.factoryReset = [] {
        control::call(
            [] {
                for (uint32_t s = 0; s_store && s < s_store->slots(); s++)
                    s_store->remove(s);
                while (s_samples && !s_samples->list().empty())
                    s_samples->remove(s_samples->list().front().name);
                while (s_presetStore && !s_presetStore->list().empty())
                    s_presetStore->remove(s_presetStore->list().front().name);
                settingsErase();
            },
            10000);
    };
    if (DAW_QEMU)
        cb.testFailNextHealth = [] { settingsSetFlag("tfh"); };
    web::start(cb, CONFIG_DAW_WEB_PORT, settings.adminPassword /* empty: privileged operations disabled */);
    xTaskCreatePinnedToCore(healthTask, "health", 3072, nullptr, 3, nullptr, 0);

#if CONFIG_DAW_SELFTEST
    selftest::startLive(s_session, s_engine);
#endif
    ESP_LOGI(TAG, "running: internal heap %u B free (min %u), PSRAM %u B free", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
