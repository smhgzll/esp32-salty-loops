// SPDX-License-Identifier: GPL-3.0-or-later
#include "control.hpp"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "web_server.hpp"

namespace daw::control {

static const char *TAG = "control";

struct Msg {
    enum Type : uint8_t { Text, Open, Close, Call } type;
    int client;
    char *data;
    size_t len;
    const std::function<void()> *fn;
    SemaphoreHandle_t done;
};

static QueueHandle_t s_q;
static Session *s_session;
static volatile bool s_booted;

bool booted() { return s_booted; }

static void task(void *)
{
    s_session->setSender([](int client, const std::string &text, MsgClass cls) { web::send(client, text, cls); });
    s_session->boot();
    s_booted = true;
    ESP_LOGI(TAG, "session ready");
    Msg m;
    for (;;) {
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
            switch (m.type) {
            case Msg::Text:
                s_session->handleText(m.client, m.data, m.len);
                free(m.data);
                break;
            case Msg::Open: s_session->clientConnected(m.client); break;
            case Msg::Close: s_session->clientDisconnected(m.client); break;
            case Msg::Call:
                (*m.fn)();
                xSemaphoreGive(m.done);
                break;
            }
        }
        web::kick();
        const int resync = web::takeResync();
        if (resync >= 0)
            s_session->resync(resync);
        s_session->tick((uint32_t)(esp_timer_get_time() / 1000));
    }
}

void start(Session *s)
{
    s_session = s;
    s_q = xQueueCreate(48, sizeof(Msg));
    xTaskCreatePinnedToCore(task, "daw_ctl", 12288, nullptr, 6, nullptr, 0);
}

bool postText(int client, char *data, size_t len)
{
    Msg m{Msg::Text, client, data, len, nullptr, nullptr};
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        free(data);
        return false;
    }
    return true;
}

void postOpen(int client)
{
    Msg m{Msg::Open, client, nullptr, 0, nullptr, nullptr};
    xQueueSend(s_q, &m, pdMS_TO_TICKS(100));
}

void postClose(int client)
{
    Msg m{Msg::Close, client, nullptr, 0, nullptr, nullptr};
    xQueueSend(s_q, &m, pdMS_TO_TICKS(500));  // must not be lost: it releases held notes
}

bool call(const std::function<void()> &fn, uint32_t timeoutMs)
{
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    static SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(timeoutMs)) != pdTRUE)
        return false;
    Msg m{Msg::Call, 0, nullptr, 0, &fn, done};
    // once queued, wait for it whatever the timeout: fn lives on the caller's stack
    bool ok = xQueueSend(s_q, &m, pdMS_TO_TICKS(timeoutMs)) == pdTRUE && xSemaphoreTake(done, portMAX_DELAY) == pdTRUE;
    xSemaphoreGive(lock);
    return ok;
}

}  // namespace daw::control
