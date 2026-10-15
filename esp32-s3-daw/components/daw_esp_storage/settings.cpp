// SPDX-License-Identifier: GPL-3.0-or-later
#include "esp_storage.hpp"
#include "nvs.h"
#include "sdkconfig.h"

namespace daw {

static std::string getStr(nvs_handle_t h, const char *k, const char *def)
{
    char buf[96];
    size_t n = sizeof buf;
    return nvs_get_str(h, k, buf, &n) == ESP_OK ? std::string(buf) : std::string(def);
}

DeviceSettings settingsLoad()
{
    DeviceSettings s{CONFIG_DAW_WIFI_MODE, CONFIG_DAW_WIFI_SSID, CONFIG_DAW_WIFI_PASSWORD, CONFIG_DAW_HOSTNAME, CONFIG_DAW_ADMIN_PASSWORD, false};
    nvs_handle_t h;
    if (nvs_open("daw", NVS_READONLY, &h) != ESP_OK)
        return s;
    uint8_t set = 0;
    if (nvs_get_u8(h, "set", &set) == ESP_OK && set) {
        s.wifiMode = getStr(h, "mode", s.wifiMode.c_str());
        s.ssid = getStr(h, "ssid", s.ssid.c_str());
        s.password = getStr(h, "pass", s.password.c_str());
        s.hostname = getStr(h, "host", s.hostname.c_str());
        s.adminPassword = getStr(h, "admin", s.adminPassword.c_str());
        s.fromNvs = true;
    }
    nvs_close(h);
    return s;
}

bool settingsSave(const DeviceSettings &s)
{
    nvs_handle_t h;
    if (nvs_open("daw", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_str(h, "mode", s.wifiMode.c_str()) == ESP_OK && nvs_set_str(h, "ssid", s.ssid.c_str()) == ESP_OK &&
              nvs_set_str(h, "pass", s.password.c_str()) == ESP_OK && nvs_set_str(h, "host", s.hostname.c_str()) == ESP_OK &&
              nvs_set_str(h, "admin", s.adminPassword.c_str()) == ESP_OK && nvs_set_u8(h, "set", 1) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool settingsErase()
{
    nvs_handle_t h;
    if (nvs_open("daw", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool settingsSetFlag(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("daw", NVS_READWRITE, &h) != ESP_OK)
        return false;
    const bool ok = nvs_set_u8(h, key, 1) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool settingsTakeFlag(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("daw", NVS_READWRITE, &h) != ESP_OK)
        return false;
    uint8_t v = 0;
    const bool set = nvs_get_u8(h, key, &v) == ESP_OK && v;
    if (set) {
        nvs_erase_key(h, key);
        nvs_commit(h);
    }
    nvs_close(h);
    return set;
}

uint32_t bootCounter()
{
    static uint32_t value = 0;
    if (value)
        return value;
    nvs_handle_t h;
    if (nvs_open("daw", NVS_READWRITE, &h) != ESP_OK)
        return 0;
    uint32_t b = 0;
    nvs_get_u32(h, "boots", &b);
    value = b + 1;
    nvs_set_u32(h, "boots", value);
    nvs_commit(h);
    nvs_close(h);
    return value;
}

}  // namespace daw
