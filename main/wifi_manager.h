#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_STATE_DISCONNECTED = 0,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
    WIFI_MGR_STATE_CONNECT_FAILED,  /**< user-supplied credentials rejected (bad password / AP gone) */
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    char ssid[33];
    char ip[16];
    char gateway[16];
    char netmask[16];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
} wifi_mgr_status_t;

/** One access point found by a scan. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;  /**< authmode != open -> needs a password */
} wifi_mgr_ap_t;

#define WIFI_MGR_SCAN_MAX 20

typedef void (*wifi_mgr_status_cb_t)(const wifi_mgr_status_t *status, void *user_ctx);
typedef void (*wifi_mgr_scan_cb_t)(void *user_ctx);

/**
 * @brief Bring up WiFi station mode and keep it connected in the background.
 *
 * Credentials come from NVS (namespace "wifi_cfg", written by
 * wifi_manager_connect) with CONFIG_ESP_WIFI_SSID/PASSWORD as first-boot
 * fallback. With no credentials at all the radio still starts (so scanning
 * works) but no connection is attempted.
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

/**
 * @brief Start an async AP scan. cb fires (from the esp_event task) when
 *        results are ready; fetch them with wifi_manager_get_scan_results().
 *
 * Pauses the background reconnect timer while the scan runs so the retry
 * doesn't abort it; the timer is re-armed on scan completion if applicable.
 */
esp_err_t wifi_manager_scan_start(wifi_mgr_scan_cb_t cb, void *user_ctx);

/**
 * @brief Copy the latest scan results (deduplicated by SSID, strongest RSSI
 *        first). Returns the number of entries written.
 */
size_t wifi_manager_get_scan_results(wifi_mgr_ap_t *out, size_t max);

/**
 * @brief Connect to a network with user-supplied credentials.
 *
 * On success (got IP) the credentials are persisted to NVS and become the
 * boot default. On repeated failure the status callback reports
 * WIFI_MGR_STATE_CONNECT_FAILED and previously saved credentials are kept.
 *
 * @param password  May be NULL or "" for open networks.
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief User-initiated disconnect. Suppresses auto-reconnect until the next
 *        wifi_manager_connect() or reboot. Saved credentials are kept.
 */
void wifi_manager_disconnect(void);

#ifdef __cplusplus
}
#endif
