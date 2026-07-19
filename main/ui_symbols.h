#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the ticker management panel (overlay on lv_layer_top).
 *
 * Lists the tracked symbols with per-row remove, and lets the user add a
 * new ticker: the typed symbol is classified as crypto vs stock via
 * market_lookup() (CoinGecko /search) before being added, so it gets fetched
 * from the right API. Must be called with the LVGL lock held (e.g. from an
 * LVGL event callback).
 */
void ui_symbols_open(void);

#ifdef __cplusplus
}
#endif
