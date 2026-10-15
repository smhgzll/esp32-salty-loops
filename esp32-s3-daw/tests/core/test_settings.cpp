// SPDX-License-Identifier: GPL-3.0-or-later
// Device settings updates from the browser: what is accepted, what is refused.
#include <string.h>

#include <string>

#include "check.hpp"
#include "daw/settings_update.hpp"

using namespace daw;

static bool ok(const char *j, SettingsUpdate *u = nullptr)
{
    SettingsUpdate tmp;
    std::string err;
    return parseSettingsUpdate(j, strlen(j), u ? *u : tmp, &err);
}

int main()
{
    SettingsUpdate u;
    CHECK(ok(R"({"wifiMode":"STA","ssid":"Home Net","wifiPassword":"correct horse","hostname":"studio-daw"})", &u));
    CHECK(u.wifiMode == "STA" && u.ssid == "Home Net" && u.wifiPassword == "correct horse" && u.hostname == "studio-daw" && !u.adminPassword);
    CHECK(ok(R"({})", &u) && !u.wifiMode && !u.ssid);
    CHECK(ok(R"({"wifiPassword":""})"));  // open STA network / random AP password
    CHECK(ok(R"({"adminPassword":"longenough"})"));
    CHECK(!ok(R"([1,2])"));
    CHECK(!ok(R"({"wifiMode":"MESH"})"));
    CHECK(!ok(R"({"ssid":""})"));
    CHECK(!ok(R"({"ssid":"123456789012345678901234567890123"})"));  // 33
    CHECK(!ok(R"({"wifiPassword":"short"})"));
    CHECK(!ok(R"({"hostname":"Studio"})"));
    CHECK(!ok(R"({"hostname":"-daw"})"));
    CHECK(!ok(R"({"hostname":"my_daw"})"));
    CHECK(!ok(R"({"adminPassword":""})"));  // cannot be removed from the browser
    CHECK(!ok(R"({"adminPassword":"1234567"})"));
    CHECK(!ok(R"({"ssid":5})"));
    CHECK(!ok("{\"ssid\":\"a\\u0001b\"}"));
    TEST_MAIN_END("test_settings");
}
