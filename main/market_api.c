#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "market_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "wifi_manager.h"

static const char *TAG = "market_api";

#define HTTP_RESP_MAX 4096
#define HTTP_TIMEOUT_MS 10000
// Alpha Vantage free tier: 5 req/min. Space consecutive stock quote requests
// out so a handful of symbols don't trip the rate limit.
#define STOCK_REQUEST_GAP_MS 1300

// CoinGecko /search responses carry coins + exchanges + NFTs + categories and
// easily exceed 10KB -- buffer them in PSRAM, allocated only for the lookup.
#define LOOKUP_RESP_MAX (48 * 1024)

#define MARKET_NVS_NAMESPACE "market_cfg"
#define MARKET_NVS_KEY_ITEMS "items"

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_wake_sem;   // given to cut the refresh sleep short
static market_item_t s_items[MARKET_MAX_ITEMS];
static size_t s_item_count;
static market_update_cb_t s_cb;
static void *s_cb_ctx;

static market_lookup_cb_t s_lookup_cb;
static void *s_lookup_ctx;
static char s_lookup_query[16];
static volatile bool s_lookup_busy;

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

/* ---------- tracked-item list + NVS persistence ---------- */

// Serialized as "c|bitcoin|Bitcoin;s|AAPL|AAPL" (kind|key|symbol).
static void save_items_to_nvs_locked(void)
{
    char buf[MARKET_MAX_ITEMS * (sizeof(s_items[0].key) + sizeof(s_items[0].symbol) + 4)];
    size_t n = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < s_item_count; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%c|%s|%s", i ? ";" : "",
                      s_items[i].is_crypto ? 'c' : 's', s_items[i].key, s_items[i].symbol);
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MARKET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open for save failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_str(nvs, MARKET_NVS_KEY_ITEMS, buf);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "saved %u tracked symbols to NVS", (unsigned)s_item_count);
}

static void append_item(const char *key, const char *symbol, bool is_crypto)
{
    if (s_item_count >= MARKET_MAX_ITEMS) {
        ESP_LOGW(TAG, "dropping '%s': MARKET_MAX_ITEMS (%d) reached", key, MARKET_MAX_ITEMS);
        return;
    }
    market_item_t *item = &s_items[s_item_count++];
    memset(item, 0, sizeof(*item));
    strlcpy(item->key, key, sizeof(item->key));
    strlcpy(item->symbol, symbol, sizeof(item->symbol));
    item->is_crypto = is_crypto;
}

// Returns true if NVS held a list (even an empty one: the user deliberately
// removed everything, don't re-seed the defaults over it).
static bool load_items_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MARKET_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    char buf[MARKET_MAX_ITEMS * (sizeof(s_items[0].key) + sizeof(s_items[0].symbol) + 4)];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(nvs, MARKET_NVS_KEY_ITEMS, buf, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return false;
    }

    char *saveptr = NULL;
    for (char *entry = strtok_r(buf, ";", &saveptr); entry; entry = strtok_r(NULL, ";", &saveptr)) {
        char *sp2 = NULL;
        char *kind = strtok_r(entry, "|", &sp2);
        char *key = strtok_r(NULL, "|", &sp2);
        char *symbol = strtok_r(NULL, "|", &sp2);
        if (kind && key && symbol) {
            append_item(key, symbol, kind[0] == 'c');
        }
    }
    ESP_LOGI(TAG, "loaded %u tracked symbols from NVS", (unsigned)s_item_count);
    return true;
}

static void seed_default_items(const char *csv, bool is_crypto)
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
        if (!*tok) {
            continue;
        }
        char symbol[16];
        strlcpy(symbol, tok, sizeof(symbol));
        if (is_crypto && symbol[0]) {
            symbol[0] = (char)toupper((unsigned char)symbol[0]);
        }
        append_item(tok, symbol, is_crypto);
    }
}

/* ---------- price fetching ---------- */

// Fetches run on a snapshot (the list can change from the UI meanwhile);
// results are written back by key so a removed item is simply dropped.
static void store_result(const char *key, bool is_crypto, double price, double change_pct, bool has_change)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_item_count; i++) {
        if (s_items[i].is_crypto == is_crypto && strcmp(s_items[i].key, key) == 0) {
            s_items[i].price = price;
            s_items[i].valid = true;
            if (has_change) {
                s_items[i].change_pct = change_pct;
            }
            s_items[i].last_update_us = esp_timer_get_time();
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void fetch_crypto(const market_item_t *items, size_t count)
{
    char ids[256] = {0};
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (!items[i].is_crypto) {
            continue;
        }
        n += snprintf(ids + n, sizeof(ids) - n, "%s%s", n ? "," : "", items[i].key);
    }
    if (n == 0) {
        return;
    }

    char url[384];
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

    for (size_t i = 0; i < count; i++) {
        if (!items[i].is_crypto) {
            continue;
        }
        cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, items[i].key);
        if (!entry) {
            ESP_LOGW(TAG, "coingecko: no entry for '%s' in response", items[i].key);
            continue;
        }
        cJSON *price = cJSON_GetObjectItemCaseSensitive(entry, "usd");
        cJSON *chg = cJSON_GetObjectItemCaseSensitive(entry, "usd_24h_change");
        if (cJSON_IsNumber(price)) {
            ESP_LOGI(TAG, "coingecko: %s = $%.2f (%.2f%%)", items[i].key, price->valuedouble,
                     cJSON_IsNumber(chg) ? chg->valuedouble : 0.0);
            store_result(items[i].key, true, price->valuedouble,
                         cJSON_IsNumber(chg) ? chg->valuedouble : 0.0, cJSON_IsNumber(chg));
        } else {
            ESP_LOGW(TAG, "coingecko: '%s' entry has no numeric 'usd' field", items[i].key);
        }
    }
    cJSON_Delete(root);
}

static void fetch_stock(const market_item_t *item)
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

    if (cJSON_IsString(price)) {
        ESP_LOGI(TAG, "alpha vantage: %s = $%s", item->key, price->valuestring);
        store_result(item->key, false, atof(price->valuestring),
                     cJSON_IsString(chg_pct) ? atof(chg_pct->valuestring) : 0.0, // "1.23%" -> atof stops at '%'
                     cJSON_IsString(chg_pct));
    }
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

        market_item_t snap[MARKET_MAX_ITEMS];
        size_t n = market_get_items(snap, MARKET_MAX_ITEMS);

        fetch_crypto(snap, n);
        for (size_t i = 0; i < n; i++) {
            if (!snap[i].is_crypto) {
                fetch_stock(&snap[i]);
                vTaskDelay(pdMS_TO_TICKS(STOCK_REQUEST_GAP_MS));
            }
        }

        if (s_cb) {
            s_cb(s_cb_ctx);
        }

        // Sleep until the next refresh -- or until market_add_item() wakes
        // us so a freshly added symbol doesn't sit at "--" for a minute.
        xSemaphoreTake(s_wake_sem, pdMS_TO_TICKS(CONFIG_MARKET_REFRESH_INTERVAL_SEC * 1000));
    }
}

/* ---------- ticker classification (crypto vs stock) ---------- */

static void lookup_task(void *arg)
{
    market_lookup_result_t res = {0};
    strlcpy(res.query, s_lookup_query, sizeof(res.query));
    for (char *p = res.query; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }

    char q_lower[16];
    strlcpy(q_lower, s_lookup_query, sizeof(q_lower));
    for (char *p = q_lower; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    char *resp = heap_caps_malloc(LOOKUP_RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!resp) {
        resp = malloc(LOOKUP_RESP_MAX);
    }

    if (resp) {
        char url[128];
        snprintf(url, sizeof(url), "https://api.coingecko.com/api/v3/search?query=%s", q_lower);

        size_t len = 0;
        if (http_get_json(url, resp, LOOKUP_RESP_MAX, &len) == ESP_OK) {
            cJSON *root = cJSON_ParseWithLength(resp, len);
            if (root) {
                res.search_ok = true;
                // Best exact symbol match wins (search results also contain
                // fuzzy matches); rank 0/absent counts as worst.
                int best_rank = INT32_MAX;
                cJSON *coins = cJSON_GetObjectItemCaseSensitive(root, "coins");
                cJSON *coin = NULL;
                cJSON_ArrayForEach(coin, coins) {
                    cJSON *sym = cJSON_GetObjectItemCaseSensitive(coin, "symbol");
                    cJSON *id = cJSON_GetObjectItemCaseSensitive(coin, "id");
                    cJSON *name = cJSON_GetObjectItemCaseSensitive(coin, "name");
                    if (!cJSON_IsString(sym) || !cJSON_IsString(id) || strcasecmp(sym->valuestring, res.query) != 0) {
                        continue;
                    }
                    cJSON *rank = cJSON_GetObjectItemCaseSensitive(coin, "market_cap_rank");
                    int r = cJSON_IsNumber(rank) && rank->valueint > 0 ? rank->valueint : INT32_MAX - 1;
                    if (r < best_rank) {
                        best_rank = r;
                        res.crypto_found = true;
                        strlcpy(res.crypto_id, id->valuestring, sizeof(res.crypto_id));
                        strlcpy(res.crypto_symbol, sym->valuestring, sizeof(res.crypto_symbol));
                        if (cJSON_IsString(name)) {
                            strlcpy(res.crypto_name, name->valuestring, sizeof(res.crypto_name));
                        }
                    }
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "search: bad JSON (%d bytes, buffer full=%d)", (int)len, len >= LOOKUP_RESP_MAX - 1);
            }
        }
        free(resp);
    }

    // Normalize the display symbol to uppercase ("btc" -> "BTC").
    for (char *p = res.crypto_symbol; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }

    ESP_LOGI(TAG, "lookup '%s': search_ok=%d crypto=%d id='%s'", res.query, res.search_ok,
             res.crypto_found, res.crypto_id);

    if (s_lookup_cb) {
        s_lookup_cb(&res, s_lookup_ctx);
    }
    s_lookup_busy = false;
    vTaskDelete(NULL);
}

esp_err_t market_lookup(const char *query, market_lookup_cb_t cb, void *user_ctx)
{
    if (!query || !query[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.state != WIFI_MGR_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lookup_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lookup_busy = true;
    s_lookup_cb = cb;
    s_lookup_ctx = user_ctx;
    strlcpy(s_lookup_query, query, sizeof(s_lookup_query));

    BaseType_t ok = xTaskCreate(lookup_task, "market_lookup", 8192, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        s_lookup_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------- public list management ---------- */

esp_err_t market_add_item(const char *key, const char *symbol, bool is_crypto)
{
    if (!key || !key[0] || !symbol || !symbol[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_item_count >= MARKET_MAX_ITEMS) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_item_count; i++) {
        if (s_items[i].is_crypto == is_crypto && strcasecmp(s_items[i].key, key) == 0) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    append_item(key, symbol, is_crypto);
    save_items_to_nvs_locked();
    xSemaphoreGive(s_mutex);

    xSemaphoreGive(s_wake_sem);  // fetch the new symbol right away
    return ESP_OK;
}

esp_err_t market_remove_item(size_t index)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index >= s_item_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = index; i + 1 < s_item_count; i++) {
        s_items[i] = s_items[i + 1];
    }
    s_item_count--;
    save_items_to_nvs_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

/* ---------- startup ---------- */

esp_err_t market_task_start(market_update_cb_t cb, void *user_ctx)
{
    s_mutex = xSemaphoreCreateMutex();
    s_wake_sem = xSemaphoreCreateBinary();
    if (!s_mutex || !s_wake_sem) {
        return ESP_ERR_NO_MEM;
    }
    s_cb = cb;
    s_cb_ctx = user_ctx;

    // market_task_start runs before wifi_manager_start (which also inits
    // NVS); nvs_flash_init is idempotent so both calling it is fine.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // NVS wins once the user edited the list on-screen; the Kconfig CSVs are
    // only the factory defaults.
    if (!load_items_from_nvs()) {
        seed_default_items(CONFIG_MARKET_CRYPTO_IDS, true);
        seed_default_items(CONFIG_MARKET_STOCK_SYMBOLS, false);
        ESP_LOGI(TAG, "seeded %u default symbols from Kconfig", (unsigned)s_item_count);
    }

    if (s_item_count == 0) {
        ESP_LOGW(TAG, "no symbols tracked -- add some from the on-screen ticker panel");
    }
    if (strlen(CONFIG_MARKET_ALPHA_VANTAGE_API_KEY) == 0) {
        ESP_LOGW(TAG, "no Alpha Vantage API key -- stock quotes will stay empty");
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
