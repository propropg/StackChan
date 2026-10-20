/*
 * Eye contact badge: a round yellow badge with an eye icon in the top left corner of the face, gently
 * blinking while it is shown. Drawn with plain LVGL objects, so it needs no image asset.
 */
#include "decorators.h"
#include <hal/hal.h>

using namespace stackchan::avatar;

static constexpr int _badge_size          = 40;
static constexpr int _badge_x             = 14;
static constexpr int _badge_y             = 34;  // below the status bar
static constexpr uint32_t _blink_interval = 400;
static const lv_color_t _badge_color      = lv_color_hex(0xFFD54A);
static const lv_color_t _icon_color       = lv_color_hex(0x3A2A00);

EyeContactDecorator::EyeContactDecorator(lv_obj_t* parent, uint32_t destroyAfterMs)
{
    _badge = lv_obj_create(parent);
    lv_obj_remove_style_all(_badge);
    lv_obj_set_size(_badge, _badge_size, _badge_size);
    lv_obj_align(_badge, LV_ALIGN_TOP_LEFT, _badge_x, _badge_y);
    lv_obj_set_style_radius(_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_badge, _badge_color, 0);
    lv_obj_set_style_bg_opa(_badge, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = lv_label_create(_badge);
    lv_label_set_text(icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, _icon_color, 0);
    lv_obj_center(icon);

    uint32_t now = GetHAL().millis();
    if (destroyAfterMs > 0) {
        _destroy_at   = now + destroyAfterMs;
        _has_lifetime = true;
    }
    _next_blink = now + _blink_interval;
}

EyeContactDecorator::~EyeContactDecorator()
{
    if (_badge) {
        lv_obj_delete(_badge);
    }
}

void EyeContactDecorator::_update()
{
    uint32_t now = GetHAL().millis();
    if (_has_lifetime && now >= _destroy_at) {
        requestDestroy();
        return;
    }
    if (now >= _next_blink) {
        _next_blink = now + _blink_interval;
        _dim        = !_dim;
        lv_obj_set_style_opa(_badge, _dim ? LV_OPA_60 : LV_OPA_COVER, 0);
    }
}

void EyeContactDecorator::setVisible(bool visible)
{
    Element::setVisible(visible);
    if (visible) {
        lv_obj_remove_flag(_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_badge, LV_OBJ_FLAG_HIDDEN);
    }
}
