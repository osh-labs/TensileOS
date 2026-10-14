/*
 * Phase 0 tests that need only the BSP: display/touch, microSD, I2C scan, time.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "p0.h"

/* ---------------------------------------------------------------- I2C scan */

/* Known addresses on the Tab5 internal bus (see docs/phase0_hw_notes.md). */
static const struct { uint8_t addr; const char *name; } k_known[] = {
    { 0x14, "GT911 touch -> display variant ILI9881C" },
    { 0x5D, "GT911 touch" },
    { 0x55, "ST7123/ST7121 touch -> display variant ST7123" },
    { 0x43, "PI4IOE5V6408 IO expander #0" },
    { 0x44, "PI4IOE5V6408 IO expander #1 (power)" },
    { 0x41, "INA226 battery monitor" },
    { 0x32, "RX8130CE RTC" },
    { 0x10, "ES8388 codec" },
    { 0x68, "BMI270 IMU" },
};

static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        p0_log("I2C bus not initialized");
        return;
    }
    char found[256] = "";
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            char item[12];
            snprintf(item, sizeof(item), "0x%02X ", a);
            strlcat(found, item, sizeof(found));
            for (size_t i = 0; i < sizeof(k_known) / sizeof(k_known[0]); i++) {
                if (k_known[i].addr == a) {
                    p0_log("  I2C 0x%02X  %s", a, k_known[i].name);
                }
            }
        }
    }
    p0_log("I2C devices: %s", found[0] ? found : "(none)");
}

/* ------------------------------------------------------------ display test */

static EventGroupHandle_t s_touch_ev;
static lv_obj_t *s_overlay;
static int s_touch_x[4], s_touch_y[4];

static void target_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    s_touch_x[i] = p.x;
    s_touch_y[i] = p.y;
    lv_obj_set_style_bg_color(lv_event_get_target(e), lv_palette_main(LV_PALETTE_GREEN), 0);
    xEventGroupSetBits(s_touch_ev, BIT0 << i);
}

static void show_fill(lv_color_t c, const char *label)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    lv_obj_set_style_bg_color(s_overlay, c, 0);
    lv_obj_clean(s_overlay);
    lv_obj_t *l = lv_label_create(s_overlay);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x808080), 0);
    lv_obj_center(l);
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(1200));
}

/* Measures full redraw time of a 1,000-point chart, the worst case for the live screen. */
static void chart_benchmark(void)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    lv_obj_clean(s_overlay);
    lv_obj_set_style_bg_color(s_overlay, lv_color_white(), 0);
    lv_obj_t *chart = lv_chart_create(s_overlay);
    lv_obj_set_size(chart, 900, 520);
    lv_obj_center(chart);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 1000);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 4000);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_series_t *ser = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    bsp_display_unlock();

    const int frames = 30;
    int64_t total_us = 0, worst_us = 0;
    for (int f = 0; f < frames; f++) {
        if (!bsp_display_lock(1000)) {
            break;
        }
        for (int i = 0; i < 1000; i++) {
            /* Ramp with noise, shifted each frame so every point changes. */
            int v = (i * 3 + f * 37) % 3800 + (int)(esp_random() % 120);
            lv_chart_set_value_by_id(chart, ser, i, v);
        }
        lv_chart_refresh(chart);
        int64_t t0 = esp_timer_get_time();
        lv_refr_now(NULL);
        int64_t dt = esp_timer_get_time() - t0;
        bsp_display_unlock();
        total_us += dt;
        if (dt > worst_us) {
            worst_us = dt;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    p0_log("Chart 1000 pts 900x520: mean %.1f ms, worst %.1f ms per full redraw (sw rotate on)",
           total_us / 1000.0 / frames, worst_us / 1000.0);
}

p0_result_t p0_test_display(void)
{
    i2c_scan();

    s_touch_ev = s_touch_ev ? s_touch_ev : xEventGroupCreate();
    xEventGroupClearBits(s_touch_ev, 0x0F);

    if (!bsp_display_lock(1000)) {
        p0_record("display", P0_FAIL, "could not take display lock");
        return P0_FAIL;
    }
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_display_t *disp = lv_display_get_default();
    int w = lv_display_get_horizontal_resolution(disp);
    int h = lv_display_get_vertical_resolution(disp);
    bsp_display_unlock();
    p0_log("LVGL resolution after rotation: %dx%d (expect 1280x720)", w, h);

    show_fill(lv_palette_main(LV_PALETTE_RED), "RED");
    show_fill(lv_palette_main(LV_PALETTE_GREEN), "GREEN");
    show_fill(lv_palette_main(LV_PALETTE_BLUE), "BLUE");
    show_fill(lv_color_white(), "WHITE");
    show_fill(lv_color_black(), "BLACK");
    chart_benchmark();

    /* Touch: four corner targets, 30 s to hit them all. */
    if (bsp_display_lock(1000)) {
        lv_obj_clean(s_overlay);
        lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x202020), 0);
        lv_obj_t *l = lv_label_create(s_overlay);
        lv_label_set_text(l, "Tap the four red targets (30 s)");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
        const lv_align_t al[4] = { LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *t = lv_button_create(s_overlay);
            lv_obj_set_size(t, 90, 90);
            lv_obj_set_style_bg_color(t, lv_palette_main(LV_PALETTE_RED), 0);
            lv_obj_align(t, al[i], 0, 0);
            lv_obj_add_event_cb(t, target_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        bsp_display_unlock();
    }
    EventBits_t bits = xEventGroupWaitBits(s_touch_ev, 0x0F, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    if (bsp_display_lock(1000)) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        bsp_display_unlock();
    }

    int hits = __builtin_popcount(bits & 0x0F);
    for (int i = 0; i < 4; i++) {
        if (bits & (BIT0 << i)) {
            p0_log("  target %d touched at (%d,%d)", i, s_touch_x[i], s_touch_y[i]);
        }
    }
    bool ok = (w == 1280 && h == 720 && hits == 4);
    p0_record("display", ok ? P0_PASS : P0_FAIL, "res %dx%d, touch targets %d/4", w, h, hits);
    return ok ? P0_PASS : P0_FAIL;
}

/* ----------------------------------------------------------------- SD test */

p0_result_t p0_test_sd(void)
{
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(P0_SD_ROOT, &total, &free_b) != ESP_OK) {
        p0_record("sd", P0_FAIL, "card not mounted");
        return P0_FAIL;
    }
    p0_log("SD: %.1f GB total, %.1f GB free", total / 1e9, free_b / 1e9);

    const size_t chunk = 32 * 1024, n_chunks = 64; /* 2 MiB */
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        p0_record("sd", P0_FAIL, "no memory");
        return P0_FAIL;
    }
    const char *path = P0_OUT_DIR "/sdtest.bin";
    bool ok = true;

    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(path, "wb");
    for (size_t c = 0; f && c < n_chunks && ok; c++) {
        memset(buf, (int)(c & 0xFF), chunk);
        ok = fwrite(buf, 1, chunk, f) == chunk;
    }
    if (f) {
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    } else {
        ok = false;
    }
    int64_t t_write = esp_timer_get_time() - t0;

    t0 = esp_timer_get_time();
    f = ok ? fopen(path, "rb") : NULL;
    for (size_t c = 0; f && c < n_chunks && ok; c++) {
        ok = fread(buf, 1, chunk, f) == chunk && buf[0] == (uint8_t)c && buf[chunk - 1] == (uint8_t)c;
    }
    if (f) {
        fclose(f);
    }
    int64_t t_read = esp_timer_get_time() - t0;
    unlink(path);
    free(buf);

    double mb = chunk * n_chunks / 1e6;
    p0_record("sd", ok ? P0_PASS : P0_FAIL, "2 MiB write %.2f MB/s (incl fsync), read %.2f MB/s, verify %s",
              mb / (t_write / 1e6), mb / (t_read / 1e6), ok ? "ok" : "MISMATCH");
    return ok ? P0_PASS : P0_FAIL;
}

/* --------------------------------------------------------------- time test */

p0_result_t p0_test_time(void)
{
    if (!p0_wifi_is_up()) {
        p0_record("time", P0_FAIL, "Wi-Fi not connected; run the Wi-Fi test first");
        return P0_FAIL;
    }
    static bool started;
    if (!started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&cfg);
        started = true;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    if (err != ESP_OK) {
        p0_record("time", P0_FAIL, "SNTP sync timed out after %lld ms", dt_ms);
        return P0_FAIL;
    }

    time_t now = time(NULL);
    struct tm lt, ut;
    localtime_r(&now, &lt);
    gmtime_r(&now, &ut);
    char lbuf[40], ubuf[40];
    strftime(lbuf, sizeof(lbuf), "%Y-%m-%d %H:%M:%S %Z", &lt);
    strftime(ubuf, sizeof(ubuf), "%Y-%m-%dT%H:%M:%SZ", &ut);

    /* Check the DST rule by converting a known instant on each side of the 2026 change. */
    struct tm probe;
    time_t jul = 1784808000; /* 2026-07-23 12:00:00Z */
    time_t jan = 1767182400; /* 2025-12-31 12:00:00Z */
    localtime_r(&jul, &probe);
    int jul_h = probe.tm_hour;
    localtime_r(&jan, &probe);
    int jan_h = probe.tm_hour;

    p0_log("Local %s  UTC %s  TZ=%s", lbuf, ubuf, getenv("TZ"));
    p0_log("TZ rule check: 12:00Z -> %02d:00 in July, %02d:00 in December (Eastern expects 08 and 07)", jul_h, jan_h);
    p0_log("RTC (RX8130CE) read/write is a Phase 1 item; presence shown in the display test I2C scan");
    p0_record("time", P0_PASS, "SNTP synced in %lld ms; %s", dt_ms, lbuf);
    return P0_PASS;
}
