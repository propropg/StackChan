/*
 * Speech + motion + LED actions, and the Claude Code event templates
 *
 * An action speaks text with VOICEVOX while playing a head motion preset and blinking the LEDs (the same
 * choreography the PC hub used to drive). Events are named actions whose templates are stored on the device
 * and edited from the settings page; the Claude Code hook on the PC only says which event happened.
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <settings.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ArduinoJson.hpp>
#include <algorithm>
#include <array>
#include <map>

using namespace stackchan;

static const std::string_view _tag = "Action";

static const char* _settings_ns        = "events";
static constexpr uint32_t _slice_ms    = 250;
static const char* _led_off_json       = R"({"leftRgbColor":"#000000","rightRgbColor":"#000000"})";

/* --------------------------------- Motion --------------------------------- */

// [yaw, pitch, speed, durationMs] steps; angles in 0.1 degree
struct MotionStep {
    int yaw, pitch, speed, ms;
};

static const std::map<std::string, std::vector<MotionStep>>& motions()
{
    static const std::map<std::string, std::vector<MotionStep>> table = {
        {"nod", {{0, 150, 500, 300}, {0, 0, 500, 300}, {0, 150, 500, 300}, {0, 0, 400, 400}}},
        {"bow", {{0, -150, 250, 900}, {0, -150, 250, 400}, {0, 0, 250, 900}}},
        {"look", {{200, 100, 300, 600}, {200, 100, 300, 900}, {0, 0, 300, 600}}},
        {"shake", {{250, 0, 600, 300}, {-250, 0, 600, 400}, {250, 0, 600, 400}, {0, 0, 400, 400}}},
    };
    return table;
}

// Merge the motion and the LED blink into one keyframe timeline of fixed slices (every dance keyframe sets
// both servos and LEDs). The LEDs blink `blinks` times, then stay lit until the timeline ends. The motion is
// played around `front` (the conversation front, 0.1 degree) so it ends facing the user.
static std::string build_timeline(const std::string& motion, const std::string& color, int blinks,
                                  uitk::Vector2i front, uint32_t& duration_ms)
{
    static const std::vector<MotionStep> none;
    auto it          = motions().find(motion);
    const auto& steps = it == motions().end() ? none : it->second;

    uint32_t motion_ms = 0;
    for (const auto& step : steps) {
        motion_ms += step.ms;
    }
    const uint32_t blink_ms = color.empty() ? 0 : blinks * 2 * _slice_ms;
    const uint32_t count    = (std::max(motion_ms, blink_ms) + _slice_ms - 1) / _slice_ms;
    duration_ms             = count * _slice_ms;
    if (count == 0) {
        return {};
    }

    ArduinoJson::JsonDocument doc;
    auto frames = doc.to<ArduinoJson::JsonArray>();
    for (uint32_t i = 0; i < count; ++i) {
        // Servo target of the motion step active at this slice (hold the last one afterwards)
        uint32_t t = i * _slice_ms, acc = 0;
        MotionStep active{0, 0, 300, 0};
        for (const auto& step : steps) {
            active = step;
            acc += step.ms;
            if (t < acc) {
                break;
            }
        }
        std::string rgb = "#000000";
        if (!color.empty() && (i >= static_cast<uint32_t>(blinks) * 2 || i % 2 == 0)) {
            rgb = color;
        }

        auto frame                     = frames.add<ArduinoJson::JsonObject>();
        frame["leftEye"]["weight"]     = 100;
        frame["rightEye"]["weight"]    = 100;
        frame["mouth"]["weight"]       = 0;
        frame["yawServo"]["angle"]     = std::clamp(front.x + active.yaw, -1280, 1280);
        frame["yawServo"]["speed"]     = active.speed;
        frame["pitchServo"]["angle"]   = std::clamp(front.y + active.pitch, 0, 900);
        frame["pitchServo"]["speed"]   = active.speed;
        frame["leftRgbColor"]          = rgb;
        frame["rightRgbColor"]         = rgb;
        frame["durationMs"]            = _slice_ms;
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return out;
}

/* --------------------------------- Action --------------------------------- */

Action_t Action_t::fromJson(ArduinoJson::JsonVariantConst json)
{
    Action_t out;
    out.text      = json["text"] | "";
    out.emotion   = json["emotion"] | "";
    out.motion    = json["motion"] | "none";
    out.bubble    = json["bubble"] | "";
    out.ledColor  = json["ledColor"] | "";
    out.ledBlinks = std::clamp(json["ledBlinks"] | 3, 0, 8);
    return out;
}

void Action_t::toJson(ArduinoJson::JsonObject json) const
{
    json["text"]      = text;
    json["emotion"]   = emotion;
    json["motion"]    = motion;
    json["bubble"]    = bubble;
    json["ledColor"]  = ledColor;
    json["ledBlinks"] = ledBlinks;
}

bool Hal::runAction(const Action_t& action, std::string& error)
{
    // Synthesize before anything moves, so a failure leaves the device untouched for the caller's fallback
    PsramBytes wav;
    if (!action.text.empty() && !voicevoxSynthesize(action.text, wav, error)) {
        return false;
    }

    uitk::Vector2i front;
    {
        LvglLockGuard lock;
        front = GetStackChan().motion().getFront();
    }
    uint32_t timeline_ms = 0;
    std::string timeline = build_timeline(action.motion, action.ledColor, action.ledBlinks, front, timeline_ms);
    const int64_t end_us = esp_timer_get_time() + static_cast<int64_t>(timeline_ms) * 1000;
    if (!timeline.empty()) {
        onWsDanceData.emit(timeline);
    }

    if (!wav.empty() && !playWav(wav.data(), wav.size(), action.emotion, action.bubble, error)) {
        return false;
    }

    if (!action.ledColor.empty()) {
        // Let the timeline finish first, or its last frame would turn the LEDs back on
        int64_t wait_us = end_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait_us / 1000 + 100));
        }
        LvglLockGuard lock;
        GetStackChan().updateNeonLightFromJson(_led_off_json);
    }
    return true;
}

/* --------------------------------- Events --------------------------------- */

static const std::array<const char*, 3> _event_names = {"stop", "notification", "sessionEnd"};

static Action_t default_event(std::string_view name)
{
    Action_t a;
    if (name == "stop") {
        a = {"クロードの処理が完了しました。{session}", "happy", "bow", "クロード完了！", "#00A800", 4};
    } else if (name == "notification") {
        a = {"クロードが確認を待っています。{session}", "doubt", "look", "クロード待ち", "#A85000", 5};
    } else {
        a = {"おつかれさまでした。またね", "happy", "bow", "Bye!", "#0030A8", 2};
    }
    return a;
}

static bool is_event_name(std::string_view name)
{
    return std::find(_event_names.begin(), _event_names.end(), name) != _event_names.end();
}

static Action_t load_event(std::string_view name)
{
    Settings settings(_settings_ns, false);
    std::string stored = settings.GetString(std::string(name), "");
    ArduinoJson::JsonDocument doc;
    if (stored.empty() || ArduinoJson::deserializeJson(doc, stored)) {
        return default_event(name);
    }
    return Action_t::fromJson(doc.as<ArduinoJson::JsonVariantConst>());
}

std::string Hal::getEventsJson()
{
    ArduinoJson::JsonDocument doc;
    doc["ok"] = true;
    for (const char* name : _event_names) {
        load_event(name).toJson(doc[name].to<ArduinoJson::JsonObject>());
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return out;
}

void Hal::setEventsJson(ArduinoJson::JsonVariantConst json)
{
    Settings settings(_settings_ns, true);
    for (const char* name : _event_names) {
        if (!json[name].is<ArduinoJson::JsonObjectConst>()) {
            continue;
        }
        ArduinoJson::JsonDocument doc;
        Action_t::fromJson(json[name]).toJson(doc.to<ArduinoJson::JsonObject>());
        std::string stored;
        ArduinoJson::serializeJson(doc, stored);
        settings.SetString(name, stored);
    }
}

bool Hal::runEvent(std::string_view name, std::string_view session, std::string& error)
{
    if (!is_event_name(name)) {
        error = "unknown event";
        return false;
    }
    if (!getVoicevoxConfig().enabled) {
        // The hook falls back to the PC voice
        error = "voicevox disabled";
        return false;
    }
    Action_t action = load_event(name);
    if (auto pos = action.text.find("{session}"); pos != std::string::npos) {
        action.text.replace(pos, 9, session);
    }
    mclog::tagInfo(_tag, "event {}: {}", name, action.text);
    return runAction(action, error);
}
