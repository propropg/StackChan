/*
 * SETUP > Voice > VOICEVOX
 */
#include "workers.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/common/loading_page/loading_page.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

LV_FONT_DECLARE(font_jp_18);

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;

static std::string _tag = "Setup-VOICEVOX";

namespace {
// Normal style of the popular characters; any other speaker can be picked on the web settings page
const std::vector<VoicevoxWorker::Speaker> _preset_speakers = {
    {3, "ずんだもん"},   {2, "四国めたん"}, {8, "春日部つむぎ"}, {10, "雨晴はう"},
    {9, "波音リツ"},     {11, "玄野武宏"},  {13, "青山龍星"},   {14, "冥鳴ひまり"},
    {16, "九州そら"},    {20, "もち子さん"}, {21, "剣崎雌雄"},
};

struct TestJob {
    std::shared_ptr<VoicevoxWorker::TestState> state;
};

void test_task(void* arg)
{
    std::unique_ptr<TestJob> job(static_cast<TestJob*>(arg));
    std::string error;
    bool ok = GetHAL().speakText("こんにちは、スタックチャンだよ。", "happy", "", error);
    job->state->error = error;
    job->state->status.store(ok ? 2 : 3);
    job.reset();
    vTaskDelete(nullptr);
}

std::unique_ptr<Container> make_card(lv_obj_t* parent, int y, int height)
{
    auto card = std::make_unique<Container>(parent);
    card->setSize(296, height);
    card->align(LV_ALIGN_TOP_MID, 0, y);
    card->setBgColor(lv_color_hex(0xD2E3FF));
    card->setBorderWidth(0);
    card->setRadius(18);
    card->setPadding(0, 0, 0, 0);
    card->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

std::unique_ptr<Label> make_title(lv_obj_t* parent, const char* text)
{
    auto label = std::make_unique<Label>(parent);
    label->setText(text);
    label->setTextFont(&lv_font_montserrat_16);
    label->setTextColor(lv_color_hex(0x26206A));
    label->setWidth(260);
    label->setTextAlign(LV_TEXT_ALIGN_CENTER);
    label->align(LV_ALIGN_TOP_MID, 0, 18);
    return label;
}
}  // namespace

VoicevoxWorker::VoicevoxWorker()
{
    mclog::info("VoicevoxWorker start");

    // The speak test and the settings page URL need Wi-Fi (same flow as Check for Updates)
    {
        auto loading_page = std::make_unique<view::LoadingPage>(0xF6F6F6, 0x26206A);
        GetHAL().lvglUnlock();
        GetHAL().startNetwork([&](std::string_view msg) {
            LvglLockGuard lock;
            loading_page->setMessage(msg);
        });
        GetHAL().lvglLock();
    }

    _config   = GetHAL().getVoicevoxConfig();
    _speakers = _preset_speakers;
    int current_index = -1;
    for (size_t i = 0; i < _speakers.size(); ++i) {
        if (_speakers[i].id == _config.speaker) {
            current_index = static_cast<int>(i);
        }
    }
    if (current_index < 0) {
        // Chosen on the web settings page; keep it selectable
        _speakers.insert(_speakers.begin(), {_config.speaker, fmt::format("ID {}（Web設定）", _config.speaker)});
        current_index = 0;
    }

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 24, 18);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    // On / off
    _panel_enabled       = make_card(_panel->get(), 20, 110);
    _label_enabled_title = make_title(_panel_enabled->get(), "Speak with VOICEVOX:");
    _switch_enabled      = std::make_unique<Switch>(_panel_enabled->get());
    _switch_enabled->setSize(64, 36);
    _switch_enabled->align(LV_ALIGN_TOP_MID, 0, 56);
    _switch_enabled->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _switch_enabled->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR | LV_STATE_CHECKED);
    _switch_enabled->setBgColor(lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    if (_config.enabled) {
        _switch_enabled->addState(LV_STATE_CHECKED);
    }

    // Speaker
    _panel_speaker       = make_card(_panel->get(), 150, 148);
    _label_speaker_title = make_title(_panel_speaker->get(), "Speaker:");

    _label_speaker_value = std::make_unique<Label>(_panel_speaker->get());
    _label_speaker_value->setTextFont(&font_jp_18);
    _label_speaker_value->setTextColor(lv_color_hex(0x26206A));
    _label_speaker_value->align(LV_ALIGN_TOP_MID, 0, 62);

    _slider_speaker = std::make_unique<Slider>(_panel_speaker->get());
    _slider_speaker->align(LV_ALIGN_TOP_MID, 0, 108);
    _slider_speaker->setRange(0, _speakers.size() - 1);
    _slider_speaker->setSize(250, 18);
    _slider_speaker->setBgColor(lv_color_hex(0x615B9E), LV_PART_KNOB);
    _slider_speaker->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR);
    _slider_speaker->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _slider_speaker->setBgOpa(255);
    _slider_speaker->setValue(current_index);
    _slider_speaker->onValueChanged().connect([this](int32_t value) { _pending_speaker_index = value; });

    // API key status; the key itself is typed on the web page (there is no on-screen keyboard)
    _panel_key       = make_card(_panel->get(), 318, 128);
    _label_key_title = make_title(
        _panel_key->get(),
        _config.apiKey.empty() ? "API key: not set"
                               : fmt::format("API key: set (...{})",
                                             _config.apiKey.substr(_config.apiKey.size() - std::min<size_t>(4, _config.apiKey.size())))
                                     .c_str());
    _label_key_info = std::make_unique<Label>(_panel_key->get());
    _label_key_info->setText(fmt::format("Set the key and more voices in a browser:\nhttp://{}/\n(open the AVATAR app first)",
                                         GetHAL().getLocalIp()));
    _label_key_info->setTextFont(&lv_font_montserrat_14);
    _label_key_info->setTextColor(lv_color_hex(0x26206A));
    _label_key_info->setWidth(270);
    _label_key_info->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_key_info->align(LV_ALIGN_TOP_MID, 0, 46);

    _btn_test = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_test);
    _btn_test->align(LV_ALIGN_TOP_MID, 0, 466);
    _btn_test->setSize(290, 50);
    _btn_test->label().setText("Test");
    _btn_test->onClick().connect([this]() { _test_flag = true; });

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 532);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });

    update_speaker_label();
}

void VoicevoxWorker::update()
{
    if (_pending_speaker_index != -1) {
        _config.speaker        = _speakers[_pending_speaker_index].id;
        _pending_speaker_index = -1;
        update_speaker_label();
    }

    if (_test_flag) {
        _test_flag = false;
        start_test();
    }

    // Reflect the test result on the button
    if (_test) {
        int status = _test->status.load();
        if (status != _shown_test_status) {
            _shown_test_status = status;
            switch (status) {
                case 1:
                    _btn_test->label().setText("Speaking...");
                    _btn_test->addState(LV_STATE_DISABLED);
                    _btn_confirm->addState(LV_STATE_DISABLED);
                    break;
                case 2:
                    _btn_test->label().setText("OK! Test again");
                    break;
                case 3: {
                    mclog::tagError(_tag, "test failed: {}", _test->error);
                    const auto& e = _test->error;
                    _btn_test->label().setText(e == "invalidApiKey"         ? "Invalid API key"
                                               : e == "notEnoughPoints"     ? "Out of points today"
                                               : e == "Wi-Fi not connected" ? "No Wi-Fi"
                                                                            : "Test failed");
                    break;
                }
                default:
                    break;
            }
            if (status >= 2) {
                _btn_test->removeState(LV_STATE_DISABLED);
                _btn_confirm->removeState(LV_STATE_DISABLED);
            }
        }
    }

    if (_confirm_flag) {
        _confirm_flag = false;
        if (_test && _test->status.load() == 1) {
            return;
        }
        save();
        _is_done = true;
    }
}

void VoicevoxWorker::update_speaker_label()
{
    for (const auto& speaker : _speakers) {
        if (speaker.id == _config.speaker) {
            _label_speaker_value->setText(speaker.name);
            return;
        }
    }
}

void VoicevoxWorker::save()
{
    // Re-read so a key saved from the web page meanwhile is not overwritten
    auto config    = GetHAL().getVoicevoxConfig();
    config.enabled = _switch_enabled->getValue();
    config.speaker = _config.speaker;
    GetHAL().setVoicevoxConfig(config);
    _config = config;
}

void VoicevoxWorker::start_test()
{
    if (_test && _test->status.load() == 1) {
        return;
    }
    // Test exactly what will be saved
    save();

    _test = std::make_shared<TestState>();
    _test->status.store(1);
    auto* job = new TestJob{_test};
    if (xTaskCreate(test_task, "voicevox_test", 8192, job, 3, nullptr) != pdPASS) {
        delete job;
        _test->error = "no memory";
        _test->status.store(3);
    }
}
