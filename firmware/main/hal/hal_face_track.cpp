/*
 * Face tracking
 *
 * While a notification is read aloud, StackChan looks for a face with the CoreS3 camera and turns its head
 * toward it. Faces are found with the ESP-DL MSR+MNP detector (the models of espressif/human_face_detect,
 * stored in the assets partition as face_msr_s8_v1.espdl / face_mnp_s8_v1.espdl). The camera sits in the
 * head, so each step turns by the face's offset from the image centre and the next frame shows what is left.
 * When no face is seen the head is left alone, except that each session first looks where the face was last
 * time and comes back if it is not there.
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "board/stackchan_camera.h"
#include <stackchan/stackchan.h>
#include <assets.h>
#include <mooncake_log.h>
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
static constexpr int _pitch_max = 600;

static constexpr uint32_t _settle_ms   = 120;  // let the head stop shaking before grabbing a frame
static constexpr uint32_t _interval_ms = 100;  // pause between detections so the audio task keeps up
static constexpr int _recall_frames    = 4;    // frames to look for the user where they were last time

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
            m_model, m_image_preprocessor, 0.5f, 0.5f, 10,
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

// Head angles (0.1 degree) that last faced the user; the next session looks there first
static bool _has_last_face = false;
static int _last_face_yaw = 0, _last_face_pitch = 0;

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

    // Look where the user was last time first; if they are not there after a few frames, go back and watch from
    // where the head started
    int start_yaw = 0, start_pitch = 0;
    {
        LvglLockGuard lock;
        auto& motion = GetStackChan().motion();
        start_yaw    = motion.getCurrentYawAngle();
        start_pitch  = motion.getCurrentPitchAngle();
    }
    int recall_tries = 0;
    if (_has_last_face && move_head(_last_face_yaw, _last_face_pitch, holding_motion)) {
        recall_tries = _recall_frames;
        mclog::tagInfo(_tag, "looking where the face was last time ({}, {})", _last_face_yaw / 10, _last_face_pitch / 10);
    }

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
        if (found) {
            recall_tries = 0;
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
            _has_last_face   = true;
            _last_face_yaw   = yaw;
            _last_face_pitch = pitch;
            if (std::fabs(yaw_deg) > _deadband_deg || std::fabs(pitch_deg) > _deadband_deg) {
                move_head(yaw, pitch, holding_motion);
            }
        } else if (recall_tries > 0 && --recall_tries == 0) {
            mclog::tagInfo(_tag, "no face where it was last time, going back");
            move_head(start_yaw, start_pitch, holding_motion);
        }
        vTaskDelay(pdMS_TO_TICKS(_interval_ms));
    }

    if (holding_motion) {
        LvglLockGuard lock;
        GetStackChan().motion().setModifyLock(false);
    }
    set_status([](FaceTrackStatus_t& s) { s.active = false; });
    mclog::tagInfo(_tag, "tracking stopped");
}

static void face_task(void*)
{
    std::unique_ptr<FaceDetector> detector;
    bool load_failed = false;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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

FaceTrackStatus_t Hal::getFaceTrackStatus()
{
    std::lock_guard<std::mutex> lock(_status_mutex);
    return _status;
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
