/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "stackchan_camera.h"
#include <cstdint>
#include <lvgl.h>
#include <driver/i2c_master.h>
#include <string_view>

namespace hal_bridge {

struct TouchPoint_t {
    int num = 0;
    int x   = -1;
    int y   = -1;
};

struct Data_t {
    TouchPoint_t touchPoint;
    bool isXiaozhiMode              = false;
    bool isXiaozhiModeToggleEnabled = false;
};

struct XiaozhiConfig_t {
    uint32_t idleShutdownTimeSeconds = 600;
    bool allowShutdownWhenCharging   = false;
    uint8_t idleRandomMovementLevel  = 2;
    bool startAiAgentOnBoot          = false;
};

void lock();
void unlock();
Data_t& get_data();

void set_touch_point(int num, int x, int y);
TouchPoint_t get_touch_point();

bool is_xiaozhi_mode();
void set_xiaozhi_mode(bool mode);
void toggle_xiaozhi_chat_state();

void disply_lvgl_lock();
void disply_lvgl_unlock();
lv_disp_t* display_get_lvgl_display();

void xiaozhi_board_init();
void start_xiaozhi_app();
bool is_xiaozhi_ready();
bool is_xiaozhi_idle();
XiaozhiConfig_t get_xiaozhi_config();
void set_xiaozhi_config(const XiaozhiConfig_t& config);

i2c_master_bus_handle_t board_get_i2c_bus();
StackChanCamera* board_get_camera();
int board_get_battery_level();
bool board_is_battery_charging();
// AXP2101 power-on (0x20) and power-off (0x21) status read at boot: why the PMIC last turned the system on / off
void board_get_pmic_power_status(uint8_t& on_status, uint8_t& off_status);
// AXP2101 ADC readings: battery / VBUS / VSYS voltage in mV and die temperature in 0.1 C (false when unreadable)
bool board_get_power_readings(int& vbat_mv, int& vbus_mv, int& vsys_mv, int& die_temp_dc);
// AXP2101 VBUS input current limit register (0x16) as found at boot and now
void board_get_pmic_vbus_limit(uint8_t& at_boot, uint8_t& now);
// AXP2101 DCDC UVP/OVP power-off control register (0x23) as found at boot and now
void board_get_pmic_uvp_ctrl(uint8_t& at_boot, uint8_t& now);
// AXP2101 PMU status registers 0x00 and 0x01
void board_get_pmic_status(uint8_t& status1, uint8_t& status2);
void board_set_backlight_brightness(uint8_t brightness, bool permanent = false);
uint8_t board_get_backlight_brightness();
void board_set_speaker_volume(uint8_t volume, bool permanent = false);
uint8_t board_get_speaker_volume();

void app_play_sound(const std::string_view& sound);

}  // namespace hal_bridge
