#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the MIPI-DSI display, capacitive touch and LVGL port.
 *
 * After this returns ESP_OK, the LVGL display/touch are registered and the
 * LVGL port task is running. Use bsp_display_lock()/bsp_display_unlock()
 * around any LVGL widget calls made from outside that task.
 */
esp_err_t bsp_display_start(void);

bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
