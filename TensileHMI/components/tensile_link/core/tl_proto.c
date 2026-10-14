/* tensile_link core: connection / protocol state machine (see tl_proto.h). */
#include "tl_proto.h"

#include <string.h>

enum { TK_STATE = 0, TK_MENU, TK_DTR };
enum { MENU_NONE = 0, MENU_OPEN, MENU_DONE };
enum { NTP_WAIT_MENU = 0, NTP_WAIT_ACK = 1 };

#define BOOT_PCT_NONE 255u

/* ------------------------------------------------------------------------ */
/* config / init                                                            */

void tl_proto_cfg_default(tl_proto_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->reset_mode = TL_RESET_DTR;
    c->rts_asserted = true;
    c->open_timeout_ms = 5000;
    c->dtr_settle_ms = 50;
    c->reset_grace_ms = 500;
    c->reset_detect_ms = 3000;
    c->boot_timeout_ms = 12000;
    c->menu_settle_ms = 400;
    c->mode_timeout_ms = 1500;
    c->pause_timeout_ms = 2500;
    c->ack_timeout_ms = 2000;
    c->resume_timeout_ms = 2000;
    c->max_retries = 1;
    c->max_resets = 1;
    c->max_setup_sends = 12;
    c->max_first_drops = 6;
    c->first_sample_max_s = 2.0;
    c->max_step_s = 3.0;
    c->peak_reset_eps = 0.0015;
}

static void reset_device_model(tl_proto_t *p)
{
    p->dev_paused = TL_TRI_UNKNOWN;
    p->dev_fmt = TL_FMT_UNKNOWN;
    p->menu_open = MENU_NONE;
    p->menu_fmt = TL_FMT_UNKNOWN;
    p->rows_seen = false;
    p->boot_pct = BOOT_PCT_NONE;
}

static void reset_session(tl_proto_t *p)
{
    p->timer_on = false;
    p->retries = 0;
    p->setup_sends = 0;
    p->resets = 0;
    p->open_attempts = 0;
    p->pausing_user = false;
    p->ntp_phase = NTP_WAIT_MENU;
    p->ntp_saw_sample = false;
    p->stop_requested = false;
    reset_device_model(p);
    p->reset_observed = TL_TRI_UNKNOWN;
    p->link_ready = false;
    p->test_active = false;
    p->awaiting_first = false;
    p->allow_jump = false;
    p->last_seen_valid = false;
    p->pre_reset_valid = false;
    p->first_drops = 0;
    p->seq = 0;
}

void tl_proto_init(tl_proto_t *p, const tl_proto_cfg_t *cfg)
{
    memset(p, 0, sizeof(*p));
    if (cfg) {
        p->cfg = *cfg;
    } else {
        tl_proto_cfg_default(&p->cfg);
    }
    p->state = TL_ST_DETACHED;
    reset_session(p);
}

/* ------------------------------------------------------------------------ */
/* output helpers                                                           */

static void begin(tl_proto_t *p, uint32_t now, tl_out_t *out)
{
    p->now = now;
    out->n_acts = 0;
    out->n_evts = 0;
    out->overflow = 0;
}

static tl_act_t *push_act(tl_out_t *out, tl_act_type_t t)
{
    if (out->n_acts >= TL_OUT_MAX_ACTS) {
        out->overflow++;
        return NULL;
    }
    tl_act_t *a = &out->acts[out->n_acts++];
    memset(a, 0, sizeof(*a));
    a->type = t;
    return a;
}

static tl_evt_t *push_evt(tl_proto_t *p, tl_out_t *out, tl_evt_type_t t)
{
    if (out->n_evts >= TL_OUT_MAX_EVTS) {
        out->overflow++;
        return NULL;
    }
    tl_evt_t *e = &out->evts[out->n_evts++];
    memset(e, 0, sizeof(*e));
    e->type = t;
    e->state = p->state;
    return e;
}

static void set_state(tl_proto_t *p, tl_state_t s, tl_out_t *out)
{
    if (p->state == s) {
        return;
    }
    tl_state_t prev = p->state;
    p->state = s;
    tl_evt_t *e = push_evt(p, out, TL_EVT_STATE);
    if (e) {
        e->u.st.prev = prev;
    }
}

static void timer_start(tl_proto_t *p, uint32_t ms, uint8_t kind, tl_out_t *out)
{
    p->timer_on = true;
    p->timer_kind = kind;
    p->deadline = p->now + ms;
    tl_act_t *a = push_act(out, TL_ACT_START_TIMER);
    if (a) {
        a->ms = ms;
    }
}

static void timer_stop(tl_proto_t *p)
{
    p->timer_on = false;
}

static void send_byte(tl_proto_t *p, uint8_t c, tl_out_t *out)
{
    (void)p;
    tl_act_t *a = push_act(out, TL_ACT_SEND);
    if (a) {
        a->byte = c;
    }
}

static void set_lines(tl_proto_t *p, bool dtr, tl_out_t *out)
{
    tl_act_t *a = push_act(out, TL_ACT_SET_LINES);
    if (a) {
        a->dtr = dtr;
        a->rts = p->cfg.rts_asserted;
    }
}

static void emit_simple(tl_proto_t *p, tl_out_t *out, tl_evt_type_t t)
{
    (void)push_evt(p, out, t);
}

/* Drop LINK_READY (if it was announced) and end any test. */
static void link_lost(tl_proto_t *p, tl_gone_reason_t reason, tl_out_t *out)
{
    if (p->link_ready || p->test_active) {
        tl_evt_t *e = push_evt(p, out, TL_EVT_LINK_GONE);
        if (e) {
            e->u.gone.reason = reason;
            e->u.gone.test_interrupted = p->test_active;
        }
    }
    p->link_ready = false;
    p->test_active = false;
    p->awaiting_first = false;
    p->allow_jump = false;
    p->stop_requested = false;
}

static void fault(tl_proto_t *p, tl_fault_t reason, tl_out_t *out)
{
    tl_state_t in = p->state;
    timer_stop(p);
    link_lost(p, TL_GONE_FAULT, out);
    set_state(p, TL_ST_FAULT, out);
    tl_evt_t *e = push_evt(p, out, TL_EVT_FAULT);
    if (e) {
        e->u.fault.reason = reason;
        e->u.fault.in_state = in;
    }
}

static void end_test(tl_proto_t *p, tl_out_t *out)
{
    if (p->test_active) {
        p->test_active = false;
        p->awaiting_first = false;
        p->allow_jump = false;
        emit_simple(p, out, TL_EVT_TEST_STOPPED);
    }
}

/* ------------------------------------------------------------------------ */
/* connect sequence                                                         */

static bool grace_passed(const tl_proto_t *p)
{
    if (p->cfg.reset_mode == TL_RESET_NONE || p->resets == 0) {
        return true;
    }
    return (uint32_t)(p->now - p->reset_at) >= p->cfg.reset_grace_ms;
}

/* BOOT_WAIT lines other than calibration rows are ignored until the grace
 * period after the DTR edge has passed (pre-reset leftovers in flight). */
static bool boot_wait_ignoring(const tl_proto_t *p)
{
    return p->state == TL_ST_BOOT_WAIT && !p->rows_seen && !grace_passed(p);
}

static void enter_boot_wait(tl_proto_t *p, tl_out_t *out)
{
    p->reset_at = p->now;
    p->rows_seen = false;
    p->boot_pct = BOOT_PCT_NONE;
    set_state(p, TL_ST_BOOT_WAIT, out);
    timer_start(p, p->cfg.reset_detect_ms, TK_STATE, out);
}

static void start_reset(tl_proto_t *p, tl_out_t *out)
{
    p->dev_paused = TL_TRI_UNKNOWN;
    p->dev_fmt = TL_FMT_UNKNOWN;
    p->menu_open = MENU_NONE;
    if (p->cfg.reset_mode == TL_RESET_DTR) {
        set_lines(p, false, out); /* deassert so the next assert is a clean edge */
        set_state(p, TL_ST_RESETTING, out);
        timer_start(p, p->cfg.dtr_settle_ms, TK_DTR, out);
    } else {
        enter_boot_wait(p, out);
    }
}

static void begin_setup(tl_proto_t *p, tl_out_t *out)
{
    p->setup_sends = 0;
    p->resets = 0;
    p->retries = 0;
    p->pausing_user = false;
    p->stop_requested = false;
    reset_device_model(p);
    p->reset_observed = TL_TRI_UNKNOWN;
    start_reset(p, out);
}

static bool setup_send(tl_proto_t *p, uint8_t c, tl_out_t *out)
{
    if (++p->setup_sends > p->cfg.max_setup_sends) {
        fault(p, TL_FAULT_SETUP_LOOP, out);
        return false;
    }
    send_byte(p, c, out);
    return true;
}

static void go_ready(tl_proto_t *p, tl_out_t *out)
{
    timer_stop(p);
    p->setup_sends = 0;
    p->retries = 0;
    set_state(p, TL_ST_READY_PAUSED, out);
    if (!p->link_ready) {
        p->link_ready = true;
        emit_simple(p, out, TL_EVT_LINK_READY);
    }
}

static void go_pausing(tl_proto_t *p, bool user, tl_out_t *out)
{
    p->pausing_user = user;
    p->retries = 0;
    set_state(p, TL_ST_PAUSING, out);
    timer_start(p, p->cfg.pause_timeout_ms, TK_STATE, out);
}

/* Goal: paused + JSON. Decide the next byte from the device model. */
static void setup_advance(tl_proto_t *p, tl_out_t *out)
{
    if (p->dev_paused == TL_TRI_YES) {
        if (p->dev_fmt == TL_FMT_JSON) {
            go_ready(p, out);
            return;
        }
        if (!setup_send(p, 'j', out)) {
            return;
        }
        p->retries = 0;
        set_state(p, TL_ST_MODE_SET, out);
        timer_start(p, p->cfg.mode_timeout_ms, TK_STATE, out);
        return;
    }
    if (!setup_send(p, 'x', out)) {
        return;
    }
    go_pausing(p, false, out);
}

/* Device started running on its own (stray byte consumed) while we wanted it paused. */
static void repause(tl_proto_t *p, tl_out_t *out)
{
    if (p->pausing_user) {
        send_byte(p, 'x', out);
        timer_start(p, p->cfg.pause_timeout_ms, TK_STATE, out);
    } else if (setup_send(p, 'x', out)) {
        timer_start(p, p->cfg.pause_timeout_ms, TK_STATE, out);
    }
}

/* ------------------------------------------------------------------------ */
/* sample acceptance (Section 5.3)                                          */

static void emit_sample(tl_proto_t *p, const tl_line_info_t *li, bool in_test, tl_out_t *out)
{
    tl_evt_t *e = push_evt(p, out, TL_EVT_SAMPLE);
    if (!e) {
        return;
    }
    e->u.sample.timestamp_s = li->timestamp_s;
    e->u.sample.current_kN = li->current_kN;
    e->u.sample.peak_kN = li->peak_kN;
    e->u.sample.in_test = in_test;
    e->u.sample.seq = in_test ? p->seq : 0;
}

/*
 * Rules, applied only while a test is active (TEST_ACK .. STOP):
 *  - first sample after the ack: timestamp < first_sample_max_s (2.0 s) and
 *    peak == max(current, 0) (the firmware zeroes max_reading on 'x'). The
 *    plan's "timestamp >= last pre-reset timestamp is dropped" rule is implied
 *    by the 2.0 s bound whenever the previous test ran past 2 s; for a shorter
 *    previous test it would reject legitimate samples, so the peak-reset check
 *    carries that case.
 *  - later samples: strictly increasing timestamps with a step of at most
 *    max_step_s (unless the test was just resumed). A stale pre-reset sample
 *    (e.g. 192.821 after 5.300) fails this.
 * Samples outside a test are emitted with in_test=false for live display only.
 */
static void accept_sample(tl_proto_t *p, const tl_line_info_t *li, tl_out_t *out)
{
    if (!p->test_active) {
        emit_sample(p, li, false, out);
        return;
    }
    double ts = li->timestamp_s;
    if (p->awaiting_first) {
        double floor0 = li->current_kN > 0.0 ? li->current_kN : 0.0;
        double d = li->peak_kN - floor0;
        if (d < 0.0) {
            d = -d;
        }
        bool ok = ts < p->cfg.first_sample_max_s && d <= p->cfg.peak_reset_eps;
        if (ok && p->pre_reset_valid && p->pre_reset_ts >= p->cfg.first_sample_max_s &&
                ts >= p->pre_reset_ts) {
            ok = false; /* explicit form of the plan rule (implied by the bound above) */
        }
        if (!ok) {
            p->stats.stale_dropped++;
            if (++p->first_drops >= p->cfg.max_first_drops) {
                fault(p, TL_FAULT_STALE_TIMESTAMPS, out);
            }
            return;
        }
        p->awaiting_first = false;
    } else {
        double step = ts - p->last_acc_ts;
        if (!(step > 0.0) || (step > p->cfg.max_step_s && !p->allow_jump)) {
            p->stats.order_dropped++;
            return;
        }
    }
    p->allow_jump = false;
    p->last_acc_ts = ts;
    emit_sample(p, li, true, out);
    p->seq++;
    p->stats.accepted++;
}

/* ------------------------------------------------------------------------ */
/* line handlers                                                            */

static void boot_progress(tl_proto_t *p, uint32_t raw, tl_out_t *out)
{
    uint32_t pct = raw >= TL_FW_CALIB_RAW_LAST ? 100u : raw * 100u / TL_FW_CALIB_RAW_LAST;
    if (p->boot_pct == BOOT_PCT_NONE || pct < p->boot_pct || pct >= (uint32_t)p->boot_pct + 5u ||
            (pct == 100u && p->boot_pct != 100u)) {
        p->boot_pct = (uint8_t)pct;
        tl_evt_t *e = push_evt(p, out, TL_EVT_BOOT_PROGRESS);
        if (e) {
            e->u.boot.pct = (uint8_t)pct;
        }
    }
}

static void on_row(tl_proto_t *p, const tl_line_info_t *li, tl_out_t *out)
{
    switch (p->state) {
    case TL_ST_BOOT_WAIT:
        if (!p->rows_seen) {
            p->rows_seen = true;
            if (p->cfg.reset_mode == TL_RESET_DTR && p->resets > 0 &&
                    p->reset_observed == TL_TRI_UNKNOWN) {
                p->reset_observed = TL_TRI_YES;
                p->stats.resets_observed++;
            }
            timer_start(p, p->cfg.boot_timeout_ms, TK_STATE, out);
        }
        break;
    case TL_ST_RESETTING:
        /* Power-on boot already in progress: that is a known state too. */
        timer_stop(p);
        p->reset_at = p->now;
        p->rows_seen = true;
        p->boot_pct = BOOT_PCT_NONE;
        set_state(p, TL_ST_BOOT_WAIT, out);
        timer_start(p, p->cfg.boot_timeout_ms, TK_STATE, out);
        break;
    case TL_ST_DETACHED:
    case TL_ST_OPENING:
        return;
    default:
        /* MODE_SET, PAUSING, READY_PAUSED, RUNNING, NEW_TEST_PENDING, RESUMING, FAULT:
         * the 328P rebooted underneath us. */
        p->stats.reboots_unexpected++;
        timer_stop(p);
        link_lost(p, TL_GONE_DEVICE_REBOOTED, out);
        p->setup_sends = 0;
        p->retries = 0;
        p->resets = 0;
        p->pausing_user = false;
        reset_device_model(p);
        p->reset_at = p->now;
        p->rows_seen = true;
        set_state(p, TL_ST_BOOT_WAIT, out);
        timer_start(p, p->cfg.boot_timeout_ms, TK_STATE, out);
        break;
    }
    p->dev_paused = TL_TRI_NO;
    p->dev_fmt = TL_FMT_CSV; /* v1.6 always boots in CSV */
    boot_progress(p, li->calib_raw, out);
}

static bool state_reacts_to_menu(const tl_proto_t *p)
{
    switch (p->state) {
    case TL_ST_BOOT_WAIT:
        return !boot_wait_ignoring(p);
    case TL_ST_MODE_SET:
    case TL_ST_PAUSING:
    case TL_ST_NEW_TEST_PENDING:
    case TL_ST_RUNNING:
    case TL_ST_RESUMING:
        return true;
    default:
        return false;
    }
}

static void menu_complete(tl_proto_t *p, tl_out_t *out)
{
    p->menu_open = MENU_DONE;
    p->dev_paused = TL_TRI_YES;
    if (p->menu_fmt != TL_FMT_UNKNOWN) {
        p->dev_fmt = p->menu_fmt;
    }
    timer_stop(p);
    switch (p->state) {
    case TL_ST_BOOT_WAIT:
    case TL_ST_MODE_SET:
        setup_advance(p, out);
        break;
    case TL_ST_PAUSING:
        if (!p->pausing_user) {
            setup_advance(p, out);
        } else if (p->dev_fmt == TL_FMT_CSV) {
            setup_advance(p, out); /* format drifted: restore JSON */
        } else {
            p->pausing_user = false;
            set_state(p, TL_ST_READY_PAUSED, out);
            emit_simple(p, out, TL_EVT_PAUSED);
        }
        break;
    case TL_ST_NEW_TEST_PENDING:
        /* paused: now the 'x' that resets peak and timestamp */
        send_byte(p, 'x', out);
        p->ntp_phase = NTP_WAIT_ACK;
        p->ntp_saw_sample = false;
        timer_start(p, p->cfg.ack_timeout_ms, TK_STATE, out);
        break;
    case TL_ST_RUNNING:
        /* paused without our request */
        if (p->dev_fmt == TL_FMT_CSV) {
            setup_advance(p, out);
        } else {
            set_state(p, TL_ST_READY_PAUSED, out);
            emit_simple(p, out, TL_EVT_PAUSED);
        }
        break;
    case TL_ST_RESUMING:
        /* menu re-printed: our 'r' was not consumed; try again (bounded) */
        if (p->retries < p->cfg.max_retries) {
            p->retries++;
            p->stats.retries++;
            send_byte(p, 'r', out);
            timer_start(p, p->cfg.resume_timeout_ms, TK_STATE, out);
        } else {
            fault(p, TL_FAULT_RESUME_TIMEOUT, out);
        }
        break;
    default:
        break;
    }
}

static void on_menu_line(tl_proto_t *p, const tl_line_info_t *li, tl_out_t *out)
{
    if (boot_wait_ignoring(p)) {
        return;
    }
    bool react = state_reacts_to_menu(p);
    if (li->kind == TL_LINE_MENU_HEADER) {
        p->menu_open = MENU_OPEN;
        p->menu_fmt = TL_FMT_UNKNOWN;
        p->menu_peak = li->value_valid ? li->peak_kN : 0.0;
        p->dev_paused = TL_TRI_YES;
        if (react) {
            timer_start(p, p->cfg.menu_settle_ms, TK_MENU, out);
        }
        return;
    }
    /* MENU_OPTION */
    if (p->menu_open == MENU_NONE) {
        /* header lost: an option line still proves the device is in the menu */
        p->menu_open = MENU_OPEN;
        p->menu_fmt = TL_FMT_UNKNOWN;
        p->dev_paused = TL_TRI_YES;
        if (react) {
            timer_start(p, p->cfg.menu_settle_ms, TK_MENU, out);
        }
    }
    if (p->menu_open != MENU_OPEN) {
        return;
    }
    if (li->option == 'j') {
        p->menu_fmt = li->fmt;
        p->dev_fmt = li->fmt;
    }
    if (li->option == 'j' || li->option == 'c') {
        if (react) {
            menu_complete(p, out);
        } else {
            p->menu_open = MENU_DONE;
        }
    }
}

/* A non-menu line ends any open menu. If the settle timer was pending, keep a
 * state timeout running so the current wait still has a deadline. */
static void menu_interrupt(tl_proto_t *p, tl_out_t *out)
{
    if (p->menu_open == MENU_OPEN && p->timer_on && p->timer_kind == TK_MENU) {
        timer_start(p, p->cfg.pause_timeout_ms, TK_STATE, out);
    }
    p->menu_open = MENU_NONE;
}

static void device_started_running(tl_proto_t *p, tl_out_t *out)
{
    /* Common handling of a "device now running" line outside the expected place. */
    switch (p->state) {
    case TL_ST_BOOT_WAIT:
    case TL_ST_MODE_SET:
        setup_advance(p, out);
        break;
    case TL_ST_PAUSING:
        repause(p, out);
        break;
    case TL_ST_READY_PAUSED:
        set_state(p, TL_ST_RUNNING, out);
        emit_simple(p, out, TL_EVT_RESUMED);
        break;
    case TL_ST_RESUMING:
        timer_stop(p);
        set_state(p, TL_ST_RUNNING, out);
        emit_simple(p, out, TL_EVT_RESUMED);
        break;
    default:
        break;
    }
}

static void stop_if_requested(tl_proto_t *p, tl_out_t *out)
{
    if (p->stop_requested && p->state == TL_ST_RUNNING) {
        p->stop_requested = false;
        send_byte(p, 'x', out);
        go_pausing(p, true, out);
    }
}

static void on_mode_changed(tl_proto_t *p, const tl_line_info_t *li, tl_out_t *out)
{
    p->dev_fmt = li->fmt;
    p->dev_paused = TL_TRI_NO;
    switch (p->state) {
    case TL_ST_MODE_SET:
    case TL_ST_BOOT_WAIT:
        /* JSON or CSV, the device left the menu and is running: pause it.
         * If it said CSV, the menu that follows triggers another 'j'. */
        setup_advance(p, out);
        break;
    case TL_ST_PAUSING:
        break; /* 'x' already on its way */
    case TL_ST_READY_PAUSED:
    case TL_ST_RESUMING:
        device_started_running(p, out);
        break;
    default:
        break;
    }
}

static void on_new_test_ack(tl_proto_t *p, tl_out_t *out)
{
    p->dev_paused = TL_TRI_NO;
    if (p->state != TL_ST_NEW_TEST_PENDING) {
        device_started_running(p, out);
        return;
    }
    timer_stop(p);
    if (p->stop_requested) {
        p->stop_requested = false;
        send_byte(p, 'x', out);
        go_pausing(p, true, out);
        return;
    }
    p->test_active = true;
    p->awaiting_first = true;
    p->allow_jump = false;
    p->first_drops = 0;
    p->seq = 0;
    p->pre_reset_ts = p->last_seen_ts;
    p->pre_reset_valid = p->last_seen_valid;
    p->retries = 0;
    set_state(p, TL_ST_RUNNING, out);
    tl_evt_t *e = push_evt(p, out, TL_EVT_TEST_ACK);
    if (e) {
        e->u.ack.pre_reset_ts = p->pre_reset_ts;
        e->u.ack.pre_reset_valid = p->pre_reset_valid;
    }
}

static void on_resumed(tl_proto_t *p, tl_out_t *out)
{
    p->dev_paused = TL_TRI_NO;
    if (p->state == TL_ST_NEW_TEST_PENDING) {
        return; /* not ours; the ack wait times out and retries */
    }
    device_started_running(p, out);
    stop_if_requested(p, out);
}

static void on_sample(tl_proto_t *p, const tl_line_info_t *li, tl_out_t *out)
{
    bool json = li->kind == TL_LINE_SAMPLE_JSON;
    bool csv = li->kind == TL_LINE_SAMPLE_CSV;
    if (json) {
        p->last_seen_ts = li->timestamp_s;
        p->last_seen_valid = true;
        p->dev_fmt = TL_FMT_JSON;
    } else if (csv) {
        p->dev_fmt = TL_FMT_CSV;
    } else {
        p->stats.bad_samples++;
    }
    p->dev_paused = TL_TRI_NO;

    switch (p->state) {
    case TL_ST_BOOT_WAIT:
        p->stats.discarded_samples++;
        if (!p->rows_seen && p->cfg.reset_mode == TL_RESET_DTR && p->resets > 0 &&
                p->reset_observed == TL_TRI_UNKNOWN) {
            /* still streaming well after the edge: DTR did not reset the 328P */
            p->reset_observed = TL_TRI_NO;
            p->stats.resets_not_observed++;
        }
        setup_advance(p, out);
        break;
    case TL_ST_MODE_SET:
        p->stats.discarded_samples++;
        if (json || csv) {
            setup_advance(p, out); /* mode reply lost; device is running */
        }
        break;
    case TL_ST_PAUSING:
        if (p->pausing_user && p->test_active && json) {
            accept_sample(p, li, out); /* in-flight sample of the current test */
        } else {
            p->stats.discarded_samples++;
        }
        break;
    case TL_ST_READY_PAUSED:
    case TL_ST_RESUMING:
        if (csv) {
            fault(p, TL_FAULT_UNEXPECTED_FORMAT, out);
        } else if (json) {
            device_started_running(p, out);
            if (p->stop_requested) {
                p->stats.discarded_samples++;
                stop_if_requested(p, out);
            } else {
                accept_sample(p, li, out);
            }
        }
        break;
    case TL_ST_RUNNING:
        if (csv) {
            fault(p, TL_FAULT_UNEXPECTED_FORMAT, out);
        } else if (json) {
            accept_sample(p, li, out);
        }
        break;
    case TL_ST_NEW_TEST_PENDING:
        /* D1: anything before "Starting new test..." belongs to the old test */
        p->stats.discarded_samples++;
        if (p->ntp_phase == NTP_WAIT_ACK && (json || csv)) {
            /* device paused and then running without an ack we could read:
             * pause again and redo the 'x' (bounded by max_retries) */
            if (p->retries >= p->cfg.max_retries) {
                fault(p, TL_FAULT_ACK_TIMEOUT, out);
                break;
            }
            p->retries++;
            p->stats.retries++;
            send_byte(p, 'x', out);
            p->ntp_phase = NTP_WAIT_MENU;
            timer_start(p, p->cfg.pause_timeout_ms, TK_STATE, out);
        } else {
            p->ntp_saw_sample = true;
        }
        break;
    default:
        p->stats.discarded_samples++;
        break;
    }
}

void tl_proto_line_info(tl_proto_t *p, uint32_t now_ms, const tl_line_info_t *li, tl_out_t *out)
{
    begin(p, now_ms, out);
    p->stats.lines++;
    if (p->state == TL_ST_DETACHED || p->state == TL_ST_OPENING) {
        return;
    }
    switch (li->kind) {
    case TL_LINE_CALIBRATION_ROW:
        menu_interrupt(p, out);
        on_row(p, li, out);
        return;
    case TL_LINE_MENU_HEADER:
    case TL_LINE_MENU_OPTION:
        on_menu_line(p, li, out);
        return;
    case TL_LINE_UNKNOWN:
    case TL_LINE_OVERFLOW:
        p->stats.unknown_lines++;
        return;
    default:
        break;
    }
    if (p->state == TL_ST_RESETTING || boot_wait_ignoring(p)) {
        if (li->kind == TL_LINE_SAMPLE_JSON) {
            p->last_seen_ts = li->timestamp_s;
            p->last_seen_valid = true;
        }
        p->stats.discarded_samples += (li->kind == TL_LINE_SAMPLE_JSON ||
                                       li->kind == TL_LINE_SAMPLE_CSV);
        return;
    }
    menu_interrupt(p, out);
    switch (li->kind) {
    case TL_LINE_MODE_CHANGED:
        on_mode_changed(p, li, out);
        break;
    case TL_LINE_NEW_TEST_ACK:
        on_new_test_ack(p, out);
        break;
    case TL_LINE_RESUMED:
    case TL_LINE_CALIB_NOT_IMPL:
        on_resumed(p, out);
        break;
    case TL_LINE_SAMPLE_JSON:
    case TL_LINE_SAMPLE_CSV:
    case TL_LINE_SAMPLE_BAD:
        on_sample(p, li, out);
        break;
    default:
        break;
    }
}

void tl_proto_line(tl_proto_t *p, uint32_t now_ms, const char *line, size_t len, bool truncated,
                   tl_out_t *out)
{
    tl_line_info_t li;
    tl_classify(line, len, truncated, &li);
    tl_proto_line_info(p, now_ms, &li, out);
}

/* ------------------------------------------------------------------------ */
/* USB lifecycle                                                            */

void tl_proto_attach(tl_proto_t *p, uint32_t now_ms, tl_out_t *out)
{
    begin(p, now_ms, out);
    if (p->state != TL_ST_DETACHED) {
        return;
    }
    reset_session(p);
    set_state(p, TL_ST_OPENING, out);
    (void)push_act(out, TL_ACT_OPEN);
    timer_start(p, p->cfg.open_timeout_ms, TK_STATE, out);
}

void tl_proto_opened(tl_proto_t *p, uint32_t now_ms, bool ok, tl_out_t *out)
{
    begin(p, now_ms, out);
    if (p->state != TL_ST_OPENING) {
        return;
    }
    timer_stop(p);
    if (ok) {
        (void)push_act(out, TL_ACT_FLUSH_RX);
        begin_setup(p, out);
        return;
    }
    if (++p->open_attempts <= p->cfg.max_retries) {
        p->stats.retries++;
        (void)push_act(out, TL_ACT_OPEN);
        timer_start(p, p->cfg.open_timeout_ms, TK_STATE, out);
    } else {
        fault(p, TL_FAULT_OPEN_FAILED, out);
    }
}

void tl_proto_detach(tl_proto_t *p, uint32_t now_ms, tl_out_t *out)
{
    begin(p, now_ms, out);
    if (p->state == TL_ST_DETACHED) {
        return;
    }
    link_lost(p, TL_GONE_DETACHED, out);
    reset_session(p);
    set_state(p, TL_ST_DETACHED, out);
}

/* ------------------------------------------------------------------------ */
/* timers                                                                   */

int32_t tl_proto_ms_to_deadline(const tl_proto_t *p, uint32_t now_ms)
{
    if (!p->timer_on) {
        return -1;
    }
    int32_t d = (int32_t)(p->deadline - now_ms);
    return d < 0 ? 0 : d;
}

static void retry_or_fault(tl_proto_t *p, uint8_t byte, uint32_t timeout, bool counted_setup,
                           tl_fault_t reason, tl_out_t *out)
{
    if (p->retries >= p->cfg.max_retries) {
        fault(p, reason, out);
        return;
    }
    p->retries++;
    p->stats.retries++;
    if (counted_setup) {
        if (!setup_send(p, byte, out)) {
            return;
        }
    } else {
        send_byte(p, byte, out);
    }
    timer_start(p, timeout, TK_STATE, out);
}

void tl_proto_tick(tl_proto_t *p, uint32_t now_ms, tl_out_t *out)
{
    begin(p, now_ms, out);
    if (!p->timer_on || (int32_t)(now_ms - p->deadline) < 0) {
        return;
    }
    p->timer_on = false;

    if (p->timer_kind == TK_MENU) {
        if (p->menu_open == MENU_OPEN) {
            menu_complete(p, out); /* "j) ..." line lost: act with what we know */
        }
        return;
    }
    if (p->timer_kind == TK_DTR) {
        if (p->state == TL_ST_RESETTING) {
            set_lines(p, true, out); /* asserting edge: DTR# falls, 328P resets */
            (void)push_act(out, TL_ACT_FLUSH_RX);
            p->resets++;
            p->stats.resets_sent++;
            enter_boot_wait(p, out);
        }
        return;
    }

    switch (p->state) {
    case TL_ST_OPENING:
        fault(p, TL_FAULT_OPEN_FAILED, out);
        break;
    case TL_ST_BOOT_WAIT:
        if (!p->rows_seen) {
            /* No reboot seen: either DTR does not reset the 328P or there was
             * no edge (TL_RESET_NONE). 'x' works from the menu and from run
             * mode; whatever the reply is, setup_advance converges. */
            if (p->cfg.reset_mode == TL_RESET_DTR && p->resets > 0 &&
                    p->reset_observed == TL_TRI_UNKNOWN) {
                p->reset_observed = TL_TRI_NO;
                p->stats.resets_not_observed++;
            }
            if (setup_send(p, 'x', out)) {
                go_pausing(p, false, out);
            }
        } else if (p->cfg.reset_mode == TL_RESET_DTR && p->resets <= p->cfg.max_resets) {
            p->stats.retries++;
            start_reset(p, out); /* booted but no menu: reset once more */
        } else if (p->cfg.reset_mode == TL_RESET_NONE) {
            p->stats.retries++; /* booted but no menu and no way to reset: try 'x' once */
            if (setup_send(p, 'x', out)) {
                go_pausing(p, false, out);
            }
        } else {
            fault(p, TL_FAULT_BOOT_TIMEOUT, out);
        }
        break;
    case TL_ST_MODE_SET:
        retry_or_fault(p, 'j', p->cfg.mode_timeout_ms, true, TL_FAULT_MODE_TIMEOUT, out);
        break;
    case TL_ST_PAUSING:
        retry_or_fault(p, 'x', p->cfg.pause_timeout_ms, !p->pausing_user, TL_FAULT_PAUSE_TIMEOUT,
                       out);
        break;
    case TL_ST_NEW_TEST_PENDING:
        if (p->retries >= p->cfg.max_retries) {
            fault(p, p->ntp_phase == NTP_WAIT_MENU ? TL_FAULT_PAUSE_TIMEOUT : TL_FAULT_ACK_TIMEOUT,
                  out);
            break;
        }
        p->retries++;
        p->stats.retries++;
        if (p->ntp_phase == NTP_WAIT_ACK && p->ntp_saw_sample) {
            p->ntp_phase = NTP_WAIT_MENU; /* it is running: pause, then redo */
        }
        p->ntp_saw_sample = false;
        send_byte(p, 'x', out);
        timer_start(p, p->ntp_phase == NTP_WAIT_MENU ? p->cfg.pause_timeout_ms : p->cfg.ack_timeout_ms,
                    TK_STATE, out);
        break;
    case TL_ST_RESUMING:
        retry_or_fault(p, 'r', p->cfg.resume_timeout_ms, false, TL_FAULT_RESUME_TIMEOUT, out);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* commands                                                                 */

static int reject(tl_proto_t *p, tl_cmd_t cmd, tl_out_t *out)
{
    tl_evt_t *e = push_evt(p, out, TL_EVT_CMD_REJECTED);
    if (e) {
        e->u.rejected.cmd = cmd;
        e->u.rejected.in_state = p->state;
    }
    return -1;
}

int tl_proto_cmd(tl_proto_t *p, uint32_t now_ms, tl_cmd_t cmd, tl_out_t *out)
{
    begin(p, now_ms, out);
    switch (cmd) {
    case TL_CMD_NEW_TEST:
        if (p->state != TL_ST_READY_PAUSED && p->state != TL_ST_RUNNING) {
            return reject(p, cmd, out);
        }
        end_test(p, out);
        p->stop_requested = false;
        p->retries = 0;
        p->ntp_saw_sample = false;
        send_byte(p, 'x', out);
        if (p->state == TL_ST_RUNNING) {
            p->ntp_phase = NTP_WAIT_MENU; /* first 'x' pauses, menu triggers the second */
            set_state(p, TL_ST_NEW_TEST_PENDING, out);
            timer_start(p, p->cfg.pause_timeout_ms, TK_STATE, out);
        } else {
            p->ntp_phase = NTP_WAIT_ACK;
            set_state(p, TL_ST_NEW_TEST_PENDING, out);
            timer_start(p, p->cfg.ack_timeout_ms, TK_STATE, out);
        }
        return 0;

    case TL_CMD_PAUSE:
        if (p->state == TL_ST_READY_PAUSED) {
            return 0;
        }
        if (p->state != TL_ST_RUNNING) {
            return reject(p, cmd, out);
        }
        send_byte(p, 'x', out);
        go_pausing(p, true, out);
        return 0;

    case TL_CMD_RESUME:
        if (p->state == TL_ST_RUNNING) {
            return 0;
        }
        if (p->state != TL_ST_READY_PAUSED) {
            return reject(p, cmd, out);
        }
        p->retries = 0;
        p->allow_jump = p->test_active; /* timestamps jump by the pause length */
        send_byte(p, 'r', out);
        set_state(p, TL_ST_RESUMING, out);
        timer_start(p, p->cfg.resume_timeout_ms, TK_STATE, out);
        return 0;

    case TL_CMD_STOP:
        if (p->state == TL_ST_DETACHED || p->state == TL_ST_OPENING) {
            return reject(p, cmd, out);
        }
        end_test(p, out);
        if (p->state == TL_ST_RUNNING) {
            send_byte(p, 'x', out);
            go_pausing(p, true, out);
        } else if (p->state == TL_ST_NEW_TEST_PENDING || p->state == TL_ST_RESUMING) {
            p->stop_requested = true; /* pause again once the device is running */
        }
        return 0;

    case TL_CMD_RECONNECT:
        if (p->state == TL_ST_DETACHED || p->state == TL_ST_OPENING) {
            return reject(p, cmd, out);
        }
        timer_stop(p);
        link_lost(p, TL_GONE_RECONNECT, out);
        (void)push_act(out, TL_ACT_FLUSH_RX);
        begin_setup(p, out);
        return 0;
    }
    return reject(p, cmd, out);
}

/* ------------------------------------------------------------------------ */
/* names                                                                    */

const char *tl_state_name(tl_state_t s)
{
    switch (s) {
    case TL_ST_DETACHED: return "DETACHED";
    case TL_ST_OPENING: return "OPENING";
    case TL_ST_RESETTING: return "RESETTING";
    case TL_ST_BOOT_WAIT: return "BOOT_WAIT";
    case TL_ST_MODE_SET: return "MODE_SET";
    case TL_ST_PAUSING: return "PAUSING";
    case TL_ST_READY_PAUSED: return "READY_PAUSED";
    case TL_ST_RUNNING: return "RUNNING";
    case TL_ST_NEW_TEST_PENDING: return "NEW_TEST_PENDING";
    case TL_ST_RESUMING: return "RESUMING";
    case TL_ST_FAULT: return "FAULT";
    }
    return "?";
}

const char *tl_fault_name(tl_fault_t f)
{
    switch (f) {
    case TL_FAULT_NONE: return "NONE";
    case TL_FAULT_OPEN_FAILED: return "OPEN_FAILED";
    case TL_FAULT_BOOT_TIMEOUT: return "BOOT_TIMEOUT";
    case TL_FAULT_MODE_TIMEOUT: return "MODE_TIMEOUT";
    case TL_FAULT_PAUSE_TIMEOUT: return "PAUSE_TIMEOUT";
    case TL_FAULT_ACK_TIMEOUT: return "ACK_TIMEOUT";
    case TL_FAULT_RESUME_TIMEOUT: return "RESUME_TIMEOUT";
    case TL_FAULT_SETUP_LOOP: return "SETUP_LOOP";
    case TL_FAULT_UNEXPECTED_FORMAT: return "UNEXPECTED_FORMAT";
    case TL_FAULT_STALE_TIMESTAMPS: return "STALE_TIMESTAMPS";
    }
    return "?";
}

const char *tl_evt_name(tl_evt_type_t e)
{
    switch (e) {
    case TL_EVT_STATE: return "STATE";
    case TL_EVT_BOOT_PROGRESS: return "BOOT_PROGRESS";
    case TL_EVT_LINK_READY: return "LINK_READY";
    case TL_EVT_LINK_GONE: return "LINK_GONE";
    case TL_EVT_TEST_ACK: return "TEST_ACK";
    case TL_EVT_SAMPLE: return "SAMPLE";
    case TL_EVT_PAUSED: return "PAUSED";
    case TL_EVT_RESUMED: return "RESUMED";
    case TL_EVT_TEST_STOPPED: return "TEST_STOPPED";
    case TL_EVT_FAULT: return "FAULT";
    case TL_EVT_CMD_REJECTED: return "CMD_REJECTED";
    }
    return "?";
}
