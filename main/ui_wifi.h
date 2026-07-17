#pragma once

#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the WiFi panel (overlay on lv_layer_top).
 *
 * Shows the connection details view when connected, otherwise the network
 * scan/connect view. Must be called with the LVGL lock held (e.g. from an
 * LVGL event callback).
 */
void ui_wifi_open(void);

/**
 * @brief Feed WiFi state changes into the panel (connect succeeded/failed,
 *        connection dropped). No-op while the panel is closed.
 *
 * Caller must hold the LVGL lock (ui_update_wifi_status already does).
 */
void ui_wifi_notify_status(const wifi_mgr_status_t *status);

#ifdef __cplusplus
}
#endif
