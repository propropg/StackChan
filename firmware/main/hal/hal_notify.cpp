/*
 * Gmail / Google Calendar notifications
 *
 * The Apps Script web app in tools/gas/stackchan_notify.gs collects new mails and upcoming events from the
 * user's Google account; StackChan polls it and reads the items aloud with VOICEVOX:
 *
 *   GET <web app url>?token=..  -> {"ok":true,"items":[{"type":"gmail","text":"<to speak>","bubble":".."}]}
 *
 * Chat items also carry "notice" (who sent it, no message text), spoken instead of "text" unless
 * NotifyConfig_t::chatReadContent is on.
 *
 * Polling only happens while the AVATAR app is open, so announcements never talk over the AI agent.
 */
#include "hal.h"
#include "utils/https_get.h"
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <settings.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ArduinoJson.hpp>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>

using namespace stackchan;

static const std::string_view _tag = "Notify";

static const char* _settings_ns              = "notify";
static constexpr size_t _max_response_size   = 64 * 1024;
static constexpr uint32_t _bubble_only_ms    = 8000;

static TaskHandle_t _task = nullptr;
static std::atomic<bool> _active{false};
static std::mutex _poll_mutex;  // the task and "check now" never poll at the same time
static std::mutex _status_mutex;
static NotifyStatus_t _status;

/* -------------------------------- Settings -------------------------------- */

NotifyConfig_t Hal::getNotifyConfig()
{
    NotifyConfig_t out;
    Settings settings(_settings_ns, false);
    out.enabled     = settings.GetBool("enabled", out.enabled);
    out.url         = settings.GetString("url", out.url);
    out.intervalSec = settings.GetInt("interval", out.intervalSec);
    out.quietHours  = settings.GetBool("quiet", out.quietHours);
    out.quietStart  = settings.GetInt("quiet_start", out.quietStart);
    out.quietEnd    = settings.GetInt("quiet_end", out.quietEnd);
    out.chatReadContent = settings.GetBool("chat_body", out.chatReadContent);
    out.lookAtFace      = settings.GetBool("look_face", out.lookAtFace);
    return out;
}

void Hal::setNotifyConfig(const NotifyConfig_t& config)
{
    Settings settings(_settings_ns, true);
    settings.SetBool("enabled", config.enabled);
    settings.SetString("url", config.url);
    settings.SetInt("interval", std::clamp(config.intervalSec, 60, 3600));
    settings.SetBool("quiet", config.quietHours);
    settings.SetInt("quiet_start", std::clamp(config.quietStart, 0, 23));
    settings.SetInt("quiet_end", std::clamp(config.quietEnd, 0, 23));
    settings.SetBool("chat_body", config.chatReadContent);
    settings.SetBool("look_face", config.lookAtFace);
    mclog::tagInfo(_tag, "config saved: enabled={}, url={}, interval={}s", config.enabled,
                   config.url.empty() ? "none" : "set", config.intervalSec);
    // Apply the new interval / enable right away
    if (_task) {
        xTaskNotifyGive(_task);
    }
}

NotifyStatus_t Hal::getNotifyStatus()
{
    std::lock_guard<std::mutex> lock(_status_mutex);
    return _status;
}

/* ---------------------------------- Poll ---------------------------------- */

static bool in_quiet_hours(const NotifyConfig_t& config)
{
    if (!config.quietHours || config.quietStart == config.quietEnd) {
        return false;
    }
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2024) {
        return false;  // clock not synced yet
    }
    int hour = local.tm_hour;
    if (config.quietStart < config.quietEnd) {
        return hour >= config.quietStart && hour < config.quietEnd;
    }
    return hour >= config.quietStart || hour < config.quietEnd;  // spans midnight, e.g. 22-7
}

// Speak one item; when VOICEVOX is unavailable fall back to showing the bubble
static void announce(const std::string& text, const std::string& bubble)
{
    std::string error;
    if (GetHAL().speakText(text, "happy", bubble, error)) {
        return;
    }
    mclog::tagWarn(_tag, "voicevox failed ({}), showing the bubble only", error);
    LvglLockGuard lock;
    auto& stackchan = GetStackChan();
    if (stackchan.hasAvatar()) {
        stackchan.addModifier(std::make_unique<TimedEmotionModifier>(avatar::Emotion::Happy, _bubble_only_ms));
        stackchan.addModifier(std::make_unique<TimedSpeechModifier>(bubble.empty() ? text : bubble, _bubble_only_ms));
    }
}

// What to say for one item. Chat messages are announced without their text unless the user opts in; the
// relay's "notice" names the sender, older relays without it get a generic line.
static std::string spoken_text(ArduinoJson::JsonVariantConst item, bool chat_read_content)
{
    if (item["type"] == "chat" && !chat_read_content) {
        return item["notice"].is<const char*>() ? item["notice"].as<std::string>() : "チャットが届いたよ。";
    }
    return item["text"].as<std::string>();
}

static bool poll(const NotifyConfig_t& config, int& count, std::string& error)
{
    std::lock_guard<std::mutex> poll_lock(_poll_mutex);
    count = 0;

    bool ok = false;
    std::vector<std::pair<std::string, std::string>> items;
    if (GetHAL().getWifiStatus() == WifiStatus::None) {
        error = "Wi-Fi not connected";
    } else {
        PsramBytes body;
        int status = 0;
        if (https_get(config.url, _max_response_size, body, status, error)) {
            ArduinoJson::JsonDocument doc;
            if (status != 200) {
                error = "HTTP " + std::to_string(status);
            } else if (ArduinoJson::deserializeJson(doc, reinterpret_cast<const char*>(body.data()), body.size())) {
                // Apps Script serves an HTML error page when the deployment is not public
                error = "not a JSON response (is the web app deployed for Anyone?)";
            } else if (!doc["ok"].as<bool>()) {
                error = doc["error"].is<const char*>() ? doc["error"].as<std::string>() : "relay error";
            } else {
                for (auto item : doc["items"].as<ArduinoJson::JsonArray>()) {
                    if (item["text"].is<const char*>()) {
                        items.emplace_back(spoken_text(item, config.chatReadContent),
                                           item["bubble"].is<const char*>() ? item["bubble"].as<std::string>() : "");
                    }
                }
                ok = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(_status_mutex);
        _status.lastCheckUs = esp_timer_get_time();
        _status.lastCount   = items.size();
        _status.lastError   = ok ? "" : error;
    }
    if (!ok) {
        mclog::tagError(_tag, "poll failed: {}", error);
        return false;
    }

    count = items.size();
    if (count > 0) {
        mclog::tagInfo(_tag, "{} new items", count);
    }
    // Start looking for the user while the first item is still being synthesized
    const bool look = count > 0 && config.lookAtFace;
    if (look) {
        GetHAL().beginFaceTracking();
    }
    for (const auto& [text, bubble] : items) {
        announce(text, bubble);
    }
    if (look) {
        GetHAL().endFaceTracking();
    }
    return true;
}

static void notify_task(void*)
{
    int64_t last_poll_us = 0;
    while (true) {
        // Wake up every few seconds (or when the config changes) and poll when the interval has passed
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

        const auto config = GetHAL().getNotifyConfig();
        if (!_active || !config.enabled || config.url.empty()) {
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (last_poll_us != 0 && now - last_poll_us < static_cast<int64_t>(config.intervalSec) * 1000000) {
            continue;
        }
        last_poll_us = now;
        if (in_quiet_hours(config)) {
            continue;  // the relay keeps unread mails; they are announced after the quiet hours
        }
        int count = 0;
        std::string error;
        poll(config, count, error);
    }
}

void Hal::startNotifyService()
{
    if (_task) {
        return;
    }
    // TLS + JSON need a roomy stack
    if (xTaskCreate(notify_task, "notify", 10240, nullptr, 2, &_task) != pdPASS) {
        mclog::tagError(_tag, "failed to start the notify task");
        _task = nullptr;
    }
}

void Hal::setNotifyActive(bool active)
{
    _active = active;
}

bool Hal::checkNotificationsNow(int& count, std::string& error)
{
    const auto config = getNotifyConfig();
    if (config.url.empty()) {
        error = "relay URL not set";
        return false;
    }
    return poll(config, count, error);
}
