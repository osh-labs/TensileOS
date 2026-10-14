/*
 * Phase 0 power tests: INA226 battery readings, charge path, USB-A 5V rail,
 * and a long-running battery logger for the discharge curve.
 */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "tab5_power.h"

#include "p0.h"

static void log_reading(const char *label, const tp_reading_t *r)
{
    p0_log("%-18s V=%.3f V  I=%+.3f A  P=%.2f W  SoC~%.0f%% (provisional)  CHG_STAT=%d charging=%d lowbat=%d",
           label, r->bus_v, r->current_a, r->power_w, r->soc_pct, r->chg_stat, r->charging, (int)r->lowbat);
}

p0_result_t p0_test_power(void)
{
    tp_reading_t r;
    if (tp_read(&r) != ESP_OK || !r.valid) {
        p0_record("power", P0_FAIL, "INA226 read failed (expected at I2C 0x41)");
        return P0_FAIL;
    }
    log_reading("now:", &r);
    bool volts_ok = r.bus_v > 6.0f && r.bus_v < 8.6f; /* 2S Li-ion window */

    /* Charge path: compare current with charging disabled and enabled. Only meaningful with USB-C attached. */
    tp_charge_enable(false);
    vTaskDelay(pdMS_TO_TICKS(1500));
    tp_reading_t off;
    tp_read(&off);
    log_reading("charge disabled:", &off);

    tp_charge_qc_enable(false);
    tp_charge_enable(true);
    vTaskDelay(pdMS_TO_TICKS(2000));
    tp_reading_t std;
    tp_read(&std);
    log_reading("charge std:", &std);

    tp_charge_qc_enable(true);
    vTaskDelay(pdMS_TO_TICKS(2000));
    tp_reading_t qc;
    tp_read(&qc);
    log_reading("charge QC on:", &qc);
    p0_log("Delta current enabled-disabled: %+.3f A (std), %+.3f A (QC). Near zero = USB-C not attached or "
           "battery full.", std.current_a - off.current_a, qc.current_a - off.current_a);

    /* USB-A rail: power-cycle it and confirm the system survives (scale may be attached). */
    p0_log("Cycling USB-A 5V (300 ms off)...");
    tp_reading_t before;
    tp_read(&before);
    esp_err_t cyc = tp_usb_a_5v_cycle(300);
    vTaskDelay(pdMS_TO_TICKS(1500));
    tp_reading_t after;
    tp_read(&after);
    p0_log("USB-A cycle: %s; battery current %+.3f A before, %+.3f A after (attached load shows as a step)",
           esp_err_to_name(cyc), before.current_a, after.current_a);

    p0_record("power", volts_ok ? P0_PASS : P0_FAIL, "pack %.2f V, %+.2f A, charge delta %+.2f A", r.bus_v,
              r.current_a, qc.current_a - off.current_a);
    return volts_ok ? P0_PASS : P0_FAIL;
}

/* ---------------------------------------------------------- battery logger */

static TaskHandle_t s_log_task;
static volatile bool s_log_stop;

static void battery_log_task(void *arg)
{
    const char *path = P0_OUT_DIR "/battery.csv";
    bool new_file = access(path, F_OK) != 0;
    FILE *f = fopen(path, "a");
    if (!f) {
        p0_log("Battery logger: cannot open %s", path);
        s_log_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (new_file) {
        fprintf(f, "uptime_s,utc,bus_v,current_a,power_w,soc_pct,chg_stat,charging,lowbat,wifi_up\n");
    }
    int64_t start = esp_timer_get_time();
    unsigned n = 0;
    while (!s_log_stop) {
        tp_reading_t r;
        if (tp_read(&r) == ESP_OK && r.valid) {
            fprintf(f, "%.0f,%lld,%.3f,%.4f,%.3f,%.1f,%d,%d,%d,%d\n", (esp_timer_get_time() - start) / 1e6,
                    (long long)time(NULL), r.bus_v, r.current_a, r.power_w, r.soc_pct, r.chg_stat, r.charging,
                    (int)r.lowbat, p0_wifi_is_up());
            /* Flush every sample so a brown-out loses at most one line. */
            fflush(f);
            fsync(fileno(f));
            if (++n % 30 == 0) {
                p0_log("Battery %.2f V %+.2f A (%u samples)", r.bus_v, r.current_a, n);
            }
            if (r.lowbat == TP_LOWBAT_SHUTDOWN) {
                p0_log("Battery logger: SHUTDOWN threshold reached (provisional curve); stopping log");
                break;
            }
        }
        for (int i = 0; i < 100 && !s_log_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(100)); /* 10 s period, responsive stop */
        }
    }
    fclose(f);
    p0_log("Battery logger stopped after %u samples (%.1f min)", n, (esp_timer_get_time() - start) / 6e7);
    s_log_task = NULL;
    vTaskDelete(NULL);
}

p0_result_t p0_test_battery_log(void)
{
    if (s_log_task) {
        s_log_stop = true;
        p0_record("battlog", P0_INFO, "stop requested");
        return P0_INFO;
    }
    s_log_stop = false;
    xTaskCreate(battery_log_task, "p0_battlog", 4096, NULL, 3, &s_log_task);
    p0_record("battlog", P0_RUNNING, "logging every 10 s to %s/battery.csv; press again to stop", P0_OUT_DIR);
    return P0_RUNNING;
}
