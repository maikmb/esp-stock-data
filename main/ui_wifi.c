#include <stdio.h>
#include <string.h>
#include "ui_wifi.h"
#include "ui_theme.h"
#include "bsp_display.h"
#include "wifi_manager.h"

#include "lvgl.h"

// All strings here are ASCII-only: the built-in Montserrat fonts have no
// accented glyphs (see ui_theme.h).

typedef enum {
    VIEW_NONE = 0,
    VIEW_SCAN,        // network list + refresh
    VIEW_PASSWORD,    // textarea + on-screen keyboard
    VIEW_CONNECTING,  // spinner while wifi_manager tries the credentials
    VIEW_DETAILS,     // connected: connection info + disconnect
} ui_wifi_view_t;

static ui_wifi_view_t s_view;
static lv_obj_t *s_overlay;   // full-screen scrim on lv_layer_top()
static lv_obj_t *s_panel;
static lv_obj_t *s_kb;        // password view only
static lv_obj_t *s_ta;

static wifi_mgr_ap_t s_aps[WIFI_MGR_SCAN_MAX];
static size_t s_ap_count;
static char s_sel_ssid[33];
static bool s_sel_secure;
static bool s_connect_failed;  // show the error hint in the password view
static bool s_scan_running;

static void show_scan_view(bool trigger_scan);
static void show_password_view(void);
static void show_connecting_view(void);
static void show_details_view(void);

/* ---------- helpers ---------- */

static void close_panel(void)
{
    if (s_overlay) {
        lv_obj_delete(s_overlay);
    }
    s_overlay = NULL;
    s_panel = NULL;
    s_kb = NULL;
    s_ta = NULL;
    s_view = VIEW_NONE;
}

static uint32_t rssi_color(int8_t rssi)
{
    if (rssi >= -60) {
        return COLOR_WIFI_OK;
    }
    if (rssi >= -75) {
        return COLOR_WIFI_MID;
    }
    return COLOR_WIFI_BAD;
}

static const char *rssi_quality(int8_t rssi)
{
    if (rssi >= -60) {
        return "forte";
    }
    if (rssi >= -75) {
        return "medio";
    }
    return "fraco";
}

static void overlay_clicked_cb(lv_event_t *e)
{
    // Only clicks on the scrim itself (outside the panel) close the overlay.
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

// Clears the panel and rebuilds the common header. Returns the body
// container (flex column, grows to fill the panel).
static lv_obj_t *reset_panel(const char *title)
{
    lv_obj_clean(s_panel);
    // The keyboard is a child of the overlay, not the panel, so it survives
    // lv_obj_clean() -- drop it explicitly when switching views.
    if (s_kb) {
        lv_obj_delete(s_kb);
        s_kb = NULL;
    }
    s_ta = NULL;

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

/* ---------- scan view ---------- */

static void scan_done_cb(void *ctx)
{
    // Runs in the esp_event task -> needs the LVGL lock.
    if (!bsp_display_lock(1000)) {
        s_scan_running = false;
        return;
    }
    s_scan_running = false;
    if (s_overlay && s_view == VIEW_SCAN) {
        show_scan_view(false);  // rebuild with the fresh results
    }
    bsp_display_unlock();
}

static void start_scan(void)
{
    if (wifi_manager_scan_start(scan_done_cb, NULL) == ESP_OK) {
        s_scan_running = true;
    }
}

static void refresh_btn_cb(lv_event_t *e)
{
    show_scan_view(true);
}

static void net_row_clicked_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_ap_count) {
        return;
    }
    strlcpy(s_sel_ssid, s_aps[idx].ssid, sizeof(s_sel_ssid));
    s_sel_secure = s_aps[idx].secure;
    s_connect_failed = false;

    if (s_sel_secure) {
        show_password_view();
    } else {
        wifi_manager_connect(s_sel_ssid, NULL);
        show_connecting_view();
    }
}

static void add_net_row(lv_obj_t *list, size_t idx)
{
    const wifi_mgr_ap_t *ap = &s_aps[idx];

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
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, net_row_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, lv_color_hex(rssi_color(ap->rssi)), 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, ap->ssid);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);

    lv_obj_t *tag = lv_label_create(row);
    lv_label_set_text(tag, ap->secure ? "WPA" : "aberta");
    lv_obj_set_style_text_color(tag, lv_color_hex(COLOR_SUBTEXT), 0);
}

static void show_scan_view(bool trigger_scan)
{
    s_view = VIEW_SCAN;
    lv_obj_set_size(s_panel, 620, 520);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *body = reset_panel("Redes WiFi");

    if (trigger_scan) {
        start_scan();
    }
    s_ap_count = wifi_manager_get_scan_results(s_aps, WIFI_MGR_SCAN_MAX);

    if (s_ap_count == 0) {
        lv_obj_t *box = lv_obj_create(body);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, LV_PCT(100));
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(box, 14, 0);

        if (s_scan_running) {
            lv_obj_t *spinner = lv_spinner_create(box);
            lv_obj_set_size(spinner, 56, 56);
            lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_BG), LV_PART_MAIN);
        }

        lv_obj_t *label = lv_label_create(box);
        lv_label_set_text(label, s_scan_running ? "Buscando redes..." : "Nenhuma rede encontrada");
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_SUBTEXT), 0);
    } else {
        lv_obj_t *list = lv_obj_create(body);
        lv_obj_remove_style_all(list);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 8, 0);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);

        for (size_t i = 0; i < s_ap_count; i++) {
            add_net_row(list, i);
        }
    }

    lv_obj_t *footer = lv_obj_create(s_panel);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_button(footer, LV_SYMBOL_REFRESH "  Buscar", COLOR_ACCENT, refresh_btn_cb);
}

/* ---------- password view ---------- */

static void do_connect(void)
{
    const char *pw = s_ta ? lv_textarea_get_text(s_ta) : "";
    wifi_manager_connect(s_sel_ssid, pw);
    show_connecting_view();
}

static void connect_btn_cb(lv_event_t *e)
{
    do_connect();
}

static void back_btn_cb(lv_event_t *e)
{
    show_scan_view(false);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {  // checkmark key
        do_connect();
    } else if (code == LV_EVENT_CANCEL) {
        show_scan_view(false);
    }
}

static void show_password_view(void)
{
    s_view = VIEW_PASSWORD;
    // Panel at the top, on-screen keyboard on the bottom half of the overlay.
    lv_obj_set_size(s_panel, 620, 264);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 12);

    char title[64];
    snprintf(title, sizeof(title), "Senha de %s", s_sel_ssid);
    lv_obj_t *body = reset_panel(title);

    s_ta = lv_textarea_create(body);
    lv_obj_set_width(s_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_password_mode(s_ta, true);
    lv_textarea_set_max_length(s_ta, 64);
    lv_textarea_set_placeholder_text(s_ta, "senha da rede");
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_ta, lv_color_hex(COLOR_SUBTEXT), 0);
    lv_obj_set_style_radius(s_ta, 8, 0);

    if (s_connect_failed) {
        lv_obj_t *err = lv_label_create(body);
        lv_label_set_text(err, "Falha ao conectar. Verifique a senha.");
        lv_obj_set_style_text_color(err, lv_color_hex(COLOR_RED), 0);
    }

    lv_obj_t *footer = lv_obj_create(s_panel);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, 12, 0);
    make_button(footer, "Voltar", COLOR_BG, back_btn_cb);
    make_button(footer, "Conectar", COLOR_ACCENT, connect_btn_cb);

    s_kb = lv_keyboard_create(s_overlay);
    lv_obj_set_size(s_kb, LV_PCT(100), 280);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(COLOR_BG), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

/* ---------- connecting view ---------- */

static void show_connecting_view(void)
{
    s_view = VIEW_CONNECTING;
    lv_obj_set_size(s_panel, 620, 260);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);

    char title[64];
    snprintf(title, sizeof(title), "Conectando a %s", s_sel_ssid);
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
    lv_label_set_text(label, "Aguarde...");
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_SUBTEXT), 0);
}

/* ---------- details view ---------- */

static void add_detail_row(lv_obj_t *parent, const char *name, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = lv_label_create(row);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_SUBTEXT), 0);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, value);
    lv_obj_set_style_text_color(value_label, lv_color_hex(COLOR_TEXT), 0);
}

static void disconnect_btn_cb(lv_event_t *e)
{
    wifi_manager_disconnect();
    show_scan_view(true);
}

static void show_details_view(void)
{
    s_view = VIEW_DETAILS;
    lv_obj_set_size(s_panel, 620, 480);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *body = reset_panel("Conexao WiFi");

    wifi_mgr_status_t st;
    wifi_manager_get_status(&st);

    char buf[48];
    add_detail_row(body, "Rede", st.ssid);
    add_detail_row(body, "IP", st.ip);
    snprintf(buf, sizeof(buf), "%d dBm (%s)", st.rssi, rssi_quality(st.rssi));
    add_detail_row(body, "Sinal", buf);
    add_detail_row(body, "Gateway", st.gateway);
    add_detail_row(body, "Mascara", st.netmask);
    snprintf(buf, sizeof(buf), "%u", st.channel);
    add_detail_row(body, "Canal", buf);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             st.bssid[0], st.bssid[1], st.bssid[2], st.bssid[3], st.bssid[4], st.bssid[5]);
    add_detail_row(body, "BSSID", buf);

    lv_obj_t *footer = lv_obj_create(s_panel);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, 12, 0);
    make_button(footer, "Desconectar", COLOR_RED, disconnect_btn_cb);
    make_button(footer, "Fechar", COLOR_BG, close_btn_cb);
}

/* ---------- public API ---------- */

void ui_wifi_open(void)
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

    wifi_mgr_status_t st;
    wifi_manager_get_status(&st);
    if (st.state == WIFI_MGR_STATE_CONNECTED) {
        show_details_view();
    } else {
        show_scan_view(true);
    }
}

void ui_wifi_notify_status(const wifi_mgr_status_t *status)
{
    if (!s_overlay) {
        return;
    }

    switch (s_view) {
    case VIEW_CONNECTING:
        if (status->state == WIFI_MGR_STATE_CONNECTED) {
            close_panel();
        } else if (status->state == WIFI_MGR_STATE_CONNECT_FAILED) {
            s_connect_failed = true;
            if (s_sel_secure) {
                show_password_view();
            } else {
                show_scan_view(false);
            }
        }
        break;
    case VIEW_DETAILS:
        if (status->state != WIFI_MGR_STATE_CONNECTED) {
            show_scan_view(true);
        }
        break;
    default:
        break;
    }
}
