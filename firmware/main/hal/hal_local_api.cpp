/*
 * Local HTTP API for StackChan
 *
 * Lets a PC on the same LAN make StackChan speak, move and change expression.
 *
 *   GET  /api/status                               -> {"ok":true,"ip":"..."}
 *   POST /api/say?text=..&emotion=happy[&voice=wav] body: WAV (16-bit PCM, mono/stereo, any rate) or empty
 *   POST /api/motion                               body: {"yaw":deg,"pitch":deg,"speed":100-1000}
 *                                                     or the native {"yawServo":{...},"pitchServo":{...}}
 *   POST /api/emotion?name=happy&ms=3000
 *   POST /api/dance                                body: keyframe array (same format as the Dance app)
 *   POST /api/avatar                               body: native avatar json (leftEye/rightEye/mouth)
 *   POST /api/led                                  body: {"leftRgbColor":"#RRGGBB","rightRgbColor":"#RRGGBB"}
 *
 *   GET  /api/settings                             -> volume, brightness, timezone, idle animation settings, device info
 *   POST /api/settings                             body: any subset of the settings above (saved to flash)
 *   GET  /api/state                                -> current head angles
 *   POST /api/servo                                body: {"power":bool} | {"calibrateZero":true} | {"resetCalibration":true}
 *   POST /api/reboot
 *
 *   GET  /                                         -> settings page (VOICEVOX, volume, idle animations)
 *   POST /api/voicevox/test                        body: {"text":".."} (optional), speaks with the saved settings
 *   GET  /api/voicevox/speakers                    -> speaker list (proxied, the key stays on the device)
 *   GET  /api/voicevox/points                      -> {"points":..,"resetInHours":..}
 *   POST /api/notify/check                         -> polls the Gmail / Calendar relay now, {"ok":true,"count":n}
 *   POST /api/face/test?seconds=15                 looks for a face for a while (0 stops); status in GET /api/settings
 *   GET  /api/face/frame.bmp                       -> the camera frame as the face detector sees it
 *   POST /api/action                               body: {"text","emotion","motion","bubble","ledColor","ledBlinks"}
 *   GET  /api/events                               -> Claude Code event templates (stop / notification / sessionEnd)
 *   POST /api/events                               body: any of the templates above (saved to flash)
 *   POST /api/event?name=stop&session=..           runs a template; 409 when VOICEVOX is off (the hook falls back)
 *
 * /api/say voices: text only -> cloud VOICEVOX. Text + WAV -> VOICEVOX when enabled in settings, the WAV
 * when VOICEVOX is disabled or fails (add voice=wav to always use the WAV).
 *
 * Handlers only take effect while the AVATAR app is open (its signal handlers are connected there).
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <mdns.h>
#include <settings.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ArduinoJson.hpp>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

using namespace stackchan;

static const std::string_view _tag = "LocalAPI";

static httpd_handle_t _server = nullptr;
static std::mutex _audio_mutex;

static constexpr size_t _max_body_size       = 3 * 1024 * 1024;  // ~60s of 24kHz mono 16-bit audio
static constexpr size_t _max_json_size       = 32 * 1024;
static constexpr size_t _playback_chunk_size = 1024;

/* --------------------------------- Helpers -------------------------------- */

static esp_err_t send_json(httpd_req_t* req, const char* status, const std::string& json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

static esp_err_t send_error(httpd_req_t* req, const char* status, std::string_view message)
{
    ArduinoJson::JsonDocument doc;
    doc["ok"]    = false;
    doc["error"] = message;
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return send_json(req, status, out);
}

static esp_err_t send_ok(httpd_req_t* req)
{
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static std::string url_decode(const char* src)
{
    std::string out;
    for (size_t i = 0; src[i]; ++i) {
        if (src[i] == '+') {
            out += ' ';
        } else if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            out += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += src[i];
        }
    }
    return out;
}

static std::string get_query_param(httpd_req_t* req, const char* key)
{
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) {
        return {};
    }
    std::vector<char> query(len + 1);
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
        return {};
    }
    std::vector<char> value(len + 1);
    if (httpd_query_key_value(query.data(), key, value.data(), value.size()) != ESP_OK) {
        return {};
    }
    return url_decode(value.data());
}

// Read the whole request body into a PSRAM buffer. Caller frees with heap_caps_free.
static uint8_t* read_body(httpd_req_t* req, size_t max_size, size_t& out_len, const char*& error)
{
    out_len = 0;
    if (req->content_len == 0) {
        error = "empty body";
        return nullptr;
    }
    if (req->content_len > max_size) {
        error = "body too large";
        return nullptr;
    }

    auto* buffer =
        static_cast<uint8_t*>(heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) {
        error = "out of memory";
        return nullptr;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, reinterpret_cast<char*>(buffer + received), req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            heap_caps_free(buffer);
            error = "receive failed";
            return nullptr;
        }
        received += ret;
    }
    buffer[received] = 0;  // Null terminate so json bodies can be used as c strings
    out_len          = received;
    return buffer;
}

static bool parse_emotion(std::string_view name, avatar::Emotion& emotion)
{
    if (name == "neutral") emotion = avatar::Emotion::Neutral;
    else if (name == "happy") emotion = avatar::Emotion::Happy;
    else if (name == "angry") emotion = avatar::Emotion::Angry;
    else if (name == "sad") emotion = avatar::Emotion::Sad;
    else if (name == "doubt") emotion = avatar::Emotion::Doubt;
    else if (name == "sleepy") emotion = avatar::Emotion::Sleepy;
    else return false;
    return true;
}

/* ----------------------------------- WAV ---------------------------------- */

struct WavInfo {
    uint16_t channels      = 0;
    uint32_t sampleRate    = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t* data    = nullptr;
    size_t dataSize        = 0;
};

static uint32_t read_u32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t read_u16(const uint8_t* p)
{
    return p[0] | (p[1] << 8);
}

static bool parse_wav(const uint8_t* buf, size_t len, WavInfo& info)
{
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool has_fmt = false;
    size_t pos   = 12;
    while (pos + 8 <= len) {
        const uint8_t* chunk = buf + pos;
        uint32_t chunk_size  = read_u32(chunk + 4);
        size_t body          = pos + 8;

        if (memcmp(chunk, "fmt ", 4) == 0 && body + 16 <= len) {
            uint16_t format    = read_u16(buf + body);
            info.channels      = read_u16(buf + body + 2);
            info.sampleRate    = read_u32(buf + body + 4);
            info.bitsPerSample = read_u16(buf + body + 14);
            // 1 = PCM, 0xFFFE = WAVE_FORMAT_EXTENSIBLE (PCM sub format assumed)
            if (format != 1 && format != 0xFFFE) {
                return false;
            }
            has_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info.data     = buf + body;
            info.dataSize = std::min<size_t>(chunk_size, len - body);
            break;
        }

        pos = body + chunk_size + (chunk_size & 1);
    }

    return has_fmt && info.data && info.dataSize >= 4u * info.channels && info.bitsPerSample == 16 && info.channels >= 1 && info.channels <= 2 &&
           info.sampleRate >= 8000 && info.sampleRate <= 48000;
}

// Downmix to mono and linearly resample to the codec output rate
static std::vector<int16_t, PsramAllocator<int16_t>> to_output_pcm(const WavInfo& info, int out_rate)
{
    const size_t in_frames = info.dataSize / (2 * info.channels);
    const auto* in         = reinterpret_cast<const int16_t*>(info.data);

    auto sample_at = [&](size_t frame) -> int32_t {
        if (info.channels == 1) {
            return in[frame];
        }
        return (static_cast<int32_t>(in[frame * 2]) + in[frame * 2 + 1]) / 2;
    };

    const size_t out_frames = static_cast<size_t>(static_cast<uint64_t>(in_frames) * out_rate / info.sampleRate);
    std::vector<int16_t, PsramAllocator<int16_t>> out(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        uint64_t src_pos_q16 = (static_cast<uint64_t>(i) * info.sampleRate << 16) / out_rate;
        size_t idx           = src_pos_q16 >> 16;
        int32_t frac         = src_pos_q16 & 0xFFFF;
        int32_t a            = sample_at(std::min(idx, in_frames - 1));
        int32_t b            = sample_at(std::min(idx + 1, in_frames - 1));
        out[i]               = static_cast<int16_t>(a + (((b - a) * frac) >> 16));
    }
    return out;
}

// Play mono PCM at the codec output rate while flapping the mouth, with optional emotion / speech bubble.
// Blocks until playback finishes.
// Write silence to the codec. Used to let the amp settle after opening the output and to flush the DMA
// buffer before closing it; otherwise the start and the last ~60ms of a clip get cut off.
static void write_silence(AudioCodec* codec, uint32_t ms)
{
    const int channels = std::max(codec->output_channels(), 1);
    size_t frames      = static_cast<size_t>(codec->output_sample_rate()) * ms / 1000;
    std::vector<int16_t> chunk;
    while (frames > 0) {
        size_t n = std::min(frames, _playback_chunk_size);
        chunk.assign(n * channels, 0);
        codec->OutputData(chunk);
        frames -= n;
    }
}

static void speak_pcm(const std::vector<int16_t, PsramAllocator<int16_t>>& pcm, AudioCodec* codec,
                      const std::string& text, const std::string& emotion_name, bool animate_mouth = true,
                      bool keep_output_open = false)
{
    const uint32_t duration_ms = pcm.size() * 1000ULL / codec->output_sample_rate();
    mclog::tagInfo(_tag, "say: {} ms, emotion: {}, text: {}", duration_ms, emotion_name, text);

    std::lock_guard<std::mutex> audio_lock(_audio_mutex);

    // Face: mouth flapping + optional emotion / speech bubble for the length of the audio
    {
        LvglLockGuard lock;
        auto& stackchan = GetStackChan();
        if (stackchan.hasAvatar()) {
            // Talk toward the user. Skipped while the face tracker / IMU hold the head or an action motion plays.
            auto& motion = stackchan.motion();
            if (motion.hasFront() && !motion.isModifyLocked() && !motion.isMoving()) {
                motion.goFront(250);
            }
            if (animate_mouth) {
                stackchan.addModifier(std::make_unique<SpeakingModifier>(duration_ms));
            }
            avatar::Emotion emotion;
            if (!emotion_name.empty() && parse_emotion(emotion_name, emotion)) {
                stackchan.addModifier(std::make_unique<TimedEmotionModifier>(emotion, duration_ms + 1500));
            }
            if (!text.empty()) {
                stackchan.addModifier(std::make_unique<TimedSpeechModifier>(text, duration_ms + 1500));
            }
        }
    }

    // Blocks until playback finishes, so the client knows when StackChan is done talking
    const int channels = std::max(codec->output_channels(), 1);
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
        write_silence(codec, 100);
    }
    std::vector<int16_t> chunk;
    chunk.reserve(_playback_chunk_size * channels);
    for (size_t pos = 0; pos < pcm.size(); pos += _playback_chunk_size) {
        size_t n = std::min(_playback_chunk_size, pcm.size() - pos);
        chunk.clear();
        for (size_t i = 0; i < n; ++i) {
            for (int c = 0; c < channels; ++c) {
                chunk.push_back(pcm[pos + i]);
            }
        }
        codec->OutputData(chunk);
    }
    if (!keep_output_open) {
        write_silence(codec, 150);
        codec->EnableOutput(false);
    }

}

// Decode a WAV buffer and speak it. Returns nullptr on success or an error message.
static const char* play_wav(const uint8_t* buf, size_t len, const std::string& text, const std::string& emotion_name)
{
    WavInfo wav;
    if (!parse_wav(buf, len, wav)) {
        return "expected 16-bit PCM WAV (8k-48kHz, mono/stereo)";
    }
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        return "audio codec unavailable";
    }
    std::vector<int16_t, PsramAllocator<int16_t>> pcm;
    try {
        pcm = to_output_pcm(wav, codec->output_sample_rate());
    } catch (const std::bad_alloc&) {
        return "out of memory";
    }
    speak_pcm(pcm, codec, text, emotion_name);
    return nullptr;
}

bool Hal::playWav(const uint8_t* data, size_t size, const std::string& emotion, const std::string& bubble,
                  std::string& error)
{
    if (const char* play_error = play_wav(data, size, bubble, emotion)) {
        error = play_error;
        return false;
    }
    return true;
}

bool Hal::speakText(const std::string& text, const std::string& emotion, const std::string& bubble, std::string& error)
{
    PsramBytes wav;
    if (!voicevoxSynthesize(text, wav, error)) {
        return false;
    }
    if (const char* play_error = play_wav(wav.data(), wav.size(), bubble, emotion)) {
        error = play_error;
        return false;
    }
    return true;
}

/* -------------------------------- Handlers -------------------------------- */

static const char* reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        default:                return "unknown";
    }
}

static esp_err_t handle_status(httpd_req_t* req)
{
    ArduinoJson::JsonDocument doc;
    doc["ok"]                = true;
    doc["ip"]                = GetHAL().getLocalIp();
    doc["uptime_s"]          = esp_timer_get_time() / 1000000;
    doc["reset_reason"]      = reset_reason_name(esp_reset_reason());
    doc["heap_internal"]     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    doc["heap_internal_min"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    doc["heap_internal_max"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    doc["heap_psram"]        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    {
        LvglLockGuard lock;
        doc["avatar"] = GetStackChan().hasAvatar();
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_say(httpd_req_t* req)
{
    std::string text         = get_query_param(req, "text");
    std::string emotion_name = get_query_param(req, "emotion");
    std::string voice        = get_query_param(req, "voice");  // "wav" skips VOICEVOX even when it is enabled

    // Text only: VOICEVOX is the only voice there is
    if (req->content_len == 0) {
        if (text.empty()) {
            return send_error(req, "400 Bad Request", "send text= and/or a WAV body");
        }
        std::string error;
        if (!GetHAL().speakText(text, emotion_name, text, error)) {
            return send_error(req, "502 Bad Gateway", "voicevox: " + error);
        }
        return send_json(req, "200 OK", "{\"ok\":true,\"voice\":\"voicevox\"}");
    }

    size_t len        = 0;
    const char* error = nullptr;
    uint8_t* body     = read_body(req, _max_body_size, len, error);
    if (!body) {
        return send_error(req, "400 Bad Request", error);
    }

    // Text + WAV: prefer VOICEVOX when enabled, the WAV is the fallback voice
    std::string voicevox_error;
    if (!text.empty() && voice != "wav" && GetHAL().getVoicevoxConfig().enabled) {
        if (GetHAL().speakText(text, emotion_name, text, voicevox_error)) {
            heap_caps_free(body);
            return send_json(req, "200 OK", "{\"ok\":true,\"voice\":\"voicevox\"}");
        }
        mclog::tagWarn(_tag, "voicevox failed ({}), falling back to the WAV body", voicevox_error);
    }

    error = play_wav(body, len, text, emotion_name);
    heap_caps_free(body);
    if (error) {
        return send_error(req, "400 Bad Request", error);
    }

    ArduinoJson::JsonDocument doc;
    doc["ok"]    = true;
    doc["voice"] = "wav";
    if (!voicevox_error.empty()) {
        doc["voicevoxError"] = voicevox_error;
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_motion(httpd_req_t* req)
{
    size_t len        = 0;
    const char* error = nullptr;
    uint8_t* body     = read_body(req, _max_json_size, len, error);
    if (!body) {
        return send_error(req, "400 Bad Request", error);
    }

    ArduinoJson::JsonDocument doc;
    auto err = ArduinoJson::deserializeJson(doc, reinterpret_cast<const char*>(body), len);
    if (err) {
        heap_caps_free(body);
        return send_error(req, "400 Bad Request", err.c_str());
    }

    std::string payload;
    if (doc["yawServo"].is<ArduinoJson::JsonObject>() || doc["pitchServo"].is<ArduinoJson::JsonObject>()) {
        // Native format, pass through
        payload.assign(reinterpret_cast<const char*>(body), len);
    } else {
        // Simple format in degrees -> native format (servo angles are in 0.1 degree)
        int speed = doc["speed"].is<int>() ? doc["speed"].as<int>() : 150;
        speed     = std::clamp(speed, 100, 1000);
        ArduinoJson::JsonDocument native;
        if (doc["yaw"].is<int>()) {
            native["yawServo"]["angle"] = std::clamp(doc["yaw"].as<int>(), -128, 128) * 10;
            native["yawServo"]["speed"] = speed;
        }
        if (doc["pitch"].is<int>()) {
            native["pitchServo"]["angle"] = std::clamp(doc["pitch"].as<int>(), 0, 90) * 10;
            native["pitchServo"]["speed"] = speed;
        }
        ArduinoJson::serializeJson(native, payload);
    }
    heap_caps_free(body);

    GetHAL().onWsMotionData.emit(payload);
    return send_ok(req);
}

static esp_err_t handle_emotion(httpd_req_t* req)
{
    std::string name = get_query_param(req, "name");
    std::string ms   = get_query_param(req, "ms");

    avatar::Emotion emotion;
    if (!parse_emotion(name, emotion)) {
        return send_error(req, "400 Bad Request", "name must be neutral/happy/angry/sad/doubt/sleepy");
    }
    uint32_t duration_ms = ms.empty() ? 3000 : std::clamp(atoi(ms.c_str()), 0, 600000);

    LvglLockGuard lock;
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return send_error(req, "409 Conflict", "open the AVATAR app first");
    }
    if (duration_ms == 0) {
        stackchan.avatar().setEmotion(emotion);
    } else {
        stackchan.addModifier(std::make_unique<TimedEmotionModifier>(emotion, duration_ms));
    }
    return send_ok(req);
}

// Shared handler for endpoints that forward a JSON body to an existing signal
template <typename Fn>
static esp_err_t forward_json(httpd_req_t* req, Fn&& forward)
{
    size_t len        = 0;
    const char* error = nullptr;
    uint8_t* body     = read_body(req, _max_json_size, len, error);
    if (!body) {
        return send_error(req, "400 Bad Request", error);
    }
    std::string payload(reinterpret_cast<const char*>(body), len);
    heap_caps_free(body);

    forward(payload);
    return send_ok(req);
}

static esp_err_t handle_dance(httpd_req_t* req)
{
    return forward_json(req, [](const std::string& payload) { GetHAL().onWsDanceData.emit(payload); });
}

static esp_err_t handle_avatar(httpd_req_t* req)
{
    return forward_json(req, [](const std::string& payload) { GetHAL().onWsAvatarData.emit(payload); });
}

static esp_err_t handle_led(httpd_req_t* req)
{
    return forward_json(req, [](const std::string& payload) {
        LvglLockGuard lock;
        GetStackChan().updateNeonLightFromJson(payload.c_str());
    });
}

/* ----------------------------- Idle animations ---------------------------- */

static const char* _settings_ns = "localapi";

struct IdleSettings {
    int motionLevel = 2;  // 0 off, 1 low (8-12s), 2 normal (4-8s), 3 high (2-4s); shared with the AI agent setting
    bool blink      = true;
    bool breath     = true;
    bool expression = true;
    bool headPet    = true;
    bool imu        = true;
};

static IdleSettings load_idle_settings()
{
    IdleSettings out;
    out.motionLevel = GetHAL().getXiaozhiConfig().idleRandomMovementLevel;
    Settings settings(_settings_ns, false);
    out.blink      = settings.GetBool("idle_blink", true);
    out.breath     = settings.GetBool("idle_breath", true);
    out.expression = settings.GetBool("idle_expr", true);
    out.headPet    = settings.GetBool("idle_headpet", true);
    out.imu        = settings.GetBool("idle_imu", true);
    return out;
}

static void save_idle_settings(const IdleSettings& idle)
{
    auto config = GetHAL().getXiaozhiConfig();
    if (config.idleRandomMovementLevel != idle.motionLevel) {
        config.idleRandomMovementLevel = idle.motionLevel;
        GetHAL().setXiaozhiConfig(config);
    }
    Settings settings(_settings_ns, true);
    settings.SetBool("idle_blink", idle.blink);
    settings.SetBool("idle_breath", idle.breath);
    settings.SetBool("idle_expr", idle.expression);
    settings.SetBool("idle_headpet", idle.headPet);
    settings.SetBool("idle_imu", idle.imu);
}

static std::vector<int> _idle_modifier_ids;

void Hal::applyIdleModifiers()
{
    auto& stackchan = GetStackChan();
    for (int id : _idle_modifier_ids) {
        stackchan.removeModifier(id);
    }
    _idle_modifier_ids.clear();

    // Idle motion centres on the conversation front
    loadFaceFront();

    const auto idle = load_idle_settings();
    auto add        = [&](std::unique_ptr<Modifier> modifier) {
        _idle_modifier_ids.push_back(stackchan.addModifier(std::move(modifier)));
    };
    if (idle.breath) add(std::make_unique<BreathModifier>());
    if (idle.blink) add(std::make_unique<BlinkModifier>());
    if (idle.headPet) add(std::make_unique<HeadPetModifier>());
    if (idle.imu) add(std::make_unique<ImuEventModifier>());
    if (idle.expression) add(std::make_unique<IdleExpressionModifier>());
    switch (idle.motionLevel) {
        case 1: add(std::make_unique<IdleMotionModifier>(8000, 12000)); break;
        case 2: add(std::make_unique<IdleMotionModifier>(4000, 8000)); break;
        case 3: add(std::make_unique<IdleMotionModifier>(2000, 4000)); break;
        default: break;
    }
    mclog::tagInfo(_tag, "idle modifiers applied: motion level {}, {} modifiers", idle.motionLevel,
                   _idle_modifier_ids.size());
}

/* ------------------------- Settings / control API ------------------------- */

static void face_status_to_json(ArduinoJson::JsonObject json);

static esp_err_t handle_get_settings(httpd_req_t* req)
{
    const auto idle = load_idle_settings();
    ArduinoJson::JsonDocument doc;
    doc["ok"]              = true;
    doc["volume"]          = GetHAL().getSpeakerVolume();
    doc["brightness"]      = GetHAL().getBackLightBrightness();
    doc["timezone"]        = GetHAL().getTimezone();
    doc["idleMotionLevel"] = idle.motionLevel;
    doc["blink"]           = idle.blink;
    doc["breath"]          = idle.breath;
    doc["idleExpression"]  = idle.expression;
    doc["headPet"]         = idle.headPet;
    doc["imu"]             = idle.imu;

    // The API key itself never leaves the device; only whether it is set and its last characters
    const auto voicevox       = GetHAL().getVoicevoxConfig();
    auto vv                   = doc["voicevox"].to<ArduinoJson::JsonObject>();
    vv["enabled"]             = voicevox.enabled;
    vv["apiKeySet"]           = !voicevox.apiKey.empty();
    vv["apiKeyHint"]          = voicevox.apiKey.size() > 4 ? voicevox.apiKey.substr(voicevox.apiKey.size() - 4) : "";
    vv["speaker"]             = voicevox.speaker;
    vv["speed"]               = voicevox.speed;
    vv["pitch"]               = voicevox.pitch;
    vv["intonation"]          = voicevox.intonation;
    vv["greetingText"]        = voicevox.greetingText;

    // The relay url carries the access token, so only its tail is shown
    const auto notify    = GetHAL().getNotifyConfig();
    const auto nstatus   = GetHAL().getNotifyStatus();
    auto nt              = doc["notify"].to<ArduinoJson::JsonObject>();
    nt["enabled"]        = notify.enabled;
    nt["urlSet"]         = !notify.url.empty();
    nt["urlHint"]        = notify.url.size() > 6 ? notify.url.substr(notify.url.size() - 6) : "";
    nt["intervalSec"]    = notify.intervalSec;
    nt["quietHours"]     = notify.quietHours;
    nt["quietStart"]     = notify.quietStart;
    nt["quietEnd"]       = notify.quietEnd;
    nt["chatReadContent"] = notify.chatReadContent;
    nt["lookAtFace"]     = notify.lookAtFace;
    nt["lastCheckAgoSec"] = nstatus.lastCheckUs == 0 ? -1 : static_cast<int>((esp_timer_get_time() - nstatus.lastCheckUs) / 1000000);
    nt["lastCount"]      = nstatus.lastCount;
    nt["lastError"]      = nstatus.lastError;
    face_status_to_json(doc["face"].to<ArduinoJson::JsonObject>());

    auto info         = doc["device"].to<ArduinoJson::JsonObject>();
    info["ip"]        = GetHAL().getLocalIp();
    info["firmware"]  = esp_app_get_description()->version;
    info["battery"]   = GetHAL().getBatteryLevel();
    info["charging"]  = GetHAL().isBatteryCharging();
    info["freeHeap"]  = esp_get_free_heap_size();
    info["uptimeSec"] = static_cast<uint32_t>(esp_timer_get_time() / 1000000);

    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return send_json(req, "200 OK", out);
}

// Parse a small JSON request body; returns false (and sends the error response) on failure
static bool read_json_body(httpd_req_t* req, ArduinoJson::JsonDocument& doc)
{
    size_t len        = 0;
    const char* error = nullptr;
    uint8_t* body     = read_body(req, _max_json_size, len, error);
    if (!body) {
        send_error(req, "400 Bad Request", error);
        return false;
    }
    auto err = ArduinoJson::deserializeJson(doc, reinterpret_cast<const char*>(body), len);
    heap_caps_free(body);
    if (err) {
        send_error(req, "400 Bad Request", err.c_str());
        return false;
    }
    return true;
}

static esp_err_t handle_post_settings(httpd_req_t* req)
{
    ArduinoJson::JsonDocument doc;
    if (!read_json_body(req, doc)) {
        return ESP_OK;
    }

    if (doc["volume"].is<int>()) {
        GetHAL().setSpeakerVolume(std::clamp(doc["volume"].as<int>(), 0, 100), true);
    }
    if (doc["brightness"].is<int>()) {
        GetHAL().setBackLightBrightness(std::clamp(doc["brightness"].as<int>(), 1, 100), true);
    }
    if (doc["timezone"].is<const char*>()) {
        GetHAL().setTimezone(doc["timezone"].as<std::string>());
    }

    if (auto vv = doc["voicevox"]; vv.is<ArduinoJson::JsonObject>()) {
        auto voicevox = GetHAL().getVoicevoxConfig();
        if (vv["enabled"].is<bool>()) voicevox.enabled = vv["enabled"].as<bool>();
        if (vv["apiKey"].is<const char*>()) voicevox.apiKey = vv["apiKey"].as<std::string>();  // "" clears it
        if (vv["speaker"].is<int>()) voicevox.speaker = vv["speaker"].as<int>();
        if (vv["speed"].is<float>()) voicevox.speed = vv["speed"].as<float>();
        if (vv["pitch"].is<float>()) voicevox.pitch = vv["pitch"].as<float>();
        if (vv["intonation"].is<float>()) voicevox.intonation = vv["intonation"].as<float>();
        if (vv["greetingText"].is<const char*>()) voicevox.greetingText = vv["greetingText"].as<std::string>();
        GetHAL().setVoicevoxConfig(voicevox);
    }

    if (auto nt = doc["notify"]; nt.is<ArduinoJson::JsonObject>()) {
        if (nt["url"].is<const char*>()) {
            std::string url = nt["url"].as<std::string>();
            if (!url.empty() && url.find("/exec?token=") == std::string::npos) {
                return send_error(req, "400 Bad Request", "relay url must end with /exec?token=<token>");
            }
        }
        auto notify = GetHAL().getNotifyConfig();
        if (nt["enabled"].is<bool>()) notify.enabled = nt["enabled"].as<bool>();
        if (nt["url"].is<const char*>()) notify.url = nt["url"].as<std::string>();  // "" clears it
        if (nt["intervalSec"].is<int>()) notify.intervalSec = nt["intervalSec"].as<int>();
        if (nt["quietHours"].is<bool>()) notify.quietHours = nt["quietHours"].as<bool>();
        if (nt["quietStart"].is<int>()) notify.quietStart = nt["quietStart"].as<int>();
        if (nt["quietEnd"].is<int>()) notify.quietEnd = nt["quietEnd"].as<int>();
        if (nt["chatReadContent"].is<bool>()) notify.chatReadContent = nt["chatReadContent"].as<bool>();
        if (nt["lookAtFace"].is<bool>()) notify.lookAtFace = nt["lookAtFace"].as<bool>();
        GetHAL().setNotifyConfig(notify);
    }
    if (doc["face"]["resetFront"].as<bool>()) {
        GetHAL().resetFaceFront();
    }

    auto idle         = load_idle_settings();
    bool idle_changed = false;
    if (doc["idleMotionLevel"].is<int>()) {
        idle.motionLevel = std::clamp(doc["idleMotionLevel"].as<int>(), 0, 3);
        idle_changed     = true;
    }
    auto read_bool = [&](const char* key, bool& target) {
        if (doc[key].is<bool>()) {
            target       = doc[key].as<bool>();
            idle_changed = true;
        }
    };
    read_bool("blink", idle.blink);
    read_bool("breath", idle.breath);
    read_bool("idleExpression", idle.expression);
    read_bool("headPet", idle.headPet);
    read_bool("imu", idle.imu);

    if (idle_changed) {
        save_idle_settings(idle);
        LvglLockGuard lock;
        if (GetStackChan().hasAvatar()) {
            GetHAL().applyIdleModifiers();
        }
    }
    return handle_get_settings(req);
}

static esp_err_t handle_state(httpd_req_t* req)
{
    ArduinoJson::JsonDocument doc;
    {
        LvglLockGuard lock;
        auto& motion  = GetStackChan().motion();
        doc["ok"]     = true;
        doc["yaw"]    = motion.yawServo().getCurrentAngle() / 10;
        doc["pitch"]  = motion.pitchServo().getCurrentAngle() / 10;
        doc["moving"] = motion.isMoving();
        doc["avatar"] = GetStackChan().hasAvatar();
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_servo(httpd_req_t* req)
{
    ArduinoJson::JsonDocument doc;
    if (!read_json_body(req, doc)) {
        return ESP_OK;
    }

    if (doc["power"].is<bool>()) {
        GetHAL().setServoPowerEnabled(doc["power"].as<bool>());
    }
    if (doc["calibrateZero"].is<bool>() && doc["calibrateZero"].as<bool>()) {
        // Same as SETUP > Servo: the current physical position becomes the new center
        LvglLockGuard lock;
        GetStackChan().motion().yawServo().setCurrentAngleAsZero();
        GetStackChan().motion().pitchServo().setCurrentAngleAsZero();
    }
    if (doc["resetCalibration"].is<bool>() && doc["resetCalibration"].as<bool>()) {
        LvglLockGuard lock;
        GetStackChan().motion().yawServo().resetZeroCalibration();
        GetStackChan().motion().pitchServo().resetZeroCalibration();
    }
    return send_ok(req);
}

/* -------------------------------- VOICEVOX -------------------------------- */

// Speak a test sentence with the saved settings: {"text":"..."} (optional)
static esp_err_t handle_voicevox_test(httpd_req_t* req)
{
    std::string text = "こんにちは、スタックチャンだよ。";
    if (req->content_len > 0) {
        ArduinoJson::JsonDocument doc;
        if (!read_json_body(req, doc)) {
            return ESP_OK;
        }
        if (doc["text"].is<const char*>() && !doc["text"].as<std::string>().empty()) {
            text = doc["text"].as<std::string>();
        }
    }
    std::string error;
    if (!GetHAL().speakText(text, "happy", text, error)) {
        return send_error(req, "502 Bad Gateway", error);
    }
    return send_ok(req);
}

// Proxy the speaker list / remaining points so the browser never needs the API key
static esp_err_t handle_voicevox_json(httpd_req_t* req)
{
    std::string out;
    std::string error;
    if (!GetHAL().voicevoxFetchJson(static_cast<const char*>(req->user_ctx), out, error)) {
        return send_error(req, "502 Bad Gateway", error);
    }
    return send_json(req, "200 OK", out);
}

/* ---------------------------- Actions / events ---------------------------- */

// {"text","emotion","motion","bubble","ledColor","ledBlinks"}: speak with VOICEVOX + motion + LEDs
static esp_err_t handle_action(httpd_req_t* req)
{
    ArduinoJson::JsonDocument doc;
    if (!read_json_body(req, doc)) {
        return ESP_OK;
    }
    std::string error;
    if (!GetHAL().runAction(Action_t::fromJson(doc.as<ArduinoJson::JsonVariantConst>()), error)) {
        return send_error(req, "502 Bad Gateway", error);
    }
    return send_ok(req);
}

static esp_err_t handle_get_events(httpd_req_t* req)
{
    return send_json(req, "200 OK", GetHAL().getEventsJson());
}

static esp_err_t handle_post_events(httpd_req_t* req)
{
    ArduinoJson::JsonDocument doc;
    if (!read_json_body(req, doc)) {
        return ESP_OK;
    }
    GetHAL().setEventsJson(doc.as<ArduinoJson::JsonVariantConst>());
    return handle_get_events(req);
}

// ?name=stop|notification|sessionEnd&session=<session title>
static esp_err_t handle_event(httpd_req_t* req)
{
    std::string error;
    if (!GetHAL().runEvent(get_query_param(req, "name"), get_query_param(req, "session"), error)) {
        const char* status = error == "unknown event" ? "400 Bad Request" : error == "voicevox disabled" ? "409 Conflict" : "502 Bad Gateway";
        return send_error(req, status, error);
    }
    return send_ok(req);
}

/* ------------------------------ Notifications ----------------------------- */

// Poll the Gmail / Calendar relay now and speak what it returns
static esp_err_t handle_notify_check(httpd_req_t* req)
{
    int count = 0;
    std::string error;
    if (!GetHAL().checkNotificationsNow(count, error)) {
        return send_error(req, "502 Bad Gateway", error);
    }
    return send_json(req, "200 OK", "{\"ok\":true,\"count\":" + std::to_string(count) + "}");
}

/* ------------------------------ Face tracking ----------------------------- */

static void face_status_to_json(ArduinoJson::JsonObject json)
{
    const auto face     = GetHAL().getFaceTrackStatus();
    json["modelLoaded"] = face.modelLoaded;
    json["error"]       = face.error;
    json["active"]      = face.active;
    json["inferMs"]     = face.inferMs;
    json["faces"]       = face.faces;
    json["lastSeenAgoMs"] = face.lastSeenUs == 0 ? -1 : static_cast<int>((esp_timer_get_time() - face.lastSeenUs) / 1000);
    json["lastX"]       = face.lastX;
    json["lastY"]       = face.lastY;
    json["hasFront"]    = face.hasFront;
    json["frontYaw"]    = face.frontYaw / 10;  // degrees
    json["frontPitch"]  = face.frontPitch / 10;
}

// Track faces for ?seconds= (default 15, 0 stops) so the camera direction can be checked without a notification
static esp_err_t handle_face_test(httpd_req_t* req)
{
    std::string seconds = get_query_param(req, "seconds");
    int sec             = seconds.empty() ? 15 : std::clamp(atoi(seconds.c_str()), 0, 120);
    {
        LvglLockGuard lock;
        if (sec > 0 && !GetStackChan().hasAvatar()) {
            return send_error(req, "409 Conflict", "open the AVATAR app first");
        }
    }
    GetHAL().trackFaceFor(sec * 1000);

    ArduinoJson::JsonDocument doc;
    doc["ok"] = true;
    face_status_to_json(doc["face"].to<ArduinoJson::JsonObject>());
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return send_json(req, "200 OK", out);
}

// What the camera sees, as the face detector gets it
static esp_err_t handle_face_frame(httpd_req_t* req)
{
    PsramBytes bmp;
    std::string error;
    if (!GetHAL().getFaceCameraBmp(bmp, error)) {
        return send_error(req, "503 Service Unavailable", error);
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(bmp.data()), bmp.size());
}

/* ------------------------------ Settings page ----------------------------- */

extern const uint8_t _settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t _settings_html_end[] asm("_binary_settings_html_end");

static esp_err_t handle_settings_page(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, reinterpret_cast<const char*>(_settings_html_start),
                           _settings_html_end - _settings_html_start);
}

static void reboot_task(void*)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handle_reboot(httpd_req_t* req)
{
    send_ok(req);
    xTaskCreate(reboot_task, "reboot", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

/* ------------------------------ Boot greeting ----------------------------- */

extern const uint8_t _boot_jingle_wav_start[] asm("_binary_boot_jingle_wav_start");
extern const uint8_t _boot_jingle_wav_end[] asm("_binary_boot_jingle_wav_end");
extern const uint8_t _boot_greeting_wav_start[] asm("_binary_boot_greeting_wav_start");
extern const uint8_t _boot_greeting_wav_end[] asm("_binary_boot_greeting_wav_end");

// Jingle: quick LED color sweep while the head rises (angles in 0.1 degree), then
// nod with green blinks during the greeting; the LEDs stay green until it ends
static const char* _boot_dance = R"([
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":800},"pitchServo":{"angle":60,"speed":800},"leftRgbColor":"#A80000","rightRgbColor":"#A80000","durationMs":80},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":800},"pitchServo":{"angle":100,"speed":800},"leftRgbColor":"#A8A800","rightRgbColor":"#A8A800","durationMs":80},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":800},"pitchServo":{"angle":130,"speed":800},"leftRgbColor":"#00A800","rightRgbColor":"#00A800","durationMs":80},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":800},"pitchServo":{"angle":150,"speed":800},"leftRgbColor":"#0040A8","rightRgbColor":"#0040A8","durationMs":80},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":400},"pitchServo":{"angle":150,"speed":400},"leftRgbColor":"#787878","rightRgbColor":"#787878","durationMs":300},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":500},"pitchServo":{"angle":0,"speed":500},"leftRgbColor":"#000000","rightRgbColor":"#000000","durationMs":250},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":500},"pitchServo":{"angle":150,"speed":500},"leftRgbColor":"#00A800","rightRgbColor":"#00A800","durationMs":250},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":500},"pitchServo":{"angle":0,"speed":500},"leftRgbColor":"#000000","rightRgbColor":"#000000","durationMs":250},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":500},"pitchServo":{"angle":150,"speed":500},"leftRgbColor":"#00A800","rightRgbColor":"#00A800","durationMs":250},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":400},"pitchServo":{"angle":0,"speed":400},"leftRgbColor":"#000000","rightRgbColor":"#000000","durationMs":250},
  {"leftEye":{"weight":100},"rightEye":{"weight":100},"mouth":{"weight":0},"yawServo":{"angle":0,"speed":400},"pitchServo":{"angle":0,"speed":400},"leftRgbColor":"#00A800","rightRgbColor":"#00A800","durationMs":250}
])";

static void play_embedded_wav(const uint8_t* start, const uint8_t* end, const char* emotion, bool animate_mouth,
                              bool keep_output_open)
{
    auto codec = Board::GetInstance().GetAudioCodec();
    WavInfo wav;
    if (!codec || !parse_wav(start, end - start, wav)) {
        mclog::tagError(_tag, "boot greeting: invalid embedded wav");
        return;
    }
    try {
        auto pcm = to_output_pcm(wav, codec->output_sample_rate());
        // Empty text keeps the IP address bubble that the AVATAR app shows on open
        speak_pcm(pcm, codec, "", emotion, animate_mouth, keep_output_open);
    } catch (const std::bad_alloc&) {
        mclog::tagError(_tag, "boot greeting: out of memory");
    }
}

// Greeting synthesized with VOICEVOX while the jingle plays, so the cloud round trip is mostly hidden
struct GreetingJob {
    std::string text;
    PsramBytes wav;
    std::string error;
    bool ok                 = false;
    SemaphoreHandle_t done  = nullptr;
};

static void greeting_synth_task(void* arg)
{
    auto* job = static_cast<GreetingJob*>(arg);
    job->ok   = GetHAL().voicevoxSynthesize(job->text, job->wav, job->error);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

static void boot_greeting_task(void*)
{
    // Let the AVATAR app finish opening and connect its signal handlers
    vTaskDelay(pdMS_TO_TICKS(800));

    // Kick off the VOICEVOX greeting first; the embedded greeting is the fallback
    std::unique_ptr<GreetingJob> job;
    if (const auto config = GetHAL().getVoicevoxConfig(); config.enabled && !config.greetingText.empty()) {
        job       = std::make_unique<GreetingJob>();
        job->text = config.greetingText;
        job->done = xSemaphoreCreateBinary();
        if (xTaskCreate(greeting_synth_task, "greeting_tts", 8192, job.get(), 3, nullptr) != pdPASS) {
            vSemaphoreDelete(job->done);
            job.reset();
        }
    }

    {
        LvglLockGuard lock;
        GetStackChan().leftNeonLight().setDuration(0.05f);
        GetStackChan().rightNeonLight().setDuration(0.05f);
    }
    GetHAL().onWsDanceData.emit(std::string(_boot_dance));

    play_embedded_wav(_boot_jingle_wav_start, _boot_jingle_wav_end, "happy", false, true);
    if (auto codec = Board::GetInstance().GetAudioCodec(); codec && codec->output_enabled()) {
        // Pause after the jingle so the greeting does not run into it
        write_silence(codec, 500);
        if (job) {
            // The VOICEVOX greeting may still be on its way; do not leave the amp open meanwhile
            codec->EnableOutput(false);
        }
    }
    bool greeted = false;
    if (job) {
        // The synth task owns the job until it signals; never abandon it, the 15s http timeout bounds the wait
        xSemaphoreTake(job->done, portMAX_DELAY);
        vSemaphoreDelete(job->done);
        if (job->ok) {
            // Empty text keeps the IP address bubble that the AVATAR app shows on open
            greeted = play_wav(job->wav.data(), job->wav.size(), "", "happy") == nullptr;
        } else {
            mclog::tagWarn(_tag, "voicevox greeting failed ({}), using the built-in voice", job->error);
        }
        job.reset();
    }
    if (!greeted) {
        play_embedded_wav(_boot_greeting_wav_start, _boot_greeting_wav_end, "happy", true, false);
    }

    {
        LvglLockGuard lock;
        GetStackChan().leftNeonLight().setColor(0, 0, 0);
        GetStackChan().rightNeonLight().setColor(0, 0, 0);
    }
    vTaskDelete(nullptr);
}

void Hal::playBootGreeting()
{
    xTaskCreate(boot_greeting_task, "boot_greeting", 8192, nullptr, 3, nullptr);
}

/* ---------------------------------- Setup --------------------------------- */

std::string Hal::getLocalIp()
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return {};
    }
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
    return buf;
}

static void start_mdns()
{
    static bool started = false;
    if (started) {
        return;
    }

    if (mdns_init() != ESP_OK) {
        mclog::tagError("LocalApi", "failed to init mdns");
        return;
    }
    mdns_hostname_set("stackchan");
    mdns_instance_name_set("Stack-chan");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    started = true;

    mclog::tagInfo("LocalApi", "mdns started: http://stackchan.local/");
}

void Hal::startLocalApiService(std::function<void(std::string_view)> onStartLog)
{
    mclog::tagInfo(_tag, "start local api service");

    startNetwork(onStartLog);

    if (_server) {
        return;
    }

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = 12288;  // TLS to the VOICEVOX cloud runs inside handlers
    config.max_uri_handlers = 24;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    if (httpd_start(&_server, &config) != ESP_OK) {
        mclog::tagError(_tag, "failed to start http server");
        _server = nullptr;
        return;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = nullptr},
        {.uri = "/api/say", .method = HTTP_POST, .handler = handle_say, .user_ctx = nullptr},
        {.uri = "/api/motion", .method = HTTP_POST, .handler = handle_motion, .user_ctx = nullptr},
        {.uri = "/api/emotion", .method = HTTP_POST, .handler = handle_emotion, .user_ctx = nullptr},
        {.uri = "/api/dance", .method = HTTP_POST, .handler = handle_dance, .user_ctx = nullptr},
        {.uri = "/api/avatar", .method = HTTP_POST, .handler = handle_avatar, .user_ctx = nullptr},
        {.uri = "/api/led", .method = HTTP_POST, .handler = handle_led, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = handle_get_settings, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = handle_post_settings, .user_ctx = nullptr},
        {.uri = "/api/state", .method = HTTP_GET, .handler = handle_state, .user_ctx = nullptr},
        {.uri = "/api/servo", .method = HTTP_POST, .handler = handle_servo, .user_ctx = nullptr},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot, .user_ctx = nullptr},
        {.uri = "/api/voicevox/test", .method = HTTP_POST, .handler = handle_voicevox_test, .user_ctx = nullptr},
        {.uri = "/api/voicevox/speakers", .method = HTTP_GET, .handler = handle_voicevox_json, .user_ctx = (void*)"speakers"},
        {.uri = "/api/voicevox/points", .method = HTTP_GET, .handler = handle_voicevox_json, .user_ctx = (void*)"points"},
        {.uri = "/api/notify/check", .method = HTTP_POST, .handler = handle_notify_check, .user_ctx = nullptr},
        {.uri = "/api/face/test", .method = HTTP_POST, .handler = handle_face_test, .user_ctx = nullptr},
        {.uri = "/api/face/frame.bmp", .method = HTTP_GET, .handler = handle_face_frame, .user_ctx = nullptr},
        {.uri = "/api/action", .method = HTTP_POST, .handler = handle_action, .user_ctx = nullptr},
        {.uri = "/api/events", .method = HTTP_GET, .handler = handle_get_events, .user_ctx = nullptr},
        {.uri = "/api/events", .method = HTTP_POST, .handler = handle_post_events, .user_ctx = nullptr},
        {.uri = "/api/event", .method = HTTP_POST, .handler = handle_event, .user_ctx = nullptr},
        {.uri = "/", .method = HTTP_GET, .handler = handle_settings_page, .user_ctx = nullptr},
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(_server, &uri);
    }

    start_mdns();

    mclog::tagInfo(_tag, "local api listening on http://{}/", getLocalIp());
}
