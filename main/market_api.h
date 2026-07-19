#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MARKET_MAX_ITEMS 8

typedef struct {
    char key[32];         // CoinGecko id ("bitcoin") or stock ticker ("AAPL") -- used for API calls
    char symbol[16];      // display label
    bool is_crypto;
    bool valid;           // true once at least one successful fetch happened
    double price;
    double change_pct;    // % change (24h for crypto, daily for stocks)
    int64_t last_update_us;
} market_item_t;

typedef void (*market_update_cb_t)(void *user_ctx);

/** Result of classifying a user-typed ticker (see market_lookup). */
typedef struct {
    bool search_ok;         // the CoinGecko search request completed
    bool crypto_found;      // an exact crypto symbol match exists
    char query[16];         // normalized (uppercased) user input
    char crypto_id[32];     // CoinGecko id to use for price queries
    char crypto_name[32];   // human name ("Bitcoin")
    char crypto_symbol[16]; // ticker ("BTC")
} market_lookup_result_t;

typedef void (*market_lookup_cb_t)(const market_lookup_result_t *result, void *user_ctx);

/**
 * @brief Load the tracked-symbol list (NVS if the user edited it on-screen,
 * otherwise the CONFIG_MARKET_* defaults) and start the background task that
 * periodically refreshes prices via CoinGecko (crypto) and Alpha Vantage
 * (stocks).
 *
 * @param cb        Called after each refresh pass (may be NULL)
 * @param user_ctx  Opaque pointer passed back to cb
 */
esp_err_t market_task_start(market_update_cb_t cb, void *user_ctx);

/**
 * @brief Thread-safe snapshot of all tracked items.
 *
 * @return number of items written to out[]
 */
size_t market_get_items(market_item_t *out, size_t max_items);

/**
 * @brief Classify a user-typed ticker as crypto or stock, asynchronously.
 *
 * Queries CoinGecko /search (no API key needed) from a short-lived task; a
 * coin whose symbol matches the query exactly (case-insensitive, best market
 * cap rank wins) classifies it as crypto and resolves the CoinGecko id the
 * price API needs. No match (or tickers that only exist on Alpha Vantage)
 * means stock. cb fires from that task -- take the LVGL lock before
 * touching widgets.
 *
 * @return ESP_ERR_INVALID_STATE if WiFi is down or a lookup is already
 *         running, ESP_OK if the lookup was started.
 */
esp_err_t market_lookup(const char *query, market_lookup_cb_t cb, void *user_ctx);

/**
 * @brief Add a symbol to the tracked list and persist the list to NVS.
 * Wakes the refresh task so the price shows up quickly.
 *
 * @param key       CoinGecko id for crypto, ticker for stocks
 * @param symbol    display label ("BTC", "AAPL")
 * @return ESP_ERR_NO_MEM if the list is full, ESP_ERR_INVALID_STATE if the
 *         key is already tracked.
 */
esp_err_t market_add_item(const char *key, const char *symbol, bool is_crypto);

/**
 * @brief Remove the item at the given snapshot index and persist to NVS.
 */
esp_err_t market_remove_item(size_t index);

#ifdef __cplusplus
}
#endif
