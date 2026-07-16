/*
 * Pinout and panel timing for the Guition JC1060P470 (ESP32-P4 + ESP32-C6,
 * 7" 1024x600 MIPI-DSI IPS display, JD9165 panel controller, GT911 capacitive
 * touch over I2C).
 *
 * Pin numbers come from the board vendor's own reference firmware
 * (pins_config.h in the JC1060P470 demo package). They were not verified
 * against a physical unit for this project, so if the display/touch don't
 * come up, check these against your board's schematic first -- Guition is
 * known to change pinout between production revisions.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Display panel (JD9165 over 2-lane MIPI-DSI) ----
#define BOARD_LCD_H_RES         1024
#define BOARD_LCD_V_RES         600
#define BOARD_LCD_RST_GPIO      27
#define BOARD_LCD_BL_GPIO       GPIO_NUM_23
#define BOARD_LCD_BL_ON_LEVEL   1

// VDD_MIPI_DPHY is powered from the ESP32-P4's internal LDO regulator.
#define BOARD_MIPI_DSI_PHY_LDO_CHAN      3
#define BOARD_MIPI_DSI_PHY_LDO_MV        2500

// ---- Capacitive touch (GT911 over I2C) ----
#define BOARD_TOUCH_I2C_PORT     ((i2c_port_num_t)0)
#define BOARD_TOUCH_I2C_SDA      GPIO_NUM_7
#define BOARD_TOUCH_I2C_SCL      GPIO_NUM_8
#define BOARD_TOUCH_RST_GPIO     GPIO_NUM_22
#define BOARD_TOUCH_INT_GPIO     GPIO_NUM_21
#define BOARD_TOUCH_I2C_FREQ_HZ  400000

// Touch panel is mounted flush with the LCD in landscape orientation. If
// touch input is mirrored/rotated on your unit, adjust these three flags --
// they are passed straight to esp_lcd_touch_config_t.flags.
#define BOARD_TOUCH_SWAP_XY      false
#define BOARD_TOUCH_MIRROR_X     false
#define BOARD_TOUCH_MIRROR_Y     false

#ifdef __cplusplus
}
#endif
