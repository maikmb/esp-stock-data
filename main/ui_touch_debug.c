#include <inttypes.h>
#include "ui_touch_debug.h"
#include "ui_theme.h"

#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "touch_debug";

static lv_obj_t *s_dot;
static lv_obj_t *s_label;
static uint32_t s_press_count;
static bool s_was_pressed;

// Polls the pointer indev directly (position + pressed state) instead of
// hooking a click event, so it works no matter which widget is under the
// finger and never steals touches from the rest of the UI.
static void poll_timer_cb(lv_timer_t *timer)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        indev = lv_indev_get_next(indev);
    }
    if (!indev) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;

    if (pressed && !s_was_pressed) {
        s_press_count++;
        ESP_LOGI(TAG, "touch pressed #%" PRIu32 " at x=%d y=%d", s_press_count, (int)point.x, (int)point.y);
    } else if (!pressed && s_was_pressed) {
        ESP_LOGI(TAG, "touch released at x=%d y=%d", (int)point.x, (int)point.y);
    }
    s_was_pressed = pressed;

    if (pressed) {
        lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_dot, point.x - 15, point.y - 15);
    } else {
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text_fmt(s_label, "touch #%" PRIu32 "  x=%d y=%d  %s",
                           s_press_count, (int)point.x, (int)point.y,
                           pressed ? "PRESSIONADO" : "solto");
}

void ui_touch_debug_init(void)
{
    lv_obj_t *top = lv_layer_top();

    s_dot = lv_obj_create(top);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 30, 30);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_70, 0);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    s_label = lv_label_create(top);
    lv_obj_set_style_text_color(s_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_bg_color(s_label, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_label, 4, 0);
    lv_obj_clear_flag(s_label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_label, "toque na tela para testar o touch");
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_timer_create(poll_timer_cb, 50, NULL);

    ESP_LOGI(TAG, "touch debug overlay active");
}
