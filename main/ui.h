#pragma once

#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the dashboard screen (WiFi status bar + market cards).
 *
 * Call after bsp_display_start() and market_task_start() (the latter so the
 * configured symbol list is already known and one card per symbol can be
 * created up front). Must be called with the LVGL lock held by the caller,
 * or right after bsp_display_start() before other tasks touch LVGL.
 */
void ui_init(void);

/** Update the WiFi status pill in the top bar. Takes the LVGL lock itself. */
void ui_update_wifi_status(const wifi_mgr_status_t *status);

/** Re-read market_get_items() and refresh all cards. Takes the LVGL lock itself. */
void ui_refresh_market(void);

#ifdef __cplusplus
}
#endif
