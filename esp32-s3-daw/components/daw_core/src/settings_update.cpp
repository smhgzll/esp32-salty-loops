// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/settings_update.hpp"

#include "cJSON.h"

namespace daw {

static bool printable(const std::string &s)
{
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F)
            return false;
    return true;
}

bool parseSettingsUpdate(const char *json, size_t len, SettingsUpdate &out, std::string *err)
{
    auto fail = [&](const char *m) {
        if (err)
            *err = m;
        return false;
    };
    out = SettingsUpdate{};
    cJSON *j = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return fail("not a JSON object");
    }
    auto get = [&](const char *k, std::optional<std::string> &dst) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
        if (cJSON_IsString(v))
            dst = std::string(v->valuestring);
        return !v || cJSON_IsString(v);
    };
    const bool types = get("wifiMode", out.wifiMode) && get("ssid", out.ssid) && get("wifiPassword", out.wifiPassword) &&
                       get("hostname", out.hostname) && get("adminPassword", out.adminPassword);
    cJSON_Delete(j);
    if (!types)
        return fail("settings are strings");
    if (out.wifiMode && *out.wifiMode != "AP" && *out.wifiMode != "STA")
        return fail("Wi-Fi mode: AP or STA");
    if (out.ssid && (out.ssid->empty() || out.ssid->size() > 32 || !printable(*out.ssid)))
        return fail("network name (SSID): 1 to 32 characters");
    if (out.wifiPassword && !out.wifiPassword->empty() &&
        (out.wifiPassword->size() < 8 || out.wifiPassword->size() > 63 || !printable(*out.wifiPassword)))
        return fail("Wi-Fi password: 8 to 63 characters (or empty)");
    if (out.hostname) {
        const std::string &h = *out.hostname;
        bool ok = !h.empty() && h.size() <= 32 && h.front() != '-' && h.back() != '-';
        for (char c : h)
            ok &= (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok)
            return fail("device name: 1 to 32 of a-z, 0-9 and -");
    }
    if (out.adminPassword && (out.adminPassword->size() < 8 || out.adminPassword->size() > 64 || !printable(*out.adminPassword)))
        return fail("admin password: 8 to 64 characters");
    return true;
}

}  // namespace daw
