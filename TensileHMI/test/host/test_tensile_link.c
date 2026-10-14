/*
 * Host tests for tensile_link core (line assembler, classifier, protocol state machine).
 *
 * Two kinds of transcripts:
 *  - scripted: exact lines from src/main.cpp (v1.6) fed at chosen times;
 *  - closed loop: a small simulator of the v1.6 firmware (boot dump, 5 s delay,
 *    modal menu, 500 ms measurement loop, serial RX buffering, DTR reset) wired
 *    to the state machine with USB latency, so commands and in-flight lines race
 *    the way they do on the bench.
 *
 * Every firmware string below is typed out literally (copied from src/main.cpp),
 * not taken from the parser's TL_FW_* macros, so a typo in either is caught.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "tl_line.h"
#include "tl_parse.h"
#include "tl_proto.h"

/* ======================================================================== */
/* firmware strings (src/main.cpp v1.6)                                     */

#define FW_SEP      "--------"
#define FW_OPT_R    "r) Resume measurement"
#define FW_OPT_X    "x) Start new test (reset peak and timestamp)"
#define FW_OPT_C    "c) Enter calibration mode (future feature)"
#define FW_NEWTEST  "Starting new test..."
#define FW_RESUMING "Resuming measurements..."
#define FW_CALIB    "Calibration mode not yet implemented."

/* ======================================================================== */
/* firmware simulator                                                       */

typedef enum { D_OFF, D_BOOTLOADER, D_DUMP, D_DELAY, D_MENU, D_RUN } dmode_t;

typedef struct {
    dmode_t mode;
    uint32_t t_mode;
    bool json;
    double maxr;
    uint32_t test_start;
    uint32_t raw;
    uint32_t meas_start;
    bool printed;
    uint8_t rx[64];
    int rx_n;
    bool dtr_resets;   /* bench unknown: does a DTR assert reset the 328P? */
    bool dtr;
    bool deaf;         /* ignores all input (stuck firmware) */
    int garble_j_line; /* corrupt the next N "j) ..." menu lines */
    double force;      /* constant reading, kN */
    double ramp;       /* kN per second since test start */
    uint32_t prints;   /* sample lines printed */
    char last_sample[96];
} dsim_t;

#define SIM_MAXQ 1024
#define SIM_MAXEV 8192

typedef struct {
    uint32_t at;
    char line[160];
} qline_t;

typedef struct {
    tl_proto_t p;
    tl_line_asm_t la;
    uint32_t now;
    uint32_t latency;   /* device -> host line latency, ms (FTDI latency timer) */
    dsim_t d;
    qline_t q[SIM_MAXQ];
    int qh, qt;
    char sent[512];
    int n_sent;
    int n_set_lines;
    bool last_dtr;
    int n_open;
    bool open_pending;
    bool open_ok;
    int n_flush;
    tl_evt_t ev[SIM_MAXEV];
    int n_ev;
    int overflow;
    uint32_t lines_delivered;
} sim_t;

static sim_t S; /* large: static */

static void dev_print(sim_t *s, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int next = (s->qt + 1) % SIM_MAXQ;
    if (next == s->qh) {
        return; /* host not reading: drop (FTDI would too) */
    }
    s->q[s->qt].at = s->now + s->latency;
    snprintf(s->q[s->qt].line, sizeof(s->q[s->qt].line), "%s", buf);
    s->qt = next;
}

static void dev_menu(sim_t *s)
{
    dsim_t *d = &s->d;
    dev_print(s, "Measurement Paused. Peak: %.2f kN", d->maxr);
    dev_print(s, "");
    dev_print(s, FW_SEP);
    dev_print(s, FW_OPT_R);
    dev_print(s, FW_OPT_X);
    if (d->garble_j_line > 0) {
        d->garble_j_line--;
        dev_print(s, "j) Toggle out\xff\xfe format (cu");
    } else {
        dev_print(s, "j) Toggle output format (current: %s)", d->json ? "JSON" : "CSV");
    }
    dev_print(s, FW_OPT_C);
    d->mode = D_MENU;
    d->t_mode = s->now;
}

static void dev_run(sim_t *s)
{
    s->d.mode = D_RUN;
    s->d.meas_start = s->now;
    s->d.printed = false;
}

static int dev_pop(dsim_t *d)
{
    if (d->rx_n == 0) {
        return -1;
    }
    int c = d->rx[0];
    memmove(d->rx, d->rx + 1, (size_t)--d->rx_n);
    return c;
}

static void dev_reset(sim_t *s, bool power_on)
{
    dsim_t *d = &s->d;
    d->json = false;
    d->maxr = 0;
    d->rx_n = 0;
    d->raw = 0;
    d->mode = power_on ? D_DUMP : D_BOOTLOADER; /* optiboot only waits on external reset */
    d->t_mode = s->now;
}

static void dev_step(sim_t *s)
{
    dsim_t *d = &s->d;
    switch (d->mode) {
    case D_OFF:
        d->rx_n = 0;
        break;
    case D_BOOTLOADER:
        if (s->now - d->t_mode >= 500) {
            d->rx_n = 0;
            d->mode = D_DUMP;
            d->raw = 0;
        }
        break;
    case D_DUMP: /* ~1 row per ms at 115200 */
        dev_print(s, "%u\t%.2f", (unsigned)d->raw, (double)d->raw * 0.00117 - 0.15);
        d->raw += 20;
        if (d->raw > 7000) {
            d->mode = D_DELAY;
            d->t_mode = s->now;
        }
        break;
    case D_DELAY:
        if (s->now - d->t_mode >= 5000) {
            d->test_start = s->now;
            dev_menu(s);
        }
        break;
    case D_MENU: {
        if (d->deaf) {
            d->rx_n = 0;
            break;
        }
        int c = dev_pop(d);
        if (c == 'x') {
            d->maxr = 0;
            d->test_start = s->now;
            dev_print(s, FW_NEWTEST);
            dev_run(s);
        } else if (c == 'j') {
            d->json = !d->json;
            dev_print(s, "Output format changed to: %s", d->json ? "JSON" : "CSV");
            dev_run(s);
        } else if (c == 'c') {
            dev_print(s, FW_CALIB);
            dev_run(s);
        } else if (c == 'r') {
            dev_print(s, FW_RESUMING);
            dev_run(s);
        }
        break;
    }
    case D_RUN:
        if (!d->printed && s->now - d->meas_start >= 450) { /* get_units(5) at 10 SPS */
            double t = (s->now - d->test_start) / 1000.0;
            double cur = d->force + d->ramp * t;
            if (cur >= d->maxr) {
                d->maxr = cur;
            }
            if (d->json) {
                snprintf(d->last_sample, sizeof(d->last_sample),
                         "{\"timestamp\":%.3f,\"current\":%.3f,\"peak\":%.3f}", t, cur, d->maxr);
            } else {
                snprintf(d->last_sample, sizeof(d->last_sample), "%.2f,%.2f", cur, d->maxr);
            }
            dev_print(s, "%s", d->last_sample);
            d->prints++;
            d->printed = true;
        }
        if (d->printed) {
            int c;
            while (!d->deaf && (c = dev_pop(d)) >= 0) {
                if (c == 'x') {
                    dev_menu(s);
                    return;
                }
            }
        }
        if (s->now - d->meas_start >= 500) {
            d->meas_start = s->now;
            d->printed = false;
        }
        break;
    }
}

static void dev_set_dtr(sim_t *s, bool dtr)
{
    bool edge = dtr && !s->d.dtr; /* assert: DTR# falls, cap pulls RESET low */
    s->d.dtr = dtr;
    if (edge && s->d.dtr_resets && s->d.mode != D_OFF) {
        dev_reset(s, false);
    }
}

/* ======================================================================== */
/* host side                                                                */

static void apply(sim_t *s, const tl_out_t *out)
{
    s->overflow += out->overflow;
    for (int i = 0; i < out->n_acts; i++) {
        const tl_act_t *a = &out->acts[i];
        switch (a->type) {
        case TL_ACT_OPEN:
            s->n_open++;
            s->open_pending = true;
            break;
        case TL_ACT_SEND:
            if (s->n_sent < (int)sizeof(s->sent) - 1) {
                s->sent[s->n_sent++] = (char)a->byte;
            }
            if (s->d.mode != D_OFF && s->d.rx_n < 63) {
                s->d.rx[s->d.rx_n++] = a->byte;
            }
            break;
        case TL_ACT_SET_LINES:
            s->n_set_lines++;
            s->last_dtr = a->dtr;
            dev_set_dtr(s, a->dtr);
            break;
        case TL_ACT_FLUSH_RX:
            s->n_flush++;
            tl_line_reset(&s->la);
            break;
        case TL_ACT_START_TIMER:
            break;
        }
    }
    for (int i = 0; i < out->n_evts && s->n_ev < SIM_MAXEV; i++) {
        s->ev[s->n_ev++] = out->evts[i];
    }
}

static void host_bytes(sim_t *s, const uint8_t *b, size_t n)
{
    tl_out_t out;
    for (size_t i = 0; i < n; i++) {
        size_t len;
        bool tr;
        const char *line = tl_line_put(&s->la, b[i], &len, &tr);
        if (line) {
            s->lines_delivered++;
            tl_proto_line(&s->p, s->now, line, len, tr, &out);
            apply(s, &out);
        }
    }
}

static void host_line(sim_t *s, const char *line)
{
    host_bytes(s, (const uint8_t *)line, strlen(line));
    host_bytes(s, (const uint8_t *)"\r\n", 2);
}

static void sim_init(sim_t *s, uint32_t t0, const tl_proto_cfg_t *cfg)
{
    memset(s, 0, sizeof(*s));
    tl_proto_init(&s->p, cfg);
    tl_line_init(&s->la);
    s->now = t0;
    s->latency = 16;
    s->open_ok = true;
    s->d.mode = D_OFF;
    s->d.force = 0.022;
    s->d.dtr_resets = true;
}

/* advance one ms: device, deliveries, open completion, timers */
static void sim_step(sim_t *s)
{
    tl_out_t out;
    s->now++;
    dev_step(s);
    while (s->qh != s->qt && (int32_t)(s->now - s->q[s->qh].at) >= 0) {
        char line[160];
        snprintf(line, sizeof(line), "%s", s->q[s->qh].line);
        s->qh = (s->qh + 1) % SIM_MAXQ;
        host_line(s, line);
    }
    if (s->open_pending) {
        s->open_pending = false;
        tl_proto_opened(&s->p, s->now, s->open_ok, &out);
        apply(s, &out);
    }
    tl_proto_tick(&s->p, s->now, &out);
    apply(s, &out);
}

static void sim_run(sim_t *s, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        sim_step(s);
    }
}

/* run until predicate or limit; returns ms elapsed or -1 */
static int sim_run_until_state(sim_t *s, tl_state_t st, uint32_t limit)
{
    for (uint32_t i = 0; i < limit; i++) {
        if (tl_proto_state(&s->p) == st) {
            return (int)i;
        }
        sim_step(s);
    }
    return tl_proto_state(&s->p) == st ? (int)limit : -1;
}

static void sim_attach(sim_t *s)
{
    tl_out_t out;
    tl_proto_attach(&s->p, s->now, &out);
    apply(s, &out);
}

static void sim_detach(sim_t *s)
{
    tl_out_t out;
    tl_proto_detach(&s->p, s->now, &out);
    apply(s, &out);
    s->d.mode = D_OFF;
    s->qh = s->qt = 0;
}

static int sim_cmd(sim_t *s, tl_cmd_t c)
{
    tl_out_t out;
    int r = tl_proto_cmd(&s->p, s->now, c, &out);
    apply(s, &out);
    return r;
}

static int count_ev(const sim_t *s, tl_evt_type_t t, int from)
{
    int n = 0;
    for (int i = from; i < s->n_ev; i++) {
        n += s->ev[i].type == t;
    }
    return n;
}

static const tl_evt_t *last_ev(const sim_t *s, tl_evt_type_t t)
{
    for (int i = s->n_ev - 1; i >= 0; i--) {
        if (s->ev[i].type == t) {
            return &s->ev[i];
        }
    }
    return NULL;
}

static const tl_evt_t *first_ev(const sim_t *s, tl_evt_type_t t, int from)
{
    for (int i = from; i < s->n_ev; i++) {
        if (s->ev[i].type == t) {
            return &s->ev[i];
        }
    }
    return NULL;
}

static int count_in_test(const sim_t *s, int from)
{
    int n = 0;
    for (int i = from; i < s->n_ev; i++) {
        n += s->ev[i].type == TL_EVT_SAMPLE && s->ev[i].u.sample.in_test;
    }
    return n;
}

static void sent_reset(sim_t *s)
{
    s->n_sent = 0;
    memset(s->sent, 0, sizeof(s->sent));
}

static void dump_events(const sim_t *s, int from)
{
    for (int i = from; i < s->n_ev; i++) {
        const tl_evt_t *e = &s->ev[i];
        if (e->type == TL_EVT_SAMPLE || e->type == TL_EVT_BOOT_PROGRESS) {
            continue;
        }
        fprintf(stderr, "   ev %-14s state=%s", tl_evt_name(e->type), tl_state_name(e->state));
        if (e->type == TL_EVT_FAULT) {
            fprintf(stderr, " reason=%s", tl_fault_name(e->u.fault.reason));
        }
        fprintf(stderr, "\n");
    }
}

/* TL_VERBOSE=1 ./test_tensile_link prints the bytes sent and the event trace. */
static void verbose(const sim_t *s, const char *what)
{
    if (!getenv("TL_VERBOSE")) {
        return;
    }
    fprintf(stderr, "   [%s] sent=\"%s\" state=%s t=%u lines=%u\n", what, s->sent,
            tl_state_name(tl_proto_state(&s->p)), (unsigned)s->now, (unsigned)s->lines_delivered);
    dump_events(s, 0);
}

/* Bring a fresh sim to READY_PAUSED through the cold-boot path. */
static void sim_to_ready(sim_t *s)
{
    s->d.mode = D_DELAY; /* power-on dump already done, sitting in delay(5000) */
    s->d.t_mode = s->now;
    sim_attach(s);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
}

/* ======================================================================== */
/* line assembler                                                           */

static void test_line_assembler(void)
{
    tl_line_asm_t a;
    tl_line_init(&a);
    const char *in = "abc\r\ndef\nghi\rjkl\r\n\r\n\n\rmno";
    char got[8][TL_LINE_MAX];
    int n = 0;
    for (const char *c = in; *c; c++) {
        size_t len;
        bool tr;
        const char *l = tl_line_put(&a, (uint8_t)*c, &len, &tr);
        if (l) {
            TT_CHECK(!tr);
            TT_EQ_INT(len, strlen(l));
            snprintf(got[n++], TL_LINE_MAX, "%s", l);
        }
    }
    TT_EQ_INT(n, 4);
    TT_EQ_STR(got[0], "abc");
    TT_EQ_STR(got[1], "def");
    TT_EQ_STR(got[2], "ghi");
    TT_EQ_STR(got[3], "jkl");
    /* "mno" is pending; reset drops it */
    tl_line_reset(&a);
    const char *l = NULL;
    for (const char *c = "\n"; *c; c++) {
        l = tl_line_put(&a, (uint8_t)*c, NULL, NULL);
    }
    TT_CHECK(l == NULL);

    /* overflow: truncated, flagged, remainder discarded, next line fine */
    tl_line_init(&a);
    bool tr = false;
    size_t len = 0;
    for (int i = 0; i < 300; i++) {
        l = tl_line_put(&a, 'A', &len, &tr);
        TT_CHECK(l == NULL);
    }
    l = tl_line_put(&a, '\n', &len, &tr);
    TT_CHECK(l != NULL);
    TT_CHECK(tr);
    TT_EQ_INT(len, TL_LINE_MAX - 1);
    TT_EQ_INT(a.overflows, 1);
    const char *ok = "Starting new test...\r\n";
    for (const char *c = ok; *c; c++) {
        l = tl_line_put(&a, (uint8_t)*c, &len, &tr);
        if (l) {
            break;
        }
    }
    TT_CHECK(l && !tr);
    TT_EQ_STR(l, "Starting new test...");

    /* NUL bytes are replaced */
    tl_line_init(&a);
    const uint8_t nul[] = {'a', 0, 'b', '\n'};
    for (size_t i = 0; i < sizeof(nul); i++) {
        l = tl_line_put(&a, nul[i], &len, &tr);
    }
    TT_EQ_STR(l, "a?b");
}

/* The same transcript chunked at every split point gives identical lines. */
static void test_line_chunking(void)
{
    const char *tx =
        "Measurement Paused. Peak: 0.00 kN\r\n\r\n--------\r\nr) Resume measurement\r\n"
        "{\"timestamp\":12.345,\"current\":1.234,\"peak\":5.678}\r\n1.23,5.68\r\n";
    size_t n = strlen(tx);
    char ref[16][TL_LINE_MAX];
    int nref = 0;
    tl_line_asm_t a;
    tl_line_init(&a);
    for (size_t i = 0; i < n; i++) {
        const char *l = tl_line_put(&a, (uint8_t)tx[i], NULL, NULL);
        if (l) {
            snprintf(ref[nref++], TL_LINE_MAX, "%s", l);
        }
    }
    TT_EQ_INT(nref, 5);
    srand(1234);
    int mismatches = 0;
    for (int trial = 0; trial < 200; trial++) {
        tl_line_init(&a);
        int k = 0;
        size_t i = 0;
        while (i < n) {
            size_t chunk = 1 + (size_t)(rand() % 17);
            for (size_t j = 0; j < chunk && i < n; j++, i++) {
                const char *l = tl_line_put(&a, (uint8_t)tx[i], NULL, NULL);
                if (l) {
                    if (k >= nref || strcmp(l, ref[k]) != 0) {
                        mismatches++;
                    }
                    k++;
                }
            }
        }
        if (k != nref) {
            mismatches++;
        }
    }
    TT_EQ_INT(mismatches, 0);
}

/* ======================================================================== */
/* classifier                                                               */

static tl_line_info_t cls(const char *s)
{
    tl_line_info_t li;
    tl_classify(s, strlen(s), false, &li);
    return li;
}

static void test_classifier(void)
{
    tl_line_info_t li;

    li = cls("Measurement Paused. Peak: 3.38 kN");
    TT_EQ_INT(li.kind, TL_LINE_MENU_HEADER);
    TT_CHECK(li.value_valid);
    TT_NEAR(li.peak_kN, 3.38, 1e-9);
    li = cls("Measurement Paused. Peak: -0.01 kN");
    TT_EQ_INT(li.kind, TL_LINE_MENU_HEADER);
    TT_NEAR(li.peak_kN, -0.01, 1e-9);
    li = cls("Measurement Paused. Peak: nan kN");
    TT_EQ_INT(li.kind, TL_LINE_MENU_HEADER);
    TT_CHECK(!li.value_valid);

    li = cls(FW_SEP);
    TT_EQ_INT(li.kind, TL_LINE_MENU_OPTION);
    TT_EQ_INT(li.option, '-');
    li = cls(FW_OPT_R);
    TT_EQ_INT(li.option, 'r');
    li = cls(FW_OPT_X);
    TT_EQ_INT(li.option, 'x');
    li = cls(FW_OPT_C);
    TT_EQ_INT(li.option, 'c');
    li = cls("j) Toggle output format (current: JSON)");
    TT_EQ_INT(li.kind, TL_LINE_MENU_OPTION);
    TT_EQ_INT(li.option, 'j');
    TT_EQ_INT(li.fmt, TL_FMT_JSON);
    li = cls("j) Toggle output format (current: CSV)");
    TT_EQ_INT(li.fmt, TL_FMT_CSV);
    li = cls("j) Toggle output format (current: CS");
    TT_EQ_INT(li.kind, TL_LINE_UNKNOWN);

    li = cls("Output format changed to: JSON");
    TT_EQ_INT(li.kind, TL_LINE_MODE_CHANGED);
    TT_EQ_INT(li.fmt, TL_FMT_JSON);
    li = cls("Output format changed to: CSV");
    TT_EQ_INT(li.kind, TL_LINE_MODE_CHANGED);
    TT_EQ_INT(li.fmt, TL_FMT_CSV);
    TT_EQ_INT(cls(FW_NEWTEST).kind, TL_LINE_NEW_TEST_ACK);
    TT_EQ_INT(cls(FW_RESUMING).kind, TL_LINE_RESUMED);
    TT_EQ_INT(cls(FW_CALIB).kind, TL_LINE_CALIB_NOT_IMPL);
    TT_EQ_INT(cls("Starting new test..").kind, TL_LINE_UNKNOWN);

    li = cls("0\t-0.15");
    TT_EQ_INT(li.kind, TL_LINE_CALIBRATION_ROW);
    TT_EQ_INT(li.calib_raw, 0);
    TT_CHECK(li.value_valid);
    li = cls("7000\t8.04");
    TT_EQ_INT(li.kind, TL_LINE_CALIBRATION_ROW);
    TT_EQ_INT(li.calib_raw, 7000);
    li = cls("6980\tnan");
    TT_EQ_INT(li.kind, TL_LINE_CALIBRATION_ROW);
    TT_CHECK(!li.value_valid);

    li = cls("{\"timestamp\":12.345,\"current\":1.234,\"peak\":5.678}");
    TT_EQ_INT(li.kind, TL_LINE_SAMPLE_JSON);
    TT_NEAR(li.timestamp_s, 12.345, 1e-9);
    TT_NEAR(li.current_kN, 1.234, 1e-9);
    TT_NEAR(li.peak_kN, 5.678, 1e-9);
    li = cls("{\"timestamp\":0.373,\"current\":-0.002,\"peak\":0.000}");
    TT_EQ_INT(li.kind, TL_LINE_SAMPLE_JSON);
    TT_NEAR(li.current_kN, -0.002, 1e-9);

    static const char *const bad_json[] = {
        "{\"timestamp\":nan,\"current\":1.234,\"peak\":5.678}",
        "{\"timestamp\":1.000,\"current\":ovf,\"peak\":5.678}",
        "{\"timestamp\":1.000,\"current\":inf,\"peak\":5.678}",
        "{\"timestamp\":1.000,\"current\":1.0}",
        "{\"timestamp\":1.000,\"current\":1.0,\"peak\":2.0,\"x\":1}",
        "{\"timestamp\":1.000,\"timestamp\":1.0,\"peak\":2.0}",
        "{\"timestamp\":1.000,\"current\":1.0,\"peak\":2.0}x",
        "{\"timestamp\":1e3,\"current\":1.0,\"peak\":2.0}",
        "{\"timestamp\":+1.0,\"current\":1.0,\"peak\":2.0}",
        "{\"timestamp\":-1.000,\"current\":1.0,\"peak\":2.0}",
        "{\"timestamp\":1.,\"current\":1.0,\"peak\":2.0}",
        "{\"timestamp\":,\"current\":1.0,\"peak\":2.0}",
        "{\"timestamp\":1.0,\"current\":1.0,\"peak\":\"2.0\"}",
        "{\"timestamp\":12.3{\"timestamp\":12.8,\"current\":1.2,\"peak\":5.6}",
        "{\"timest",
        "{",
    };
    for (size_t i = 0; i < sizeof(bad_json) / sizeof(bad_json[0]); i++) {
        li = cls(bad_json[i]);
        if (li.kind != TL_LINE_SAMPLE_BAD) {
            fprintf(stderr, "   accepted bad json: %s\n", bad_json[i]);
        }
        TT_EQ_INT(li.kind, TL_LINE_SAMPLE_BAD);
    }

    li = cls("1.23,5.68");
    TT_EQ_INT(li.kind, TL_LINE_SAMPLE_CSV);
    TT_NEAR(li.current_kN, 1.23, 1e-9);
    TT_NEAR(li.peak_kN, 5.68, 1e-9);
    li = cls("-0.01,0.00");
    TT_EQ_INT(li.kind, TL_LINE_SAMPLE_CSV);
    TT_EQ_INT(cls("nan,0.00").kind, TL_LINE_SAMPLE_BAD);
    TT_EQ_INT(cls("1.23,ovf").kind, TL_LINE_SAMPLE_BAD);
    TT_EQ_INT(cls("1.2.3,4").kind, TL_LINE_UNKNOWN);
    TT_EQ_INT(cls("hello, world").kind, TL_LINE_UNKNOWN);
    TT_EQ_INT(cls("\xff\xfe\x80").kind, TL_LINE_UNKNOWN);
    TT_EQ_INT(cls("").kind, TL_LINE_UNKNOWN);

    tl_classify("Starting new test...", 20, true, &li);
    TT_EQ_INT(li.kind, TL_LINE_OVERFLOW);

    double d;
    TT_CHECK(tl_parse_number("192.821", 7, &d));
    TT_NEAR(d, 192.821, 1e-9);
    TT_CHECK(!tl_parse_number("12345678901.0", 13, &d));
    TT_CHECK(!tl_parse_number("-", 1, &d));
    TT_CHECK(!tl_parse_number("1.2345678", 9, &d));
}

/* ======================================================================== */
/* state machine: connect paths                                             */

/* Cold boot: device is in its post-dump delay when the host opens it; the
 * DTR edge reboots it; dump; menu (CSV); 'j' -> JSON (running); 'x' -> menu. */
static void test_cold_boot_dtr_reset(void)
{
    sim_t *s = &S;
    sim_init(s, 1000, NULL);
    s->d.mode = D_DELAY;
    s->d.t_mode = s->now;
    sim_attach(s);
    uint32_t t_attach = s->now;
    int ms = sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_CHECK(ms > 0);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_READY, 0), 1);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
    TT_EQ_STR(s->sent, "jx");
    TT_EQ_INT(s->n_open, 1);
    TT_EQ_INT(s->n_set_lines, 2); /* deassert, then the asserting edge */
    TT_CHECK(s->last_dtr);
    TT_EQ_INT(s->p.reset_observed, TL_TRI_YES);
    TT_EQ_INT(s->p.stats.resets_observed, 1);
    TT_CHECK(s->d.json);
    TT_EQ_INT(s->d.mode, D_MENU);
    /* plan 5.1: ~8 s plug-to-ready with v1.6 firmware */
    TT_CHECK(s->now - t_attach < 8000);
    const tl_evt_t *bp = last_ev(s, TL_EVT_BOOT_PROGRESS);
    TT_CHECK(bp && bp->u.boot.pct == 100);
    TT_CHECK(count_ev(s, TL_EVT_BOOT_PROGRESS, 0) >= 10);
    TT_EQ_INT(s->overflow, 0);
    /* idle in READY: nothing more happens */
    int n = s->n_ev;
    sim_run(s, 10000);
    TT_EQ_INT(s->n_ev, n);
    TT_EQ_STR(s->sent, "jx");
    verbose(s, "test_cold_boot_dtr_reset");
}

/* Power-on: the host opens the port while the power-on dump is streaming.
 * Rows during RESETTING are taken as a known boot; no DTR edge needed. */
static void test_power_on_rows_during_reset(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    dev_reset(s, true);
    sim_run(s, 30);
    sim_attach(s);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_STR(s->sent, "jx");
    TT_EQ_INT(s->n_set_lines, 1); /* only the deassert went out */
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_READY, 0), 1);
}

/* DTR does not reset the 328P; device is streaming CSV. */
static void test_no_reset_running_csv(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    s->d.dtr_resets = false;
    s->d.json = false;
    dev_run(s);
    sim_run(s, 1234);
    sim_attach(s);
    uint32_t t0 = s->now;
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_STR(s->sent, "xjx");
    TT_EQ_INT(s->p.reset_observed, TL_TRI_NO);
    TT_EQ_INT(s->p.stats.resets_not_observed, 1);
    TT_CHECK(s->d.json);
    TT_CHECK(s->now - t0 < 4000); /* no 12 s boot wait */
    TT_EQ_INT(count_ev(s, TL_EVT_SAMPLE, 0), 0); /* nothing leaks during setup */
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
    verbose(s, "test_no_reset_running_csv");
}

/* DTR does not reset; device already streaming JSON (e.g. HMI restarted). */
static void test_no_reset_running_json(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    s->d.dtr_resets = false;
    s->d.json = true;
    dev_run(s);
    sim_run(s, 777);
    sim_attach(s);
    uint32_t t0 = s->now;
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_STR(s->sent, "x");
    TT_CHECK(s->now - t0 < 2500);
    TT_EQ_INT(count_ev(s, TL_EVT_SAMPLE, 0), 0);
}

/* DTR does not reset; device sits silently in the menu (JSON). 'x' fallback
 * after reset_detect_ms starts a test; we pause it again. */
static void test_no_reset_paused_silent(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    s->d.dtr_resets = false;
    s->d.json = true;
    dev_menu(s);
    sim_run(s, 100);
    s->qh = s->qt; /* old menu text was printed before we attached */
    sim_attach(s);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_STR(s->sent, "xx");
    TT_EQ_INT(s->p.reset_observed, TL_TRI_NO);
    verbose(s, "test_no_reset_paused_silent");
}

/* DTR does not reset and the device is inside its 5 s post-boot delay: the
 * fallback 'x' is buffered, the menu prints, then the buffered 'x' starts a
 * test before our 'j' is read (and ignored in run mode). Must converge. */
static void test_no_reset_during_boot_delay(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    s->d.dtr_resets = false;
    s->d.mode = D_DELAY;
    s->d.t_mode = s->now;
    sim_attach(s);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 30000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_CHECK(s->d.json);
    TT_EQ_INT(s->d.mode, D_MENU);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_READY, 0), 1);
    verbose(s, "test_no_reset_during_boot_delay");
}

/* reset_mode NONE (bench result: DTR does not reset) with a running device */
static void test_reset_mode_none(void)
{
    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    cfg.reset_mode = TL_RESET_NONE;
    sim_init(s, 0, &cfg);
    s->d.json = false;
    dev_run(s);
    sim_run(s, 300);
    sim_attach(s);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_INT(s->n_set_lines, 0);
    TT_EQ_STR(s->sent, "xjx");
}

/* Scripted: menu "j)" line garbled, so the mode is unknown; 'j' reply says CSV
 * (device was JSON). v1.6 ignores 'j' while running, so the machine pauses
 * first and toggles again from the menu. */
static void test_mode_reply_csv_scripted(void)
{
    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    cfg.reset_mode = TL_RESET_NONE;
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_BOOT_WAIT);
    host_line(s, "0\t-0.15");
    host_line(s, "20\t-0.13");
    host_line(s, "Measurement Paused. Peak: 0.00 kN");
    host_line(s, "");
    host_line(s, FW_SEP);
    host_line(s, FW_OPT_R);
    host_line(s, FW_OPT_X);
    host_line(s, "j) Toggle o\x8f\x8f\x8f format (current: JS");
    host_line(s, FW_OPT_C);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_MODE_SET);
    TT_EQ_STR(s->sent, "j");
    sim_run(s, 20);
    host_line(s, "Output format changed to: CSV");
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_PAUSING);
    TT_EQ_STR(s->sent, "jx");
    sim_run(s, 300);
    host_line(s, "0.02,0.02"); /* in flight before the 'x' was read */
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_PAUSING);
    host_line(s, "Measurement Paused. Peak: 0.02 kN");
    host_line(s, "");
    host_line(s, FW_SEP);
    host_line(s, FW_OPT_R);
    host_line(s, FW_OPT_X);
    host_line(s, "j) Toggle output format (current: CSV)");
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_MODE_SET);
    TT_EQ_STR(s->sent, "jxj");
    host_line(s, FW_OPT_C);
    TT_EQ_STR(s->sent, "jxj"); /* "c)" after "j)" does not act twice */
    host_line(s, "Output format changed to: JSON");
    TT_EQ_STR(s->sent, "jxjx");
    host_line(s, "{\"timestamp\":5.873,\"current\":0.022,\"peak\":0.022}");
    host_line(s, "Measurement Paused. Peak: 0.02 kN");
    host_line(s, "");
    host_line(s, FW_SEP);
    host_line(s, FW_OPT_R);
    host_line(s, FW_OPT_X);
    host_line(s, "j) Toggle output format (current: JSON)");
    host_line(s, FW_OPT_C);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_STR(s->sent, "jxjx");
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_READY, 0), 1);
    TT_EQ_INT(count_ev(s, TL_EVT_SAMPLE, 0), 0);
}

/* Closed loop variant: the simulator garbles the first two "j)" lines. */
static void test_mode_unknown_closed_loop(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    s->d.mode = D_DELAY;
    s->d.t_mode = s->now;
    s->d.garble_j_line = 2;
    sim_attach(s);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_CHECK(s->d.json);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
    verbose(s, "test_mode_unknown_closed_loop");
}

/* Menu header seen but every option line lost: settle timer acts. */
static void test_menu_settle_timeout(void)
{
    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    cfg.reset_mode = TL_RESET_NONE;
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    host_line(s, "{\"timestamp\":3.100,\"current\":0.022,\"peak\":0.022}");
    TT_EQ_STR(s->sent, "x");
    host_line(s, "Measurement Paused. Peak: 0.02 kN");
    TT_EQ_STR(s->sent, "x");
    sim_run(s, cfg.menu_settle_ms - 2);
    TT_EQ_STR(s->sent, "x");
    sim_run(s, 3);
    /* format known from the JSON sample: paused + JSON = ready */
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
}

/* ======================================================================== */
/* state machine: tests and D1                                              */

/* New test while running, with a sample in flight when the command is issued.
 * The stale sample (old timestamp, old peak) must never reach the test. */
static void test_new_test_stale_inflight(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    s->latency = 40;
    sim_to_ready(s);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);

    /* first test: ramp up to a big peak */
    s->d.ramp = 0.5;
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    TT_EQ_INT(count_ev(s, TL_EVT_TEST_ACK, 0), 1);
    sim_run(s, 8000);
    int n1 = count_in_test(s, 0);
    TT_CHECK(n1 >= 14);

    /* stop ramping; peak stays high. Wait until a sample was printed but is
     * still in flight, then ask for a new test. */
    s->d.ramp = 0.0;
    s->d.force = 0.023;
    uint32_t prints = s->d.prints;
    while (s->d.prints == prints) {
        sim_step(s);
    }
    char stale[96];
    snprintf(stale, sizeof(stale), "%s", s->d.last_sample);
    tl_line_info_t sli = cls(stale);
    TT_EQ_INT(sli.kind, TL_LINE_SAMPLE_JSON);
    TT_CHECK(sli.peak_kN > 3.0);
    int mark = s->n_ev;
    uint32_t discarded = s->p.stats.discarded_samples;
    sent_reset(s);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_NEW_TEST_PENDING);
    TT_EQ_INT(count_ev(s, TL_EVT_TEST_STOPPED, mark), 1);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    TT_EQ_STR(s->sent, "xx");
    TT_CHECK(s->p.stats.discarded_samples > discarded); /* the in-flight one */
    const tl_evt_t *ack = first_ev(s, TL_EVT_TEST_ACK, mark);
    TT_CHECK(ack != NULL);
    if (ack) {
        TT_CHECK(ack->u.ack.pre_reset_valid);
        TT_NEAR(ack->u.ack.pre_reset_ts, sli.timestamp_s, 1e-9);
    }
    sim_run(s, 5000);
    const tl_evt_t *first = NULL;
    double maxpeak = 0;
    int n2 = 0;
    double prev = -1;
    for (int i = mark; i < s->n_ev; i++) {
        if (s->ev[i].type == TL_EVT_SAMPLE && s->ev[i].u.sample.in_test) {
            if (!first) {
                first = &s->ev[i];
            }
            if (s->ev[i].u.sample.peak_kN > maxpeak) {
                maxpeak = s->ev[i].u.sample.peak_kN;
            }
            TT_CHECK(s->ev[i].u.sample.timestamp_s > prev);
            TT_CHECK(s->ev[i].u.sample.timestamp_s < sli.timestamp_s);
            TT_EQ_INT(s->ev[i].u.sample.seq, n2);
            prev = s->ev[i].u.sample.timestamp_s;
            n2++;
        }
    }
    TT_CHECK(first != NULL);
    if (first) {
        TT_CHECK(first->u.sample.timestamp_s < 2.0);
        TT_EQ_INT(first->u.sample.seq, 0);
    }
    TT_CHECK(n2 >= 9);
    TT_NEAR(maxpeak, 0.023, 1e-9); /* old 3.x kN peak never leaks in */
    TT_EQ_INT(s->p.stats.stale_dropped, 0);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
    verbose(s, "test_new_test_stale_inflight");
}

/* Read data rows ("t,c,p") of a TensileCompanion CSV. */
static int read_fixture(const char *name, char rows[][3][24], int max)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "   cannot open %s\n", path);
        return -1;
    }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        if (line[0] == '#' || strncmp(line, "timestamp_s", 11) == 0) {
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        char *a = strtok(line, ",");
        char *b = strtok(NULL, ",");
        char *c = strtok(NULL, ",");
        if (!a || !b || !c) {
            continue;
        }
        snprintf(rows[n][0], 24, "%s", a);
        snprintf(rows[n][1], 24, "%s", b);
        snprintf(rows[n][2], 24, "%s", c);
        n++;
    }
    fclose(f);
    return n;
}

static void json_row(char *buf, size_t n, char row[3][24])
{
    snprintf(buf, n, "{\"timestamp\":%s,\"current\":%s,\"peak\":%s}", row[0], row[1], row[2]);
}

/* D1 from TensileCompanion/Tests/2026-01-20/Test_2_211047.csv: the desktop
 * recorded "192.821,0.023,3.379" (Test 1's peak) as row 1 of Test 2. Replay
 * that traffic through the HMI state machine. */
static void test_d1_fixture(void)
{
    static char rows[256][3][24];
    int n = read_fixture("Test_2_211047.csv", rows, 256);
    TT_EQ_INT(n, 77);
    if (n != 77) {
        return;
    }
    TT_EQ_STR(rows[0][0], "192.821");
    TT_EQ_STR(rows[0][2], "3.379");

    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    cfg.reset_mode = TL_RESET_NONE;
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    /* device left running after Test 1 (JSON, peak 3.379) */
    host_line(s, "{\"timestamp\":191.925,\"current\":0.023,\"peak\":3.379}");
    host_line(s, "Measurement Paused. Peak: 3.38 kN");
    host_line(s, "");
    host_line(s, FW_SEP);
    host_line(s, FW_OPT_R);
    host_line(s, FW_OPT_X);
    host_line(s, "j) Toggle output format (current: JSON)");
    host_line(s, FW_OPT_C);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_INT(sim_cmd(s, TL_CMD_RESUME), 0);
    host_line(s, FW_RESUMING);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_RUNNING);
    sim_run(s, 400);
    host_line(s, "{\"timestamp\":192.373,\"current\":0.023,\"peak\":3.379}");

    /* Operator taps New Test; the 192.821 reading is already on the wire. */
    int mark = s->n_ev;
    sent_reset(s);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    char buf[128];
    json_row(buf, sizeof(buf), rows[0]);
    sim_run(s, 30);
    host_line(s, buf); /* stale, arrives in NEW_TEST_PENDING */
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_NEW_TEST_PENDING);
    sim_run(s, 60);
    host_line(s, "Measurement Paused. Peak: 3.38 kN");
    host_line(s, "");
    host_line(s, FW_SEP);
    host_line(s, FW_OPT_R);
    host_line(s, FW_OPT_X);
    host_line(s, "j) Toggle output format (current: JSON)");
    TT_EQ_STR(s->sent, "xx");
    host_line(s, FW_OPT_C);
    sim_run(s, 5);
    host_line(s, FW_NEWTEST);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_RUNNING);
    const tl_evt_t *ack = first_ev(s, TL_EVT_TEST_ACK, mark);
    TT_CHECK(ack && ack->u.ack.pre_reset_valid);
    if (ack) {
        TT_NEAR(ack->u.ack.pre_reset_ts, 192.821, 1e-9);
    }
    for (int i = 1; i < n; i++) {
        sim_run(s, 450);
        json_row(buf, sizeof(buf), rows[i]);
        host_line(s, buf);
    }
    int acc = count_in_test(s, mark);
    TT_EQ_INT(acc, n - 1);
    double maxpeak = 0;
    const tl_evt_t *first = NULL;
    for (int i = mark; i < s->n_ev; i++) {
        if (s->ev[i].type == TL_EVT_SAMPLE && s->ev[i].u.sample.in_test) {
            first = first ? first : &s->ev[i];
            maxpeak = fmax(maxpeak, s->ev[i].u.sample.peak_kN);
        }
    }
    TT_CHECK(first != NULL);
    if (first) {
        TT_NEAR(first->u.sample.timestamp_s, 0.373, 1e-9);
    }
    TT_NEAR(maxpeak, 3.841, 1e-9); /* matches "# Peak Force: 3.841 kN" */

    /* Defense in depth: even if the stale row arrived AFTER the ack (it cannot
     * over an in-order serial link, but a replay or a buggy glue could), the
     * first-sample rules drop it, and the next real row is taken. */
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    host_line(s, "{\"timestamp\":192.373,\"current\":0.023,\"peak\":3.379}");
    host_line(s, "Measurement Paused. Peak: 3.38 kN");
    host_line(s, "j) Toggle output format (current: JSON)");
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    host_line(s, FW_NEWTEST);
    mark = s->n_ev;
    json_row(buf, sizeof(buf), rows[0]);
    host_line(s, buf);
    TT_EQ_INT(count_in_test(s, mark), 0);
    TT_EQ_INT(s->p.stats.stale_dropped, 1);
    /* also a same-era stale value with a small timestamp but the old peak */
    host_line(s, "{\"timestamp\":0.100,\"current\":0.023,\"peak\":3.379}");
    TT_EQ_INT(count_in_test(s, mark), 0);
    TT_EQ_INT(s->p.stats.stale_dropped, 2);
    for (int i = 1; i < 6; i++) {
        json_row(buf, sizeof(buf), rows[i]);
        host_line(s, buf);
    }
    TT_EQ_INT(count_in_test(s, mark), 5);
    /* and a stale row in the middle of the test is dropped by the step rule */
    json_row(buf, sizeof(buf), rows[0]);
    host_line(s, buf);
    TT_EQ_INT(count_in_test(s, mark), 5);
    TT_EQ_INT(s->p.stats.order_dropped, 1);
    json_row(buf, sizeof(buf), rows[6]);
    host_line(s, buf);
    TT_EQ_INT(count_in_test(s, mark), 6);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
}

/* Pause, resume (timestamp jump allowed once), stop. */
static void test_pause_resume_stop(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    sim_to_ready(s);
    TT_EQ_INT(sim_cmd(s, TL_CMD_PAUSE), 0);   /* no-op when paused */
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    sim_run(s, 3000);
    int a = count_in_test(s, 0);
    TT_CHECK(a >= 5);
    TT_EQ_INT(sim_cmd(s, TL_CMD_PAUSE), 0);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 5000);
    TT_EQ_INT(count_ev(s, TL_EVT_PAUSED, 0), 1);
    TT_CHECK(s->p.test_active);
    sim_run(s, 20000); /* long pause: next timestamp jumps ~20 s */
    TT_EQ_INT(sim_cmd(s, TL_CMD_RESUME), 0);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    TT_EQ_INT(count_ev(s, TL_EVT_RESUMED, 0), 1);
    int b = count_in_test(s, 0);
    sim_run(s, 3000);
    TT_CHECK(count_in_test(s, 0) >= b + 5);
    TT_EQ_INT(s->p.stats.order_dropped, 0);
    TT_EQ_INT(sim_cmd(s, TL_CMD_STOP), 0);
    TT_EQ_INT(count_ev(s, TL_EVT_TEST_STOPPED, 0), 1);
    int c = count_in_test(s, 0);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 5000);
    TT_EQ_INT(count_in_test(s, 0), c);
    /* commands in the wrong state are rejected with an event */
    TT_EQ_INT(sim_cmd(s, TL_CMD_RESUME), 0);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), -1); /* RESUMING */
    TT_EQ_INT(count_ev(s, TL_EVT_CMD_REJECTED, 0), 1);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    sim_run(s, 2000);
    /* running outside a test: samples are live-only */
    TT_EQ_INT(count_in_test(s, 0), c);
    const tl_evt_t *ls = last_ev(s, TL_EVT_SAMPLE);
    TT_CHECK(ls && !ls->u.sample.in_test);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
}

/* STOP while the new-test handshake is in flight: the test never starts. */
static void test_stop_during_new_test(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    sim_to_ready(s);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    TT_EQ_INT(sim_cmd(s, TL_CMD_STOP), 0);
    sim_run(s, 5000);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_EQ_INT(count_in_test(s, 0), 0);
    TT_CHECK(!s->p.test_active);
    TT_EQ_INT(s->d.mode, D_MENU);
}

/* ======================================================================== */
/* timeouts, faults, detach, reboot                                         */

static void test_timeouts_boot(void)
{
    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 1 + cfg.dtr_settle_ms);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_BOOT_WAIT);
    TT_EQ_INT(s->n_set_lines, 2);
    sim_run(s, 600);
    host_line(s, "0\t-0.15"); /* rebooted ... and then silence */
    host_line(s, "20\t-0.13");
    sim_run(s, cfg.boot_timeout_ms - 1);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_BOOT_WAIT);
    sim_run(s, 2);
    /* boot timeout: one more reset */
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_RESETTING);
    sim_run(s, cfg.dtr_settle_ms + 1);
    TT_EQ_INT(s->n_set_lines, 4);
    sim_run(s, 600);
    host_line(s, "0\t-0.15");
    sim_run(s, cfg.boot_timeout_ms + 1);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_FAULT);
    const tl_evt_t *f = last_ev(s, TL_EVT_FAULT);
    TT_CHECK(f && f->u.fault.reason == TL_FAULT_BOOT_TIMEOUT);
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_GONE, 0), 0); /* never was ready */
    TT_EQ_STR(s->sent, "");

    /* No rows, no traffic at all: fallback 'x', retry once, then FAULT */
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2 + cfg.dtr_settle_ms + cfg.reset_detect_ms);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_PAUSING);
    TT_EQ_STR(s->sent, "x");
    sim_run(s, cfg.pause_timeout_ms);
    TT_EQ_STR(s->sent, "xx");
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_PAUSING);
    sim_run(s, cfg.pause_timeout_ms);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_FAULT);
    f = last_ev(s, TL_EVT_FAULT);
    TT_CHECK(f && f->u.fault.reason == TL_FAULT_PAUSE_TIMEOUT);
    TT_EQ_STR(s->sent, "xx");
    /* RECONNECT re-runs the sequence */
    TT_EQ_INT(sim_cmd(s, TL_CMD_RECONNECT), 0);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_RESETTING);
}

static void test_timeouts_mode_and_ack(void)
{
    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    cfg.reset_mode = TL_RESET_NONE;
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    host_line(s, "Measurement Paused. Peak: 0.00 kN");
    host_line(s, "j) Toggle output format (current: CSV)");
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_MODE_SET);
    sim_run(s, cfg.mode_timeout_ms);
    TT_EQ_STR(s->sent, "jj");
    sim_run(s, cfg.mode_timeout_ms);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_FAULT);
    TT_CHECK(last_ev(s, TL_EVT_FAULT)->u.fault.reason == TL_FAULT_MODE_TIMEOUT);

    /* deaf device after READY: NEW_TEST ack never comes */
    sim_init(s, 0, NULL);
    sim_to_ready(s);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    s->d.deaf = true;
    sent_reset(s);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    sim_run(s, cfg.ack_timeout_ms);
    TT_EQ_STR(s->sent, "xx");
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_NEW_TEST_PENDING);
    sim_run(s, cfg.ack_timeout_ms);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_FAULT);
    TT_CHECK(last_ev(s, TL_EVT_FAULT)->u.fault.reason == TL_FAULT_ACK_TIMEOUT);
    const tl_evt_t *g = last_ev(s, TL_EVT_LINK_GONE);
    TT_CHECK(g && g->u.gone.reason == TL_GONE_FAULT && !g->u.gone.test_interrupted);

    /* lost ack, device running the new test: pause and redo once */
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    host_line(s, "Measurement Paused. Peak: 0.00 kN");
    host_line(s, "j) Toggle output format (current: JSON)");
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    sent_reset(s);
    sim_run(s, 400);
    host_line(s, "{\"timestamp\":0.451,\"current\":0.022,\"peak\":0.022}"); /* ack was lost */
    TT_EQ_STR(s->sent, "x");
    TT_EQ_INT(count_in_test(s, 0), 0);
    host_line(s, "Measurement Paused. Peak: 0.02 kN");
    host_line(s, "j) Toggle output format (current: JSON)");
    TT_EQ_STR(s->sent, "xx");
    host_line(s, FW_NEWTEST);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_RUNNING);
    host_line(s, "{\"timestamp\":0.452,\"current\":0.022,\"peak\":0.022}");
    TT_EQ_INT(count_in_test(s, 0), 1);
}

/* Timestamps never reset after an ack (firmware anomaly) -> FAULT */
static void test_stale_fault(void)
{
    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    cfg.reset_mode = TL_RESET_NONE;
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    host_line(s, "{\"timestamp\":50.000,\"current\":0.022,\"peak\":1.000}");
    host_line(s, "Measurement Paused. Peak: 1.00 kN");
    host_line(s, "j) Toggle output format (current: JSON)");
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    host_line(s, FW_NEWTEST);
    char buf[96];
    for (int i = 0; i < cfg.max_first_drops; i++) {
        snprintf(buf, sizeof(buf), "{\"timestamp\":%.3f,\"current\":0.022,\"peak\":1.000}",
                 50.5 + 0.5 * i);
        host_line(s, buf);
    }
    TT_EQ_INT(count_in_test(s, 0), 0);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_FAULT);
    TT_CHECK(last_ev(s, TL_EVT_FAULT)->u.fault.reason == TL_FAULT_STALE_TIMESTAMPS);
    const tl_evt_t *g = last_ev(s, TL_EVT_LINK_GONE);
    TT_CHECK(g && g->u.gone.test_interrupted);
}

static void test_detach_mid_test(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    sim_to_ready(s);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    sim_run(s, 2000);
    int n = count_in_test(s, 0);
    TT_CHECK(n >= 3);
    sim_detach(s);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_DETACHED);
    const tl_evt_t *g = last_ev(s, TL_EVT_LINK_GONE);
    TT_CHECK(g && g->u.gone.reason == TL_GONE_DETACHED && g->u.gone.test_interrupted);
    /* late bytes after the detach are ignored */
    host_line(s, "{\"timestamp\":9.000,\"current\":0.022,\"peak\":0.022}");
    TT_EQ_INT(count_in_test(s, 0), n);
    TT_EQ_INT(tl_proto_ms_to_deadline(&s->p, s->now), -1);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), -1);
    /* replug: full connect again */
    int mark = s->n_ev;
    s->d.mode = D_DELAY;
    s->d.t_mode = s->now;
    sim_attach(s);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_READY, mark), 1);

    /* detach while still connecting: LINK_GONE is not emitted (never ready) */
    sim_init(s, 0, NULL);
    sim_attach(s);
    sim_run(s, 300);
    sim_detach(s);
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_GONE, 0), 0);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_DETACHED);

    /* open failure: one retry, then FAULT */
    sim_init(s, 0, NULL);
    s->open_ok = false;
    sim_attach(s);
    sim_run(s, 5);
    TT_EQ_INT(s->n_open, 2);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_FAULT);
    TT_CHECK(last_ev(s, TL_EVT_FAULT)->u.fault.reason == TL_FAULT_OPEN_FAILED);
}

/* 328P reboots mid-test (brownout): LINK_GONE(DEVICE_REBOOTED), reconnect. */
static void test_device_reboot_mid_test(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    sim_to_ready(s);
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    sim_run(s, 2000);
    dev_reset(s, true);
    int mark = s->n_ev;
    sim_run(s, 200);
    const tl_evt_t *g = last_ev(s, TL_EVT_LINK_GONE);
    TT_CHECK(g && g->u.gone.reason == TL_GONE_DEVICE_REBOOTED && g->u.gone.test_interrupted);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_BOOT_WAIT);
    sim_run_until_state(s, TL_ST_READY_PAUSED, 20000);
    TT_EQ_INT(count_ev(s, TL_EVT_LINK_READY, mark), 1);
    TT_EQ_INT(s->p.stats.reboots_unexpected, 1);
    verbose(s, "test_device_reboot_mid_test");
}

/* Garbage, truncated lines and broken samples while running a test. */
static void test_garbled_running(void)
{
    sim_t *s = &S;
    tl_proto_cfg_t cfg;
    tl_proto_cfg_default(&cfg);
    cfg.reset_mode = TL_RESET_NONE;
    sim_init(s, 0, &cfg);
    sim_attach(s);
    sim_run(s, 2);
    host_line(s, "Measurement Paused. Peak: 0.00 kN");
    host_line(s, "j) Toggle output format (current: JSON)");
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    host_line(s, FW_NEWTEST);
    const char *chunks[] = {
        "{\"timestamp\":0.4", "51,\"current\":0.0", "22,\"peak\":0.022}\r", "\n\xff\xfe\r\n",
        "{\"timestamp\":0.9", "{\"timestamp\":1.351,\"current\":0.030,\"peak\":0.030}\n",
        "{\"timestamp\":1.800,\"current\":nan,\"peak\":0.030}\r\n",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n",
        "{\"timestamp\":2.251,\"current\":0.040,", "\"peak\":0.040}", "\r", "\n",
    };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        host_bytes(s, (const uint8_t *)chunks[i], strlen(chunks[i]));
        sim_run(s, 7);
    }
    TT_EQ_INT(count_in_test(s, 0), 2); /* 0.451 and 2.251; the spliced 1.351 line is garbage */
    TT_EQ_INT(s->p.stats.bad_samples, 2);
    TT_EQ_INT(s->la.overflows, 1);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_RUNNING);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
    /* CSV while linked in JSON is a protocol fault */
    host_line(s, "0.04,0.04");
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_FAULT);
    TT_CHECK(last_ev(s, TL_EVT_FAULT)->u.fault.reason == TL_FAULT_UNEXPECTED_FORMAT);
}

/* Monotonic clock wrap (uint32 ms) during connect and a test. */
static void test_clock_wrap(void)
{
    sim_t *s = &S;
    sim_init(s, 0xFFFFFFFFu - 3000u, NULL);
    sim_to_ready(s);
    TT_EQ_INT(tl_proto_state(&s->p), TL_ST_READY_PAUSED);
    TT_CHECK(s->now < 0x80000000u); /* wrapped */
    TT_EQ_INT(sim_cmd(s, TL_CMD_NEW_TEST), 0);
    sim_run_until_state(s, TL_ST_RUNNING, 5000);
    sim_run(s, 2000);
    TT_CHECK(count_in_test(s, 0) >= 3);
    TT_EQ_INT(count_ev(s, TL_EVT_FAULT, 0), 0);
}

/* Twenty back-to-back tests in closed loop with jittery latency (Phase 1 exit
 * criterion: 20 tests, no stale-sample artifacts). */
static void test_twenty_tests(void)
{
    sim_t *s = &S;
    sim_init(s, 0, NULL);
    sim_to_ready(s);
    srand(42);
    int bad_first = 0, leaks = 0, faults = 0;
    for (int t = 0; t < 20; t++) {
        s->latency = 2 + (uint32_t)(rand() % 60);
        s->d.ramp = 0.1 + 0.05 * (t % 5);
        int mark = s->n_ev;
        if (sim_cmd(s, TL_CMD_NEW_TEST) != 0) {
            faults++;
            break;
        }
        sim_run(s, 1500 + (uint32_t)(rand() % 6000));
        double first_ts = -1, maxpeak = 0, prev = -1;
        int n = 0;
        for (int i = mark; i < s->n_ev; i++) {
            if (s->ev[i].type == TL_EVT_SAMPLE && s->ev[i].u.sample.in_test) {
                if (first_ts < 0) {
                    first_ts = s->ev[i].u.sample.timestamp_s;
                }
                if (s->ev[i].u.sample.timestamp_s <= prev) {
                    leaks++;
                }
                prev = s->ev[i].u.sample.timestamp_s;
                maxpeak = fmax(maxpeak, s->ev[i].u.sample.peak_kN);
                n++;
            }
        }
        if (n == 0 || first_ts >= 2.0) {
            bad_first++;
        }
        /* peak can only come from this test's ramp (JSON rounds to 3 decimals) */
        if (maxpeak > s->d.maxr + 0.0006) {
            leaks++;
        }
        if (t % 3 == 2) {
            sim_cmd(s, TL_CMD_STOP);
            sim_run(s, 1500);
        }
    }
    faults += count_ev(s, TL_EVT_FAULT, 0);
    TT_EQ_INT(bad_first, 0);
    TT_EQ_INT(leaks, 0);
    TT_EQ_INT(faults, 0);
    TT_EQ_INT(s->p.stats.stale_dropped, 0);
    if (faults) {
        dump_events(s, 0);
    }
}

int main(void)
{
    TT_RUN(test_line_assembler);
    TT_RUN(test_line_chunking);
    TT_RUN(test_classifier);
    TT_RUN(test_cold_boot_dtr_reset);
    TT_RUN(test_power_on_rows_during_reset);
    TT_RUN(test_no_reset_running_csv);
    TT_RUN(test_no_reset_running_json);
    TT_RUN(test_no_reset_paused_silent);
    TT_RUN(test_no_reset_during_boot_delay);
    TT_RUN(test_reset_mode_none);
    TT_RUN(test_mode_reply_csv_scripted);
    TT_RUN(test_mode_unknown_closed_loop);
    TT_RUN(test_menu_settle_timeout);
    TT_RUN(test_new_test_stale_inflight);
    TT_RUN(test_d1_fixture);
    TT_RUN(test_pause_resume_stop);
    TT_RUN(test_stop_during_new_test);
    TT_RUN(test_timeouts_boot);
    TT_RUN(test_timeouts_mode_and_ack);
    TT_RUN(test_stale_fault);
    TT_RUN(test_detach_mid_test);
    TT_RUN(test_device_reboot_mid_test);
    TT_RUN(test_garbled_running);
    TT_RUN(test_clock_wrap);
    TT_RUN(test_twenty_tests);
    return tt_summary();
}
