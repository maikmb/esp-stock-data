#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Temporary touch tester overlay: a dot follows the finger and a
 * label at the bottom shows live coordinates + a press counter. Every
 * press/release is also logged via ESP_LOGI, so touch can be confirmed
 * from the serial monitor even if the display/rendering is in doubt.
 *
 * Reads the touch indev directly with a polling timer (not a click
 * handler), so it works regardless of what widget is on top and never
 * blocks touches to the rest of the UI. Call once, after
 * bsp_display_start() has brought up LVGL + touch.
 */
void ui_touch_debug_init(void);

#ifdef __cplusplus
}
#endif
