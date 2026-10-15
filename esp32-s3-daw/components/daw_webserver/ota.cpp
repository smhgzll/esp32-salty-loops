// SPDX-License-Identifier: GPL-3.0-or-later
// Browser firmware update: POST /api/ota with the application image (build/esp32_s3_daw.bin).
// The image is checked before anything is written (ESP image magic, chip, project name, firmware
// profile: a QEMU image is refused on hardware and the reverse), streamed into the other OTA slot,
// verified by esp_ota_end (segments + SHA-256), and booted. The bootloader rolls it back unless the
// new firmware confirms itself after a healthy start (main/app_main.cpp). Project storage is a
// separate partition: an update never touches it. Audio pauses during the update (flash writes).
#include <string.h>

#include <string>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_server.hpp"

namespace daw::web {

static const char *TAG = "ota";

static bool isQemuVersion(const char *v) { return strstr(v, "+qemu") != nullptr; }

const char *otaCheckImageHeader(const uint8_t *data, size_t len)
{
    const size_t descAt = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < descAt + sizeof(esp_app_desc_t))
        return "image too short";
    esp_image_header_t h;
    memcpy(&h, data, sizeof h);
    if (h.magic != ESP_IMAGE_HEADER_MAGIC)
        return "not an ESP application image";
    if (h.chip_id != ESP_CHIP_ID_ESP32S3)
        return "image is for another chip";
    esp_app_desc_t d;
    memcpy(&d, data + descAt, sizeof d);
    if (d.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "no application description";
    const esp_app_desc_t *self = esp_app_get_description();
    if (strncmp(d.project_name, self->project_name, sizeof d.project_name) != 0)
        return "image is not an esp32-s3-daw firmware";
    if (isQemuVersion(d.version) != isQemuVersion(self->version))
        return isQemuVersion(d.version) ? "QEMU firmware refused on this device" : "hardware firmware refused under QEMU";
    return nullptr;
}

static void restartSoon(void *)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const std::string &msg)
{
    if (h)
        esp_ota_abort(h);
    ESP_LOGE(TAG, "update refused: %s", msg.c_str());
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    const std::string body = "{\"error\":\"" + msg + "\"}";
    return httpd_resp_send(req, body.data(), (ssize_t)body.size());
}

esp_err_t otaHandler(httpd_req_t *req)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    if (!next)
        return fail(req, 0, "no OTA partition");
    if (req->content_len < 1024 || req->content_len > next->size)
        return fail(req, 0, "image size " + std::to_string(req->content_len) + " does not fit the " + std::to_string(next->size) + " B slot");
    static uint8_t buf[4096];  // internal RAM: flash writes from PSRAM are slow
    esp_ota_handle_t h = 0;
    size_t got = 0;
    bool checked = false;
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)req->content_len, next->label);
    while (got < req->content_len) {
        size_t want = req->content_len - got < sizeof buf ? req->content_len - got : sizeof buf;
        // the first chunk must hold the header and the app description: read it fully
        size_t fill = 0;
        while (fill < want) {
            const int r = httpd_req_recv(req, (char *)buf + fill, want - fill);
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            if (r <= 0)
                return fail(req, h, "connection lost");
            fill += (size_t)r;
        }
        if (!checked) {
            if (const char *why = otaCheckImageHeader(buf, fill))
                return fail(req, 0, why);
            if (esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
                return fail(req, 0, "esp_ota_begin failed");
            checked = true;
        }
        if (esp_ota_write(h, buf, fill) != ESP_OK)
            return fail(req, h, "flash write failed");
        got += fill;
    }
    esp_err_t err = esp_ota_end(h);  // verifies the image (segments, SHA-256)
    if (err != ESP_OK)
        return fail(req, 0, std::string("image verification failed: ") + esp_err_to_name(err));
    if (esp_ota_set_boot_partition(next) != ESP_OK)
        return fail(req, 0, "cannot select the new image");
    ESP_LOGI(TAG, "update written to %s, restarting", next->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart\":true}");
    xTaskCreate(restartSoon, "ota_restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

}  // namespace daw::web
