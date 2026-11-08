/*
 * Digital clock at the top of the face: cyan seven-segment digits with the unlit segments faintly showing,
 * like an LCD panel, and a colon that blinks every second. Drawn with plain LVGL objects, so it needs no font
 * or image asset.
 */
#include "decorators.h"
#include <algorithm>
#include <ctime>

using namespace stackchan::avatar;

static constexpr int _clock_x = 12;  // margin from the screen edge when aligned left / right
static constexpr int _clock_y = 8;   // the eyes start around y = 72
static constexpr lv_opa_t _off_opa = LV_OPA_10;
static const lv_color_t _lit_color = lv_color_hex(0x5CE1FF);

// Segments a b c d e f g as bits 0..6, for 0-9 and then "-"
static constexpr uint8_t _digit_masks[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x40};
static constexpr int _dash = 10;

static lv_obj_t* create_bar(lv_obj_t* parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_radius(bar, radius, 0);
    lv_obj_set_style_bg_color(bar, _lit_color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(bar, _off_opa, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

ClockDecorator::ClockDecorator(lv_obj_t* parent, lv_align_t align, int digitHeight)
{
    // Every part scales with the digit height (proportions tuned at 50 px)
    const int digit_h   = digitHeight;
    const int digit_w   = digit_h * 28 / 50;
    const int thick     = std::max(4, digit_h * 7 / 50);
    const int digit_gap = digit_h * 8 / 50;
    const int colon_w   = digit_h * 16 / 50;
    const int radius    = thick / 2;

    // D D : D D
    const int digit_x[_digit_count] = {0, digit_w + digit_gap, 2 * (digit_w + digit_gap) + colon_w,
                                       3 * (digit_w + digit_gap) + colon_w};
    const int width                 = digit_x[3] + digit_w;

    _panel = lv_obj_create(parent);
    lv_obj_remove_style_all(_panel);
    lv_obj_set_size(_panel, width, digit_h);
    const int offset_x = align == LV_ALIGN_TOP_LEFT ? _clock_x : align == LV_ALIGN_TOP_RIGHT ? -_clock_x : 0;
    lv_obj_align(_panel, align, offset_x, _clock_y);
    lv_obj_remove_flag(_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    const int half    = digit_h / 2;
    const int h_len   = digit_w - thick - 2;  // horizontal segments sit between the vertical ones
    const int v_len   = half - thick / 2 - 2;
    const int h_x     = thick / 2 + 1;
    const int right_x = digit_w - thick;
    for (int i = 0; i < _digit_count; i++) {
        const int x = digit_x[i];
        auto& seg = _segments[i];
        seg[0] = create_bar(_panel, x + h_x, 0, h_len, thick, radius);  // a
        seg[1] = create_bar(_panel, x + right_x, thick / 2 + 1, thick, v_len, radius);  // b
        seg[2] = create_bar(_panel, x + right_x, half + 1, thick, v_len, radius);  // c
        seg[3] = create_bar(_panel, x + h_x, digit_h - thick, h_len, thick, radius);  // d
        seg[4] = create_bar(_panel, x, half + 1, thick, v_len, radius);  // e
        seg[5] = create_bar(_panel, x, thick / 2 + 1, thick, v_len, radius);  // f
        seg[6] = create_bar(_panel, x + h_x, half - thick / 2, h_len, thick, radius);  // g
    }

    const int colon_x = 2 * digit_w + digit_gap + (digit_gap + colon_w - thick) / 2;
    _colon[0]         = create_bar(_panel, colon_x, digit_h * 3 / 10 - thick / 2, thick, thick, radius);
    _colon[1]         = create_bar(_panel, colon_x, digit_h * 7 / 10 - thick / 2, thick, thick, radius);

    _update();
}

ClockDecorator::~ClockDecorator()
{
    if (_panel) {
        lv_obj_delete(_panel);
    }
}

void ClockDecorator::show_digit(int index, int value)
{
    const uint8_t mask = _digit_masks[value];
    for (int s = 0; s < _segment_count; s++) {
        lv_obj_set_style_opa(_segments[index][s], (mask >> s) & 1 ? LV_OPA_COVER : _off_opa, 0);
    }
}

void ClockDecorator::_update()
{
    const time_t now = time(nullptr);
    struct tm local_tm;
    localtime_r(&now, &local_tm);

    // Before the first SNTP sync the clock still reads 1970
    const bool synced = local_tm.tm_year + 1900 >= 2024;
    const int minute  = synced ? local_tm.tm_hour * 60 + local_tm.tm_min : -1;
    if (minute != _shown_minute) {
        _shown_minute = minute;
        if (synced) {
            show_digit(0, local_tm.tm_hour / 10);
            show_digit(1, local_tm.tm_hour % 10);
            show_digit(2, local_tm.tm_min / 10);
            show_digit(3, local_tm.tm_min % 10);
        } else {
            for (int i = 0; i < _digit_count; i++) {
                show_digit(i, _dash);
            }
        }
    }

    if (local_tm.tm_sec != _shown_second) {
        _shown_second = local_tm.tm_sec;
        const lv_opa_t opa = !synced || local_tm.tm_sec % 2 == 0 ? LV_OPA_COVER : _off_opa;
        lv_obj_set_style_opa(_colon[0], opa, 0);
        lv_obj_set_style_opa(_colon[1], opa, 0);
    }
}

void ClockDecorator::setVisible(bool visible)
{
    Element::setVisible(visible);
    if (visible) {
        lv_obj_remove_flag(_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_panel, LV_OBJ_FLAG_HIDDEN);
    }
}
