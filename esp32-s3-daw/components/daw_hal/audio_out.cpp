// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_out.hpp"

#include <string.h>

#include "daw/store.hpp"
#include "daw_pins.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace daw {

static const char *TAG = "audio";
static AudioOutConfig s_cfg;
static AudioKind s_kind = AudioKind::None;
static AudioOutStats s_st;
static i2s_chan_handle_t s_tx;
static Dither s_dither;
static int64_t s_t0;
static uint64_t s_hash = 0xCBF29CE484222325ull;
static volatile bool s_started;
// DMA source: interleaved 32-bit slots, internal RAM
static DRAM_ATTR int32_t s_stage[2 * 256];

static bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *)
{
    // the DMA wrapped around without new data: it sends the cleared buffer (silence)
    if (s_started)
        s_st.underruns.fetch_add(1, std::memory_order_relaxed);
    return false;
}

static esp_err_t i2sInit()
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = s_cfg.dmaDesc;
    chan.dma_frame_num = s_cfg.dmaFrames;
    chan.auto_clear = true;  // an underrun or a flash write plays silence, not a repeated buffer
    // Philips I2S (one BCLK of delay after WS, FMT=GND on the PCM5102A), 32-bit slots, stereo.
    // The PCM5102A's PLL derives its clocks from BCK (SCK tied to GND): no MCLK.
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_cfg.sampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)s_cfg.bclk,
                .ws = (gpio_num_t)s_cfg.ws,
                .dout = (gpio_num_t)s_cfg.dout,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
    };
    esp_err_t err = i2s_new_channel(&chan, &s_tx, nullptr);
    if (err != ESP_OK)
        return err;
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err == ESP_OK) {
        i2s_event_callbacks_t cbs = {};
        cbs.on_send_q_ovf = onSendQueueOverflow;
        err = i2s_channel_register_event_callback(s_tx, &cbs, nullptr);
    }
    if (err == ESP_OK)
        err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = nullptr;
    }
    return err;
}

bool audioOutInit(const AudioOutConfig &cfg)
{
    s_cfg = cfg;
    if (cfg.i2s) {
        if (const char *why = daw_pins_check(cfg.bclk, cfg.ws, cfg.dout)) {
            ESP_LOGE(TAG, "I2S pins BCLK %d WS %d DOUT %d rejected: %s; null output", cfg.bclk, cfg.ws, cfg.dout, why);
        } else {
            esp_err_t err = i2sInit();
            if (err == ESP_OK) {
                s_kind = AudioKind::I2S;
                ESP_LOGI(TAG, "I2S0 master TX: %u Hz, Philips, 32-bit slots (24-bit data), BCLK GPIO%d, WS GPIO%d, DOUT GPIO%d, DMA %u x %u",
                         (unsigned)cfg.sampleRate, cfg.bclk, cfg.ws, cfg.dout, (unsigned)cfg.dmaDesc, (unsigned)cfg.dmaFrames);
                return true;
            }
            ESP_LOGE(TAG, "I2S init failed: %s; null output", esp_err_to_name(err));
        }
    }
    s_kind = AudioKind::Null;
    s_t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "null audio output, paced at %u Hz", (unsigned)cfg.sampleRate);
    return true;
}

AudioKind audioOutKind() { return s_kind; }
AudioOutStats &audioOutStats() { return s_st; }
uint64_t audioOutHash() { return s_hash; }
void audioOutHashReset() { s_hash = 0xCBF29CE484222325ull; }

void audioOutWrite(const float *L, const float *R, uint32_t frames)
{
    const int64_t t0 = esp_timer_get_time();
    if (frames > 256)
        frames = 256;
    toI2S32(L, R, s_stage, frames, s_dither, s_cfg.dither && s_kind == AudioKind::I2S);
    // the hash covers the undithered null-backend stream (deterministic)
    if (s_kind == AudioKind::Null) {
        uint64_t h = s_hash;
        for (uint32_t i = 0; i < 2 * frames; i++) {
            uint32_t v = ((uint32_t)s_stage[i] >> 8) & 0xFFFFFFu;
            h = (h ^ (v & 0xFF)) * 0x100000001B3ull;
            h = (h ^ ((v >> 8) & 0xFF)) * 0x100000001B3ull;
            h = (h ^ (v >> 16)) * 0x100000001B3ull;
        }
        s_hash = h;
    }
    if (s_kind == AudioKind::I2S) {
        size_t wrote = 0;
        i2s_channel_write(s_tx, s_stage, frames * 2 * sizeof(int32_t), &wrote, portMAX_DELAY);
        s_started = true;
    } else {
        // paced by the clock like a DMA ring two blocks deep; far behind (an emulator) = no catch-up
        const uint64_t done = s_st.frames.load();
        const int64_t due = s_t0 + (int64_t)(done * 1000000ull / s_cfg.sampleRate);
        const int64_t lead = (int64_t)(2ull * frames * 1000000ull / s_cfg.sampleRate);
        const int64_t now = esp_timer_get_time();
        if (now > due + lead)
            s_st.late.fetch_add(1, std::memory_order_relaxed);
        else if (due - lead > now)
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((due - lead - now) / 1000)) + 1);
        else
            taskYIELD();
        if (now > due + 50 * lead)
            s_t0 = now - (int64_t)(done * 1000000ull / s_cfg.sampleRate);
    }
    s_st.frames.fetch_add(frames, std::memory_order_relaxed);
    s_st.writes.fetch_add(1, std::memory_order_relaxed);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_st.writeUsMax.load())
        s_st.writeUsMax.store(us);
}

}  // namespace daw
