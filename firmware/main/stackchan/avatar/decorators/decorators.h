/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../avatar/decorator.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <cstdint>
#include <memory>

namespace stackchan::avatar {

/**
 * @brief
 *
 */
class HeartDecorator : public Decorator {
public:
    /**
     * @brief
     *
     * @param parent
     * @param destroyAfterMs Destroy after milliseconds, 0 for infinite
     * @param animationIntervalMs Animation update interval in milliseconds, 0 for none
     */
    HeartDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 500);
    ~HeartDecorator();

    void _update() override;

    using Element::setPosition;

    void setPosition(int x, int y);
    void setRotation(int rotation) override;
    void setColor(lv_color_t color);

private:
    std::unique_ptr<uitk::lvgl_cpp::Image> _heart;

    uint32_t _destroy_at            = 0;
    uint32_t _next_animation_tick   = 0;
    uint32_t _animation_interval_ms = 0;
    bool _has_lifetime              = false;

    int _animation_index = 0;
};

/**
 * @brief
 *
 */
class AngryDecorator : public Decorator {
public:
    /**
     * @brief
     *
     * @param parent
     * @param destroyAfterMs Destroy after milliseconds, 0 for infinite
     * @param animationIntervalMs Animation update interval in milliseconds, 0 for none
     */
    AngryDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 500);
    ~AngryDecorator();

    void _update() override;

    using Element::setPosition;

    void setPosition(int x, int y);
    void setRotation(int rotation) override;
    void setColor(lv_color_t color);

private:
    std::unique_ptr<uitk::lvgl_cpp::Image> _angry;

    uint32_t _destroy_at            = 0;
    uint32_t _next_animation_tick   = 0;
    uint32_t _animation_interval_ms = 0;
    bool _has_lifetime              = false;

    int _animation_index = 0;
};

/**
 * @brief
 *
 */
class SweatDecorator : public Decorator {
public:
    /**
     * @brief
     *
     * @param parent
     * @param destroyAfterMs Destroy after milliseconds, 0 for infinite
     * @param animationIntervalMs Animation update interval in milliseconds, 0 for none
     */
    SweatDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 700);
    ~SweatDecorator();

    void _update() override;

    using Element::setPosition;

    void setPosition(int x, int y);
    void setRotation(int rotation) override;
    void setColor(lv_color_t color);
    void setVisible(bool visible) override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Image> _sweat;

    uint32_t _destroy_at            = 0;
    uint32_t _next_animation_tick   = 0;
    uint32_t _animation_interval_ms = 0;
    bool _has_lifetime              = false;

    int _animation_index = 0;
};

/**
 * @brief
 *
 */
class ShyDecorator : public Decorator {
public:
    /**
     * @brief
     *
     * @param parent
     * @param destroyAfterMs Destroy after milliseconds, 0 for infinite
     */
    ShyDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0);
    ~ShyDecorator();

    void _update() override;

    using Element::setPosition;

    void setPosition(int x, int y);
    void setRotation(int rotation) override;
    void setColor(lv_color_t color);
    void setVisible(bool visible) override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Image> _left;
    std::unique_ptr<uitk::lvgl_cpp::Image> _right;

    uint32_t _destroy_at = 0;
    bool _has_lifetime   = false;
};

/**
 * @brief
 *
 */
class DizzyDecorator : public Decorator {
public:
    /**
     * @brief
     *
     * @param parent
     * @param destroyAfterMs Destroy after milliseconds, 0 for infinite
     * @param animationIntervalMs Animation update interval in milliseconds, 0 for none
     */
    DizzyDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 500);
    ~DizzyDecorator();

    void _update() override;

    using Element::setPosition;

    void setPosition(int x, int y);
    void setRotation(int rotation) override;
    void setColor(lv_color_t color);
    void setVisible(bool visible) override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Image> _left;
    std::unique_ptr<uitk::lvgl_cpp::Image> _right;

    uint32_t _destroy_at            = 0;
    uint32_t _next_animation_tick   = 0;
    uint32_t _animation_interval_ms = 0;
    bool _has_lifetime              = false;

    int _animation_index = 0;
};

/**
 * @brief "We made eye contact" badge: a small blinking eye icon in the top left corner, shown when the face
 * tracker finds the user
 *
 */
class EyeContactDecorator : public Decorator {
public:
    /**
     * @param parent
     * @param destroyAfterMs Destroy after milliseconds, 0 for infinite
     */
    EyeContactDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 3000);
    ~EyeContactDecorator();

    void _update() override;
    void setVisible(bool visible) override;

private:
    lv_obj_t* _badge = nullptr;

    uint32_t _destroy_at     = 0;
    uint32_t _next_blink     = 0;
    bool _has_lifetime       = false;
    bool _dim                = false;
};

/**
 * @brief Digital clock (HH:MM, seven-segment style) at the top of the face, above the eyes. Stays until removed.
 *
 */
class ClockDecorator : public Decorator {
public:
    /**
     * @param parent
     * @param align LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_MID or LV_ALIGN_TOP_RIGHT
     * @param digitHeight Digit height in pixels (keep it under ~60 so the clock stays above the eyes)
     */
    ClockDecorator(lv_obj_t* parent, lv_align_t align = LV_ALIGN_TOP_MID, int digitHeight = 50);
    ~ClockDecorator();

    void _update() override;
    void setVisible(bool visible) override;

private:
    static constexpr int _digit_count   = 4;
    static constexpr int _segment_count = 7;

    lv_obj_t* _panel = nullptr;
    lv_obj_t* _segments[_digit_count][_segment_count] = {};
    lv_obj_t* _colon[2]                                 = {};

    int _shown_minute = -2;  // -1 = clock not set yet ("--:--")
    int _shown_second = -1;

    void show_digit(int index, int value);
};

}  // namespace stackchan::avatar
