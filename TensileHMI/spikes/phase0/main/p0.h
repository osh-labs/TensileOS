/*
 * Phase 0 bench spike: shared harness API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define P0_SD_ROOT      "/sdcard"
#define P0_CONFIG_PATH  P0_SD_ROOT "/phase0.json"
#define P0_OUT_DIR      P0_SD_ROOT "/phase0"
#define P0_REPORT_PATH  P0_OUT_DIR "/report.txt"

/* Loaded from /sdcard/phase0.json. Secrets are never logged. */
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char dav_base_url[160];   /* .../remote.php/dav/files/tensile-hmi/TensileOS/ */
    char dav_user[64];
    char dav_app_password[96];
    char tz[64];              /* POSIX TZ string */
    bool loaded;
} p0_config_t;

extern p0_config_t g_p0_cfg;

esp_err_t p0_config_load(void);

typedef enum { P0_PENDING = 0, P0_RUNNING, P0_PASS, P0_FAIL, P0_INFO } p0_result_t;

typedef struct {
    const char *id;           /* short id used in report */
    const char *title;        /* button label */
    p0_result_t (*run)(void); /* runs in the worker task, may block */
    bool long_running;        /* toggles on/off instead of run-once */
} p0_test_t;

/* Log a line to the on-screen console, the serial log and the SD report. */
void p0_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Record a key result line ("id: PASS detail") in the report. */
void p0_record(const char *id, p0_result_t r, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

void p0_request_run(size_t idx);
void p0_ui_init(const p0_test_t *tests, size_t n);
void p0_ui_set_status(size_t index, p0_result_t r);
void p0_ui_console_append(const char *line);

/* Individual tests (p0_tests_*.c). */
p0_result_t p0_test_display(void);
p0_result_t p0_test_sd(void);
p0_result_t p0_test_power(void);
p0_result_t p0_test_battery_log(void);
p0_result_t p0_test_usb_scale(void);
p0_result_t p0_test_dtr_probe(void);
p0_result_t p0_test_wifi(void);
p0_result_t p0_test_time(void);
p0_result_t p0_test_webdav(void);
p0_result_t p0_test_pdf(void);
p0_result_t p0_upload_report(void);

bool p0_wifi_is_up(void);
