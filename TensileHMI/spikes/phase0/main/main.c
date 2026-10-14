/*
 * TensileOS HMI - Phase 0 bench spike.
 *
 * Brings up each risky subsystem of the Tab5 Kit in isolation and records
 * PASS/FAIL plus measurements to /sdcard/phase0/report.txt. Tests are started
 * from the touchscreen; results can then be uploaded to Nextcloud.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "tab5_power.h"

#include "p0.h"

static const char *TAG = "phase0";

static const p0_test_t s_tests[] = {
    { "display",  "Display + touch",      p0_test_display,     false },
    { "sd",       "microSD write/read",   p0_test_sd,          false },
    { "power",    "Power + USB-A 5V",     p0_test_power,       false },
    { "usb",      "Scale on USB-A",       p0_test_usb_scale,   false },
    { "dtr",      "DTR/RTS reset probe",  p0_test_dtr_probe,   false },
    { "wifi",     "Wi-Fi (ESP32-C6)",     p0_test_wifi,        false },
    { "time",     "SNTP + time zone",     p0_test_time,        false },
    { "webdav",   "Nextcloud WebDAV",     p0_test_webdav,      false },
    { "pdf",      "PDF proof page",       p0_test_pdf,         false },
    { "battlog",  "Battery logger",       p0_test_battery_log, true  },
    { "upload",   "Upload report",        p0_upload_report,    false },
};
#define N_TESTS (sizeof(s_tests) / sizeof(s_tests[0]))

static QueueHandle_t s_run_queue;
static SemaphoreHandle_t s_report_mutex;
static bool s_sd_ok;

static void report_append(const char *line)
{
    if (!s_sd_ok) {
        return;
    }
    xSemaphoreTake(s_report_mutex, portMAX_DELAY);
    FILE *f = fopen(P0_REPORT_PATH, "a");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
    xSemaphoreGive(s_report_mutex);
}

static void timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}

void p0_log(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char ts[24];
    timestamp(ts, sizeof(ts));
    char line[300];
    snprintf(line, sizeof(line), "%s  %s", ts + 11, msg);

    ESP_LOGI(TAG, "%s", msg);
    p0_ui_console_append(line);
    report_append(line);
}

static const char *result_str(p0_result_t r)
{
    switch (r) {
    case P0_PASS: return "PASS";
    case P0_FAIL: return "FAIL";
    case P0_INFO: return "INFO";
    case P0_RUNNING: return "RUNNING";
    default: return "PENDING";
    }
}

void p0_record(const char *id, p0_result_t r, const char *fmt, ...)
{
    char detail[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    p0_log("RESULT %s: %s  %s", id, result_str(r), detail);
}

static void worker_task(void *arg)
{
    size_t idx;
    for (;;) {
        if (xQueueReceive(s_run_queue, &idx, portMAX_DELAY) != pdTRUE || idx >= N_TESTS) {
            continue;
        }
        const p0_test_t *t = &s_tests[idx];
        p0_ui_set_status(idx, P0_RUNNING);
        p0_log("=== %s ===", t->title);
        p0_result_t r = t->run();
        p0_ui_set_status(idx, r);
    }
}

/* Called from the UI (LVGL task) when a test button is pressed. */
void p0_request_run(size_t idx)
{
    if (xQueueSend(s_run_queue, &idx, 0) != pdTRUE) {
        p0_log("Busy: test queue full");
    }
}

static void write_report_header(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);

    char ts[24];
    timestamp(ts, sizeof(ts));
    char line[200];
    report_append("");
    report_append("==================================================");
    snprintf(line, sizeof(line), "TensileOS HMI Phase 0 report  boot %s (clock may be unset)", ts);
    report_append(line);
    snprintf(line, sizeof(line), "App %s %s  IDF %s", app->project_name, app->version, app->idf_ver);
    report_append(line);
    snprintf(line, sizeof(line), "Chip ESP32-P4 rev v%d.%d, %d cores  Device TAB5-%02X%02X%02X",
             chip.revision / 100, chip.revision % 100, chip.cores, mac[3], mac[4], mac[5]);
    report_append(line);
    snprintf(line, sizeof(line), "Reset reason %d  Free heap %u  Free PSRAM %u",
             (int)esp_reset_reason(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    report_append(line);
    report_append("==================================================");
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_report_mutex = xSemaphoreCreateMutex();
    s_run_queue = xQueueCreate(4, sizeof(size_t));

    /* Display first so failures below are visible on screen. */
    lv_display_t *disp = bsp_display_start();
    if (disp) {
        bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90); /* panel is 720x1280 portrait */
        bsp_display_brightness_set(70);
    }

    /* Must precede Wi-Fi and USB: first use of the 0x44 IO expander resets it, which
     * drops C6 power, USB-A 5V and charging; tp_init() turns them back on. */
    esp_err_t pwr_err = tp_init(NULL);

    s_sd_ok = (bsp_sdcard_mount() == ESP_OK);
    if (s_sd_ok) {
        mkdir(P0_OUT_DIR, 0775);
    }

    if (p0_config_load() == ESP_OK && g_p0_cfg.tz[0]) {
        setenv("TZ", g_p0_cfg.tz, 1);
    } else {
        setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    }
    tzset();

    if (bsp_display_lock(0)) {
        p0_ui_init(s_tests, N_TESTS);
        bsp_display_unlock();
    }

    write_report_header();
    p0_log("Power init: %s", esp_err_to_name(pwr_err));
    p0_log("SD card: %s", s_sd_ok ? "mounted at " P0_SD_ROOT : "NOT mounted - results only on screen and serial");
    p0_log("Config %s: %s", P0_CONFIG_PATH, g_p0_cfg.loaded ? "loaded" : "missing (Wi-Fi/WebDAV tests will fail)");

    xTaskCreatePinnedToCore(worker_task, "p0_worker", 12288, NULL, 5, NULL, 1);
}
