#include "esp_log.h"
#include "bsp_display.h"
#include "wifi_manager.h"
#include "market_api.h"
#include "ui.h"

static const char *TAG = "app_main";

static void on_wifi_status(const wifi_mgr_status_t *status, void *ctx)
{
    ui_update_wifi_status(status);
}

static void on_market_update(void *ctx)
{
    ui_refresh_market();
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_start());

    // Populate the tracked-symbol list (from Kconfig) and start the
    // background price-refresh task before building the UI, so ui_init()
    // knows how many cards to lay out.
    ESP_ERROR_CHECK(market_task_start(on_market_update, NULL));

    if (bsp_display_lock(0)) {
        ui_init();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "could not take LVGL lock to build UI");
    }

    ESP_ERROR_CHECK(wifi_manager_start(on_wifi_status, NULL));
}
