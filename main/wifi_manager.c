#include <string.h>
#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#define WIFI_RECONNECT_DELAY_MS   3000
#define WIFI_RECONNECT_BACKOFF_MS 15000
#define WIFI_NEW_CREDS_MAX_TRIES  3

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID  "ssid"
#define WIFI_NVS_KEY_PASS  "pass"

static const char *TAG = "wifi_manager";

static SemaphoreHandle_t s_status_mutex;
static wifi_mgr_status_t s_status;
static wifi_mgr_status_cb_t s_status_cb;
static void *s_status_cb_ctx;
static esp_timer_handle_t s_reconnect_timer;
static int s_fast_retry_count;

// Credentials currently in use / being tried. When a user-initiated connect
// succeeds (GOT_IP) they are persisted to NVS.
static bool s_have_creds;
static bool s_trying_new_creds;
static int s_new_creds_fail_count;
static char s_pending_ssid[33];
static char s_pending_pass[65];

// Set by wifi_manager_disconnect(); suppresses auto-reconnect until the next
// wifi_manager_connect() or reboot.
static bool s_user_disconnected;

static wifi_mgr_ap_t s_scan_results[WIFI_MGR_SCAN_MAX];
static size_t s_scan_count;
static bool s_scan_in_progress;
static wifi_mgr_scan_cb_t s_scan_cb;
static void *s_scan_cb_ctx;

// The status callback must run OUTSIDE s_status_mutex: UI handlers react by
// calling back into wifi_manager getters (wifi_manager_get_status /
// _get_scan_results), which take this same non-recursive mutex. Notifying
// while holding it self-deadlocked the LVGL task -- frozen screen when
// tapping "Desconectar".
static void notify_status(const wifi_mgr_status_t *snapshot)
{
    if (s_status_cb) {
        s_status_cb(snapshot, s_status_cb_ctx);
    }
}

static void set_state(wifi_mgr_state_t state)
{
    wifi_mgr_status_t snapshot;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = state;
    if (state != WIFI_MGR_STATE_CONNECTED) {
        s_status.ip[0] = '\0';
        s_status.gateway[0] = '\0';
        s_status.netmask[0] = '\0';
        memset(s_status.bssid, 0, sizeof(s_status.bssid));
        s_status.channel = 0;
        s_status.rssi = 0;
    }
    snapshot = s_status;
    xSemaphoreGive(s_status_mutex);

    notify_status(&snapshot);
}

static bool nvs_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = ssid_len;
    esp_err_t err = nvs_get_str(nvs, WIFI_NVS_KEY_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        if (nvs_get_str(nvs, WIFI_NVS_KEY_PASS, pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(nvs);
    return err == ESP_OK && ssid[0] != '\0';
}

static void nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open for save failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, WIFI_NVS_KEY_PASS, pass ? pass : "");
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "credentials for '%s' saved to NVS", ssid);
}

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass ? pass : "", sizeof(wifi_config.sta.password));
    // Threshold OPEN: accept whatever authmode the AP offers (the password
    // still has to match for protected networks). Networks are user-picked
    // from a scan, so don't reject e.g. WPA1-only APs.
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
    }
}

static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = reconnect_timer_cb,
            .name = "wifi_reconnect",
        };
        esp_timer_create(&args, &s_reconnect_timer);
    }
    // Retry quickly for the first CONFIG_ESP_MAXIMUM_RETRY attempts, then back
    // off to avoid hammering an AP that's genuinely down/out of range. Unlike
    // the plain wifi_station example, we never give up permanently -- this
    // device is meant to sit on a shelf showing live prices.
    uint64_t delay_us = (s_fast_retry_count <= CONFIG_ESP_MAXIMUM_RETRY)
                             ? (WIFI_RECONNECT_DELAY_MS * 1000ULL)
                             : (WIFI_RECONNECT_BACKOFF_MS * 1000ULL);
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, delay_us);
}

static void handle_scan_done(void)
{
    s_scan_in_progress = false;

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);

    wifi_ap_record_t *records = NULL;
    if (num > 0) {
        records = calloc(num, sizeof(wifi_ap_record_t));
    }
    if (records) {
        if (esp_wifi_scan_get_ap_records(&num, records) != ESP_OK) {
            num = 0;
        }
    } else {
        num = 0;
        // Still need to drain the driver's scan buffer even on alloc failure.
        uint16_t zero = 0;
        esp_wifi_scan_get_ap_records(&zero, NULL);
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_scan_count = 0;
    for (uint16_t i = 0; i < num; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;  // hidden networks: nothing to show/tap
        }
        // Dedupe by SSID (mesh / multi-AP networks), keep the strongest RSSI.
        wifi_mgr_ap_t *slot = NULL;
        for (size_t j = 0; j < s_scan_count; j++) {
            if (strcmp(s_scan_results[j].ssid, ssid) == 0) {
                slot = &s_scan_results[j];
                break;
            }
        }
        if (slot) {
            if (records[i].rssi > slot->rssi) {
                slot->rssi = records[i].rssi;
                slot->secure = records[i].authmode != WIFI_AUTH_OPEN;
            }
            continue;
        }
        if (s_scan_count >= WIFI_MGR_SCAN_MAX) {
            continue;
        }
        slot = &s_scan_results[s_scan_count++];
        strlcpy(slot->ssid, ssid, sizeof(slot->ssid));
        slot->rssi = records[i].rssi;
        slot->secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    // Sort strongest first (list is tiny, insertion sort is fine).
    for (size_t i = 1; i < s_scan_count; i++) {
        wifi_mgr_ap_t tmp = s_scan_results[i];
        size_t j = i;
        while (j > 0 && s_scan_results[j - 1].rssi < tmp.rssi) {
            s_scan_results[j] = s_scan_results[j - 1];
            j--;
        }
        s_scan_results[j] = tmp;
    }
    size_t count = s_scan_count;
    xSemaphoreGive(s_status_mutex);
    free(records);

    ESP_LOGI(TAG, "scan done: %u APs (%u unique)", num, (unsigned)count);

    if (s_scan_cb) {
        s_scan_cb(s_scan_cb_ctx);
    }

    // The reconnect timer was paused for the scan -- resume background
    // recovery if we still have a network to go back to.
    if (!s_user_disconnected && s_have_creds &&
        s_status.state != WIFI_MGR_STATE_CONNECTED && !s_trying_new_creds) {
        schedule_reconnect();
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_have_creds) {
            set_state(WIFI_MGR_STATE_CONNECTING);
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "no credentials configured, waiting for on-screen setup");
            set_state(WIFI_MGR_STATE_DISCONNECTED);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        handle_scan_done();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;

        if (s_user_disconnected) {
            set_state(WIFI_MGR_STATE_DISCONNECTED);
            return;
        }
        // Our own esp_wifi_disconnect() before switching networks in
        // wifi_manager_connect() also lands here -- don't count it as a
        // failed attempt of the new credentials.
        if (ev->reason == WIFI_REASON_ASSOC_LEAVE) {
            return;
        }

        if (s_trying_new_creds) {
            s_new_creds_fail_count++;
            ESP_LOGW(TAG, "connect with new credentials failed (reason %d, try %d/%d)",
                     ev->reason, s_new_creds_fail_count, WIFI_NEW_CREDS_MAX_TRIES);
            if (s_new_creds_fail_count >= WIFI_NEW_CREDS_MAX_TRIES) {
                s_trying_new_creds = false;
                set_state(WIFI_MGR_STATE_CONNECT_FAILED);
                return;  // stop retrying; saved NVS credentials stay untouched
            }
            set_state(WIFI_MGR_STATE_CONNECTING);
            esp_wifi_connect();
            return;
        }

        s_fast_retry_count++;
        ESP_LOGI(TAG, "disconnected (reason %d), retry #%d", ev->reason, s_fast_retry_count);
        set_state(WIFI_MGR_STATE_CONNECTING);
        schedule_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_fast_retry_count = 0;

        if (s_trying_new_creds) {
            // The user-picked network works: make it the boot default.
            nvs_save_creds(s_pending_ssid, s_pending_pass);
            s_trying_new_creds = false;
            s_have_creds = true;
        }

        wifi_ap_record_t ap_info = {0};
        esp_wifi_sta_get_ap_info(&ap_info);

        wifi_mgr_status_t snapshot;
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.state = WIFI_MGR_STATE_CONNECTED;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(s_status.gateway, sizeof(s_status.gateway), IPSTR, IP2STR(&event->ip_info.gw));
        snprintf(s_status.netmask, sizeof(s_status.netmask), IPSTR, IP2STR(&event->ip_info.netmask));
        strlcpy(s_status.ssid, (const char *)ap_info.ssid, sizeof(s_status.ssid));
        memcpy(s_status.bssid, ap_info.bssid, sizeof(s_status.bssid));
        s_status.channel = ap_info.primary;
        s_status.rssi = ap_info.rssi;
        snapshot = s_status;
        xSemaphoreGive(s_status_mutex);

        notify_status(&snapshot);

        ESP_LOGI(TAG, "got ip: %s rssi: %d", snapshot.ip, snapshot.rssi);
    }
}

esp_err_t wifi_manager_start(wifi_mgr_status_cb_t cb, void *user_ctx)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_status_cb = cb;
    s_status_cb_ctx = user_ctx;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Boot credentials come exclusively from NVS, written by
    // wifi_manager_connect() when a network picked on the WiFi panel
    // connects successfully. No Kconfig fallback: on a fresh device the
    // user configures WiFi entirely on-screen.
    char ssid[33] = {0};
    char pass[65] = {0};
    if (nvs_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "using saved network from NVS (SSID '%s')", ssid);
    }
    s_have_creds = ssid[0] != '\0';
    strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_have_creds) {
        apply_sta_config(ssid, pass);
    }
    // Start the radio even without credentials so scanning works and the
    // user can pick a network from the WiFi panel.
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_manager started%s%s", s_have_creds ? ", connecting to SSID:" : "",
             s_have_creds ? ssid : "");
    return ESP_OK;
}

void wifi_manager_get_status(wifi_mgr_status_t *out)
{
    // The market task starts before wifi_manager_start() (app_main brings the
    // symbol list up first so the UI knows how many cards to build) and polls
    // this immediately -- taking a NULL mutex trips a FreeRTOS assert and
    // reboots the board, so report "disconnected" until we're initialized.
    if (!s_status_mutex) {
        memset(out, 0, sizeof(*out));
        out->state = WIFI_MGR_STATE_DISCONNECTED;
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
}

esp_err_t wifi_manager_scan_start(wifi_mgr_scan_cb_t cb, void *user_ctx)
{
    if (!s_status_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_in_progress) {
        return ESP_OK;
    }
    s_scan_cb = cb;
    s_scan_cb_ctx = user_ctx;

    // A connect attempt fired by the reconnect timer would abort the scan;
    // handle_scan_done() re-arms the timer when the scan finishes.
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }

    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_OK) {
        s_scan_in_progress = true;
    } else {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
    }
    return err;
}

size_t wifi_manager_get_scan_results(wifi_mgr_ap_t *out, size_t max)
{
    if (!s_status_mutex) {
        return 0;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    size_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan_results, n * sizeof(wifi_mgr_ap_t));
    xSemaphoreGive(s_status_mutex);
    return n;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_status_mutex || !ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "user connect request: SSID '%s'", ssid);

    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    s_user_disconnected = false;
    s_fast_retry_count = 0;

    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_pass, password ? password : "", sizeof(s_pending_pass));
    s_new_creds_fail_count = 0;
    s_trying_new_creds = true;

    // Leaving the current AP raises a DISCONNECTED event with reason
    // ASSOC_LEAVE, which the handler ignores.
    esp_wifi_disconnect();
    apply_sta_config(s_pending_ssid, s_pending_pass);

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strlcpy(s_status.ssid, s_pending_ssid, sizeof(s_status.ssid));
    xSemaphoreGive(s_status_mutex);
    set_state(WIFI_MGR_STATE_CONNECTING);

    return esp_wifi_connect();
}

void wifi_manager_disconnect(void)
{
    if (!s_status_mutex) {
        return;
    }
    ESP_LOGI(TAG, "user disconnect request");
    s_user_disconnected = true;
    s_trying_new_creds = false;
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    esp_wifi_disconnect();
    set_state(WIFI_MGR_STATE_DISCONNECTED);
}
