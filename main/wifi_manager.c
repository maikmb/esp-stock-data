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

#define WIFI_RECONNECT_DELAY_MS   3000
#define WIFI_RECONNECT_BACKOFF_MS 15000

static const char *TAG = "wifi_manager";

static SemaphoreHandle_t s_status_mutex;
static wifi_mgr_status_t s_status;
static wifi_mgr_status_cb_t s_status_cb;
static void *s_status_cb_ctx;
static esp_timer_handle_t s_reconnect_timer;
static int s_fast_retry_count;

static void notify_status_locked(void)
{
    if (s_status_cb) {
        s_status_cb(&s_status, s_status_cb_ctx);
    }
}

static void set_state(wifi_mgr_state_t state)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = state;
    if (state != WIFI_MGR_STATE_CONNECTED) {
        s_status.ip[0] = '\0';
        s_status.rssi = 0;
    }
    notify_status_locked();
    xSemaphoreGive(s_status_mutex);
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

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        set_state(WIFI_MGR_STATE_CONNECTING);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_fast_retry_count++;
        ESP_LOGI(TAG, "disconnected, retry #%d", s_fast_retry_count);
        set_state(WIFI_MGR_STATE_CONNECTING);
        schedule_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_fast_retry_count = 0;

        wifi_ap_record_t ap_info = {0};
        esp_wifi_sta_get_ap_info(&ap_info);

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.state = WIFI_MGR_STATE_CONNECTED;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
        strlcpy(s_status.ssid, (const char *)ap_info.ssid, sizeof(s_status.ssid));
        s_status.rssi = ap_info.rssi;
        notify_status_locked();
        xSemaphoreGive(s_status_mutex);

        ESP_LOGI(TAG, "got ip: %s rssi: %d", s_status.ip, s_status.rssi);
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
    strlcpy(s_status.ssid, CONFIG_ESP_WIFI_SSID, sizeof(s_status.ssid));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_manager started, connecting to SSID:%s", CONFIG_ESP_WIFI_SSID);
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
