// SPDX-License-Identifier: GPL-3.0-or-later
// One instrument at a time on the other core. Task notifications sleep both the
// idle worker and the audio task waiting for its result; no render-time allocation.
#include "audio_worker.hpp"
#include <atomic>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
struct State {
    TaskHandle_t task = nullptr, caller = nullptr;
    void (*function)(void *) = nullptr;
    void *argument = nullptr;
    uint32_t blockPeriodUs = 0;
    std::atomic<bool> published{false}, done{false}, claimed{false};
} state;

void workerTask(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!state.published.exchange(false, std::memory_order_acquire)) continue;
        const int64_t began = esp_timer_get_time();
        state.function(state.argument);
        state.done.store(true, std::memory_order_release);
        xTaskNotifyGive(state.caller);
        // Normal blocks sleep in the notification wait. If the render itself is
        // over budget, the caller may immediately publish another block: let
        // lower-priority network/control tasks run so the user can stop it.
        if (state.blockPeriodUs && esp_timer_get_time() - began >= state.blockPeriodUs)
            vTaskDelay(1);
    }
}
void start(void *, void (*function)(void *), void *argument)
{
    state.claimed.store(true, std::memory_order_relaxed);
    state.caller = xTaskGetCurrentTaskHandle();
    state.function = function;
    state.argument = argument;
    state.done.store(false, std::memory_order_relaxed);
    state.published.store(true, std::memory_order_release);
    xTaskNotifyGive(state.task);
}
void join(void *)
{
    do { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); }
    while (!state.done.load(std::memory_order_acquire));
    state.claimed.store(false, std::memory_order_release);
}
daw::Engine::Worker worker{nullptr, start, join};
}

daw::Engine::Worker *audioWorkerStart(unsigned core, unsigned priority, uint32_t blockPeriodUs)
{
    state.blockPeriodUs = blockPeriodUs;
    if (!state.task && xTaskCreatePinnedToCore(workerTask, "daw_dsp", 6144, nullptr,
                                              priority, &state.task, core) != pdPASS) return nullptr;
    return &worker;
}
unsigned audioWorkerStackFree() { return state.task ? uxTaskGetStackHighWaterMark(state.task) : 0; }

// Share the existing task; never launch a nested job from the worker or disturb an engine job.
int32_t audioWorkerParallel(void *, void (*local)(void *), void *localArg,
                             void (*remote)(void *), void *remoteArg)
{
    if (!state.task || xTaskGetCurrentTaskHandle() == state.task) return 0;
    bool free = false;
    if (!state.claimed.compare_exchange_strong(free, true, std::memory_order_acq_rel)) return 0;
    start(nullptr, remote, remoteArg);
    local(localArg);
    join(nullptr);
    return 1;
}
