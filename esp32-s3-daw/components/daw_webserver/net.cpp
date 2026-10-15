// SPDX-License-Identifier: GPL-3.0-or-later
// Network bring-up: Wi-Fi (AP, or STA with an AP fallback) on hardware, OpenCores Ethernet in QEMU.
// The radio and lwIP run on core 0; the audio task owns core 1.
#include <string.h>

#include <string>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include "web_server.hpp"

#if CONFIG_ETH_USE_OPENETH
#include "esp_eth.h"
#else
#include "esp_wifi.h"
#endif
#if CONFIG_DAW_MDNS && __has_include("mdns.h")
#include "mdns.h"
#define DAW_HAVE_MDNS 1
#endif

namespace daw::web {

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static char s_ip[20];
static std::string s_mode, s_ssid, s_pass, s_host;
#define EV_IP BIT0

static void announce()
{
#ifdef DAW_HAVE_MDNS
    static bool done;
    if (!done && mdns_init() == ESP_OK) {
        mdns_hostname_set(s_host.c_str());
        mdns_instance_name_set("ESP32-S3 DAW");
        mdns_service_add("ESP32-S3 DAW", "_http", "_tcp", CONFIG_DAW_WEB_PORT, nullptr, 0);
        done = true;
        ESP_LOGI(TAG, "mDNS: http://%s.local/", s_host.c_str());
    }
#endif
}

static void onIp(void *, esp_event_base_t, int32_t, void *data)
{
    auto *e = static_cast<ip_event_got_ip_t *>(data);
    snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "IP %s: the DAW is at http://%s/", s_ip, s_ip);
    announce();
    xEventGroupSetBits(s_ev, EV_IP);
}

#if CONFIG_ETH_USE_OPENETH
static esp_err_t netEth()
{
    eth_mac_config_t macCfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phyCfg = ETH_PHY_DEFAULT_CONFIG();
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&cfg);
    esp_netif_set_hostname(netif, s_host.c_str());
    phyCfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&macCfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phyCfg);
    esp_eth_config_t ethCfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&ethCfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, onIp, nullptr));
    ESP_LOGI(TAG, "Ethernet (QEMU OpenCores MAC)");
    return esp_eth_start(eth);
}
#else
static int s_retries;
static bool s_fallback;

static void copyField(uint8_t *dst, size_t cap, const std::string &s) { memcpy(dst, s.data(), s.size() < cap ? s.size() : cap); }

static void startAp()
{
    wifi_config_t ap = {};
    const std::string &ssid = s_fallback ? s_host : s_ssid;
    copyField(ap.ap.ssid, sizeof ap.ap.ssid, ssid);
    ap.ap.ssid_len = (uint8_t)(ssid.size() < sizeof ap.ap.ssid ? ssid.size() : sizeof ap.ap.ssid);
    copyField(ap.ap.password, sizeof ap.ap.password - 1, s_pass);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;  // never open: the password is checked by the caller (>= 8)
    if (s_fallback) {
        esp_wifi_stop();
        esp_netif_create_default_wifi_ap();
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (s_fallback)
        esp_wifi_start();
}

static void onWifi(void *, esp_event_base_t, int32_t id, void *)
{
    if (id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_retries <= 8) {
            ESP_LOGW(TAG, "\"%s\": not connected, retry %d", s_ssid.c_str(), s_retries);
            esp_wifi_connect();
        } else if (!s_fallback) {
            ESP_LOGW(TAG, "\"%s\" unreachable: starting the device's own access point \"%s\"", s_ssid.c_str(), s_host.c_str());
            s_fallback = true;
            startAp();
        }
    } else if (id == WIFI_EVENT_AP_START) {
        snprintf(s_ip, sizeof s_ip, "192.168.4.1");
        ESP_LOGI(TAG, "access point up: http://192.168.4.1/");
        announce();
        xEventGroupSetBits(s_ev, EV_IP);
    }
}

static esp_err_t netWifi()
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifi, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onIp, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // credentials come from the build / NVS settings
    if (s_mode == "STA") {
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(sta, s_host.c_str());
        wifi_config_t c = {};
        copyField(c.sta.ssid, sizeof c.sta.ssid, s_ssid);
        copyField(c.sta.password, sizeof c.sta.password - 1, s_pass);
        c.sta.threshold.authmode = s_pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
        ESP_LOGI(TAG, "Wi-Fi: joining \"%s\" as %s", s_ssid.c_str(), s_host.c_str());
    } else {
        esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        startAp();
        ESP_LOGI(TAG, "Wi-Fi: access point \"%s\"", s_ssid.c_str());
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // power save adds tens of ms to every control message
    return ESP_OK;
}
#endif

esp_err_t netStart(const char *mode, const char *ssid, const char *password, const char *hostname)
{
    s_mode = mode;
    s_ssid = ssid;
    s_pass = password;
    s_host = hostname;
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        return e;
#if CONFIG_ETH_USE_OPENETH
    return netEth();
#else
    return netWifi();
#endif
}

const char *netAddress(uint32_t waitMs)
{
    if (s_ev)
        xEventGroupWaitBits(s_ev, EV_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(waitMs));
    return s_ip;
}

}  // namespace daw::web
