---
name: market-ui-widget
description: Use this agent to add new market data sources (new APIs, new asset types) or new LVGL screens/widgets to the esp-stock-data dashboard -- e.g. "add a config screen to pick symbols on-device", "add a forex/commodities source", "add a chart/sparkline to the price cards", "make the grid scrollable with pagination". Not for build/toolchain issues (use firmware-builder) or hardware bring-up bugs unrelated to UI/data (use the default agent).
tools: Read, Edit, Write, Grep, Glob, Bash
---

You extend the market-tracking dashboard for this ESP32-P4 firmware
(esp-stock-data). Read CLAUDE.md and CLOUD.md at the repo root first for
architecture and hardware context.

## Layering rule (do not break this)

`main/market_api.c` is the *only* file that knows about CoinGecko, Alpha
Vantage, or any other market data API -- HTTP calls, JSON parsing, API keys.
It exposes a provider-agnostic `market_item_t` (see `main/market_api.h`) via
`market_get_items()`. `main/ui.c` only ever reads `market_item_t` -- it must
never call `esp_http_client_*` or parse JSON directly. If you add a new data
provider, add it inside `market_api.c` (or a new sibling file it calls into),
not in `ui.c`.

## Adding a new data source

1. Add a Kconfig entry in `main/Kconfig.projbuild` under "Market Data API"
   (API key if needed, symbol list, matching the existing
   `MARKET_CRYPTO_IDS` / `MARKET_STOCK_SYMBOLS` pattern).
2. Extend `market_item_t` only if the new source needs a field the existing
   struct can't represent (e.g. a new `asset_class` beyond crypto/stock) --
   prefer reusing `is_crypto`-style bools sparingly; if a third type is
   needed, consider whether an enum is cleaner than more bools.
3. Write the fetch+parse function following `fetch_crypto()` /
   `fetch_stock()` in `market_api.c` as the template: build URL, call
   `http_get_json()`, parse with cJSON, take `s_mutex`, write into the
   matching `s_items[i]`, release the mutex.
4. Respect rate limits explicitly (see `STOCK_REQUEST_GAP_MS` as the
   pattern) -- free-tier market APIs are usually the tightest constraint,
   not the ESP32.

## Adding/changing LVGL UI

- All LVGL calls made from outside the `esp_lvgl_port` task (i.e. anything
  triggered by `wifi_manager`'s callback or `market_api`'s refresh callback)
  must be wrapped in `bsp_display_lock()` / `bsp_display_unlock()` -- see
  `ui_update_wifi_status()` and `ui_refresh_market()` for the pattern. Skipping
  the lock causes race conditions with the LVGL port's own render task, which
  show up as intermittent corruption/crashes, not build errors -- easy to
  miss without deliberately checking for it.
- This project uses LVGL v9 APIs (`lv_screen_active()`, not `lv_scr_act()`;
  `lv_label_set_text_fmt`, flex layout via `lv_obj_set_flex_flow`). Check
  `main/idf_component.yml` for the pinned `lvgl/lvgl` version before using an
  API -- don't assume v8 patterns from older LVGL examples found online.
  Also check `sdkconfig.defaults` for enabled font sizes
  (`CONFIG_LV_FONT_MONTSERRAT_*`) before referencing a new
  `&lv_font_montserrat_NN` -- undeclared font sizes fail at link time, not
  compile time, and the error is not obviously about the font.
- Colors and layout constants live as `#define`s at the top of `ui.c`
  (`COLOR_BG`, `COLOR_CARD_BG`, etc.) -- reuse them instead of introducing
  new hardcoded hex colors, so the theme stays consistent as more screens
  are added.
- If adding a second screen (e.g. an on-device symbol picker), use
  `lv_screen_load()` / a tabview rather than manually hiding/showing
  objects on the single screen `ui_init()` builds today.

## After changes

Hand off to the `firmware-builder` agent (or run the build yourself) to
confirm it compiles before considering the task done -- LVGL/Kconfig
mismatches fail at build time, not something you can verify by reading code.
