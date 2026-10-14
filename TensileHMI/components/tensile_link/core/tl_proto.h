/*
 * tensile_link core: connection / protocol state machine for TensileOS
 * firmware v1.6 (build plan Sections 5.2 and 5.3).
 *
 * Event driven and side-effect free: every entry point takes the current
 * monotonic time in ms and fills a tl_out_t with actions for the glue to
 * execute (in order) and app events to deliver. There are no sleeps; every
 * wait is a deadline checked by tl_proto_tick().
 *
 *   DETACHED --attach--> OPENING --opened--> RESETTING --(DTR settle)--> BOOT_WAIT
 *   BOOT_WAIT: calibration rows => reset confirmed, wait for menu (12 s, one
 *              re-reset, then FAULT). No rows within reset_detect_ms, or a
 *              sample after reset_grace_ms => DTR did not reset the 328P:
 *              fall back to 'x' (works whether the device was paused or running).
 *   menu seen (paused): JSON => READY_PAUSED (LINK_READY); else send 'j' => MODE_SET
 *   MODE_SET: "changed to: JSON" or "CSV" => device is now RUNNING => send 'x' => PAUSING
 *             (a 'j' while running is ignored by v1.6, so the CSV case pauses
 *             first and toggles again from the menu)
 *   PAUSING:  menu => re-evaluate (above)
 *   READY_PAUSED --NEW_TEST: 'x'--> NEW_TEST_PENDING --"Starting new test..."--> RUNNING (TEST_ACK)
 *   RUNNING --NEW_TEST: 'x'--> NEW_TEST_PENDING(wait menu) --menu: 'x'--> (wait ack) --> RUNNING
 *   RUNNING --PAUSE/STOP: 'x'--> PAUSING --menu--> READY_PAUSED
 *   READY_PAUSED --RESUME: 'r'--> RESUMING --"Resuming measurements..."--> RUNNING
 *   any --detach--> DETACHED (LINK_GONE);  calibration rows while linked => LINK_GONE + BOOT_WAIT
 *   every wait: timeout => one retry => FAULT (LINK_GONE if it was ready)
 *
 * Pure C11, no ESP-IDF headers, no allocation.
 */
#ifndef TL_PROTO_H
#define TL_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tl_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TL_ST_DETACHED = 0,
    TL_ST_OPENING,          /* USB device seen, glue opening the FTDI VCP */
    TL_ST_RESETTING,        /* DTR deasserted, waiting dtr_settle_ms before the asserting edge */
    TL_ST_BOOT_WAIT,        /* waiting for calibration dump + menu (or fallback detection) */
    TL_ST_MODE_SET,         /* 'j' sent, waiting for "Output format changed to: ..." */
    TL_ST_PAUSING,          /* 'x' sent while running, waiting for the menu */
    TL_ST_READY_PAUSED,     /* device paused in menu, JSON mode: link ready */
    TL_ST_RUNNING,          /* device streaming JSON samples */
    TL_ST_NEW_TEST_PENDING, /* pausing and/or waiting for "Starting new test..." */
    TL_ST_RESUMING,         /* 'r' sent, waiting for "Resuming measurements..." */
    TL_ST_FAULT,            /* gave up; needs TL_CMD_RECONNECT, a detach, or a device reboot */
} tl_state_t;

typedef enum {
    TL_CMD_NEW_TEST = 0,
    TL_CMD_PAUSE,
    TL_CMD_RESUME,
    TL_CMD_STOP,      /* end the active test (no more samples accepted) and pause the device */
    TL_CMD_RECONNECT, /* re-run the connect sequence (e.g. from FAULT) */
} tl_cmd_t;

typedef enum {
    TL_RESET_DTR = 0, /* pulse DTR (deassert, settle, assert) to reboot the 328P */
    TL_RESET_NONE,    /* never touch DTR; detect state from traffic only */
} tl_reset_mode_t;

typedef enum {
    TL_ACT_OPEN = 0,     /* open the FTDI VCP; report with tl_proto_opened() */
    TL_ACT_SEND,         /* write one byte: act.byte */
    TL_ACT_SET_LINES,    /* set DTR/RTS: act.dtr, act.rts (true = asserted) */
    TL_ACT_START_TIMER,  /* informational: next deadline in act.ms (also via tl_proto_ms_to_deadline) */
    TL_ACT_FLUSH_RX,     /* drop any partial line in the assembler */
} tl_act_type_t;

typedef struct {
    tl_act_type_t type;
    uint8_t byte;
    bool dtr;
    bool rts;
    uint32_t ms;
} tl_act_t;

typedef enum {
    TL_EVT_STATE = 0,      /* state changed: state, prev_state */
    TL_EVT_BOOT_PROGRESS,  /* boot dump progress: boot_pct 0..100 */
    TL_EVT_LINK_READY,     /* device paused, JSON mode, commands accepted */
    TL_EVT_LINK_GONE,      /* gone.reason, gone.test_interrupted */
    TL_EVT_TEST_ACK,       /* "Starting new test..." seen; ack.pre_reset_ts */
    TL_EVT_SAMPLE,         /* sample.* */
    TL_EVT_PAUSED,         /* device paused at user request (or unexpectedly) */
    TL_EVT_RESUMED,        /* device running again */
    TL_EVT_TEST_STOPPED,   /* active test ended by STOP / NEW_TEST */
    TL_EVT_FAULT,          /* fault.reason, fault.in_state */
    TL_EVT_CMD_REJECTED,   /* rejected.cmd, rejected.in_state */
} tl_evt_type_t;

typedef enum {
    TL_GONE_DETACHED = 0,
    TL_GONE_DEVICE_REBOOTED, /* calibration dump seen while linked (brownout, external reset) */
    TL_GONE_FAULT,
    TL_GONE_RECONNECT,
} tl_gone_reason_t;

typedef enum {
    TL_FAULT_NONE = 0,
    TL_FAULT_OPEN_FAILED,
    TL_FAULT_BOOT_TIMEOUT,
    TL_FAULT_MODE_TIMEOUT,
    TL_FAULT_PAUSE_TIMEOUT,
    TL_FAULT_ACK_TIMEOUT,
    TL_FAULT_RESUME_TIMEOUT,
    TL_FAULT_SETUP_LOOP,        /* too many bytes sent without converging */
    TL_FAULT_UNEXPECTED_FORMAT, /* CSV samples while linked in JSON */
    TL_FAULT_STALE_TIMESTAMPS,  /* after TEST_ACK the timestamps never reset */
} tl_fault_t;

typedef struct {
    double timestamp_s;
    double current_kN;
    double peak_kN;
    uint32_t seq;      /* 0-based index within the test (in_test only) */
    bool in_test;      /* accepted into the active test (between TEST_ACK and STOP) */
} tl_sample_t;

typedef struct {
    tl_evt_type_t type;
    tl_state_t state;  /* state after the event */
    union {
        struct { tl_state_t prev; } st;
        struct { uint8_t pct; } boot;
        struct { tl_gone_reason_t reason; bool test_interrupted; } gone;
        struct { double pre_reset_ts; bool pre_reset_valid; } ack;
        tl_sample_t sample;
        struct { tl_fault_t reason; tl_state_t in_state; } fault;
        struct { tl_cmd_t cmd; tl_state_t in_state; } rejected;
    } u;
} tl_evt_t;

#define TL_OUT_MAX_ACTS 8
#define TL_OUT_MAX_EVTS 8

typedef struct {
    tl_act_t acts[TL_OUT_MAX_ACTS];
    uint8_t n_acts;
    tl_evt_t evts[TL_OUT_MAX_EVTS];
    uint8_t n_evts;
    uint8_t overflow; /* entries dropped because the arrays were full (bug if nonzero) */
} tl_out_t;

typedef struct {
    tl_reset_mode_t reset_mode;
    bool rts_asserted;          /* RTS level kept during/after reset (PC-like default: true) */
    uint32_t open_timeout_ms;   /* OPENING without tl_proto_opened() */
    uint32_t dtr_settle_ms;     /* DTR held deasserted before the asserting (reset) edge */
    uint32_t reset_grace_ms;    /* lines before this after the edge may be pre-reset leftovers */
    uint32_t reset_detect_ms;   /* no calibration row by then => DTR did not reset */
    uint32_t boot_timeout_ms;   /* edge -> menu (plan: 12 s) */
    uint32_t menu_settle_ms;    /* header seen but "j) ..." line lost */
    uint32_t mode_timeout_ms;
    uint32_t pause_timeout_ms;
    uint32_t ack_timeout_ms;
    uint32_t resume_timeout_ms;
    uint8_t max_retries;        /* per wait (plan: 1) */
    uint8_t max_resets;         /* re-resets on boot timeout (plan: 1) */
    uint8_t max_setup_sends;    /* bytes per connect sequence before SETUP_LOOP */
    uint8_t max_first_drops;    /* stale samples after ack before STALE_TIMESTAMPS */
    double first_sample_max_s;  /* first accepted sample after ack must be below (plan: 2.0) */
    double max_step_s;          /* max timestamp step inside a test (except right after resume) */
    double peak_reset_eps;      /* first sample: |peak - max(current,0)| must be <= this */
} tl_proto_cfg_t;

typedef struct {
    uint32_t lines;
    uint32_t unknown_lines;
    uint32_t bad_samples;
    uint32_t discarded_samples;   /* outside a test window / during setup / NEW_TEST_PENDING */
    uint32_t stale_dropped;       /* failed the first-sample-after-ack checks */
    uint32_t order_dropped;       /* non-monotonic or jumping timestamps inside a test */
    uint32_t accepted;            /* in_test samples emitted */
    uint32_t retries;
    uint32_t resets_sent;
    uint32_t resets_observed;     /* calibration dump seen after our DTR edge */
    uint32_t resets_not_observed; /* DTR edge without a reboot (fallback used) */
    uint32_t reboots_unexpected;
} tl_proto_stats_t;

typedef enum { TL_TRI_UNKNOWN = 0, TL_TRI_NO, TL_TRI_YES } tl_tri_t;

typedef struct {
    tl_proto_cfg_t cfg;
    tl_state_t state;
    uint32_t now;

    bool timer_on;
    uint8_t timer_kind;
    uint32_t deadline;

    uint8_t retries;
    uint8_t setup_sends;
    uint8_t resets;
    uint8_t open_attempts;
    bool pausing_user;       /* PAUSING on behalf of the user (not connect) */
    uint8_t ntp_phase;       /* NEW_TEST_PENDING: 0 wait menu, 1 wait ack */
    bool ntp_saw_sample;
    bool stop_requested;

    /* device model, inferred from traffic */
    tl_tri_t dev_paused;
    tl_fmt_t dev_fmt;
    uint8_t menu_open;       /* menu tracking: none / open (not yet acted on) / done */
    tl_fmt_t menu_fmt;
    double menu_peak;

    /* boot */
    uint32_t reset_at;
    bool rows_seen;
    uint8_t boot_pct;
    tl_tri_t reset_observed;

    bool link_ready;

    /* test window (Section 5.3) */
    bool test_active;
    bool awaiting_first;
    bool allow_jump;
    double last_seen_ts;     /* last JSON timestamp seen from the device, any state */
    bool last_seen_valid;
    double pre_reset_ts;
    bool pre_reset_valid;
    double last_acc_ts;
    uint8_t first_drops;
    uint32_t seq;

    tl_proto_stats_t stats;
} tl_proto_t;

void tl_proto_cfg_default(tl_proto_cfg_t *cfg);

/* cfg may be NULL for defaults. */
void tl_proto_init(tl_proto_t *p, const tl_proto_cfg_t *cfg);

/* USB device with the expected VID/PID appeared. */
void tl_proto_attach(tl_proto_t *p, uint32_t now_ms, tl_out_t *out);
/* Result of TL_ACT_OPEN. */
void tl_proto_opened(tl_proto_t *p, uint32_t now_ms, bool ok, tl_out_t *out);
/* USB device gone (CDC_ACM_HOST_DEVICE_DISCONNECTED). */
void tl_proto_detach(tl_proto_t *p, uint32_t now_ms, tl_out_t *out);
/* One assembled line (from tl_line_put). */
void tl_proto_line(tl_proto_t *p, uint32_t now_ms, const char *line, size_t len, bool truncated,
                   tl_out_t *out);
/* Same, with an already classified line. */
void tl_proto_line_info(tl_proto_t *p, uint32_t now_ms, const tl_line_info_t *li, tl_out_t *out);
/* Call whenever the deadline may have passed (cheap if not). */
void tl_proto_tick(tl_proto_t *p, uint32_t now_ms, tl_out_t *out);
/* User command. Returns 0 if accepted, -1 if rejected (TL_EVT_CMD_REJECTED also emitted). */
int tl_proto_cmd(tl_proto_t *p, uint32_t now_ms, tl_cmd_t cmd, tl_out_t *out);

/* ms until the pending deadline (0 if already due), or -1 if no timer. */
int32_t tl_proto_ms_to_deadline(const tl_proto_t *p, uint32_t now_ms);

static inline tl_state_t tl_proto_state(const tl_proto_t *p) { return p->state; }

const char *tl_state_name(tl_state_t s);
const char *tl_fault_name(tl_fault_t f);
const char *tl_evt_name(tl_evt_type_t e);

#ifdef __cplusplus
}
#endif

#endif /* TL_PROTO_H */
