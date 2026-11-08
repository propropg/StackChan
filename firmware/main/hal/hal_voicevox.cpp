/*
 * Cloud VOICEVOX client (WEB版VOICEVOX API, https://voicevox.su-shiki.com/su-shikiapis/)
 *
 *   GET https://deprecatedapis.tts.quest/v2/voicevox/audio/?key=..&speaker=..&speed=..&pitch=..&intonationScale=..&text=..
 *       -> 200 audio/x-wav (24kHz mono 16-bit), or 4xx {"errorMessage":"invalidApiKey"|"notEnoughPoints"|"failed"}
 *   GET .../v2/voicevox/speakers/?key=..  -> speaker list
 *   GET .../v2/api/?key=..                -> {"points":..,"resetInHours":..}
 *
 * Each synthesis costs 1500 + 100 * (UTF-8 length) points out of ~10M per day.
 */
#include "hal.h"
#include <mooncake_log.h>
#include <settings.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include "utils/https_get.h"
#include <ArduinoJson.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>

static const std::string_view _tag = "VOICEVOX";

static const char* _settings_ns = "voicevox";
static const char* _api_base    = "https://deprecatedapis.tts.quest/v2/";

static constexpr size_t _max_wav_size  = 3 * 1024 * 1024;
static constexpr size_t _max_json_size = 256 * 1024;
static constexpr size_t _max_text_size = 1000;  // UTF-8 bytes (~330 Japanese characters)

/* -------------------------------- Settings -------------------------------- */

// Floats are stored as x100 integers
VoicevoxConfig_t Hal::getVoicevoxConfig()
{
    VoicevoxConfig_t out;
    Settings settings(_settings_ns, false);
    out.enabled      = settings.GetBool("enabled", out.enabled);
    out.apiKey       = settings.GetString("api_key", out.apiKey);
    out.speaker      = settings.GetInt("speaker", out.speaker);
    out.speed        = settings.GetInt("speed", 100) / 100.0f;
    out.pitch        = settings.GetInt("pitch", 0) / 100.0f;
    out.intonation   = settings.GetInt("intonation", 100) / 100.0f;
    out.greetingText = settings.GetString("greeting", out.greetingText);
    return out;
}

void Hal::setVoicevoxConfig(const VoicevoxConfig_t& config)
{
    Settings settings(_settings_ns, true);
    settings.SetBool("enabled", config.enabled);
    settings.SetString("api_key", config.apiKey);
    settings.SetInt("speaker", std::max(config.speaker, 0));
    settings.SetInt("speed", static_cast<int32_t>(std::clamp(config.speed, 0.5f, 2.0f) * 100 + 0.5f));
    settings.SetInt("pitch", static_cast<int32_t>(std::clamp(config.pitch, -0.15f, 0.15f) * 100 + (config.pitch < 0 ? -0.5f : 0.5f)));
    settings.SetInt("intonation", static_cast<int32_t>(std::clamp(config.intonation, 0.0f, 2.0f) * 100 + 0.5f));
    settings.SetString("greeting", config.greetingText);
    mclog::tagInfo(_tag, "config saved: enabled={}, key={}, speaker={}", config.enabled,
                   config.apiKey.empty() ? "none" : "set", config.speaker);
}

/* ---------------------------------- HTTP ---------------------------------- */

static std::string url_encode(std::string_view s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

static std::string format_float(float value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

// Read the whole response body of an opened request (headers already fetched) into out
static bool read_response(esp_http_client_handle_t client, int64_t content_length, size_t max_size, PsramBytes& out,
                          std::string& error)
{
    bool ok = false;
    try {
        out.clear();
        if (content_length > 0) {
            out.reserve(std::min<size_t>(content_length, max_size));
        }
        size_t received = 0;
        while (true) {
            if (received >= max_size) {
                error = "response too large";
                break;
            }
            if (out.size() < received + 4096) {
                out.resize(std::min(received + 4096, max_size));
            }
            int n = esp_http_client_read(client, reinterpret_cast<char*>(out.data() + received), out.size() - received);
            if (n < 0) {
                error = "read failed";
                break;
            }
            if (n == 0) {
                ok = esp_http_client_is_complete_data_received(client);
                if (!ok) {
                    error = "connection closed early";
                }
                break;
            }
            received += n;
        }
        out.resize(received);
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        ok    = false;
    }
    return ok;
}

bool https_get(const std::string& url, size_t max_size, PsramBytes& out, int& status, std::string& error)
{
    esp_http_client_config_t config = {};
    config.url                      = url.c_str();
    config.timeout_ms               = 15000;
    config.crt_bundle_attach        = esp_crt_bundle_attach;
    config.buffer_size              = 4096;
    config.buffer_size_tx           = 4096;  // the request line carries the url-encoded text
    config.user_agent               = "StackChan";

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        error = "http client init failed";
        return false;
    }

    bool ok                = false;
    int64_t content_length = 0;
    esp_err_t err          = ESP_OK;
    // Follow redirects by hand (open/read mode does not); Apps Script web apps always answer with a 302
    for (int redirects = 0;; ++redirects) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            break;
        }
        content_length = esp_http_client_fetch_headers(client);
        status         = esp_http_client_get_status_code(client);
        bool redirect  = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!redirect || redirects >= 5) {
            break;
        }
        esp_http_client_flush_response(client, nullptr);
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
    }
    if (err != ESP_OK) {
        error = std::string("connect failed: ") + esp_err_to_name(err);
    } else {
        ok = read_response(client, content_length, max_size, out, error);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool https_post(const std::string& url, const HttpHeaders& headers, const uint8_t* body, size_t body_size,
                size_t max_size, PsramBytes& out, int& status, std::string& error, int timeout_ms)
{
    esp_http_client_config_t config = {};
    config.url                      = url.c_str();
    config.method                   = HTTP_METHOD_POST;
    config.timeout_ms               = timeout_ms;
    config.crt_bundle_attach        = esp_crt_bundle_attach;
    config.buffer_size              = 4096;
    config.buffer_size_tx           = 2048;
    config.user_agent               = "StackChan";

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        error = "http client init failed";
        return false;
    }
    for (const auto& [name, value] : headers) {
        esp_http_client_set_header(client, name.c_str(), value.c_str());
    }

    bool ok       = false;
    esp_err_t err = esp_http_client_open(client, body_size);
    if (err != ESP_OK) {
        error = std::string("connect failed: ") + esp_err_to_name(err);
    } else {
        size_t sent = 0;
        while (sent < body_size) {
            int n = esp_http_client_write(client, reinterpret_cast<const char*>(body + sent),
                                          std::min<size_t>(body_size - sent, 8192));
            if (n <= 0) {
                break;
            }
            sent += n;
        }
        if (sent < body_size) {
            error = "send failed";
        } else {
            int64_t content_length = esp_http_client_fetch_headers(client);
            status                 = esp_http_client_get_status_code(client);
            ok                     = read_response(client, content_length, max_size, out, error);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

// Pull "errorMessage" out of an error response, or describe the status code
static std::string describe_error(const PsramBytes& body, int status)
{
    ArduinoJson::JsonDocument doc;
    if (!ArduinoJson::deserializeJson(doc, reinterpret_cast<const char*>(body.data()), body.size()) &&
        doc["errorMessage"].is<const char*>()) {
        return doc["errorMessage"].as<std::string>();
    }
    return "HTTP " + std::to_string(status);
}

static bool wifi_ready(std::string& error)
{
    if (GetHAL().getWifiStatus() == WifiStatus::None) {
        error = "Wi-Fi not connected";
        return false;
    }
    return true;
}

/* ----------------------------------- API ---------------------------------- */

bool Hal::voicevoxSynthesize(std::string_view text, PsramBytes& wav, std::string& error)
{
    if (text.empty()) {
        error = "empty text";
        return false;
    }
    if (text.size() > _max_text_size) {
        error = "text too long";
        return false;
    }
    if (!wifi_ready(error)) {
        return false;
    }

    const auto config = getVoicevoxConfig();
    std::string url   = std::string(_api_base) + "voicevox/audio/?";
    if (!config.apiKey.empty()) {
        url += "key=" + url_encode(config.apiKey) + "&";
    }
    url += "speaker=" + std::to_string(config.speaker);
    url += "&speed=" + format_float(config.speed);
    url += "&pitch=" + format_float(config.pitch);
    url += "&intonationScale=" + format_float(config.intonation);
    url += "&text=" + url_encode(text);

    const int64_t start = esp_timer_get_time();
    int status          = 0;
    if (!https_get(url, _max_wav_size, wav, status, error)) {
        mclog::tagError(_tag, "synthesis failed: {}", error);
        return false;
    }
    if (status != 200 || wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) != 0) {
        error = describe_error(wav, status);
        wav.clear();
        mclog::tagError(_tag, "synthesis failed: {}", error);
        return false;
    }

    mclog::tagInfo(_tag, "synthesized {} bytes in {} ms", wav.size(), (esp_timer_get_time() - start) / 1000);
    return true;
}

bool Hal::voicevoxFetchJson(std::string_view endpoint, std::string& out, std::string& error)
{
    std::string path;
    if (endpoint == "speakers") {
        path = "voicevox/speakers/";
    } else if (endpoint == "points") {
        path = "api/";
    } else {
        error = "unknown endpoint";
        return false;
    }
    if (!wifi_ready(error)) {
        return false;
    }

    const auto config = getVoicevoxConfig();
    std::string url   = std::string(_api_base) + path;
    if (!config.apiKey.empty()) {
        url += "?key=" + url_encode(config.apiKey);
    }

    PsramBytes body;
    int status = 0;
    if (!https_get(url, _max_json_size, body, status, error)) {
        return false;
    }
    if (status != 200) {
        error = describe_error(body, status);
        return false;
    }
    out.assign(reinterpret_cast<const char*>(body.data()), body.size());
    return true;
}
