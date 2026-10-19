/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <wifi_manager.h>
#include <board.h>
#include <mutex>
#include <queue>
#include <vector>
#include <ctime>
#include <sys/time.h>
#include <esp_sntp.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <ping/ping_sock.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>

static std::string _tag           = "Network";
static bool _is_network_connected = false;

/* ------------------------------ Link watchdog ----------------------------- */
// The station sometimes stays associated while the link is dead (ARP and DNS go unanswered for hours), and
// no disconnect event ever comes to trigger the reconnect. Ping the gateway once a minute and drop the
// association after a few failed rounds; the Wi-Fi manager then reconnects as after any disconnect.

static constexpr int _watchdog_interval_ms   = 60 * 1000;
static constexpr int _watchdog_fail_rounds   = 3;
static constexpr uint32_t _pings_per_round   = 4;

static void ping_on_end(esp_ping_handle_t, void* args)
{
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(args));
}

// Returns the number of replies, or -1 when the gateway is unknown or the ping could not start
static int ping_gateway(SemaphoreHandle_t done)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.gw.addr == 0) {
        return -1;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr.type      = IPADDR_TYPE_V4;
    config.target_addr.u_addr.ip4.addr = ip_info.gw.addr;
    config.count                 = _pings_per_round;
    config.interval_ms           = 500;
    config.timeout_ms            = 1000;
    config.task_stack_size       = 3072;

    esp_ping_callbacks_t callbacks = {};
    callbacks.cb_args              = done;
    callbacks.on_ping_end          = ping_on_end;

    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&config, &callbacks, &ping) != ESP_OK) {
        return -1;
    }
    esp_ping_start(ping);
    xSemaphoreTake(done, pdMS_TO_TICKS(_pings_per_round * 1500 + 2000));
    esp_ping_stop(ping);
    uint32_t replies = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &replies, sizeof(replies));
    esp_ping_delete_session(ping);
    return static_cast<int>(replies);
}

static void link_watchdog_task(void*)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    bool gateway_answers   = false;  // some routers drop ICMP; never act on those
    int failed_rounds      = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(_watchdog_interval_ms));
        if (!WifiManager::GetInstance().IsConnected()) {
            failed_rounds = 0;
            continue;
        }
        int replies = ping_gateway(done);
        if (replies > 0) {
            gateway_answers = true;
            failed_rounds   = 0;
            continue;
        }
        if (replies < 0 || !gateway_answers) {
            continue;
        }
        failed_rounds++;
        mclog::tagWarn(_tag, "gateway unreachable ({}/{})", failed_rounds, _watchdog_fail_rounds);
        if (failed_rounds >= _watchdog_fail_rounds) {
            mclog::tagWarn(_tag, "link looks dead, reconnecting Wi-Fi");
            failed_rounds = 0;
            esp_wifi_disconnect();  // the Wi-Fi manager reconnects on the disconnect event
        }
    }
}

static void start_link_watchdog()
{
    static bool started = false;
    if (started) {
        return;
    }
    started = xTaskCreate(link_watchdog_task, "link_wdt", 3072, nullptr, 2, nullptr) == pdPASS;
    if (!started) {
        mclog::tagError(_tag, "failed to start the link watchdog");
    }
}

static void time_sync_notification_cb(struct timeval* tv)
{
    mclog::tagInfo(_tag, "SNTP time synchronized");
    GetHAL().syncSystemTimeToRtc();
}

void Hal::startSntp()
{
    mclog::tagInfo(_tag, "SNTP init");

    if (esp_sntp_enabled()) {
    } else {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_setservername(2, "cn.pool.ntp.org");

        sntp_set_time_sync_notification_cb(time_sync_notification_cb);

        esp_sntp_init();
    }
}

void Hal::startNetwork(std::function<void(std::string_view)> onLog)
{
    if (_is_network_connected) {
        mclog::tagInfo(_tag, "network already connected");
        return;
    }

    std::atomic<bool> network_connected = false;

    auto& board = Board::GetInstance();
    mclog::tagInfo(_tag, "start and wait for network connected...");

    board.SetNetworkEventCallback([&network_connected, &onLog](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Scanning:
                if (onLog) {
                    onLog("WiFi scanning...");
                }
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    if (onLog) {
                        onLog("WiFi connecting...");
                    }
                } else {
                    if (onLog) {
                        onLog(fmt::format("Connecting to {} ...", data));
                    }
                }
                break;
            }
            case NetworkEvent::Connected: {
                network_connected = true;
                break;
            }
            case NetworkEvent::Disconnected:
                break;
            case NetworkEvent::WifiConfigModeEnter: {
                auto& wifi_manager = WifiManager::GetInstance();
                auto msg = fmt::format("Enter WiFi config mode. Hotspot: {}, Config URL: {}", wifi_manager.GetApSsid(),
                                       wifi_manager.GetApWebUrl());
                if (onLog) {
                    onLog(msg);
                }
                break;
            }
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                break;
            case NetworkEvent::ModemErrorNoSim:
                break;
            case NetworkEvent::ModemErrorRegDenied:
                break;
            case NetworkEvent::ModemErrorInitFailed:
                break;
            case NetworkEvent::ModemErrorTimeout:
                break;
        }
    });
    board.StartNetwork();

    while (!network_connected) {
        GetHAL().delay(500);
    }
    mclog::tagInfo(_tag, "network connected");
    board.SetNetworkEventCallback(nullptr);

    startSntp();
    start_link_watchdog();

    _is_network_connected = true;
}

WifiStatus Hal::getWifiStatus()
{
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return WifiStatus::None;
    }
    if (!wifi.IsConnected()) {
        return WifiStatus::None;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return WifiStatus::High;
    } else if (rssi >= -75) {
        return WifiStatus::Medium;
    }
    return WifiStatus::Low;
}
