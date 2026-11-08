/*
 * Face tracking
 *
 * While a notification is read aloud, StackChan looks for a face with the CoreS3 camera and turns its head
 * toward it. Faces are found with the ESP-DL MSR+MNP detector (the models of espressif/human_face_detect,
 * stored in the assets partition as face_msr_s8_v1.espdl / face_mnp_s8_v1.espdl). The camera sits in the
 * head, so each step turns by the face's offset from the image centre and the next frame shows what is left.
 * Until a face is found, each session looks where the face was last time, then a little up and to the sides,
 * and comes back to where it started if nobody is there.
 *
 * Besides notifications, a short session runs every few minutes while the AVATAR app idles, and finding the
 * user shows an eye-contact badge on the face (EyeContactDecorator).
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "board/stackchan_camera.h"
#include <stackchan/stackchan.h>
#include <stackchan/avatar/decorators/decorators.h>
#include <esp_random.h>
#include <assets.h>
#include <mooncake_log.h>
#include <settings.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <dl_model_base.hpp>
#include <dl_image_preprocessor.hpp>
#include <dl_detect_base.hpp>
#include <dl_detect_msr_postprocessor.hpp>
#include <dl_detect_mnp_postprocessor.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

using namespace stackchan;

static const std::string_view _tag = "FaceTrack";

static const char* _msr_asset = "face_msr_s8_v1.espdl";
static const char* _mnp_asset = "face_mnp_s8_v1.espdl";

// GC0308 field of view (degrees) and how much of the offset to turn per step (< 1 so it does not overshoot)
static constexpr float _hfov_deg = 57.0f;
static constexpr float _vfov_deg = 43.0f;
static constexpr float _gain     = 0.7f;
// Offsets below this are close enough (degrees)
static constexpr float _deadband_deg = 3.0f;
static constexpr int _move_speed     = 300;
// Keep the head in a natural range (0.1 degree)
static constexpr int _yaw_limit = 700;
static constexpr int _pitch_max = 850;  // StackChan often sits well below the user's face

static constexpr uint32_t _settle_ms   = 120;  // let the head stop shaking before grabbing a frame
static constexpr uint32_t _interval_ms = 100;  // pause between detections so the audio task keeps up
static constexpr int _search_frames    = 2;    // frames to look at each search position before the next

// False hits (a fan grille once became the front, 82 degrees up): a face must be at least this wide (fraction
// of the frame, about 25 px), seen in consecutive frames within _same_place (0.1 degree) before the head
// follows it, and a few more before it becomes the conversation front, which never sits higher than
// _front_pitch_max even though tracking may look further up.
static constexpr float _min_face_size  = 0.08f;
static constexpr int _same_place       = 150;
static constexpr int _confirm_move     = 2;
static constexpr int _confirm_front    = 3;
static constexpr int _front_pitch_max  = 600;

// MSR only proposes candidates; MNP (threshold 0.5) decides. The GC0308's flat, low-contrast frames score a
// clearly visible face around 0.3 in MSR, so its stock 0.5 threshold found nothing.
static constexpr float _msr_score_thr = 0.25f;

/* -------------------------------- Detector -------------------------------- */

namespace {

// Same setup as espressif/human_face_detect 0.5.0 (MIT), loading the models from a memory address instead of
// the component's own rodata / partition
class FaceMsr : public dl::detect::DetectImpl {
public:
    explicit FaceMsr(const char* model_data)
    {
        m_model = new dl::Model(model_data, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        m_model->minimize();
        m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {1, 1, 1}, true);
        m_postprocessor      = new dl::detect::MSRPostprocessor(
            m_model, m_image_preprocessor, _msr_score_thr, 0.5f, 10,
            {{8, 8, 9, 9, {{16, 16}, {32, 32}}}, {16, 16, 9, 9, {{64, 64}, {128, 128}}}});
    }
};

class FaceMnp {
public:
    explicit FaceMnp(const char* model_data)
    {
        _model = new dl::Model(model_data, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        _model->minimize();
        _preprocessor  = new dl::image::ImagePreprocessor(_model, {0, 0, 0}, {1, 1, 1}, true);
        _postprocessor = new dl::detect::MNPPostprocessor(_model, _preprocessor, 0.5f, 0.5f, 10, {{1, 1, 0, 0, {{48, 48}}}});
    }
    ~FaceMnp()
    {
        delete _postprocessor;
        delete _preprocessor;
        delete _model;
    }

    // Refine each MSR candidate on a square crop around it
    std::list<dl::detect::result_t>& run(const dl::image::img_t& img, std::list<dl::detect::result_t>& candidates)
    {
        _postprocessor->clear_result();
        for (auto& candidate : candidates) {
            int center_x     = (candidate.box[0] + candidate.box[2]) >> 1;
            int center_y     = (candidate.box[1] + candidate.box[3]) >> 1;
            int side         = std::max(candidate.box[2] - candidate.box[0], candidate.box[3] - candidate.box[1]);
            candidate.box[0] = center_x - (side >> 1);
            candidate.box[1] = center_y - (side >> 1);
            candidate.box[2] = candidate.box[0] + side;
            candidate.box[3] = candidate.box[1] + side;
            candidate.limit_box(img.width, img.height);

            _preprocessor->preprocess(img, candidate.box);
            _model->run();
            _postprocessor->postprocess();
        }
        _postprocessor->nms();
        return _postprocessor->get_result(img.width, img.height);
    }

private:
    dl::Model* _model;
    dl::image::ImagePreprocessor* _preprocessor;
    dl::detect::MNPPostprocessor* _postprocessor;
};

class FaceDetector {
public:
    FaceDetector(const char* msr_data, const char* mnp_data) : _msr(msr_data), _mnp(mnp_data)
    {
    }
    std::list<dl::detect::result_t>& run(const dl::image::img_t& img)
    {
        return _mnp.run(img, _msr.run(img));
    }

private:
    FaceMsr _msr;
    FaceMnp _mnp;
};

}  // namespace

static std::unique_ptr<FaceDetector> load_detector(std::string& error)
{
    void* msr  = nullptr;
    void* mnp  = nullptr;
    size_t len = 0;
    if (!Assets::GetInstance().GetAssetData(_msr_asset, msr, len) ||
        !Assets::GetInstance().GetAssetData(_mnp_asset, mnp, len)) {
        error = "face models not in the assets partition (flash the assets again)";
        return nullptr;
    }
    const int64_t start = esp_timer_get_time();
    auto detector = std::make_unique<FaceDetector>(static_cast<const char*>(msr), static_cast<const char*>(mnp));
    mclog::tagInfo(_tag, "models loaded in {} ms, free psram {}", (esp_timer_get_time() - start) / 1000,
                   heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return detector;
}

/* --------------------------------- Tracker -------------------------------- */

static TaskHandle_t _task = nullptr;
static std::atomic<int> _holds{0};
static std::atomic<int64_t> _until_us{0};
static std::mutex _status_mutex;
static FaceTrackStatus_t _status;

static bool wanted()
{
    return _holds > 0 || esp_timer_get_time() < _until_us;
}

static void set_status(const std::function<void(FaceTrackStatus_t&)>& update)
{
    std::lock_guard<std::mutex> lock(_status_mutex);
    update(_status);
}

struct Face {
    float x, y;  // centre, -1..1 from the image centre (x right, y down)
    float size;  // box width / image width
};

static inline uint8_t clamp_u8(int v)
{
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

// Convert a camera frame to RGB888. ESP-DL can read YUYV / RGB565 itself, but each of its pixel converters
// costs 15-120 KB of flash, so only RGB888 input is built in (sdkconfig.defaults).
static bool to_rgb888(const uint8_t* src, int width, int height, int format, uint8_t* dst)
{
    const int pixels = width * height;
    if (format == V4L2_PIX_FMT_YUYV) {
        // Y0 U Y1 V per pixel pair, BT.601 in fixed point
        for (int i = 0; i < pixels; i += 2, src += 4) {
            const int u = src[1] - 128, v = src[3] - 128;
            const int r = (359 * v) >> 8, g = (88 * u + 183 * v) >> 8, b = (454 * u) >> 8;
            for (int k = 0; k < 2; ++k) {
                const int y = src[k * 2];
                *dst++      = clamp_u8(y + r);
                *dst++      = clamp_u8(y - g);
                *dst++      = clamp_u8(y + b);
            }
        }
        return true;
    }
    if (format == V4L2_PIX_FMT_RGB565) {
        auto px = reinterpret_cast<const uint16_t*>(src);
        for (int i = 0; i < pixels; ++i) {
            const uint16_t c = px[i];
            *dst++           = ((c >> 11) & 0x1f) << 3;
            *dst++           = ((c >> 5) & 0x3f) << 2;
            *dst++           = (c & 0x1f) << 3;
        }
        return true;
    }
    if (format == V4L2_PIX_FMT_RGB24) {
        memcpy(dst, src, pixels * 3);
        return true;
    }
    return false;
}

static std::mutex _camera_mutex;  // the tracker and the snapshot endpoint share the camera

// Grab a fresh frame as RGB888 into buf (reallocated when the size changes). Returns false when the camera fails.
static bool grab_rgb888(uint8_t*& buf, size_t& buf_size, int& width, int& height)
{
    std::lock_guard<std::mutex> lock(_camera_mutex);
    auto camera = hal_bridge::board_get_camera();
    if (!camera) {
        return false;
    }
    // The single DVP buffer may hold a frame from before the head moved; drop it
    if (!camera->StreamCaptures() || !camera->StreamCaptures()) {
        return false;
    }
    width             = camera->GetFrameWidth();
    height            = camera->GetFrameHeight();
    const size_t need = static_cast<size_t>(width) * height * 3;
    if (buf_size != need) {
        heap_caps_free(buf);
        buf      = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        buf_size = buf ? need : 0;
        if (!buf) {
            return false;
        }
    }
    if (!to_rgb888(camera->GetFrameData(), width, height, camera->GetFrameFormat(), buf)) {
        mclog::tagError(_tag, "unsupported camera format 0x{:08x}", camera->GetFrameFormat());
        return false;
    }
    return true;
}

// Grab a fresh frame and find the most confident face. Returns false when the camera fails.
static bool detect(FaceDetector& detector, bool& found, Face& face)
{
    static uint8_t* rgb    = nullptr;
    static size_t rgb_size = 0;

    int width = 0, height = 0;
    if (!grab_rgb888(rgb, rgb_size, width, height)) {
        return false;
    }
    dl::image::img_t img = {.data     = rgb,
                            .width    = static_cast<uint16_t>(width),
                            .height   = static_cast<uint16_t>(height),
                            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888};

    const int64_t start = esp_timer_get_time();
    auto& results       = detector.run(img);
    const int infer_ms  = (esp_timer_get_time() - start) / 1000;

    found = !results.empty();
    if (found) {
        auto best = std::max_element(results.begin(), results.end(),
                                     [](const auto& a, const auto& b) { return a.score < b.score; });
        face.x    = ((best->box[0] + best->box[2]) * 0.5f - width * 0.5f) / (width * 0.5f);
        face.y    = ((best->box[1] + best->box[3]) * 0.5f - height * 0.5f) / (height * 0.5f);
        face.size = static_cast<float>(best->box[2] - best->box[0]) / width;
    }
    set_status([&](FaceTrackStatus_t& s) {
        s.inferMs = infer_ms;
        s.faces   = results.size();
        if (found) {
            s.lastSeenUs = esp_timer_get_time();
            s.lastX      = face.x;
            s.lastY      = face.y;
        }
    });
    return true;
}

/* -------------------------- Conversation front --------------------------- */

// The head angles that last faced the user become the conversation front (Motion::setFront): idle motion,
// actions and speech centre on it, and the next tracking session looks there first. Saved to flash at the
// end of a session so it survives a reboot.
static const char* _settings_ns = "face";

void Hal::loadFaceFront()
{
    Settings settings(_settings_ns, false);
    auto& motion = GetStackChan().motion();
    if (settings.GetBool("has_front", false)) {
        motion.setFront(settings.GetInt("front_yaw", 0), std::min<int>(settings.GetInt("front_pitch", 0), _front_pitch_max));
    } else {
        motion.clearFront();
    }
}

void Hal::resetFaceFront()
{
    {
        LvglLockGuard lock;
        GetStackChan().motion().clearFront();
    }
    Settings settings(_settings_ns, true);
    settings.SetBool("has_front", false);
    mclog::tagInfo(_tag, "conversation front cleared");
}

// Writing flash needs a task whose stack is in internal RAM, and the tracker's stack is in PSRAM, so the save
// runs in a short-lived task of its own
static void save_front(int yaw, int pitch)
{
    static int saved[2];
    saved[0] = yaw;
    saved[1] = pitch;
    auto task = [](void* arg) {
        auto angles = static_cast<int*>(arg);
        {
            Settings settings(_settings_ns, true);
            settings.SetBool("has_front", true);
            settings.SetInt("front_yaw", angles[0]);
            settings.SetInt("front_pitch", angles[1]);
        }
        mclog::tagInfo(_tag, "conversation front saved ({}, {})", angles[0] / 10, angles[1] / 10);
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(task, "face_front", 4096, saved, 2, nullptr) != pdPASS) {
        mclog::tagError(_tag, "failed to start the save task");
    }
}

// "We made eye contact" badge on the face when the user is found
static std::atomic<bool> _eye_mark{true};
static int64_t _last_mark_us                = 0;
static constexpr uint32_t _mark_ms          = 3000;
static constexpr int64_t _mark_min_gap_us   = 5 * 1000000LL;  // do not flash it again right after it went away

static void show_eye_contact()
{
    const int64_t now = esp_timer_get_time();
    if (!_eye_mark || (_last_mark_us != 0 && now - _last_mark_us < _mark_ms * 1000LL + _mark_min_gap_us)) {
        return;
    }
    LvglLockGuard lock;
    auto& stackchan = GetStackChan();
    if (stackchan.hasAvatar()) {
        stackchan.avatar().addDecorator(std::make_unique<avatar::EyeContactDecorator>(lv_screen_active(), _mark_ms));
        _last_mark_us = now;
    }
}

// Move the head unless someone else (IMU pick-up) holds it. Our modify lock keeps idle motion / head pet away.
static bool move_head(int yaw, int pitch, bool& holding_motion)
{
    LvglLockGuard lock;
    auto& motion = GetStackChan().motion();
    if (!holding_motion && motion.isModifyLocked()) {
        return false;
    }
    motion.setModifyLock(true);
    holding_motion = true;
    motion.moveWithSpeed(std::clamp(yaw, -_yaw_limit, _yaw_limit), std::clamp(pitch, 0, _pitch_max), _move_speed);
    return true;
}

static void track(FaceDetector& detector)
{
    mclog::tagInfo(_tag, "tracking started");
    set_status([](FaceTrackStatus_t& s) { s.active = true; });
    bool holding_motion = false;

    // Until a face is found, look in turn: where the user was last time, straight on from where the head is,
    // then a little up and to the sides (StackChan usually sits below the user's face). With nobody found the
    // head goes back to where it started and just watches.
    int start_yaw = 0, start_pitch = 0;
    bool has_front = false;
    uitk::Vector2i front;
    {
        LvglLockGuard lock;
        auto& motion = GetStackChan().motion();
        start_yaw    = motion.getCurrentYawAngle();
        start_pitch  = motion.getCurrentPitchAngle();
        has_front    = motion.hasFront();
        front        = motion.getFront();
    }
    std::vector<uitk::Vector2i> search;
    if (has_front) {
        search.emplace_back(front.x, front.y);
    }
    search.emplace_back(start_yaw, start_pitch);
    for (auto [dy, dp] : {std::pair{0, 150}, {0, 300}, {-250, 200}, {250, 200}}) {
        search.emplace_back(std::clamp(start_yaw + dy, -_yaw_limit, _yaw_limit),
                            std::clamp(start_pitch + dp, 0, _pitch_max));
    }
    size_t search_index = 0;
    int search_frames   = _search_frames;
    move_head(search[0].x, search[0].y, holding_motion);
    int hits = 0, hit_yaw = 0, hit_pitch = 0;  // consecutive frames with a face, and where it was
    bool seen = false;
    int seen_yaw = 0, seen_pitch = 0;

    while (wanted()) {
        bool moving = false;
        {
            LvglLockGuard lock;
            auto& stackchan = GetStackChan();
            if (!stackchan.hasAvatar()) {
                break;
            }
            moving = stackchan.motion().isMoving();
        }
        if (moving) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(_settle_ms));

        bool found = false;
        Face face{};
        if (!detect(detector, found, face)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // Far-away "faces" are mostly patterns (fan grilles, posters)
        if (found && face.size < _min_face_size) {
            found = false;
        }
        if (found) {
            // Image right is StackChan's right (positive yaw); image up is positive pitch
            const float yaw_deg   = face.x * _hfov_deg * 0.5f * _gain;
            const float pitch_deg = -face.y * _vfov_deg * 0.5f * _gain;
            int yaw = 0, pitch = 0;
            {
                LvglLockGuard lock;
                auto& motion = GetStackChan().motion();
                yaw          = std::clamp(motion.getCurrentYawAngle() + static_cast<int>(yaw_deg * 10), -_yaw_limit, _yaw_limit);
                pitch        = std::clamp(motion.getCurrentPitchAngle() + static_cast<int>(pitch_deg * 10), 0, _pitch_max);
            }
            // A single frame can be a false hit; only trust a face seen in consecutive frames at about the
            // same place
            hits = (hits > 0 && std::abs(yaw - hit_yaw) <= _same_place && std::abs(pitch - hit_pitch) <= _same_place)
                       ? hits + 1
                       : 1;
            hit_yaw   = yaw;
            hit_pitch = pitch;

            if (hits == _confirm_move) {
                show_eye_contact();
            }
            if (hits >= _confirm_move) {
                search.clear();
                if (std::fabs(yaw_deg) > _deadband_deg || std::fabs(pitch_deg) > _deadband_deg) {
                    move_head(yaw, pitch, holding_motion);
                }
            }
            if (hits >= _confirm_front) {
                seen       = true;
                seen_yaw   = yaw;
                seen_pitch = std::min(pitch, _front_pitch_max);
                LvglLockGuard lock;
                GetStackChan().motion().setFront(seen_yaw, seen_pitch);
            }
        } else {
            hits = 0;
        }
        // Keep looking at a search position while an unconfirmed face is being checked
        if (hits == 0 && !search.empty() && --search_frames == 0) {
            if (++search_index < search.size()) {
                search_frames = _search_frames;
                move_head(search[search_index].x, search[search_index].y, holding_motion);
            } else {
                mclog::tagInfo(_tag, "no face found nearby, going back");
                search.clear();
                move_head(start_yaw, start_pitch, holding_motion);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(_interval_ms));
    }

    if (holding_motion) {
        LvglLockGuard lock;
        GetStackChan().motion().setModifyLock(false);
    }
    // Once per session, and only when it moved noticeably, to spare the flash
    if (seen && (!has_front || std::abs(seen_yaw - front.x) > 30 || std::abs(seen_pitch - front.y) > 30)) {
        save_front(seen_yaw, seen_pitch);
    }
    set_status([](FaceTrackStatus_t& s) { s.active = false; });
    mclog::tagInfo(_tag, "tracking stopped");
}

/* ----------------------------- Idle face search ---------------------------- */

// Every few minutes while the AVATAR app idles, look for the user for a few seconds (the session's usual
// search: the front, straight on, a little up and to the sides)
static std::atomic<bool> _idle_active{false};
static std::atomic<int> _idle_minutes{0};  // 0 = off
static std::atomic<int64_t> _next_idle_us{0};
static constexpr uint32_t _idle_session_ms = 8000;

static void schedule_idle_search(int64_t now)
{
    // Spread it out a little (75-125% of the interval) so it does not feel mechanical
    const int64_t interval = static_cast<int64_t>(_idle_minutes) * 60 * 1000000;
    _next_idle_us          = now + interval * (75 + esp_random() % 51) / 100;
}

// Start an idle search when it is due and StackChan is not busy talking / listening / being handled
static void maybe_idle_search()
{
    const int64_t now = esp_timer_get_time();
    if (!_idle_active || _idle_minutes <= 0 || wanted()) {
        return;
    }
    if (_next_idle_us == 0) {
        schedule_idle_search(now);
        return;
    }
    if (now < _next_idle_us) {
        return;
    }
    bool idle = !GetHAL().isAudioBusy();
    if (idle) {
        LvglLockGuard lock;
        auto& stackchan = GetStackChan();
        idle            = stackchan.hasAvatar() && !stackchan.motion().isModifyLocked();
    }
    if (!idle) {
        _next_idle_us = now + 30 * 1000000LL;  // try again shortly
        return;
    }
    mclog::tagInfo(_tag, "idle face search");
    _until_us = now + static_cast<int64_t>(_idle_session_ms) * 1000;
    schedule_idle_search(now);
}

static void face_task(void*)
{
    std::unique_ptr<FaceDetector> detector;
    bool load_failed = false;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        maybe_idle_search();
        if (!wanted()) {
            continue;
        }
        if (!detector && !load_failed) {
            std::string error;
            detector = load_detector(error);
            if (!detector) {
                mclog::tagError(_tag, "{}", error);
                load_failed = true;
            }
            set_status([&](FaceTrackStatus_t& s) {
                s.modelLoaded = detector != nullptr;
                s.error       = error;
            });
        }
        if (detector) {
            track(*detector);
        }
    }
}

static void wake_task()
{
    if (!_task) {
        // Inference only needs PSRAM; keep internal RAM for Wi-Fi / TLS. Lower priority than the notify task
        // so feeding the speaker always comes first.
        if (xTaskCreatePinnedToCoreWithCaps(face_task, "face_track", 12288, nullptr, 1, &_task, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            mclog::tagError(_tag, "failed to start the face tracking task");
            _task = nullptr;
            return;
        }
    }
    xTaskNotifyGive(_task);
}

void Hal::beginFaceTracking()
{
    _holds++;
    wake_task();
}

void Hal::endFaceTracking()
{
    if (_holds > 0) {
        _holds--;
    }
}

void Hal::trackFaceFor(uint32_t ms)
{
    _until_us = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
    wake_task();
}

void Hal::setFaceIdleSearch(int intervalMinutes, bool eyeContactMark)
{
    if (_idle_minutes != intervalMinutes) {
        _next_idle_us = 0;  // reschedule from now
    }
    _idle_minutes = std::max(intervalMinutes, 0);
    _eye_mark     = eyeContactMark;
}

void Hal::setFaceIdleActive(bool active)
{
    _idle_active = active;
    _next_idle_us = 0;
    if (active) {
        wake_task();  // the task keeps the idle schedule
    }
}

FaceTrackStatus_t Hal::getFaceTrackStatus()
{
    FaceTrackStatus_t out;
    {
        std::lock_guard<std::mutex> lock(_status_mutex);
        out = _status;
    }
    LvglLockGuard lock;
    auto& motion = GetStackChan().motion();
    out.hasFront = motion.hasFront();
    out.frontYaw = motion.getFront().x;
    out.frontPitch = motion.getFront().y;
    return out;
}

bool Hal::getFaceCameraBmp(PsramBytes& bmp, std::string& error)
{
    uint8_t* rgb    = nullptr;
    size_t rgb_size = 0;
    int width = 0, height = 0;
    if (!grab_rgb888(rgb, rgb_size, width, height)) {
        heap_caps_free(rgb);
        error = "camera capture failed";
        return false;
    }

    // 24-bit BMP: bottom-up rows of BGR, each padded to 4 bytes
    const uint32_t row   = (width * 3 + 3) & ~3u;
    const uint32_t data  = row * height;
    const uint32_t total = 54 + data;
    bmp.assign(total, 0);
    auto put16 = [&](size_t at, uint16_t v) { bmp[at] = v & 0xff; bmp[at + 1] = v >> 8; };
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) bmp[at + i] = (v >> (i * 8)) & 0xff; };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, total);
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, height);
    put16(26, 1);
    put16(28, 24);
    put32(34, data);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = rgb + static_cast<size_t>(height - 1 - y) * width * 3;
        uint8_t* dst       = bmp.data() + 54 + static_cast<size_t>(y) * row;
        for (int x = 0; x < width; ++x, src += 3, dst += 3) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
    }
    heap_caps_free(rgb);
    return true;
}
