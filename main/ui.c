#include <stdio.h>
#include <inttypes.h>
#include "ui.h"
#include "bsp_display.h"
#include "market_api.h"

#include "lvgl.h"
#include "esp_timer.h"

#define COLOR_BG        0x14181F
#define COLOR_CARD_BG   0x1E2530
#define COLOR_TEXT      0xE8ECF1
#define COLOR_SUBTEXT   0x8B96A5
#define COLOR_GREEN     0x33C481
#define COLOR_RED       0xE5484D
#define COLOR_WIFI_OK   0x33C481
#define COLOR_WIFI_BAD  0xE5484D
#define COLOR_WIFI_MID  0xE0A93E

typedef struct {
    lv_obj_t *card;
    lv_obj_t *symbol_label;
    lv_obj_t *price_label;
    lv_obj_t *change_label;
    lv_obj_t *updated_label;
} market_card_t;

static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_wifi_text;
static lv_obj_t *s_grid;
static market_card_t s_cards[MARKET_MAX_ITEMS];
static size_t s_card_count;

static lv_obj_t *build_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 20, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "ESP Stock Ticker");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t *status_box = lv_obj_create(bar);
    lv_obj_remove_style_all(status_box);
    lv_obj_set_flex_flow(status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_box, 8, 0);

    s_wifi_icon = lv_label_create(status_box);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(COLOR_WIFI_BAD), 0);

    s_wifi_text = lv_label_create(status_box);
    lv_label_set_text(s_wifi_text, "conectando...");
    lv_obj_set_style_text_color(s_wifi_text, lv_color_hex(COLOR_SUBTEXT), 0);

    return bar;
}

static void build_card(lv_obj_t *parent, market_card_t *c, const market_item_t *item)
{
    c->card = lv_obj_create(parent);
    lv_obj_set_size(c->card, 220, 130);
    lv_obj_set_style_bg_color(c->card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(c->card, 14, 0);
    lv_obj_set_style_border_width(c->card, 0, 0);
    lv_obj_set_style_pad_all(c->card, 14, 0);
    lv_obj_clear_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kind = lv_label_create(c->card);
    lv_label_set_text(kind, item->is_crypto ? "CRIPTO" : "AÇÃO");
    lv_obj_set_style_text_color(kind, lv_color_hex(COLOR_SUBTEXT), 0);
    lv_obj_align(kind, LV_ALIGN_TOP_LEFT, 0, 0);

    c->symbol_label = lv_label_create(c->card);
    lv_label_set_text(c->symbol_label, item->symbol);
    lv_obj_set_style_text_color(c->symbol_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(c->symbol_label, &lv_font_montserrat_20, 0);
    lv_obj_align(c->symbol_label, LV_ALIGN_TOP_LEFT, 0, 20);

    c->price_label = lv_label_create(c->card);
    lv_label_set_text(c->price_label, "--");
    lv_obj_set_style_text_color(c->price_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(c->price_label, &lv_font_montserrat_28, 0);
    lv_obj_align(c->price_label, LV_ALIGN_TOP_LEFT, 0, 50);

    c->change_label = lv_label_create(c->card);
    lv_label_set_text(c->change_label, "");
    lv_obj_align(c->change_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    c->updated_label = lv_label_create(c->card);
    lv_label_set_text(c->updated_label, "");
    lv_obj_set_style_text_color(c->updated_label, lv_color_hex(COLOR_SUBTEXT), 0);
    lv_obj_align(c->updated_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_row(scr, 0, 0);

    build_top_bar(scr);

    s_grid = lv_obj_create(scr);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_grid, 20, 0);
    lv_obj_set_style_pad_gap(s_grid, 16, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);

    market_item_t items[MARKET_MAX_ITEMS];
    s_card_count = market_get_items(items, MARKET_MAX_ITEMS);
    for (size_t i = 0; i < s_card_count; i++) {
        build_card(s_grid, &s_cards[i], &items[i]);
    }
}

void ui_update_wifi_status(const wifi_mgr_status_t *status)
{
    // WiFi events can fire before ui_init() has built the widgets.
    if (!s_wifi_icon || !s_wifi_text) {
        return;
    }
    if (!bsp_display_lock(200)) {
        return;
    }

    switch (status->state) {
    case WIFI_MGR_STATE_CONNECTED:
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(COLOR_WIFI_OK), 0);
        lv_label_set_text_fmt(s_wifi_text, "%s  %s", status->ssid, status->ip);
        break;
    case WIFI_MGR_STATE_CONNECTING:
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(COLOR_WIFI_MID), 0);
        lv_label_set_text(s_wifi_text, "conectando...");
        break;
    default:
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(COLOR_WIFI_BAD), 0);
        lv_label_set_text(s_wifi_text, "sem WiFi");
        break;
    }

    bsp_display_unlock();
}

void ui_refresh_market(void)
{
    if (s_card_count == 0) {
        return;
    }

    market_item_t items[MARKET_MAX_ITEMS];
    size_t n = market_get_items(items, MARKET_MAX_ITEMS);

    if (!bsp_display_lock(200)) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < n && i < s_card_count; i++) {
        market_card_t *c = &s_cards[i];
        if (!items[i].valid) {
            lv_label_set_text(c->price_label, "--");
            lv_label_set_text(c->change_label, "");
            lv_label_set_text(c->updated_label, "");
            continue;
        }

        lv_label_set_text_fmt(c->price_label, "$%.2f", items[i].price);

        bool up = items[i].change_pct >= 0;
        lv_label_set_text_fmt(c->change_label, "%s %.2f%%", up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, items[i].change_pct);
        lv_obj_set_style_text_color(c->change_label, lv_color_hex(up ? COLOR_GREEN : COLOR_RED), 0);

        int64_t age_s = (now_us - items[i].last_update_us) / 1000000;
        lv_label_set_text_fmt(c->updated_label, "%" PRId64 "s atrás", age_s);
    }

    bsp_display_unlock();
}
