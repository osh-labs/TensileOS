/*
 * Phase 0 UI: test buttons with status on the left, console on the right.
 * Landscape 1280x720 after rotation. All LVGL calls take the BSP display lock
 * unless already running in the LVGL task.
 */
#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "p0.h"

#define MAX_TESTS       16
#define CONSOLE_MAX_LEN 12000

static lv_obj_t *s_status[MAX_TESTS];
static lv_obj_t *s_console;
static size_t s_n_tests;

static lv_color_t status_color(p0_result_t r)
{
    switch (r) {
    case P0_PASS: return lv_palette_main(LV_PALETTE_GREEN);
    case P0_FAIL: return lv_palette_main(LV_PALETTE_RED);
    case P0_RUNNING: return lv_palette_main(LV_PALETTE_AMBER);
    case P0_INFO: return lv_palette_main(LV_PALETTE_BLUE);
    default: return lv_palette_main(LV_PALETTE_GREY);
    }
}

static const char *status_text(p0_result_t r)
{
    switch (r) {
    case P0_PASS: return "PASS";
    case P0_FAIL: return "FAIL";
    case P0_RUNNING: return "...";
    case P0_INFO: return "INFO";
    default: return "-";
    }
}

static void btn_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    p0_request_run(idx);
}

void p0_ui_init(const p0_test_t *tests, size_t n)
{
    s_n_tests = n < MAX_TESTS ? n : MAX_TESTS;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 8, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "TensileOS HMI - Phase 0 bench spike");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *list = lv_obj_create(row);
    lv_obj_set_size(list, 420, LV_PCT(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x1c2228), 0);
    lv_obj_set_style_border_width(list, 0, 0);

    for (size_t i = 0; i < s_n_tests; i++) {
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 52);
        lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tests[i].title);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        s_status[i] = lv_label_create(btn);
        lv_label_set_text(s_status[i], status_text(P0_PENDING));
        lv_obj_set_style_text_font(s_status[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_bg_opa(s_status[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_status[i], status_color(P0_PENDING), 0);
        lv_obj_set_style_pad_hor(s_status[i], 8, 0);
        lv_obj_set_style_radius(s_status[i], 4, 0);
        lv_obj_align(s_status[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    s_console = lv_textarea_create(row);
    lv_obj_set_flex_grow(s_console, 1);
    lv_obj_set_height(s_console, LV_PCT(100));
    lv_textarea_set_text(s_console, "");
    lv_textarea_set_cursor_click_pos(s_console, false);
    lv_obj_remove_flag(s_console, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_text_font(s_console, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(s_console, lv_color_black(), 0);
    lv_obj_set_style_text_color(s_console, lv_color_hex(0xd0d0d0), 0);
}

void p0_ui_set_status(size_t index, p0_result_t r)
{
    if (index >= s_n_tests || !s_status[index]) {
        return;
    }
    if (bsp_display_lock(1000)) {
        lv_label_set_text(s_status[index], status_text(r));
        lv_obj_set_style_bg_color(s_status[index], status_color(r), 0);
        bsp_display_unlock();
    }
}

void p0_ui_console_append(const char *line)
{
    if (!s_console) {
        return;
    }
    if (bsp_display_lock(1000)) {
        /* Keep the console bounded: drop the oldest half when full. */
        const char *txt = lv_textarea_get_text(s_console);
        size_t len = strlen(txt);
        if (len > CONSOLE_MAX_LEN) {
            const char *cut = strchr(txt + len / 2, '\n');
            lv_textarea_set_text(s_console, cut ? cut + 1 : "");
        }
        lv_textarea_add_text(s_console, line);
        lv_textarea_add_text(s_console, "\n");
        bsp_display_unlock();
    }
}
