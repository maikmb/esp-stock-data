#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_STATE_DISCONNECTED = 0,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    char ssid[33];
    char ip[16];
    int8_t rssi;
} wifi_mgr_status_t;

typedef void (*wifi_mgr_status_cb_t)(const wifi_mgr_status_t *status, void *user_ctx);

/**
 * @brief Bring up WiFi station mode and keep it connected in the background.
 *
 * Non-blocking: kicks off the connection attempt and returns immediately.
 * On ESP32-P4 this runs over the onboard ESP32-C6 via esp_wifi_remote/esp-hosted,
 * using the same esp_wifi.h API as a chip with native WiFi.
 *
 * @param cb        Optional callback invoked on every state change (may be NULL)
 * @param user_ctx  Opaque pointer passed back to cb
 */
esp_err_t wifi_manager_start(wifi_mgr_status_cb_t cb, void *user_ctx);

/**
 * @brief Get a thread-safe snapshot of the current WiFi status.
 */
void wifi_manager_get_status(wifi_mgr_status_t *out);

#ifdef __cplusplus
}
#endif
