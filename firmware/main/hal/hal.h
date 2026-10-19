/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <memory>
#include <cstdint>
#include <string>
#include <lvgl.h>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <smooth_lvgl.hpp>
#include <array>
#include <lvgl_image.h>
#include <string_view>
#include "utils/psram_allocator.h"
#include <ArduinoJson.hpp>

/**
 * @brief
 *
 */
enum class HeadPetGesture { None, Press, Release, SwipeForward, SwipeBackward };

/**
 * @brief
 *
 */
enum class WsSignalSource {
    Local = 0,
    Remote,
};

/**
 * @brief
 *
 */
struct WsTextMessage_t {
    std::string name;
    std::string content;
};

/**
 * @brief
 *
 */
enum class ImuMotionEvent {
    None = 0,
    Shake,
    PickUp,
};

/**
 * @brief
 *
 */
enum class AppConfigEvent {
    None = 0,
    AppConnected,
    AppDisconnected,
    TryWifiConnect,
    WifiConnectFailed,
    WifiConnected,
};

/**
 * @brief
 *
 */
enum class CommonLogLevel {
    Info = 0,
    Warning,
    Error,
};

/**
 * @brief
 *
 */
namespace app_center {

struct AppInfo_t {
    std::string name;
    std::string iconUrl;
    std::string description;
    std::string firmwareUrl;
};

using AppInfoList_t = std::vector<AppInfo_t>;

};  // namespace app_center

/**
 * @brief
 *
 */
enum class WifiStatus {
    None = 0,
    Low,
    Medium,
    High,
};

/**
 * @brief
 *
 */
struct UserAccountInfo_t {
    std::string username;
    std::string deviceName;
};

/**
 * @brief Cloud VOICEVOX speech settings
 *
 */
struct VoicevoxConfig_t {
    bool enabled = false;
    std::string apiKey;
    int speaker      = 3;  // ずんだもん (ノーマル)
    float speed      = 1.0f;
    float pitch      = 0.0f;
    float intonation = 1.0f;
    std::string greetingText = "こんにちは！スタックチャンだよ。";
};

/**
 * @brief Speech + head motion + LED blink, run on the device (settings page buttons, Claude Code events)
 *
 */
struct Action_t {
    std::string text;            // spoken with VOICEVOX; empty = motion / LEDs only
    std::string emotion;         // neutral / happy / angry / sad / doubt / sleepy, empty = keep
    std::string motion = "none";  // none / nod / bow / look / shake
    std::string bubble;          // speech bubble text, empty = none
    std::string ledColor;        // "#RRGGBB", empty = no LEDs
    int ledBlinks = 3;

    static Action_t fromJson(ArduinoJson::JsonVariantConst json);
    void toJson(ArduinoJson::JsonObject json) const;
};

/**
 * @brief Gmail / Calendar notifications through the Apps Script relay
 *
 */
struct NotifyConfig_t {
    bool enabled = false;
    std::string url;  // web app url including ?token=
    int intervalSec = 120;
    bool quietHours = true;  // no announcements between quietStart and quietEnd (hours, local time)
    int quietStart  = 22;
    int quietEnd    = 7;
    bool chatReadContent = false;  // read Google Chat message text aloud; otherwise only who sent it
    bool lookAtFace      = true;   // turn toward a face seen by the camera while reading items aloud
};

struct FaceTrackStatus_t {
    bool modelLoaded = false;
    std::string error;   // why the models could not be loaded
    bool active      = false;
    int inferMs      = 0;  // detection time of the last frame
    int faces        = 0;  // faces in the last frame
    int64_t lastSeenUs = 0;  // esp_timer time a face was last seen, 0 = never
    float lastX = 0.0f, lastY = 0.0f;  // where it was, -1..1 from the image centre (x right, y down)
    bool hasFront = false;  // conversation front (head angles facing the user, 0.1 degree)
    int frontYaw = 0, frontPitch = 0;
};

struct NotifyStatus_t {
    int64_t lastCheckUs = 0;  // esp_timer time of the last poll, 0 = never
    int lastCount       = 0;
    std::string lastError;
};

/**
 * @brief
 *
 */
struct XiaozhiConfig_t {
    uint32_t idleShutdownTimeSeconds = 600;
    bool allowShutdownWhenCharging   = false;
    uint8_t idleRandomMovementLevel  = 2;
    bool startAiAgentOnBoot          = false;
};

/**
 * @brief
 *
 */
enum class MicTestStatus {
    Starting = 0,
    Recording,
    Playing,
    Done,
    Failed,
};

/**
 * @brief
 *
 */
class BootLogo {
public:
    BootLogo()
    {
        _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
        _panel->setSize(320, 240);
        _panel->setAlign(LV_ALIGN_CENTER);
        _panel->setBorderWidth(0);
        _panel->setBgOpa(0);
        _panel->setPaddingAll(0);

        _label_logo = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_logo->setTextFont(&lv_font_montserrat_24);
        _label_logo->setTextColor(lv_color_hex(0xFFFFFF));
        _label_logo->align(LV_ALIGN_CENTER, 0, -14);
        _label_logo->setText("STACKCHAN");

        _label_msg = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_msg->setTextFont(&lv_font_montserrat_16);
        _label_msg->setTextColor(lv_color_hex(0xBFBFBF));
        _label_msg->align(LV_ALIGN_CENTER, 0, 14);
        _label_msg->setText("Starting up ...");

        _label_version = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_version->setTextFont(&lv_font_montserrat_14);
        _label_version->setTextColor(lv_color_hex(0x8B8B8B));
        _label_version->align(LV_ALIGN_BOTTOM_RIGHT, -7, -6);
        _label_version->setText("V" FIRMWARE_VERSION);
    }

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_logo;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_msg;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_version;
};

/**
 * @brief
 *
 */
class Hal {
public:
    void init();

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms);
    std::uint32_t millis();
    void feedTheDog();
    std::array<uint8_t, 6> getFactoryMac();
    std::string getFactoryMacString(std::string divider = "");
    void reboot();
    void updateHeapStatusLog();
    uint8_t getBatteryLevel();
    bool isBatteryCharging();
    void factoryReset();

    /* --------------------------------- Display -------------------------------- */
    lv_indev_t* lvTouchpad = nullptr;
    std::unique_ptr<BootLogo> bootLogo;
    void lvglLock();
    void lvglUnlock();
    void setBackLightBrightness(uint8_t brightness, bool permanent = false);
    uint8_t getBackLightBrightness();

    /* --------------------------------- Xiaozhi -------------------------------- */
    void requestXiaozhiStart()
    {
        _xiaozhi_start_requested = true;
    }
    bool isXiaozhiStartRequested()
    {
        return _xiaozhi_start_requested;
    }
    void startXiaozhi();
    XiaozhiConfig_t getXiaozhiConfig();
    void setXiaozhiConfig(XiaozhiConfig_t config);

    /* ----------------------------------- BLE ---------------------------------- */
    uitk::Signal<const char*> onBleMotionData;
    uitk::Signal<const char*> onBleAvatarData;
    uitk::Signal<const char*> onBleConfigData;
    uitk::Signal<const char*> onBleRgbData;
    uitk::Signal<AppConfigEvent> onAppConfigEvent;

    void startBleServer();
    bool isBleConnected();
    void startAppConfigServer();
    bool isAppConfiged();
    void resetAppConfiged();

    /* --------------------------------- HeadPet -------------------------------- */
    uitk::Signal<HeadPetGesture> onHeadPetGesture;

    /* ----------------------------------- RGB ---------------------------------- */
    void setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    void showRgbColor(uint8_t r, uint8_t g, uint8_t b);
    void refreshRgb();

    /* ---------------------------------- Power --------------------------------- */
    void setServoPowerEnabled(bool enabled);

    /* -------------------------------- Websocket ------------------------------- */
    uitk::Signal<std::string_view> onWsMotionData;
    uitk::Signal<std::string_view> onWsAvatarData;
    uitk::Signal<std::string> onWsCallRequest;
    uitk::Signal<bool> onWsCallResponse;
    uitk::Signal<WsSignalSource> onWsCallEnd;
    uitk::Signal<const WsTextMessage_t&> onWsTextMessage;
    uitk::Signal<bool> onWsVideoModeChange;
    uitk::Signal<std::shared_ptr<LvglImage>> onWsVideoFrame;
    uitk::Signal<std::string_view> onWsDanceData;
    uitk::Signal<CommonLogLevel, std::string_view> onWsLog;

    void startWebSocketAvatarService(std::function<void(std::string_view)> onStartLog);

    /* -------------------------------- Local API ------------------------------- */
    // Local HTTP API (port 80) for speech / motion / emotion control from the LAN
    void startLocalApiService(std::function<void(std::string_view)> onStartLog);
    std::string getLocalIp();
    // Greeting voice + LED blink + nod so the user can tell StackChan is up and the API is ready
    void playBootGreeting();
    // (Re)create the idle animation modifiers from the saved settings. Call with the LVGL lock held.
    void applyIdleModifiers();
    // Synthesize text with cloud VOICEVOX and play it (mouth flapping / emotion / bubble when the avatar is up).
    // Empty bubble shows none. Blocks until playback ends. Returns false with error set when synthesis fails
    // (nothing is played).
    bool speakText(const std::string& text, const std::string& emotion, const std::string& bubble, std::string& error);
    // Play a 16-bit PCM WAV like speakText does (blocks until playback ends)
    bool playWav(const uint8_t* data, size_t size, const std::string& emotion, const std::string& bubble,
                 std::string& error);

    /* --------------------------------- Actions -------------------------------- */
    // Speak (VOICEVOX) with a head motion and LED blinks. Synthesizes before moving, so on failure nothing
    // happens and error is set. Blocks until done.
    bool runAction(const Action_t& action, std::string& error);
    // Claude Code event templates ("stop", "notification", "sessionEnd") stored on the device
    std::string getEventsJson();
    void setEventsJson(ArduinoJson::JsonVariantConst json);
    // Run an event's template with {session} filled in. Fails with "voicevox disabled" when VOICEVOX is off
    // so the PC hook can fall back to its own voice.
    bool runEvent(std::string_view name, std::string_view session, std::string& error);

    /* ------------------------------ Notifications ----------------------------- */
    // Polls the Apps Script relay (tools/gas/stackchan_notify.gs) and reads new Gmail / Calendar items aloud
    NotifyConfig_t getNotifyConfig();
    void setNotifyConfig(const NotifyConfig_t& config);
    NotifyStatus_t getNotifyStatus();
    // Starts the polling task once; it only polls while notifications are active (AVATAR app open)
    void startNotifyService();
    void setNotifyActive(bool active);
    // Poll right now regardless of the interval / quiet hours; speaks what it finds. Blocks.
    bool checkNotificationsNow(int& count, std::string& error);

    /* ------------------------------ Face tracking ----------------------------- */
    // Turn the head toward a face seen by the camera (ESP-DL, models in the assets partition) while held.
    // The head is left alone when no face is seen. Calls nest; every begin needs an end.
    void beginFaceTracking();
    void endFaceTracking();
    // Track for a while without an end call (settings page test)
    void trackFaceFor(uint32_t ms);
    FaceTrackStatus_t getFaceTrackStatus();
    // The camera frame exactly as the face detector sees it (24-bit BMP)
    bool getFaceCameraBmp(PsramBytes& bmp, std::string& error);
    // The conversation front (Motion::setFront) is where a face was last seen; it is saved to flash.
    // Load it with the LVGL lock held (AVATAR app start). Reset forgets it (head centres on 0 again).
    void loadFaceFront();
    void resetFaceFront();

    /* -------------------------------- VOICEVOX -------------------------------- */
    // Cloud VOICEVOX (WEB版VOICEVOX API by su-shiki), configured from the web settings page or SETUP
    VoicevoxConfig_t getVoicevoxConfig();
    void setVoicevoxConfig(const VoicevoxConfig_t& config);
    // Returns a 16-bit PCM WAV in wav, or false with error set (Wi-Fi down, invalidApiKey, notEnoughPoints, ...)
    bool voicevoxSynthesize(std::string_view text, PsramBytes& wav, std::string& error);
    // GET a JSON endpoint of the API with the saved key ("speakers" or "points")
    bool voicevoxFetchJson(std::string_view endpoint, std::string& out, std::string& error);

    /* ----------------------------------- IMU ---------------------------------- */
    uitk::Signal<ImuMotionEvent> onImuMotionEvent;

    /* ---------------------------------- Time ---------------------------------- */
    void syncRtcTimeToSystem();
    void syncSystemTimeToRtc();
    void setTimezone(std::string_view tz);
    std::string getTimezone();

    /* --------------------------------- EspNow --------------------------------- */
    uitk::Signal<const std::vector<uint8_t>&> onEspNowData;
    void startEspNow(int channel);
    bool espNowSend(const std::vector<uint8_t>& data, const uint8_t* destAddr = nullptr);
    void setLaserEnabled(bool enabled);

    /* ------------------------------- Warm Reboot ------------------------------ */
    void requestWarmReboot(int appIndex);
    int getWarmRebootTarget();
    void clearWarmRebootRequest();

    /* --------------------------------- Network -------------------------------- */
    void startNetwork(std::function<void(std::string_view)> onLog);
    WifiStatus getWifiStatus();
    void startSntp();

    /* -------------------------------- App center ------------------------------- */
    app_center::AppInfoList_t fetchAppList();
    void launchApp(std::string_view url, std::function<void(int)> onProgress);

    /* --------------------------------- EzData --------------------------------- */
    void startEzDataService(std::function<void(std::string_view)> onStartLog);
    uitk::Signal<std::string_view> onEzdataPairCode;

    /* ------------------------------- User Acount ------------------------------ */
    UserAccountInfo_t getUserAccountInfo();
    bool updateAccountInfo(std::function<void(std::string_view)> onLog);
    bool unbindAccount(std::function<void(std::string_view)> onLog);

    /* ----------------------------------- OTA ---------------------------------- */
    bool updateFirmware(std::function<void(std::string_view)> onLog);

    /* ---------------------------------- Audio --------------------------------- */
    void setSpeakerVolume(uint8_t volume, bool permanent = false);
    uint8_t getSpeakerVolume();
    std::string startMicTest(std::function<void(MicTestStatus)> onStatusUpdate);
    void getMicWaveformFrame(std::vector<int16_t>& data);
    void clearupMicTest();

private:
    bool _xiaozhi_start_requested = false;

    void xiaozhi_board_init();
    void lvgl_init();
    void xiaozhi_mcp_init();
    void ble_init(bool useAltUuid);
    void servo_init();
    void head_touch_init();
    void io_expander_init();
    void imu_init();
    void rtc_init();
};

Hal& GetHAL();

/**
 * @brief
 *
 */
class LvglLockGuard {
public:
    LvglLockGuard()
    {
        GetHAL().lvglLock();
    }
    ~LvglLockGuard()
    {
        GetHAL().lvglUnlock();
    }
};
