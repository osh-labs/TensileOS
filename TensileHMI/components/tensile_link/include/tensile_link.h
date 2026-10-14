/*
 * tensile_link: USB serial link to the SparkFun OpenScale running TensileOS
 * firmware v1.6 (FT231X, VID 0x0403 PID 0x6015, 115200 8N1), for the Tab5 HMI.
 *
 * Preconditions: the USB Host library is installed and the USB-A 5 V rail is on
 * (bsp_usb_host_start() from espressif/m5stack_tab5). tl_start() installs the
 * CDC-ACM host driver (unless told otherwise) and starts the link task, which
 * owns the protocol state machine (core/tl_proto.h). App events (tl_evt_t) are
 * delivered through a FreeRTOS queue; raw lines go to an optional callback.
 *
 * Threading: all functions are safe to call from any task. Commands are queued
 * to the link task; a command that is not valid in the current state produces
 * a TL_EVT_CMD_REJECTED event (the call itself still returns ESP_OK).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "tl_line.h"
#include "tl_parse.h"
#include "tl_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TL_FTDI_VID        0x0403
#define TL_FTDI_PID_FT231X 0x6015 /* OpenScale */
#define TL_FTDI_PID_AUTO   0      /* probe 0x6001 then 0x6015 */

/*
 * Raw line callback, called from the link task for every assembled line
 * (diagnostics console, SD log). Keep it short; copy the string if needed.
 */
typedef void (*tl_raw_line_cb_t)(const char *line, size_t len, uint32_t t_ms, void *ctx);

typedef struct {
    uint16_t pid;                  /* TL_FTDI_PID_FT231X (default) or TL_FTDI_PID_AUTO */
    tl_proto_cfg_t proto;          /* protocol timing; see tl_proto_cfg_default() */
    UBaseType_t task_priority;     /* link task; must be below cdc_driver_priority */
    uint32_t task_stack;
    BaseType_t task_core;          /* tskNO_AFFINITY allowed */
    uint16_t event_queue_len;      /* tl_evt_t items */
    bool install_cdc_driver;       /* false if the app already called cdc_acm_host_install() */
    unsigned cdc_driver_priority;
    tl_raw_line_cb_t raw_line_cb;  /* optional */
    void *raw_line_ctx;
} tl_config_t;

void tl_config_default(tl_config_t *cfg);

/* Start the link. cfg may be NULL for defaults. Call once. */
esp_err_t tl_start(const tl_config_t *cfg);

/* Queue of tl_evt_t (LINK_READY, LINK_GONE, TEST_ACK, SAMPLE, FAULT, ...). */
QueueHandle_t tl_event_queue(void);

esp_err_t tl_cmd_new_test(void);
esp_err_t tl_cmd_pause(void);
esp_err_t tl_cmd_resume(void);
esp_err_t tl_cmd_stop(void);
esp_err_t tl_cmd_reconnect(void);

tl_state_t tl_get_state(void);
/* Snapshot of protocol counters plus link-level drops. */
void tl_get_stats(tl_proto_stats_t *out, uint32_t *rx_bytes_dropped, uint32_t *events_dropped);

/* ------------------------------------------------------------------------ */
/* Phase 0 bench helper                                                     */

typedef struct {
    bool reset;               /* a calibration dump followed the edge => the 328P rebooted */
    int32_t first_line_ms;    /* edge -> first complete line, -1 if none */
    int32_t first_calib_ms;   /* edge -> first calibration row, -1 if none */
    int32_t menu_ms;          /* edge -> "Measurement Paused" header, -1 if none */
    uint16_t calib_rows;
    uint16_t samples;
    uint16_t lines;
} tl_probe_edge_t;

typedef struct {
    esp_err_t err;
    bool streaming_before;        /* samples seen during the 300 ms baseline */
    tl_probe_edge_t dtr_assert;   /* DTR 0 -> 1 (DTR# pin falls: the edge expected to reset) */
    tl_probe_edge_t dtr_deassert; /* DTR 1 -> 0 */
    tl_probe_edge_t rts_assert;   /* RTS 0 -> 1, DTR held 0 */
    tl_probe_edge_t rts_deassert; /* RTS 1 -> 0 */
} tl_probe_result_t;

/*
 * With the scale attached, toggle DTR and RTS one edge at a time and record
 * whether the 328P rebooted (calibration rows, then the menu) and how long it
 * took. Suspends the state machine for the duration (up to ~45 s), then runs
 * TL_CMD_RECONNECT. Blocks the caller. Results are also logged.
 */
esp_err_t tl_probe_dtr(tl_probe_result_t *out);

#ifdef __cplusplus
}
#endif
