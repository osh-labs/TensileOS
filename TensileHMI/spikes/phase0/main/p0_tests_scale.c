/*
 * Phase 0 scale tests: OpenScale (FT231X) on the Tab5 USB-A port through
 * tensile_link, plus the DTR/RTS reset probe. Every raw line from the scale is
 * appended to /sdcard/phase0/scale_transcript.txt for replay in host tests.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "tensile_link.h"

#include "p0.h"

#define TRANSCRIPT_PATH P0_OUT_DIR "/scale_transcript.txt"

static bool s_usb_host_started, s_link_started;
static FILE *s_transcript;

/* Runs in the link task. Lines arrive at ~2 Hz, faster during the boot dump. */
static void raw_line_cb(const char *line, size_t len, uint32_t t_ms, void *ctx)
{
    if (!s_transcript) {
        return;
    }
    fprintf(s_transcript, "%10lu  %.*s\n", (unsigned long)t_ms, (int)len, line);
    fflush(s_transcript);
}

static esp_err_t ensure_link(void)
{
    if (s_link_started) {
        return ESP_OK;
    }
    esp_err_t err;
    if (!s_usb_host_started) {
        /* Installs the USB Host library; a second call would abort (BSP error check). */
        err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
        if (err != ESP_OK) {
            p0_log("bsp_usb_host_start: %s", esp_err_to_name(err));
            return err;
        }
        s_usb_host_started = true;
    }
    if (!s_transcript) {
        s_transcript = fopen(TRANSCRIPT_PATH, "a");
        if (s_transcript) {
            fprintf(s_transcript, "---- link start (columns: link ms, raw line) ----\n");
        }
    }
    tl_config_t cfg;
    tl_config_default(&cfg);
    cfg.raw_line_cb = raw_line_cb;
    err = tl_start(&cfg);
    if (err != ESP_OK) {
        p0_log("tl_start: %s", esp_err_to_name(err));
        return err;
    }
    s_link_started = true;
    return ESP_OK;
}

static void drain_events(void)
{
    tl_evt_t e;
    while (xQueueReceive(tl_event_queue(), &e, 0) == pdTRUE) {
    }
}

static void log_event(const tl_evt_t *e)
{
    switch (e->type) {
    case TL_EVT_STATE:
        p0_log("  state %s -> %s", tl_state_name(e->u.st.prev), tl_state_name(tl_get_state()));
        break;
    case TL_EVT_BOOT_PROGRESS:
        if (e->u.boot.pct % 25 == 0) {
            p0_log("  scale boot dump %u%%", e->u.boot.pct);
        }
        break;
    case TL_EVT_FAULT:
        p0_log("  FAULT %s in %s", tl_fault_name(e->u.fault.reason), tl_state_name(e->u.fault.in_state));
        break;
    case TL_EVT_LINK_GONE:
        p0_log("  LINK_GONE reason %d, test interrupted %d", (int)e->u.gone.reason, e->u.gone.test_interrupted);
        break;
    case TL_EVT_SAMPLE:
        break;
    default:
        p0_log("  %s", tl_evt_name(e->type));
        break;
    }
}

/* Waits for an event of the given type, logging everything else. */
static bool wait_for(tl_evt_type_t want, uint32_t timeout_ms, tl_evt_t *out)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    tl_evt_t e;
    while (esp_timer_get_time() < deadline) {
        if (xQueueReceive(tl_event_queue(), &e, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        log_event(&e);
        if (e.type == want) {
            if (out) {
                *out = e;
            }
            return true;
        }
        if (e.type == TL_EVT_FAULT) {
            return false;
        }
    }
    return false;
}

p0_result_t p0_test_usb_scale(void)
{
    if (ensure_link() != ESP_OK) {
        p0_record("usb", P0_FAIL, "USB host / link start failed");
        return P0_FAIL;
    }

    /* 1. Plug-to-ready. */
    int64_t t0 = esp_timer_get_time();
    if (tl_get_state() != TL_ST_READY_PAUSED) {
        p0_log("Plug the OpenScale into USB-A now (waiting up to 40 s)...");
        drain_events();
        if (!wait_for(TL_EVT_LINK_READY, 40000, NULL)) {
            p0_record("usb", P0_FAIL, "no LINK_READY (state %s)", tl_state_name(tl_get_state()));
            return P0_FAIL;
        }
    }
    int64_t ready_ms = (esp_timer_get_time() - t0) / 1000;
    p0_log("Scale ready (paused, JSON) %lld ms after the test started", ready_ms);

    /* 2. New test: ack, then 10 s of samples. Apply load to the cell now for a visible peak. */
    tl_proto_stats_t st0;
    uint32_t rx_drop0 = 0, ev_drop0 = 0;
    tl_get_stats(&st0, &rx_drop0, &ev_drop0);
    drain_events();
    p0_log("Starting a 10 s test; load the cell if you can");
    tl_cmd_new_test();
    tl_evt_t ack;
    if (!wait_for(TL_EVT_TEST_ACK, 5000, &ack)) {
        p0_record("usb", P0_FAIL, "no TEST_ACK after New Test");
        return P0_FAIL;
    }
    p0_log("TEST_ACK; pre-reset timestamp %.3f s (%s)", ack.u.ack.pre_reset_ts,
           ack.u.ack.pre_reset_valid ? "valid" : "n/a");

    int64_t t_end = esp_timer_get_time() + 10 * 1000000LL;
    unsigned n = 0;
    double first_ts = -1, last_ts = 0, peak = 0, sum_dt = 0;
    tl_evt_t e;
    while (esp_timer_get_time() < t_end) {
        if (xQueueReceive(tl_event_queue(), &e, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (e.type != TL_EVT_SAMPLE) {
            log_event(&e);
            continue;
        }
        if (first_ts < 0) {
            first_ts = e.u.sample.timestamp_s;
        } else {
            sum_dt += e.u.sample.timestamp_s - last_ts;
        }
        last_ts = e.u.sample.timestamp_s;
        if (e.u.sample.peak_kN > peak) {
            peak = e.u.sample.peak_kN;
        }
        if (n % 4 == 0) {
            p0_log("  t=%.3f s  current %.3f kN  peak %.3f kN", e.u.sample.timestamp_s, e.u.sample.current_kN,
                   e.u.sample.peak_kN);
        }
        n++;
    }
    tl_cmd_pause();
    bool paused = wait_for(TL_EVT_PAUSED, 3000, NULL);

    tl_proto_stats_t st;
    uint32_t rx_drop = 0, ev_drop = 0;
    tl_get_stats(&st, &rx_drop, &ev_drop);
    rx_drop -= rx_drop0; /* this run only */
    ev_drop -= ev_drop0;
    double mean_dt = n > 1 ? sum_dt / (n - 1) : 0;
    p0_log("Samples %u, first ts %.3f s, mean interval %.3f s, peak %.3f kN, paused %d, rx dropped %lu, events "
           "dropped %lu", n, first_ts, mean_dt, peak, paused, (unsigned long)rx_drop, (unsigned long)ev_drop);

    bool ok = n >= 15 && first_ts >= 0 && first_ts < 2.0 && paused && rx_drop == 0;
    p0_record("usb", ok ? P0_PASS : P0_FAIL, "ready in %lld ms, %u samples @ %.2f s, first %.3f s, rx drop %lu",
              ready_ms, n, mean_dt, first_ts, (unsigned long)rx_drop);

    /* 3. Optional hot-unplug check. */
    p0_log("Optional: unplug the scale within 15 s to test detach handling");
    drain_events();
    if (wait_for(TL_EVT_LINK_GONE, 15000, NULL)) {
        p0_record("usb_detach", P0_PASS, "LINK_GONE reported, no crash; replug to reconnect");
    } else {
        p0_log("No unplug seen (skipped)");
    }
    return ok ? P0_PASS : P0_FAIL;
}

static void log_edge(const char *name, const tl_probe_edge_t *e)
{
    p0_log("  %-13s reset=%d first_line=%ld ms first_calib=%ld ms menu=%ld ms rows=%u samples=%u lines=%u", name,
           e->reset, (long)e->first_line_ms, (long)e->first_calib_ms, (long)e->menu_ms, e->calib_rows, e->samples,
           e->lines);
}

p0_result_t p0_test_dtr_probe(void)
{
    if (ensure_link() == ESP_OK) {
        /* The link task may not have seen the attach yet right after start. */
        for (int i = 0; i < 30 && tl_get_state() == TL_ST_DETACHED; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!s_link_started || tl_get_state() == TL_ST_DETACHED) {
        p0_record("dtr", P0_FAIL, "scale not attached; run 'Scale on USB-A' first");
        return P0_FAIL;
    }
    p0_log("Probing DTR and RTS edges (up to ~45 s); do not touch the scale");
    tl_probe_result_t r;
    esp_err_t err = tl_probe_dtr(&r);
    if (err != ESP_OK || r.err != ESP_OK) {
        p0_record("dtr", P0_FAIL, "probe error %s", esp_err_to_name(err != ESP_OK ? err : r.err));
        return P0_FAIL;
    }
    p0_log("Streaming before probe: %d", r.streaming_before);
    log_edge("DTR assert", &r.dtr_assert);
    log_edge("DTR deassert", &r.dtr_deassert);
    log_edge("RTS assert", &r.rts_assert);
    log_edge("RTS deassert", &r.rts_deassert);

    const char *verdict = r.dtr_assert.reset && !r.dtr_deassert.reset && !r.rts_assert.reset && !r.rts_deassert.reset
                              ? "DTR assert resets the 328P (default reset_mode is correct)"
                          : (!r.dtr_assert.reset && !r.dtr_deassert.reset)
                              ? "DTR does not reset: set proto.reset_mode = TL_RESET_NONE"
                              : "unexpected pattern: review the edges above";
    p0_record("dtr", P0_INFO, "%s; boot menu %ld ms after reset", verdict, (long)r.dtr_assert.menu_ms);
    return P0_INFO;
}
