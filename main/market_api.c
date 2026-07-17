#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "market_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include "wifi_manager.h"

static const char *TAG = "market_api";

#define HTTP_RESP_MAX 4096
#define HTTP_TIMEOUT_MS 10000
// Alpha Vantage free tier: 5 req/min. Space consecutive stock quote requests
// out so a handful of symbols don't trip the rate limit.
#define STOCK_REQUEST_GAP_MS 1300

static SemaphoreHandle_t s_mutex;
static market_item_t s_items[MARKET_MAX_ITEMS];
static size_t s_item_count;
static market_update_cb_t s_cb;
static void *s_cb_ctx;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
        size_t space = ctx->cap - ctx->len;
        size_t to_copy = (size_t)evt->data_len < space ? (size_t)evt->data_len : space;
        if (to_copy > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, to_copy);
            ctx->len += to_copy;
        }
    }
    return ESP_OK;
}

static esp_err_t http_get_json(const char *url, char *out_buf, size_t out_cap, size_t *out_len)
{
    http_ctx_t ctx = { .buf = out_buf, .len = 0, .cap = out_cap - 1 };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
        return ESP_FAIL;
    }
    out_buf[ctx.len] = '\0';
    *out_len = ctx.len;
    return ESP_OK;
}

static void add_item(const char *key, bool is_crypto)
{
    if (s_item_count >= MARKET_MAX_ITEMS) {
        ESP_LOGW(TAG, "dropping '%s': MARKET_MAX_ITEMS (%d) reached", key, MARKET_MAX_ITEMS);
        return;
    }
    market_item_t *item = &s_items[s_item_count++];
    memset(item, 0, sizeof(*item));
    strlcpy(item->key, key, sizeof(item->key));
    item->is_crypto = is_crypto;

    strlcpy(item->symbol, key, sizeof(item->symbol));
    if (is_crypto && item->symbol[0]) {
        item->symbol[0] = (char)toupper((unsigned char)item->symbol[0]);
    }
}

static void parse_csv_into_items(const char *csv, bool is_crypto)
{
    if (!csv || !csv[0]) {
        return;
    }
    char buf[192];
    strlcpy(buf, csv, sizeof(buf));

    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
        while (*tok == ' ') {
            tok++;
        }
        if (*tok) {
            add_item(tok, is_crypto);
        }
    }
}

static void fetch_crypto(void)
{
    char ids[192] = {0};
    size_t n = 0;
    for (size_t i = 0; i < s_item_count; i++) {
        if (!s_items[i].is_crypto) {
            continue;
        }
        n += snprintf(ids + n, sizeof(ids) - n, "%s%s", n ? "," : "", s_items[i].key);
    }
    if (n == 0) {
        return;
    }

    char url[320];
    snprintf(url, sizeof(url),
             "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=usd&include_24hr_change=true",
             ids);

    static char resp[HTTP_RESP_MAX];
    size_t len = 0;
    if (http_get_json(url, resp, sizeof(resp), &len) != ESP_OK) {
        ESP_LOGW(TAG, "coingecko: request failed for ids=%s", ids);
        return;
    }
    ESP_LOGI(TAG, "coingecko: got %d bytes for ids=%s", (int)len, ids);

    cJSON *root = cJSON_ParseWithLength(resp, len);
    if (!root) {
        ESP_LOGW(TAG, "coingecko: bad JSON: %.200s", resp);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_item_count; i++) {
        if (!s_items[i].is_crypto) {
            continue;
        }
        cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, s_items[i].key);
        if (!entry) {
            ESP_LOGW(TAG, "coingecko: no entry for '%s' in response (check MARKET_CRYPTO_IDS)", s_items[i].key);
            continue;
        }
        cJSON *price = cJSON_GetObjectItemCaseSensitive(entry, "usd");
        cJSON *chg = cJSON_GetObjectItemCaseSensitive(entry, "usd_24h_change");
        if (cJSON_IsNumber(price)) {
            s_items[i].price = price->valuedouble;
            s_items[i].valid = true;
            ESP_LOGI(TAG, "coingecko: %s = $%.2f (%.2f%%)", s_items[i].key, price->valuedouble,
                     cJSON_IsNumber(chg) ? chg->valuedouble : 0.0);
        } else {
            ESP_LOGW(TAG, "coingecko: '%s' entry has no numeric 'usd' field", s_items[i].key);
        }
        if (cJSON_IsNumber(chg)) {
            s_items[i].change_pct = chg->valuedouble;
        }
        s_items[i].last_update_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_mutex);
    cJSON_Delete(root);
}

static void fetch_stock(market_item_t *item)
{
    if (strlen(CONFIG_MARKET_ALPHA_VANTAGE_API_KEY) == 0) {
        return;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://www.alphavantage.co/query?function=GLOBAL_QUOTE&symbol=%s&apikey=%s",
             item->key, CONFIG_MARKET_ALPHA_VANTAGE_API_KEY);

    static char resp[HTTP_RESP_MAX];
    size_t len = 0;
    if (http_get_json(url, resp, sizeof(resp), &len) != ESP_OK) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(resp, len);
    if (!root) {
        ESP_LOGW(TAG, "alpha vantage: bad JSON for %s", item->key);
        return;
    }

    cJSON *quote = cJSON_GetObjectItemCaseSensitive(root, "Global Quote");
    if (!quote) {
        ESP_LOGW(TAG, "alpha vantage: no quote for %s (bad symbol or rate limited)", item->key);
        cJSON_Delete(root);
        return;
    }

    cJSON *price = cJSON_GetObjectItemCaseSensitive(quote, "05. price");
    cJSON *chg_pct = cJSON_GetObjectItemCaseSensitive(quote, "10. change percent");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (cJSON_IsString(price)) {
        item->price = atof(price->valuestring);
        item->valid = true;
    }
    if (cJSON_IsString(chg_pct)) {
        item->change_pct = atof(chg_pct->valuestring); // "1.23%" -> atof stops at '%'
    }
    item->last_update_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);

    cJSON_Delete(root);
}

static void market_task(void *arg)
{
    while (1) {
        wifi_mgr_status_t wifi;
        wifi_manager_get_status(&wifi);
        if (wifi.state != WIFI_MGR_STATE_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        fetch_crypto();
        for (size_t i = 0; i < s_item_count; i++) {
            if (!s_items[i].is_crypto) {
                fetch_stock(&s_items[i]);
                vTaskDelay(pdMS_TO_TICKS(STOCK_REQUEST_GAP_MS));
            }
        }

        if (s_cb) {
            s_cb(s_cb_ctx);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_MARKET_REFRESH_INTERVAL_SEC * 1000));
    }
}

esp_err_t market_task_start(market_update_cb_t cb, void *user_ctx)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_cb = cb;
    s_cb_ctx = user_ctx;

    parse_csv_into_items(CONFIG_MARKET_CRYPTO_IDS, true);
    parse_csv_into_items(CONFIG_MARKET_STOCK_SYMBOLS, false);

    if (s_item_count == 0) {
        ESP_LOGW(TAG, "no symbols configured (MARKET_CRYPTO_IDS / MARKET_STOCK_SYMBOLS)");
    }
    if (strlen(CONFIG_MARKET_ALPHA_VANTAGE_API_KEY) == 0 && strlen(CONFIG_MARKET_STOCK_SYMBOLS) > 0) {
        ESP_LOGW(TAG, "MARKET_STOCK_SYMBOLS set but no Alpha Vantage API key -- stock quotes will stay empty");
    }

    BaseType_t ok = xTaskCreate(market_task, "market_task", 8192, NULL, tskIDLE_PRIORITY + 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t market_get_items(market_item_t *out, size_t max_items)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = s_item_count < max_items ? s_item_count : max_items;
    memcpy(out, s_items, n * sizeof(market_item_t));
    xSemaphoreGive(s_mutex);
    return n;
}
