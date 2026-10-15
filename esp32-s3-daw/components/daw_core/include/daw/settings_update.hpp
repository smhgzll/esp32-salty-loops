// SPDX-License-Identifier: GPL-3.0-or-later
// Device settings changed from the browser (POST /api/settings, admin token): parsing and the
// rules, shared by the ESP32-S3 and the Linux host. Absent fields stay as they are; passwords are
// write-only (never returned). The admin password can be changed but not removed this way.
#pragma once

#include <stddef.h>

#include <optional>
#include <string>

namespace daw {

struct SettingsUpdate {
    std::optional<std::string> wifiMode;   // "AP" or "STA"
    std::optional<std::string> ssid;       // 1..32 bytes, printable
    std::optional<std::string> wifiPassword;  // "" (AP: random per boot; STA: open network) or 8..63
    std::optional<std::string> hostname;   // 1..32 of a-z 0-9 -, not starting or ending with -
    std::optional<std::string> adminPassword;  // 8..64
};

bool parseSettingsUpdate(const char *json, size_t len, SettingsUpdate &out, std::string *err);

}  // namespace daw
