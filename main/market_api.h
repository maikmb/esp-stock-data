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
    char key[24];        // CoinGecko id ("bitcoin") or stock ticker ("AAPL") -- used for API calls
    char symbol[16];      // display label
    bool is_crypto;
    bool valid;           // true once at least one successful fetch happened
    double price;
    double change_pct;    // % change (24h for crypto, daily for stocks)
    int64_t last_update_us;
} market_item_t;

typedef void (*market_update_cb_t)(void *user_ctx);

/**
 * @brief Parse CONFIG_MARKET_CRYPTO_IDS / CONFIG_MARKET_STOCK_SYMBOLS and
 * start the background task that periodically refreshes prices via
 * CoinGecko (crypto) and Alpha Vantage (stocks).
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

#ifdef __cplusplus
}
#endif
