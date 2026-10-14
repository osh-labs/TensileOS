/*
 * tensile_link ESP-IDF glue: FTDI VCP (espressif/usb_host_ftdi_vcp ^2.1.1 on
 * usb_host_cdc_acm ^2.3) + link task that owns the protocol state machine.
 *
 * Contexts:
 *  - CDC driver task ("USB-CDC"): rx_cb, dev_event_cb, new_dev_cb. They only
 *    post messages to the control queue; nothing blocks there.
 *  - link task ("tl_link"): line assembly, classification, state machine,
 *    USB writes and control requests, event delivery, the DTR probe.
 */
#include "tensile_link.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp_ftdi.h"

static const char *TAG = "tensile_link";

#define TL_CTL_QUEUE_LEN     96   /* 96 x 64 B; boot dump bursts ~4 KB */
#define TL_RX_CHUNK          60
#define TL_TX_TIMEOUT_MS     100
#define TL_OPEN_TIMEOUT_MS   1000 /* connection wait inside ftdi_vcp_open() */
#define TL_POLL_OPEN_MS      10   /* quick presence check while detached */
#define TL_DETACHED_POLL_MS  2000
#define TL_IDLE_WAIT_MS      1000

#define PROBE_BASELINE_MS    300
#define PROBE_DETECT_MS      3000  /* no calibration row by then => no reset */
#define PROBE_BOOT_MS        12000 /* edge -> menu limit when it did reset */
#define PROBE_AFTER_MENU_MS  300

typedef enum {
    MSG_RX = 0,
    MSG_ATTACH,
    MSG_DETACH,
    MSG_CMD,
    MSG_PROBE,
} msg_type_t;

typedef struct {
    tl_probe_result_t *out;
    SemaphoreHandle_t done;
} probe_req_t;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t cmd;
    uint8_t pad;
    union {
        uint8_t data[TL_RX_CHUNK];
        probe_req_t *probe;
    } u;
} tl_msg_t;

static tl_config_t s_cfg;
static QueueHandle_t s_ctl;
static QueueHandle_t s_evt;
static TaskHandle_t s_task;
static cdc_acm_dev_hdl_t s_dev;     /* link task only */
static tl_proto_t s_p;              /* link task only */
static tl_line_asm_t s_la;          /* link task only */
static volatile tl_state_t s_state = TL_ST_DETACHED;
static volatile bool s_detach_pending;
static volatile uint32_t s_rx_dropped;
static volatile uint32_t s_evt_dropped;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static tl_proto_stats_t s_stats_snap;

/* probe bookkeeping (link task only) */
typedef struct {
    bool active;
    uint32_t t0;
    tl_probe_edge_t *edge;
    uint32_t baseline_samples;
} probe_ctx_t;
static probe_ctx_t s_probe;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------------ */
/* callbacks (CDC driver task)                                              */

static bool rx_cb(const uint8_t *data, size_t data_len, void *user_arg)
{
    (void)user_arg;
    tl_msg_t m = {.type = MSG_RX};
    while (data_len > 0) {
        size_t n = data_len > TL_RX_CHUNK ? TL_RX_CHUNK : data_len;
        m.len = (uint8_t)n;
        memcpy(m.u.data, data, n);
        if (xQueueSend(s_ctl, &m, 0) != pdTRUE) {
            s_rx_dropped += (uint32_t)n;
        }
        data += n;
        data_len -= n;
    }
    return true; /* consumed: driver may reuse its buffer */
}

static void dev_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGW(TAG, "CDC-ACM error %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
        /* The handle is closed by the link task (it is the only user). The
         * flag backs up the message in case the queue is full of RX data. */
        s_detach_pending = true;
        tl_msg_t m = {.type = MSG_DETACH};
        (void)xQueueSendToFront(s_ctl, &m, 0);
        break;
    }
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "serial state 0x%04x", (unsigned)event->data.serial_state.val);
        break;
    default:
        break;
    }
}

static void new_dev_cb(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) != ESP_OK) {
        return;
    }
    bool pid_ok = (s_cfg.pid == TL_FTDI_PID_AUTO)
                      ? (desc->idProduct == FT231_PID || desc->idProduct == FT232_PID)
                      : (desc->idProduct == s_cfg.pid);
    if (desc->idVendor != TL_FTDI_VID || !pid_ok) {
        ESP_LOGI(TAG, "ignoring USB device %04x:%04x", desc->idVendor, desc->idProduct);
        return;
    }
    tl_msg_t m = {.type = MSG_ATTACH};
    (void)xQueueSendToFront(s_ctl, &m, 0);
}

/* ------------------------------------------------------------------------ */
/* USB operations (link task)                                               */

static esp_err_t usb_open(uint32_t connection_timeout_ms)
{
    if (s_dev) {
        return ESP_OK;
    }
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = connection_timeout_ms,
        .out_buffer_size = 64,
        .in_buffer_size = 0, /* FTDI driver requires 0 (IN MPS) */
        .event_cb = dev_event_cb,
        .data_cb = rx_cb,
        .user_arg = NULL,
    };
    cdc_acm_dev_hdl_t dev = NULL;
    esp_err_t err = ftdi_vcp_open(s_cfg.pid, 0, &dev_config, &dev);
    if (err != ESP_OK) {
        return err;
    }
    /* ftdi_vcp_open() already sets 115200 8N1; set it explicitly anyway so a
     * driver default change cannot silently alter the link. */
    cdc_acm_line_coding_t lc = {
        .dwDTERate = 115200,
        .bCharFormat = 0, /* 1 stop bit */
        .bParityType = 0, /* none */
        .bDataBits = 8,
    };
    err = cdc_acm_host_line_coding_set(dev, &lc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "line coding: %s", esp_err_to_name(err));
    }
    s_detach_pending = false;
    s_dev = dev;
    ESP_LOGI(TAG, "FTDI VCP open (pid 0x%04x)", s_cfg.pid);
    return ESP_OK;
}

static void usb_close(void)
{
    if (s_dev) {
        esp_err_t err = cdc_acm_host_close(s_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "close: %s", esp_err_to_name(err));
        }
        s_dev = NULL;
    }
}

static esp_err_t usb_send(uint8_t b)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_dev, &b, 1, TL_TX_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tx '%c': %s", b, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t usb_lines(bool dtr, bool rts)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = cdc_acm_host_set_control_line_state(s_dev, dtr, rts);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DTR=%d RTS=%d: %s", dtr, rts, esp_err_to_name(err));
    }
    return err;
}

/* ------------------------------------------------------------------------ */
/* state machine plumbing (link task)                                       */

static void publish(void)
{
    s_state = s_p.state;
    portENTER_CRITICAL(&s_stats_mux);
    s_stats_snap = s_p.stats;
    portEXIT_CRITICAL(&s_stats_mux);
}

static void exec(const tl_out_t *out);

static void deliver(const tl_evt_t *e)
{
    switch (e->type) {
    case TL_EVT_STATE:
        ESP_LOGI(TAG, "state %s -> %s", tl_state_name(e->u.st.prev), tl_state_name(e->state));
        break;
    case TL_EVT_FAULT:
        ESP_LOGE(TAG, "FAULT %s in %s", tl_fault_name(e->u.fault.reason),
                 tl_state_name(e->u.fault.in_state));
        break;
    case TL_EVT_LINK_GONE:
        ESP_LOGW(TAG, "link gone (reason %d, test interrupted %d)", e->u.gone.reason,
                 e->u.gone.test_interrupted);
        break;
    case TL_EVT_SAMPLE:
    case TL_EVT_BOOT_PROGRESS:
        break;
    default:
        ESP_LOGI(TAG, "%s", tl_evt_name(e->type));
        break;
    }
    if (xQueueSend(s_evt, e, 0) != pdTRUE) {
        s_evt_dropped++;
    }
}

/* Events first (they describe what already happened), then actions in order.
 * TL_ACT_OPEN feeds its result straight back, which may nest one level. */
static void exec(const tl_out_t *out)
{
    if (out->overflow) {
        ESP_LOGE(TAG, "state machine output overflow (%u)", (unsigned)out->overflow);
    }
    for (int i = 0; i < out->n_evts; i++) {
        deliver(&out->evts[i]);
    }
    for (int i = 0; i < out->n_acts; i++) {
        const tl_act_t *a = &out->acts[i];
        switch (a->type) {
        case TL_ACT_OPEN: {
            esp_err_t err = usb_open(TL_OPEN_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "open: %s", esp_err_to_name(err));
            }
            tl_out_t o2;
            tl_proto_opened(&s_p, now_ms(), err == ESP_OK, &o2);
            exec(&o2);
            break;
        }
        case TL_ACT_SEND:
            ESP_LOGD(TAG, "send '%c'", a->byte);
            (void)usb_send(a->byte);
            break;
        case TL_ACT_SET_LINES:
            ESP_LOGI(TAG, "DTR=%d RTS=%d", a->dtr, a->rts);
            (void)usb_lines(a->dtr, a->rts);
            break;
        case TL_ACT_FLUSH_RX:
            tl_line_reset(&s_la);
            break;
        case TL_ACT_START_TIMER:
            break; /* the task waits on tl_proto_ms_to_deadline() */
        }
    }
}

static void handle_detach(void)
{
    s_detach_pending = false;
    usb_close();
    tl_line_reset(&s_la);
    tl_out_t out;
    tl_proto_detach(&s_p, now_ms(), &out);
    exec(&out);
}

static void probe_line(const tl_line_info_t *li, uint32_t t);

static void feed_rx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t n;
        bool truncated;
        const char *line = tl_line_put(&s_la, data[i], &n, &truncated);
        if (!line) {
            continue;
        }
        uint32_t t = now_ms();
        if (s_cfg.raw_line_cb) {
            s_cfg.raw_line_cb(line, n, t, s_cfg.raw_line_ctx);
        }
        tl_line_info_t li;
        tl_classify(line, n, truncated, &li);
        if (s_probe.active) {
            probe_line(&li, t);
            continue;
        }
        tl_out_t out;
        tl_proto_line_info(&s_p, t, &li, &out);
        exec(&out);
    }
}

/* ------------------------------------------------------------------------ */
/* Phase 0 DTR/RTS probe (link task)                                        */

static void probe_line(const tl_line_info_t *li, uint32_t t)
{
    if (!s_probe.edge) {
        if (li->kind == TL_LINE_SAMPLE_JSON || li->kind == TL_LINE_SAMPLE_CSV) {
            s_probe.baseline_samples++;
        }
        return;
    }
    tl_probe_edge_t *e = s_probe.edge;
    int32_t dt = (int32_t)(t - s_probe.t0);
    e->lines++;
    if (e->first_line_ms < 0) {
        e->first_line_ms = dt;
    }
    switch (li->kind) {
    case TL_LINE_CALIBRATION_ROW:
        e->calib_rows++;
        if (e->first_calib_ms < 0) {
            e->first_calib_ms = dt;
        }
        e->reset = true;
        break;
    case TL_LINE_MENU_HEADER:
        if (e->menu_ms < 0) {
            e->menu_ms = dt;
        }
        break;
    case TL_LINE_SAMPLE_JSON:
    case TL_LINE_SAMPLE_CSV:
        e->samples++;
        break;
    default:
        break;
    }
}

/* Pump the control queue for up to ms, or until the edge result is final.
 * Returns ESP_ERR_INVALID_STATE if the device went away. */
static esp_err_t probe_pump(uint32_t ms, bool stop_when_done)
{
    uint32_t start = now_ms();
    for (;;) {
        uint32_t el = now_ms() - start;
        if (el >= ms) {
            return ESP_OK;
        }
        if (stop_when_done && s_probe.edge) {
            tl_probe_edge_t *e = s_probe.edge;
            uint32_t since = now_ms() - s_probe.t0;
            if (e->menu_ms >= 0 && since >= (uint32_t)e->menu_ms + PROBE_AFTER_MENU_MS) {
                return ESP_OK;
            }
            if (!e->reset && since >= PROBE_DETECT_MS) {
                return ESP_OK;
            }
        }
        tl_msg_t m;
        if (xQueueReceive(s_ctl, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
            switch (m.type) {
            case MSG_RX:
                feed_rx(m.u.data, m.len);
                break;
            case MSG_DETACH:
                return ESP_ERR_INVALID_STATE;
            case MSG_PROBE:
                m.u.probe->out->err = ESP_ERR_INVALID_STATE; /* one at a time */
                xSemaphoreGive(m.u.probe->done);
                break;
            default:
                break; /* commands are dropped while probing */
            }
        }
        if (s_detach_pending) {
            return ESP_ERR_INVALID_STATE;
        }
    }
}

static esp_err_t probe_edge(tl_probe_edge_t *e, bool dtr0, bool rts0, bool dtr1, bool rts1)
{
    memset(e, 0, sizeof(*e));
    e->first_line_ms = e->first_calib_ms = e->menu_ms = -1;
    s_probe.edge = NULL;
    ESP_RETURN_ON_ERROR(usb_lines(dtr0, rts0), TAG, "probe lines");
    ESP_RETURN_ON_ERROR(probe_pump(PROBE_BASELINE_MS, false), TAG, "probe baseline");
    s_probe.t0 = now_ms();
    s_probe.edge = e;
    ESP_RETURN_ON_ERROR(usb_lines(dtr1, rts1), TAG, "probe edge");
    esp_err_t err = probe_pump(PROBE_BOOT_MS + PROBE_AFTER_MENU_MS, true);
    s_probe.edge = NULL;
    ESP_LOGI(TAG, "probe DTR %d->%d RTS %d->%d: reset=%d first_line=%ld ms first_calib=%ld ms "
             "menu=%ld ms rows=%u samples=%u lines=%u",
             dtr0, dtr1, rts0, rts1, e->reset, (long)e->first_line_ms, (long)e->first_calib_ms,
             (long)e->menu_ms, (unsigned)e->calib_rows, (unsigned)e->samples, (unsigned)e->lines);
    return err;
}

static void run_probe(tl_probe_result_t *r)
{
    memset(r, 0, sizeof(*r));
    if (!s_dev) {
        r->err = ESP_ERR_INVALID_STATE;
        return;
    }
    s_probe.active = true;
    s_probe.edge = NULL;
    s_probe.baseline_samples = 0;
    tl_line_reset(&s_la);

    esp_err_t err = probe_edge(&r->dtr_assert, false, false, true, false);
    r->streaming_before = s_probe.baseline_samples > 0;
    if (err == ESP_OK) {
        err = probe_edge(&r->dtr_deassert, true, false, false, false);
    }
    if (err == ESP_OK) {
        err = probe_edge(&r->rts_assert, false, false, false, true);
    }
    if (err == ESP_OK) {
        err = probe_edge(&r->rts_deassert, false, true, false, false);
    }
    r->err = err;
    s_probe.active = false;
    tl_line_reset(&s_la);

    if (s_detach_pending) {
        handle_detach();
        return;
    }
    tl_out_t out;
    tl_proto_cmd(&s_p, now_ms(), TL_CMD_RECONNECT, &out); /* device state changed under us */
    exec(&out);
}

/* ------------------------------------------------------------------------ */
/* link task                                                                */

static void try_attach(uint32_t connection_timeout_ms)
{
    if (s_p.state != TL_ST_DETACHED) {
        return;
    }
    if (usb_open(connection_timeout_ms) != ESP_OK) {
        return;
    }
    tl_out_t out;
    tl_proto_attach(&s_p, now_ms(), &out); /* emits TL_ACT_OPEN; usb_open() is then a no-op */
    exec(&out);
}

static void handle_msg(const tl_msg_t *m)
{
    tl_out_t out;
    switch (m->type) {
    case MSG_RX:
        feed_rx(m->u.data, m->len);
        break;
    case MSG_ATTACH:
        if (s_p.state == TL_ST_DETACHED) {
            tl_proto_attach(&s_p, now_ms(), &out);
            exec(&out);
        }
        break;
    case MSG_DETACH:
        handle_detach();
        break;
    case MSG_CMD:
        (void)tl_proto_cmd(&s_p, now_ms(), (tl_cmd_t)m->cmd, &out);
        exec(&out);
        break;
    case MSG_PROBE:
        run_probe(m->u.probe->out);
        xSemaphoreGive(m->u.probe->done);
        break;
    default:
        break;
    }
}

static void link_task(void *arg)
{
    (void)arg;
    uint32_t last_poll = now_ms();
    try_attach(TL_POLL_OPEN_MS); /* scale plugged in before tl_start() */
    publish();
    for (;;) {
        int32_t d = tl_proto_ms_to_deadline(&s_p, now_ms());
        uint32_t wait = d < 0 ? TL_IDLE_WAIT_MS : (uint32_t)d + 1;
        if (s_p.state == TL_ST_DETACHED && wait > TL_DETACHED_POLL_MS) {
            wait = TL_DETACHED_POLL_MS;
        }
        tl_msg_t m;
        if (xQueueReceive(s_ctl, &m, pdMS_TO_TICKS(wait)) == pdTRUE) {
            handle_msg(&m);
        }
        if (s_detach_pending && s_dev) {
            handle_detach();
        }
        tl_out_t out;
        tl_proto_tick(&s_p, now_ms(), &out);
        exec(&out);
        /* new_dev_cb covers hot-plug; this covers a missed event */
        if (s_p.state == TL_ST_DETACHED && now_ms() - last_poll >= TL_DETACHED_POLL_MS) {
            last_poll = now_ms();
            try_attach(TL_POLL_OPEN_MS);
        }
        publish();
    }
}

/* ------------------------------------------------------------------------ */
/* public API                                                               */

void tl_config_default(tl_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->pid = TL_FTDI_PID_FT231X;
    tl_proto_cfg_default(&cfg->proto);
    cfg->task_priority = 5;
    cfg->task_stack = 4096;
    cfg->task_core = tskNO_AFFINITY;
    cfg->event_queue_len = 64;
    cfg->install_cdc_driver = true;
    cfg->cdc_driver_priority = 10;
}

esp_err_t tl_start(const tl_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    if (cfg) {
        s_cfg = *cfg;
    } else {
        tl_config_default(&s_cfg);
    }
    ESP_RETURN_ON_FALSE(s_cfg.task_priority < s_cfg.cdc_driver_priority || !s_cfg.install_cdc_driver,
                        ESP_ERR_INVALID_ARG, TAG, "link task priority must be below the CDC driver's");

    s_ctl = xQueueCreate(TL_CTL_QUEUE_LEN, sizeof(tl_msg_t));
    s_evt = xQueueCreate(s_cfg.event_queue_len, sizeof(tl_evt_t));
    ESP_RETURN_ON_FALSE(s_ctl && s_evt, ESP_ERR_NO_MEM, TAG, "queues");

    tl_proto_init(&s_p, &s_cfg.proto);
    tl_line_init(&s_la);

    if (s_cfg.install_cdc_driver) {
        const cdc_acm_host_driver_config_t drv = {
            .driver_task_stack_size = 4096,
            .driver_task_priority = s_cfg.cdc_driver_priority,
            .xCoreID = 0,
            .new_dev_cb = new_dev_cb,
        };
        /* ESP_ERR_INVALID_STATE: already installed, or the USB Host library is
         * not installed yet (call bsp_usb_host_start() first). */
        ESP_RETURN_ON_ERROR(cdc_acm_host_install(&drv), TAG, "cdc_acm_host_install");
    } else {
        ESP_RETURN_ON_ERROR(cdc_acm_host_register_new_dev_callback(new_dev_cb), TAG,
                            "register new_dev_cb");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(link_task, "tl_link", s_cfg.task_stack, NULL,
                                            s_cfg.task_priority, &s_task, s_cfg.task_core);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "link task");
    return ESP_OK;
}

QueueHandle_t tl_event_queue(void)
{
    return s_evt;
}

static esp_err_t post_cmd(tl_cmd_t cmd)
{
    ESP_RETURN_ON_FALSE(s_ctl, ESP_ERR_INVALID_STATE, TAG, "not started");
    tl_msg_t m = {.type = MSG_CMD, .cmd = (uint8_t)cmd};
    return xQueueSend(s_ctl, &m, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t tl_cmd_new_test(void)
{
    return post_cmd(TL_CMD_NEW_TEST);
}

esp_err_t tl_cmd_pause(void)
{
    return post_cmd(TL_CMD_PAUSE);
}

esp_err_t tl_cmd_resume(void)
{
    return post_cmd(TL_CMD_RESUME);
}

esp_err_t tl_cmd_stop(void)
{
    return post_cmd(TL_CMD_STOP);
}

esp_err_t tl_cmd_reconnect(void)
{
    return post_cmd(TL_CMD_RECONNECT);
}

tl_state_t tl_get_state(void)
{
    return s_state;
}

void tl_get_stats(tl_proto_stats_t *out, uint32_t *rx_bytes_dropped, uint32_t *events_dropped)
{
    if (out) {
        portENTER_CRITICAL(&s_stats_mux);
        *out = s_stats_snap;
        portEXIT_CRITICAL(&s_stats_mux);
    }
    if (rx_bytes_dropped) {
        *rx_bytes_dropped = s_rx_dropped;
    }
    if (events_dropped) {
        *events_dropped = s_evt_dropped;
    }
}

esp_err_t tl_probe_dtr(tl_probe_result_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out");
    ESP_RETURN_ON_FALSE(s_ctl && s_task, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(xTaskGetCurrentTaskHandle() != s_task, ESP_ERR_INVALID_STATE, TAG,
                        "not from the link task");
    probe_req_t req = {.out = out, .done = xSemaphoreCreateBinary()};
    ESP_RETURN_ON_FALSE(req.done, ESP_ERR_NO_MEM, TAG, "sem");
    tl_msg_t m = {.type = MSG_PROBE};
    m.u.probe = &req;
    if (xQueueSend(s_ctl, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(req.done);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(req.done, portMAX_DELAY);
    vSemaphoreDelete(req.done);
    return out->err;
}
