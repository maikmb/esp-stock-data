#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "ui_symbols.h"
#include "ui_theme.h"
#include "ui.h"
#include "bsp_display.h"
#include "market_api.h"

#include "lvgl.h"

// ASCII-only strings: the built-in Montserrat fonts have no accented glyphs
// (see ui_theme.h).

typedef enum {
    VIEW_NONE = 0,
    VIEW_LIST,       // tracked symbols + per-row remove + "add" button
    VIEW_INPUT,      // textarea + keyboard to type a new ticker
    VIEW_SEARCHING,  // spinner while market_lookup classifies it
    VIEW_RESULT,     // crypto/stock classification + add buttons
} ui_sym_view_t;

static ui_sym_view_t s_view;
static lv_obj_t *s_overlay;
static lv_obj_t *s_panel;
static lv_obj_t *s_kb;
static lv_obj_t *s_ta;
static lv_obj_t *s_err_label;   // inline error in the current view (may be NULL)

static market_item_t s_list_items[MARKET_MAX_ITEMS];
static size_t s_list_count;
static market_lookup_result_t s_result;

static void show_list_view(void);
static void show_input_view(void);
static void show_searching_view(void);
static void show_result_view(void);

/* ---------- shared helpers (same look & feel as ui_wifi.c) ---------- */

static void close_panel(void)
{
    if (s_overlay) {
        lv_obj_delete(s_overlay);
    }
    s_overlay = NULL;
    s_panel = NULL;
    s_kb = NULL;
    s_ta = NULL;
    s_err_label = NULL;
    s_view = VIEW_NONE;
}

static void overlay_clicked_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_overlay) {
        close_panel();
    }
}

static void close_btn_cb(lv_event_t *e)
{
    close_panel();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, uint32_t bg_color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_pad_hor(btn, 18, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *reset_panel(const char *title)
{
    lv_obj_clean(s_panel);
    if (s_kb) {
        lv_obj_delete(s_kb);
        s_kb = NULL;
    }
    s_ta = NULL;
    s_err_label = NULL;

    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_panel, 20, 0);
    lv_obj_set_style_pad_row(s_panel, 12, 0);

    lv_obj_t *header = lv_obj_create(s_panel);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);

    lv_obj_t *close_btn = lv_label_create(header);
    lv_label_set_text(close_btn, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_btn, lv_color_hex(COLOR_SUBTEXT), 0);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close_btn, 20);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = lv_obj_create(s_panel);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    return body;
}

static lv_obj_t *make_footer(void)
{
    lv_obj_t *footer = lv_obj_create(s_panel);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, 12, 0);
    return footer;
}

static void show_error(const char *msg)
{
    if (s_err_label) {
        lv_label_set_text(s_err_label, msg);
        lv_obj_clear_flag(s_err_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- list view ---------- */

static void remove_btn_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    market_remove_item(idx);
    ui_market_rebuild();
    show_list_view();
}

static void add_btn_cb(lv_event_t *e)
{
    show_input_view();
}

static void add_symbol_row(lv_obj_t *list, size_t idx)
{
    const market_item_t *it = &s_list_items[idx];

    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t *tag = lv_label_create(row);
    lv_label_set_text(tag, it->is_crypto ? "CRIPTO" : "BOLSA");
    lv_obj_set_style_text_color(tag, lv_color_hex(COLOR_SUBTEXT), 0);
    lv_obj_set_width(tag, 70);

    lv_obj_t *sym = lv_label_create(row);
    lv_label_set_text(sym, it->symbol);
    lv_obj_set_style_text_color(sym, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_20, 0);

    // For crypto the API key (CoinGecko id) differs from the ticker -- show
    // it so "BTC (bitcoin)" is unambiguous.
    if (it->is_crypto && strcasecmp(it->key, it->symbol) != 0) {
        lv_obj_t *key = lv_label_create(row);
        lv_label_set_text(key, it->key);
        lv_obj_set_style_text_color(key, lv_color_hex(COLOR_SUBTEXT), 0);
        lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(key, 1);
    } else {
        lv_obj_t *spacer = lv_obj_create(row);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_height(spacer, 1);
        lv_obj_set_flex_grow(spacer, 1);
    }

    lv_obj_t *trash = lv_label_create(row);
    lv_label_set_text(trash, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(trash, lv_color_hex(COLOR_RED), 0);
    lv_obj_add_flag(trash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(trash, 16);
    lv_obj_add_event_cb(trash, remove_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
}

static void show_list_view(void)
{
    s_view = VIEW_LIST;
    lv_obj_set_size(s_panel, 620, 520);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *body = reset_panel("Tickers");

    s_list_count = market_get_items(s_list_items, MARKET_MAX_ITEMS);

    if (s_list_count == 0) {
        lv_obj_t *empty = lv_label_create(body);
        lv_label_set_text(empty, "Nenhum ticker configurado");
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_SUBTEXT), 0);
    } else {
        lv_obj_t *list = lv_obj_create(body);
        lv_obj_remove_style_all(list);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 8, 0);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);

        for (size_t i = 0; i < s_list_count; i++) {
            add_symbol_row(list, i);
        }
    }

    lv_obj_t *footer = make_footer();
    if (s_list_count < MARKET_MAX_ITEMS) {
        make_button(footer, LV_SYMBOL_PLUS "  Adicionar", COLOR_ACCENT, add_btn_cb);
    } else {
        lv_obj_t *full = lv_label_create(footer);
        lv_label_set_text(full, "Lista cheia (max 8)");
        lv_obj_set_style_text_color(full, lv_color_hex(COLOR_SUBTEXT), 0);
    }
}

/* ---------- input view ---------- */

static void lookup_done_cb(const market_lookup_result_t *result, void *ctx)
{
    // Runs in the market_lookup task -> needs the LVGL lock.
    if (!bsp_display_lock(1000)) {
        return;
    }
    if (s_overlay && s_view == VIEW_SEARCHING) {
        s_result = *result;
        show_result_view();
    }
    bsp_display_unlock();
}

static void do_search(const char *query)
{
    esp_err_t err = market_lookup(query, lookup_done_cb, NULL);
    if (err == ESP_OK) {
        strlcpy(s_result.query, query, sizeof(s_result.query));
        show_searching_view();
    } else {
        show_error(err == ESP_ERR_INVALID_STATE ? "Sem WiFi ou consulta em andamento"
                                                : "Falha ao iniciar a consulta");
    }
}

static void search_btn_cb(lv_event_t *e)
{
    if (s_ta && lv_textarea_get_text(s_ta)[0]) {
        char query[16];
        strlcpy(query, lv_textarea_get_text(s_ta), sizeof(query));
        do_search(query);
    }
}

static void back_to_list_cb(lv_event_t *e)
{
    show_list_view();
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        search_btn_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        show_list_view();
    }
}

static void show_input_view(void)
{
    s_view = VIEW_INPUT;
    lv_obj_set_size(s_panel, 620, 264);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *body = reset_panel("Novo ticker");

    s_ta = lv_textarea_create(body);
    lv_obj_set_width(s_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, 15);
    lv_textarea_set_placeholder_text(s_ta, "ex: BTC, AAPL, PETR4.SAO");
    lv_textarea_set_accepted_chars(s_ta, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-");
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_ta, lv_color_hex(COLOR_SUBTEXT), 0);
    lv_obj_set_style_radius(s_ta, 8, 0);

    s_err_label = lv_label_create(body);
    lv_label_set_text(s_err_label, "");
    lv_obj_set_style_text_color(s_err_label, lv_color_hex(COLOR_RED), 0);
    lv_obj_add_flag(s_err_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *footer = make_footer();
    make_button(footer, "Voltar", COLOR_BG, back_to_list_cb);
    make_button(footer, LV_SYMBOL_REFRESH "  Buscar", COLOR_ACCENT, search_btn_cb);

    s_kb = lv_keyboard_create(s_overlay);
    lv_obj_set_size(s_kb, LV_PCT(100), 280);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(COLOR_BG), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

/* ---------- searching view ---------- */

static void show_searching_view(void)
{
    s_view = VIEW_SEARCHING;
    lv_obj_set_size(s_panel, 620, 260);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);

    char title[48];
    snprintf(title, sizeof(title), "Consultando %s", s_result.query);
    lv_obj_t *body = reset_panel(title);

    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 14, 0);

    lv_obj_t *spinner = lv_spinner_create(box);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_BG), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, "Verificando se e cripto ou acao...");
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_SUBTEXT), 0);
}

/* ---------- result view ---------- */

static void add_result(const char *key, const char *symbol, bool is_crypto)
{
    esp_err_t err = market_add_item(key, symbol, is_crypto);
    if (err == ESP_OK) {
        ui_market_rebuild();
        show_list_view();
    } else if (err == ESP_ERR_INVALID_STATE) {
        show_error("Este ticker ja esta na lista");
    } else if (err == ESP_ERR_NO_MEM) {
        show_error("Lista cheia (max 8)");
    } else {
        show_error("Falha ao adicionar");
    }
}

static void add_crypto_btn_cb(lv_event_t *e)
{
    add_result(s_result.crypto_id, s_result.crypto_symbol, true);
}

static void add_stock_btn_cb(lv_event_t *e)
{
    add_result(s_result.query, s_result.query, false);
}

static void retry_search_btn_cb(lv_event_t *e)
{
    do_search(s_result.query);
}

static void back_to_input_cb(lv_event_t *e)
{
    show_input_view();
}

static void show_result_view(void)
{
    s_view = VIEW_RESULT;
    lv_obj_set_size(s_panel, 620, 340);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *body = reset_panel("Resultado");

    lv_obj_t *footer = make_footer();

    if (!s_result.search_ok) {
        lv_obj_t *msg = lv_label_create(body);
        lv_label_set_text(msg, "Falha ao consultar a CoinGecko.\nVerifique a conexao e tente de novo.");
        lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT), 0);

        make_button(footer, "Voltar", COLOR_BG, back_to_input_cb);
        make_button(footer, "Tentar de novo", COLOR_ACCENT, retry_search_btn_cb);
        return;
    }

    if (s_result.crypto_found) {
        lv_obj_t *name = lv_label_create(body);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s - %s", s_result.crypto_symbol, s_result.crypto_name);
        lv_label_set_text(name, buf);
        lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);

        lv_obj_t *kind = lv_label_create(body);
        lv_label_set_text(kind, "Encontrado como criptomoeda (CoinGecko)");
        lv_obj_set_style_text_color(kind, lv_color_hex(COLOR_GREEN), 0);

        lv_obj_t *hint = lv_label_create(body);
        lv_label_set_text(hint, "Mesmo ticker existe na bolsa? Use \"Adicionar como acao\".");
        lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_SUBTEXT), 0);

        s_err_label = lv_label_create(body);
        lv_label_set_text(s_err_label, "");
        lv_obj_set_style_text_color(s_err_label, lv_color_hex(COLOR_RED), 0);
        lv_obj_add_flag(s_err_label, LV_OBJ_FLAG_HIDDEN);

        make_button(footer, "Voltar", COLOR_BG, back_to_input_cb);
        make_button(footer, "Adicionar como acao", COLOR_CARD_BG, add_stock_btn_cb);
        make_button(footer, "Adicionar cripto", COLOR_ACCENT, add_crypto_btn_cb);
    } else {
        lv_obj_t *name = lv_label_create(body);
        lv_label_set_text(name, s_result.query);
        lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);

        lv_obj_t *kind = lv_label_create(body);
        lv_label_set_text(kind, "Nao e cripto -- sera buscado como acao (Alpha Vantage)");
        lv_obj_set_style_text_color(kind, lv_color_hex(COLOR_WIFI_MID), 0);

        if (strlen(CONFIG_MARKET_ALPHA_VANTAGE_API_KEY) == 0) {
            lv_obj_t *warn = lv_label_create(body);
            lv_label_set_text(warn, "Atencao: sem API key da Alpha Vantage no menuconfig,\no preco ficara vazio.");
            lv_obj_set_style_text_color(warn, lv_color_hex(COLOR_RED), 0);
        }

        s_err_label = lv_label_create(body);
        lv_label_set_text(s_err_label, "");
        lv_obj_set_style_text_color(s_err_label, lv_color_hex(COLOR_RED), 0);
        lv_obj_add_flag(s_err_label, LV_OBJ_FLAG_HIDDEN);

        make_button(footer, "Voltar", COLOR_BG, back_to_input_cb);
        make_button(footer, "Adicionar acao", COLOR_ACCENT, add_stock_btn_cb);
    }
}

/* ---------- public API ---------- */

void ui_symbols_open(void)
{
    if (s_overlay) {
        return;
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, overlay_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_panel = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 16, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);  // swallow clicks so the scrim doesn't close
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    show_list_view();
}
