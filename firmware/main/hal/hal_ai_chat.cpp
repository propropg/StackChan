/*
 * AI conversation (AVATAR app): tap the face, talk, StackChan answers with VOICEVOX
 *
 *   1. Record from the mic until the user stops talking (simple energy based voice detection)
 *   2. provider "gemini": one generateContent request with the audio -> {"heard","emotion","reply"}
 *      provider "openai" / "claude": speech recognition with Gemini (free tier) or OpenAI when no Gemini key,
 *      then the chat request; the reply starts with an emotion tag such as [happy]
 *   3. Speak the reply with VOICEVOX (bubble only when VOICEVOX is off or fails)
 *
 * Gemini has a free tier (key from Google AI Studio); OpenAI and Claude are pay as you go.
 */
#include "hal.h"
#include "utils/https_get.h"
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <settings.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ArduinoJson.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>

using namespace stackchan;

static const std::string_view _tag = "AiChat";

static const char* _settings_ns = "aichat";

static constexpr size_t _max_response_size = 64 * 1024;
static constexpr size_t _max_reply_bytes   = 900;  // VOICEVOX takes up to 1000 bytes

// Voice detection, in 20ms frames
static constexpr int _frame_ms            = 20;
static constexpr int _warmup_frames       = 10;   // first 200ms after enabling the mic are dropped (codec settling)
static constexpr int _noise_frames        = 10;   // next 200ms set the noise floor
static constexpr int _speech_start_frames = 3;    // 60ms above the threshold starts the utterance
static constexpr int _silence_end_frames  = 50;   // 1s below the threshold ends it
static constexpr int _min_speech_frames   = 10;   // less than 200ms of sound is a click, not speech
static constexpr int _no_speech_frames    = 300;  // give up after 6s without speech
static constexpr int _max_frames          = 750;  // 15s max
static constexpr int _preroll_frames      = 15;   // keep 300ms before the detected start
static constexpr float _min_threshold     = 350.0f;
static constexpr float _max_threshold     = 1200.0f;  // speech is 1500+; a click in the noise window must not hide it

static TaskHandle_t _task = nullptr;
static std::mutex _chat_mutex;  // one conversation at a time (mic task or /api/chat)
static std::atomic<bool> _busy{false};

struct Turn {
    std::string user;
    std::string reply;
    std::string emotion;
};
static std::mutex _last_wav_mutex;
static PsramBytes _last_wav;  // last recording, for checking the mic from the settings page

static std::mutex _history_mutex;
static std::deque<Turn> _history;

using PcmBuffer = std::vector<int16_t, PsramAllocator<int16_t>>;

/* -------------------------------- Settings -------------------------------- */

AiChatConfig_t Hal::getAiChatConfig()
{
    AiChatConfig_t out;
    Settings settings(_settings_ns, false);
    out.enabled      = settings.GetBool("enabled", out.enabled);
    out.provider     = settings.GetString("provider", out.provider);
    out.geminiKey    = settings.GetString("gemini_key", out.geminiKey);
    out.geminiModel  = settings.GetString("gemini_model", out.geminiModel);
    out.openaiKey    = settings.GetString("openai_key", out.openaiKey);
    out.openaiModel  = settings.GetString("openai_model", out.openaiModel);
    out.claudeKey    = settings.GetString("claude_key", out.claudeKey);
    out.claudeModel  = settings.GetString("claude_model", out.claudeModel);
    out.systemPrompt = settings.GetString("prompt", out.systemPrompt);
    out.historyTurns = settings.GetInt("history", out.historyTurns);
    out.weatherName  = settings.GetString("wx_name", out.weatherName);
    out.weatherLat   = settings.GetInt("wx_lat", 0) / 10000.0f;  // degrees x10000
    out.weatherLon   = settings.GetInt("wx_lon", 0) / 10000.0f;
    return out;
}

void Hal::setAiChatConfig(const AiChatConfig_t& config)
{
    const AiChatConfig_t defaults;
    Settings settings(_settings_ns, true);
    settings.SetBool("enabled", config.enabled);
    const bool known = config.provider == "gemini" || config.provider == "openai" || config.provider == "claude";
    settings.SetString("provider", known ? config.provider : defaults.provider);
    settings.SetString("gemini_key", config.geminiKey);
    settings.SetString("gemini_model", config.geminiModel.empty() ? defaults.geminiModel : config.geminiModel);
    settings.SetString("openai_key", config.openaiKey);
    settings.SetString("openai_model", config.openaiModel.empty() ? defaults.openaiModel : config.openaiModel);
    settings.SetString("claude_key", config.claudeKey);
    settings.SetString("claude_model", config.claudeModel.empty() ? defaults.claudeModel : config.claudeModel);
    settings.SetString("prompt", config.systemPrompt.empty() ? defaults.systemPrompt : config.systemPrompt);
    settings.SetInt("history", std::clamp(config.historyTurns, 0, 20));
    settings.SetString("wx_name", config.weatherName);
    settings.SetInt("wx_lat", static_cast<int32_t>(std::lround(std::clamp(config.weatherLat, -90.0f, 90.0f) * 10000)));
    settings.SetInt("wx_lon", static_cast<int32_t>(std::lround(std::clamp(config.weatherLon, -180.0f, 180.0f) * 10000)));
    mclog::tagInfo(_tag, "config saved: enabled={}, provider={}, gemini key {}, openai key {}, claude key {}",
                   config.enabled, config.provider, config.geminiKey.empty() ? "none" : "set",
                   config.openaiKey.empty() ? "none" : "set", config.claudeKey.empty() ? "none" : "set");
}

void Hal::clearAiChatHistory()
{
    std::lock_guard<std::mutex> lock(_history_mutex);
    _history.clear();
}

/* ---------------------------------- Face ---------------------------------- */

static bool to_emotion(std::string_view name, avatar::Emotion& emotion)
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

// Status bubble while listening / thinking; empty clears it
static void show_status(const std::string& text)
{
    LvglLockGuard lock;
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }
    if (text.empty()) {
        stackchan.avatar().clearSpeech();
    } else {
        stackchan.avatar().setSpeech(text);
    }
}

// Both LED strips: green while listening, blue while thinking / answering, off when done.
// Channels stay at or below 168 like every other LED color the firmware sets.
static void set_leds(const char* color)
{
    const std::string json = std::string(R"({"leftRgbColor":")") + color + R"(","rightRgbColor":")" + color + R"("})";
    LvglLockGuard lock;
    GetStackChan().updateNeonLightFromJson(json.c_str());
}

static constexpr const char* _led_listening = "#00A800";
static constexpr const char* _led_thinking  = "#0000A8";
static constexpr const char* _led_off       = "#000000";

// Turns the LEDs off however the conversation ends
struct LedsOffOnExit {
    ~LedsOffOnExit()
    {
        set_leds(_led_off);
    }
};

static void show_bubble(const std::string& text, const std::string& emotion_name, uint32_t duration_ms)
{
    LvglLockGuard lock;
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }
    avatar::Emotion emotion;
    if (to_emotion(emotion_name, emotion)) {
        stackchan.addModifier(std::make_unique<TimedEmotionModifier>(emotion, duration_ms));
    }
    stackchan.addModifier(std::make_unique<TimedSpeechModifier>(text, duration_ms));
}

// Cut at a UTF-8 character boundary
static std::string truncate_utf8(const std::string& text, size_t max_bytes)
{
    if (text.size() <= max_bytes) {
        return text;
    }
    size_t end = max_bytes;
    while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

static void speak_reply(const AiChatResult_t& result)
{
    const std::string text = truncate_utf8(result.reply, _max_reply_bytes);
    std::string error;
    if (GetHAL().getVoicevoxConfig().enabled) {
        if (GetHAL().speakText(text, result.emotion, text, error)) {
            return;
        }
        mclog::tagWarn(_tag, "voicevox failed ({}), showing the bubble only", error);
    }
    // ~3 bytes per Japanese character, ~6 characters per second
    show_bubble(text, result.emotion, std::clamp<uint32_t>(text.size() * 60, 4000, 20000));
}

/* ---------------------------------- Mic ----------------------------------- */

// Record one utterance as mono PCM. Returns false with error "no speech" when nobody talked.
static bool record_utterance(PcmBuffer& pcm, int& sample_rate, std::string& error)
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        error = "audio codec unavailable";
        return false;
    }

    const int in_rate     = codec->input_sample_rate();
    const int channels    = std::max(codec->input_channels(), 1);  // the 2nd channel is the speaker reference
    const int decimation  = in_rate >= 24000 ? 2 : 1;               // 24kHz -> 12kHz is plenty for speech
    const size_t frame_n  = in_rate * _frame_ms / 1000;
    sample_rate           = in_rate / decimation;

    std::lock_guard<std::mutex> audio_lock(GetHAL().audioMutex());

    PcmBuffer recorded;
    try {
        recorded.reserve(static_cast<size_t>(sample_rate) * _frame_ms * _max_frames / 1000);
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        return false;
    }

    codec->EnableInput(true);
    std::vector<int16_t> chunk(frame_n * channels);
    // DC blocking filter: the mic has a large offset that would swamp the level detection
    float dc_prev_in    = 0.0f;
    float dc_prev_out   = 0.0f;
    float peak_rms      = 0.0f;
    std::array<float, _noise_frames> noise_levels{};
    float threshold     = _min_threshold;
    int loud_run        = 0;
    int quiet_run       = 0;
    int speech_start    = -1;
    int loud_frames     = 0;  // loud frames since speech_start
    bool ok             = false;

    for (int raw_frame = 0; raw_frame < _warmup_frames + _max_frames; ++raw_frame) {
        if (!codec->InputData(chunk)) {
            error = "mic read failed";
            break;
        }

        double energy = 0;
        for (size_t i = 0; i < frame_n; i += decimation) {
            int32_t sum = 0;
            for (int d = 0; d < decimation; ++d) {
                sum += chunk[(i + d) * channels];
            }
            const float in  = static_cast<float>(sum) / decimation;
            const float out = in - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in      = in;
            dc_prev_out     = out;
            if (raw_frame >= _warmup_frames) {
                const int16_t sample = static_cast<int16_t>(std::clamp(out, -32768.0f, 32767.0f));
                recorded.push_back(sample);
                energy += static_cast<double>(sample) * sample;
            }
        }
        if (raw_frame < _warmup_frames) {
            continue;
        }
        const int frame = raw_frame - _warmup_frames;
        const float rms = std::sqrt(energy / (frame_n / decimation));
        peak_rms        = std::max(peak_rms, rms);
        if (frame % 25 == 0) {
            mclog::tagInfo(_tag, "level {:.0f} (threshold {:.0f})", rms, threshold);
        }

        if (frame < _noise_frames) {
            noise_levels[frame] = rms;
            if (frame == _noise_frames - 1) {
                // Median, so a single click does not raise the floor
                std::sort(noise_levels.begin(), noise_levels.end());
                const float noise = noise_levels[_noise_frames / 2];
                threshold         = std::clamp(noise * 3.0f, _min_threshold, _max_threshold);
                mclog::tagInfo(_tag, "noise floor {:.0f}, threshold {:.0f}", noise, threshold);
            }
            continue;
        }

        const bool loud = rms > threshold;
        if (speech_start < 0) {
            loud_run = loud ? loud_run + 1 : 0;
            if (loud_run >= _speech_start_frames) {
                speech_start = frame - _speech_start_frames + 1;
                loud_frames  = loud_run;
                quiet_run    = 0;
                mclog::tagInfo(_tag, "speech started");
            } else if (frame >= _no_speech_frames) {
                error = "no speech";
                break;
            }
        } else {
            quiet_run = loud ? 0 : quiet_run + 1;
            loud_frames += loud ? 1 : 0;
            if (quiet_run >= _silence_end_frames) {
                if (loud_frames < _min_speech_frames) {
                    // Only a click: keep waiting for real speech
                    mclog::tagInfo(_tag, "ignored a short sound");
                    speech_start = -1;
                    loud_run     = 0;
                    if (frame >= _no_speech_frames) {
                        error = "no speech";
                        break;
                    }
                    continue;
                }
                ok = true;
                break;
            }
        }
    }
    if (speech_start >= 0 && loud_frames >= _min_speech_frames && error.empty()) {
        ok = true;  // hit the time limit while talking
    }
    codec->EnableInput(false);
    mclog::tagInfo(_tag, "peak level {:.0f}", peak_rms);

    if (!ok) {
        if (error.empty()) {
            error = "no speech";
        }
        return false;
    }
    const size_t per_frame = frame_n / decimation;
    const size_t begin     = static_cast<size_t>(std::max(0, speech_start - _preroll_frames)) * per_frame;
    pcm.assign(recorded.begin() + std::min(begin, recorded.size()), recorded.end());
    mclog::tagInfo(_tag, "recorded {} ms", pcm.size() * 1000 / sample_rate);
    return true;
}

static void put_u32(PsramBytes& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back((v >> (8 * i)) & 0xFF);
}

static void put_u16(PsramBytes& out, uint16_t v)
{
    out.push_back(v & 0xFF);
    out.push_back(v >> 8);
}

static void make_wav(const PcmBuffer& pcm, int sample_rate, PsramBytes& wav)
{
    const uint32_t data_size = pcm.size() * 2;
    wav.clear();
    wav.reserve(44 + data_size);
    const char* riff = "RIFF";
    wav.insert(wav.end(), riff, riff + 4);
    put_u32(wav, 36 + data_size);
    const char* wave = "WAVEfmt ";
    wav.insert(wav.end(), wave, wave + 8);
    put_u32(wav, 16);
    put_u16(wav, 1);  // PCM
    put_u16(wav, 1);  // mono
    put_u32(wav, sample_rate);
    put_u32(wav, sample_rate * 2);
    put_u16(wav, 2);
    put_u16(wav, 16);
    const char* data = "data";
    wav.insert(wav.end(), data, data + 4);
    put_u32(wav, data_size);
    const auto* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), bytes, bytes + data_size);
}

/* ---------------------------------- HTTP ---------------------------------- */

// Body = json with the placeholder "@AUDIO@" replaced by the base64 WAV (built without copying it into a string)
static bool build_body(const std::string& json, const PsramBytes* wav, PsramBytes& body, std::string& error)
{
    static const std::string placeholder = "\"@AUDIO@\"";
    const size_t pos                     = wav ? json.find(placeholder) : std::string::npos;
    try {
        body.clear();
        if (pos == std::string::npos) {
            body.assign(json.begin(), json.end());
            return true;
        }
        size_t b64_len = 0;
        mbedtls_base64_encode(nullptr, 0, &b64_len, wav->data(), wav->size());
        body.reserve(json.size() + b64_len + 2);
        body.insert(body.end(), json.begin(), json.begin() + pos);
        body.push_back('"');
        const size_t at = body.size();
        body.resize(at + b64_len);
        size_t written = 0;
        if (mbedtls_base64_encode(body.data() + at, b64_len, &written, wav->data(), wav->size()) != 0) {
            error = "base64 failed";
            return false;
        }
        body.resize(at + written);
        body.push_back('"');
        body.insert(body.end(), json.begin() + pos + placeholder.size(), json.end());
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        return false;
    }
    return true;
}

// Short readable error out of an API error response ({"error":{"message":..}} for all three providers)
static std::string describe_api_error(const PsramBytes& body, int status)
{
    std::string message = "HTTP " + std::to_string(status);
    ArduinoJson::JsonDocument doc;
    if (!ArduinoJson::deserializeJson(doc, reinterpret_cast<const char*>(body.data()), body.size())) {
        if (doc["error"]["message"].is<const char*>()) {
            message += ": " + truncate_utf8(doc["error"]["message"].as<std::string>(), 160);
        } else if (doc["error"].is<const char*>()) {
            message += ": " + truncate_utf8(doc["error"].as<std::string>(), 160);
        }
    }
    return message;
}

static bool post_json(const std::string& url, const HttpHeaders& headers, const std::string& json,
                      const PsramBytes* wav, ArduinoJson::JsonDocument& response, std::string& error)
{
    if (GetHAL().getWifiStatus() == WifiStatus::None) {
        error = "Wi-Fi not connected";
        return false;
    }
    PsramBytes body;
    if (!build_body(json, wav, body, error)) {
        return false;
    }
    HttpHeaders all = headers;
    all.emplace_back("Content-Type", "application/json");

    const int64_t start = esp_timer_get_time();
    PsramBytes out;
    int status = 0;
    if (!https_post(url, all, body.data(), body.size(), _max_response_size, out, status, error)) {
        return false;
    }
    mclog::tagInfo(_tag, "HTTP {} in {} ms ({} bytes sent)", status, (esp_timer_get_time() - start) / 1000,
                   body.size());
    if (status != 200) {
        error = describe_api_error(out, status);
        return false;
    }
    if (ArduinoJson::deserializeJson(response, reinterpret_cast<const char*>(out.data()), out.size())) {
        error = "invalid JSON response";
        return false;
    }
    return true;
}

/* --------------------------------- Context -------------------------------- */

// The models know neither the time nor the weather; both are added to the system prompt of every request

static std::string now_text()
{
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2024) {
        return "";  // clock not synced yet
    }
    static const char* weekdays[] = {"日", "月", "火", "水", "木", "金", "土"};
    char buf[96];
    snprintf(buf, sizeof(buf), "%d年%d月%d日（%s曜日）%d時%02d分", local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, weekdays[local.tm_wday], local.tm_hour, local.tm_min);
    return buf;
}

static const char* weather_name(int code)
{
    if (code == 0) return "快晴";
    if (code == 1) return "晴れ";
    if (code == 2) return "晴れ時々くもり";
    if (code == 3) return "くもり";
    if (code == 45 || code == 48) return "霧";
    if (code >= 51 && code <= 57) return "霧雨";
    if (code == 61) return "小雨";
    if (code == 63) return "雨";
    if (code == 65) return "大雨";
    if (code == 66 || code == 67) return "みぞれ";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "にわか雨";
    if (code == 85 || code == 86) return "にわか雪";
    if (code >= 95) return "雷雨";
    return "不明";
}

static constexpr int64_t _weather_cache_us = 20LL * 60 * 1000000;  // 20 minutes
static std::mutex _weather_mutex;
static std::string _weather_text;
static std::string _weather_key;
static int64_t _weather_time_us = 0;

static std::string weather_text(const AiChatConfig_t& config)
{
    if (config.weatherName.empty()) {
        return "";
    }
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=3",
             config.weatherLat, config.weatherLon);

    std::lock_guard<std::mutex> lock(_weather_mutex);
    const int64_t now = esp_timer_get_time();
    if (_weather_key == url && _weather_time_us != 0 && now - _weather_time_us < _weather_cache_us) {
        return _weather_text;
    }

    PsramBytes body;
    int status = 0;
    std::string error;
    ArduinoJson::JsonDocument doc;
    if (!https_get(url, 16 * 1024, body, status, error) || status != 200 ||
        ArduinoJson::deserializeJson(doc, reinterpret_cast<const char*>(body.data()), body.size())) {
        mclog::tagWarn(_tag, "weather fetch failed: {} (HTTP {})", error, status);
        return _weather_key == url ? _weather_text : "";  // a stale forecast is better than none
    }

    char buf[192];
    std::string text = config.weatherName + "の天気: ";
    snprintf(buf, sizeof(buf), "いま%s、気温%.1f度、風速%.0fkm/h。", weather_name(doc["current"]["weather_code"].as<int>()),
             doc["current"]["temperature_2m"].as<float>(), doc["current"]["wind_speed_10m"].as<float>());
    text += buf;
    static const char* days[] = {"今日", "明日", "あさって"};
    auto daily = doc["daily"];
    for (int i = 0; i < 3 && i < static_cast<int>(daily["time"].size()); ++i) {
        snprintf(buf, sizeof(buf), "%s（%s）は%s、最高%.0f度、最低%.0f度、降水確率%d%%。", days[i],
                 daily["time"][i].as<const char*>(), weather_name(daily["weather_code"][i].as<int>()),
                 daily["temperature_2m_max"][i].as<float>(), daily["temperature_2m_min"][i].as<float>(),
                 daily["precipitation_probability_max"][i].as<int>());
        text += buf;
    }
    mclog::tagInfo(_tag, "{}", text);
    _weather_text    = text;
    _weather_key     = url;
    _weather_time_us = now;
    return text;
}

static std::string system_prompt(const AiChatConfig_t& config)
{
    std::string prompt    = config.systemPrompt;
    const std::string now = now_text();
    if (!now.empty()) {
        prompt += "\n\n現在の日時: " + now;
    }
    const std::string weather = weather_text(config);
    if (!weather.empty()) {
        prompt += "\n" + weather + "\n天気を聞かれたら、この予報をもとに短く答えてください。";
    } else {
        prompt +=
            "\n天気の情報はありません。天気を聞かれたら、設定ページの「AI会話」タブで場所を設定すると分かるようになると"
            "伝えてください。";
    }
    return prompt;
}

/* -------------------------------- Providers ------------------------------- */

static const char* _emotion_hint =
    "\n返事の先頭に、そのときの気持ちを [neutral] [happy] [angry] [sad] [doubt] [sleepy] のどれか1つで付けてください。"
    "例: [happy]こんにちは！";

// "[happy]text" -> emotion + text
static void split_emotion_tag(const std::string& raw, AiChatResult_t& result)
{
    result.reply   = raw;
    result.emotion = "neutral";
    size_t start   = raw.find_first_not_of(" \n");
    if (start == std::string::npos || raw[start] != '[') {
        return;
    }
    size_t end = raw.find(']', start);
    if (end == std::string::npos || end - start > 10) {
        return;
    }
    avatar::Emotion emotion;
    std::string name = raw.substr(start + 1, end - start - 1);
    if (to_emotion(name, emotion)) {
        result.emotion = name;
    }
    result.reply = raw.substr(end + 1);
    result.reply.erase(0, result.reply.find_first_not_of(" \n"));
}

static std::deque<Turn> history_snapshot(int turns)
{
    std::lock_guard<std::mutex> lock(_history_mutex);
    std::deque<Turn> out = _history;
    while (static_cast<int>(out.size()) > turns) {
        out.pop_front();
    }
    return out;
}

static void remember(const AiChatResult_t& result, int turns)
{
    std::lock_guard<std::mutex> lock(_history_mutex);
    _history.push_back({result.heard, result.reply, result.emotion});
    while (static_cast<int>(_history.size()) > std::max(turns, 0)) {
        _history.pop_front();
    }
}

static std::string gemini_url(const AiChatConfig_t& config)
{
    return "https://generativelanguage.googleapis.com/v1beta/models/" + config.geminiModel + ":generateContent";
}

// Concatenated text parts of the first candidate (thought summaries skipped)
static std::string gemini_text(ArduinoJson::JsonDocument& response)
{
    std::string text;
    ArduinoJson::JsonArray parts = response["candidates"][0]["content"]["parts"];
    for (auto part : parts) {
        if (part["text"].is<const char*>() && !part["thought"].as<bool>()) {
            text += part["text"].as<const char*>();
        }
    }
    return text;
}

// One Gemini request: audio (or text) in, {"heard","emotion","reply"} out
static bool gemini_chat(const AiChatConfig_t& config, const std::string& text, const PsramBytes* wav,
                        AiChatResult_t& result, std::string& error)
{
    ArduinoJson::JsonDocument doc;
    doc["systemInstruction"]["parts"][0]["text"] =
        system_prompt(config) +
        "\nheard にはユーザーの発言（音声なら聞き取った文）、emotion には返事をするときの気持ち、reply には返事を入れてください。";
    auto contents = doc["contents"].to<ArduinoJson::JsonArray>();
    for (const auto& turn : history_snapshot(config.historyTurns)) {
        auto user                    = contents.add<ArduinoJson::JsonObject>();
        user["role"]                 = "user";
        user["parts"][0]["text"]     = turn.user;
        auto model                   = contents.add<ArduinoJson::JsonObject>();
        model["role"]                = "model";
        model["parts"][0]["text"]    = turn.reply;
    }
    auto user  = contents.add<ArduinoJson::JsonObject>();
    user["role"] = "user";
    auto parts = user["parts"].to<ArduinoJson::JsonArray>();
    if (wav) {
        auto audio                    = parts.add<ArduinoJson::JsonObject>();
        audio["inlineData"]["mimeType"] = "audio/wav";
        audio["inlineData"]["data"]     = "@AUDIO@";
        parts.add<ArduinoJson::JsonObject>()["text"] =
            "（ユーザーが音声で話しかけました。聞き取れなかったり無音だったりしたら heard を空にしてください）";
    } else {
        parts.add<ArduinoJson::JsonObject>()["text"] = text;
    }
    auto gen                  = doc["generationConfig"].to<ArduinoJson::JsonObject>();
    gen["responseMimeType"]   = "application/json";
    auto schema               = gen["responseSchema"].to<ArduinoJson::JsonObject>();
    schema["type"]            = "OBJECT";
    schema["properties"]["heard"]["type"]   = "STRING";
    schema["properties"]["emotion"]["type"] = "STRING";
    auto emotions = schema["properties"]["emotion"]["enum"].to<ArduinoJson::JsonArray>();
    for (const char* e : {"neutral", "happy", "angry", "sad", "doubt", "sleepy"}) {
        emotions.add(e);
    }
    schema["properties"]["reply"]["type"] = "STRING";
    auto required = schema["required"].to<ArduinoJson::JsonArray>();
    required.add("heard");
    required.add("emotion");
    required.add("reply");

    std::string json;
    ArduinoJson::serializeJson(doc, json);
    doc.clear();

    ArduinoJson::JsonDocument response;
    if (!post_json(gemini_url(config), {{"x-goog-api-key", config.geminiKey}}, json, wav, response, error)) {
        return false;
    }
    ArduinoJson::JsonDocument answer;
    std::string answer_text = gemini_text(response);
    if (ArduinoJson::deserializeJson(answer, answer_text)) {
        error = "unexpected reply: " + truncate_utf8(answer_text, 80);
        return false;
    }
    result.heard   = wav ? answer["heard"].as<std::string>() : text;
    result.emotion = answer["emotion"].as<std::string>();
    result.reply   = answer["reply"].as<std::string>();
    if (result.reply.empty()) {
        error = wav && result.heard.empty() ? "no speech" : "empty reply";
        return false;
    }
    return true;
}

// Speech recognition only (Gemini free tier), for the OpenAI / Claude providers
static bool gemini_transcribe(const AiChatConfig_t& config, const PsramBytes& wav, std::string& text,
                              std::string& error)
{
    ArduinoJson::JsonDocument doc;
    auto parts = doc["contents"][0]["parts"].to<ArduinoJson::JsonArray>();
    doc["contents"][0]["role"] = "user";
    auto audio                      = parts.add<ArduinoJson::JsonObject>();
    audio["inlineData"]["mimeType"] = "audio/wav";
    audio["inlineData"]["data"]     = "@AUDIO@";
    parts.add<ArduinoJson::JsonObject>()["text"] =
        "この音声の発話を日本語でそのまま書き起こしてください。書き起こした文だけを出力し、"
        "発話が聞き取れない・無音のときは何も出力しないでください。";
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    doc.clear();

    ArduinoJson::JsonDocument response;
    if (!post_json(gemini_url(config), {{"x-goog-api-key", config.geminiKey}}, json, &wav, response, error)) {
        return false;
    }
    text = gemini_text(response);
    text.erase(0, text.find_first_not_of(" \n"));
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return true;
}

// Speech recognition with OpenAI (paid) when only an OpenAI key is set
static bool openai_transcribe(const AiChatConfig_t& config, const PsramBytes& wav, std::string& text,
                              std::string& error)
{
    if (GetHAL().getWifiStatus() == WifiStatus::None) {
        error = "Wi-Fi not connected";
        return false;
    }
    static const std::string boundary = "----StackChanBoundary7d3f";
    std::string head = "--" + boundary +
                       "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\ngpt-4o-mini-transcribe\r\n--" +
                       boundary + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nja\r\n--" + boundary +
                       "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                       "Content-Type: audio/wav\r\n\r\n";
    std::string tail = "\r\n--" + boundary + "--\r\n";
    PsramBytes body;
    try {
        body.reserve(head.size() + wav.size() + tail.size());
        body.insert(body.end(), head.begin(), head.end());
        body.insert(body.end(), wav.begin(), wav.end());
        body.insert(body.end(), tail.begin(), tail.end());
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        return false;
    }

    PsramBytes out;
    int status = 0;
    if (!https_post("https://api.openai.com/v1/audio/transcriptions",
                    {{"Authorization", "Bearer " + config.openaiKey},
                     {"Content-Type", "multipart/form-data; boundary=" + boundary}},
                    body.data(), body.size(), _max_response_size, out, status, error)) {
        return false;
    }
    if (status != 200) {
        error = describe_api_error(out, status);
        return false;
    }
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, reinterpret_cast<const char*>(out.data()), out.size())) {
        error = "invalid JSON response";
        return false;
    }
    text = doc["text"].as<std::string>();
    return true;
}

static bool openai_chat(const AiChatConfig_t& config, const std::string& text, AiChatResult_t& result,
                        std::string& error)
{
    ArduinoJson::JsonDocument doc;
    doc["model"]  = config.openaiModel;
    auto messages = doc["messages"].to<ArduinoJson::JsonArray>();
    auto system   = messages.add<ArduinoJson::JsonObject>();
    system["role"]    = "system";
    system["content"] = system_prompt(config) + _emotion_hint;
    for (const auto& turn : history_snapshot(config.historyTurns)) {
        auto user           = messages.add<ArduinoJson::JsonObject>();
        user["role"]        = "user";
        user["content"]     = turn.user;
        auto assistant       = messages.add<ArduinoJson::JsonObject>();
        assistant["role"]    = "assistant";
        assistant["content"] = "[" + turn.emotion + "]" + turn.reply;
    }
    auto user       = messages.add<ArduinoJson::JsonObject>();
    user["role"]    = "user";
    user["content"] = text;
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    doc.clear();

    ArduinoJson::JsonDocument response;
    if (!post_json("https://api.openai.com/v1/chat/completions", {{"Authorization", "Bearer " + config.openaiKey}},
                   json, nullptr, response, error)) {
        return false;
    }
    if (!response["choices"][0]["message"]["content"].is<const char*>()) {
        error = "empty reply";
        return false;
    }
    result.heard = text;
    split_emotion_tag(response["choices"][0]["message"]["content"].as<std::string>(), result);
    return true;
}

static bool claude_chat(const AiChatConfig_t& config, const std::string& text, AiChatResult_t& result,
                        std::string& error)
{
    ArduinoJson::JsonDocument doc;
    doc["model"]      = config.claudeModel;
    doc["max_tokens"] = 512;
    doc["system"]     = system_prompt(config) + _emotion_hint;
    auto messages     = doc["messages"].to<ArduinoJson::JsonArray>();
    for (const auto& turn : history_snapshot(config.historyTurns)) {
        auto user            = messages.add<ArduinoJson::JsonObject>();
        user["role"]         = "user";
        user["content"]      = turn.user;
        auto assistant       = messages.add<ArduinoJson::JsonObject>();
        assistant["role"]    = "assistant";
        assistant["content"] = "[" + turn.emotion + "]" + turn.reply;
    }
    auto user       = messages.add<ArduinoJson::JsonObject>();
    user["role"]    = "user";
    user["content"] = text;
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    doc.clear();

    ArduinoJson::JsonDocument response;
    if (!post_json("https://api.anthropic.com/v1/messages",
                   {{"x-api-key", config.claudeKey}, {"anthropic-version", "2023-06-01"}}, json, nullptr, response,
                   error)) {
        return false;
    }
    std::string reply;
    ArduinoJson::JsonArray blocks = response["content"];
    for (auto block : blocks) {
        if (block["type"] == "text") {
            reply += block["text"].as<const char*>();
        }
    }
    if (reply.empty()) {
        error = "empty reply";
        return false;
    }
    result.heard = text;
    split_emotion_tag(reply, result);
    return true;
}

static bool check_keys(const AiChatConfig_t& config, bool voice, std::string& error)
{
    if (config.provider == "gemini" && config.geminiKey.empty()) {
        error = "gemini key not set";
    } else if (config.provider == "openai" && config.openaiKey.empty()) {
        error = "openai key not set";
    } else if (config.provider == "claude" && config.claudeKey.empty()) {
        error = "claude key not set";
    } else if (voice && config.provider == "claude" && config.geminiKey.empty()) {
        error = "gemini key not set";  // Claude cannot listen; speech recognition needs Gemini
    }
    return error.empty();
}

// Answer text (already transcribed or typed) with the selected provider
static bool chat_text(const AiChatConfig_t& config, const std::string& text, AiChatResult_t& result,
                      std::string& error)
{
    if (config.provider == "openai") {
        return openai_chat(config, text, result, error);
    }
    if (config.provider == "claude") {
        return claude_chat(config, text, result, error);
    }
    return gemini_chat(config, text, nullptr, result, error);
}

static bool chat_voice(const AiChatConfig_t& config, const PsramBytes& wav, AiChatResult_t& result,
                       std::string& error)
{
    if (config.provider == "gemini") {
        return gemini_chat(config, "", &wav, result, error);
    }
    std::string text;
    bool ok = config.geminiKey.empty() ? openai_transcribe(config, wav, text, error)
                                       : gemini_transcribe(config, wav, text, error);
    if (!ok) {
        return false;
    }
    if (text.empty()) {
        error = "no speech";
        return false;
    }
    mclog::tagInfo(_tag, "heard: {}", text);
    show_status("「" + truncate_utf8(text, 60) + "」");
    return chat_text(config, text, result, error);
}

// What StackChan says when something goes wrong
static void show_error(const std::string& error)
{
    std::string text;
    if (error == "no speech") {
        text = "うまく聞き取れなかったよ。もう一回話しかけてね。";
    } else if (error.find("key not set") != std::string::npos) {
        text = error.rfind("gemini", 0) == 0 ? "GeminiのAPIキーを設定してね。" : "AIのAPIキーを設定してね。";
    } else if (error.rfind("HTTP 429", 0) == 0) {
        text = "たくさん話したから、少し休ませてね。（APIの利用上限）";
    } else if (error == "Wi-Fi not connected") {
        text = "Wi-Fiにつながっていないよ。";
    } else {
        text = "AIにつながらなかったよ。";
    }
    show_bubble(text, error == "no speech" ? "doubt" : "sad", 5000);
}

/* ---------------------------------- Task ---------------------------------- */

static void listen_and_answer()
{
    const auto config = GetHAL().getAiChatConfig();
    LedsOffOnExit leds_off;
    std::string error;
    if (!check_keys(config, true, error)) {
        mclog::tagWarn(_tag, "{}", error);
        show_error(error);
        return;
    }

    show_status("きいてるよ…");
    set_leds(_led_listening);
    vTaskDelay(pdMS_TO_TICKS(300));  // let the LEDs settle; switching them on clicks in the mic
    PcmBuffer pcm;
    int sample_rate = 0;
    if (!record_utterance(pcm, sample_rate, error)) {
        show_status("");
        mclog::tagWarn(_tag, "recording failed: {}", error);
        show_error(error);
        return;
    }

    show_status("かんがえ中…");
    set_leds(_led_thinking);
    PsramBytes wav;
    AiChatResult_t result;
    bool ok = false;
    try {
        make_wav(pcm, sample_rate, wav);
        PcmBuffer().swap(pcm);
        {
            std::lock_guard<std::mutex> lock(_last_wav_mutex);
            _last_wav = wav;
        }
        ok = chat_voice(config, wav, result, error);
    } catch (const std::bad_alloc&) {
        error = "out of memory";
    }
    show_status("");
    if (!ok) {
        mclog::tagError(_tag, "chat failed: {}", error);
        show_error(error);
        return;
    }
    mclog::tagInfo(_tag, "[{}] {} -> [{}] {}", config.provider, result.heard, result.emotion, result.reply);
    if (!result.heard.empty()) {
        remember(result, config.historyTurns);
    }
    speak_reply(result);
}

static void chat_task(void*)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        std::lock_guard<std::mutex> lock(_chat_mutex);
        listen_and_answer();
        _busy = false;
    }
}

void Hal::startAiChatService()
{
    if (_task) {
        return;
    }
    // TLS + JSON need a roomy stack
    if (xTaskCreate(chat_task, "ai_chat", 12288, nullptr, 3, &_task) != pdPASS) {
        mclog::tagError(_tag, "failed to start the ai chat task");
        _task = nullptr;
    }
}

bool Hal::requestAiChatListen()
{
    if (!_task || !getAiChatConfig().enabled) {
        return false;
    }
    bool expected = false;
    if (!_busy.compare_exchange_strong(expected, true)) {
        return false;
    }
    xTaskNotifyGive(_task);
    return true;
}

bool Hal::getAiChatLastRecording(PsramBytes& wav)
{
    std::lock_guard<std::mutex> lock(_last_wav_mutex);
    wav = _last_wav;
    return !wav.empty();
}

bool Hal::isAiChatBusy()
{
    return _busy;
}

bool Hal::aiChatText(const std::string& text, AiChatResult_t& result, std::string& error)
{
    if (text.empty()) {
        error = "empty text";
        return false;
    }
    bool expected = false;
    if (!_busy.compare_exchange_strong(expected, true)) {
        error = "busy";
        return false;
    }
    std::lock_guard<std::mutex> lock(_chat_mutex);
    const auto config = getAiChatConfig();
    LedsOffOnExit leds_off;
    bool ok = check_keys(config, false, error);
    if (ok) {
        show_status("かんがえ中…");
        set_leds(_led_thinking);
        ok = chat_text(config, text, result, error);
        show_status("");
    }
    if (ok) {
        remember(result, config.historyTurns);
        speak_reply(result);
    } else {
        show_error(error);
    }
    _busy = false;
    return ok;
}
