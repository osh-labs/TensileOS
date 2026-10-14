/*
 * tensile_upload ESP-IDF glue: WebDAV over esp_http_client.
 *
 * - TLS server verification via the ESP x509 certificate bundle.
 * - One esp_http_client handle reused for all requests (HTTP/1.1 keep-alive);
 *   the connection is closed only on error or when the server says so.
 * - Every request uses the open/write/fetch_headers/read flow, so bodies are
 *   streamed and esp_http_client never auto-retries: auth_type is NONE, the
 *   Authorization header is ours, max_authorization_retries = -1 and auto
 *   redirects are disabled. A 401/403 latches s_auth_blocked and no further
 *   request is sent until tu_client_init() is called with new credentials.
 * - Credentials are never logged.
 */
#include "tensile_upload.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "tensile_upload";

#define TU_IO_CHUNK        4096
#define TU_PROPFIND_BUF   (32 * 1024)
#define TU_MIN_VALID_EPOCH 1700000000LL /* clock considered synced after 2023-11 */

typedef struct {
    char etag[TU_ETAG_MAX];
    char oc_etag[TU_ETAG_MAX];
    char retry_after[64];
} tu_hdr_capture_t;

static SemaphoreHandle_t s_mutex;
static esp_http_client_handle_t s_client;
static char s_base[TU_URL_MAX];
static char s_auth[TU_AUTH_HDR_MAX]; /* "Basic ..." - secret */
static bool s_auth_blocked;
static int s_timeout_ms = TU_DEFAULT_TIMEOUT_MS;
static tu_hdr_capture_t s_hdr;
static char s_io[TU_IO_CHUNK];

/* ---- helpers ------------------------------------------------------------ */

static void lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

static int64_t now_epoch(void)
{
    time_t t = time(NULL);
    return (t >= TU_MIN_VALID_EPOCH) ? (int64_t)t : 0;
}

static const char *method_name(tu_method_t m)
{
    switch (m) {
    case TU_METHOD_PROPFIND: return "PROPFIND";
    case TU_METHOD_MKCOL: return "MKCOL";
    case TU_METHOD_PUT: return "PUT";
    case TU_METHOD_GET: return "GET";
    case TU_METHOD_DELETE: return "DELETE";
    }
    return "?";
}

static esp_http_client_method_t esp_method(tu_method_t m)
{
    switch (m) {
    case TU_METHOD_PROPFIND: return HTTP_METHOD_PROPFIND;
    case TU_METHOD_MKCOL: return HTTP_METHOD_MKCOL;
    case TU_METHOD_PUT: return HTTP_METHOD_PUT;
    case TU_METHOD_DELETE: return HTTP_METHOD_DELETE;
    default: return HTTP_METHOD_GET;
    }
}

static void copy_hdr(char *dst, size_t dsz, const char *v)
{
    while (*v == ' ' || *v == '\t') v++;
    snprintf(dst, dsz, "%s", v);
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "ETag") == 0) {
            copy_hdr(s_hdr.etag, sizeof s_hdr.etag, evt->header_value);
        } else if (strcasecmp(evt->header_key, "OC-ETag") == 0) {
            copy_hdr(s_hdr.oc_etag, sizeof s_hdr.oc_etag, evt->header_value);
        } else if (strcasecmp(evt->header_key, "Retry-After") == 0) {
            copy_hdr(s_hdr.retry_after, sizeof s_hdr.retry_after, evt->header_value);
        }
    }
    return ESP_OK;
}

/* Classify the last transport failure and drop the (possibly half-used) connection. */
static void transport_failure(esp_err_t err, tu_method_t m, tu_response_t *out)
{
    int tls_code = 0, tls_flags = 0;
    tu_net_err_t net = TU_NET_OTHER;
    if (esp_http_client_get_and_clear_last_tls_error(s_client, &tls_code, &tls_flags) == ESP_OK && tls_code != 0) {
        if (tls_code == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) net = TU_NET_DNS;
        else if (tls_code == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) net = TU_NET_CONNECT;
        else if (tls_code == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) net = TU_NET_TIMEOUT;
        else if (tls_code == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || tls_flags != 0) net = TU_NET_TLS;
    }
    if (net == TU_NET_OTHER) {
        if (err == ESP_ERR_HTTP_EAGAIN || err == -ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_HTTP_READ_TIMEOUT ||
            err == ESP_ERR_TIMEOUT) {
            net = TU_NET_TIMEOUT;
        } else if (err == ESP_ERR_HTTP_CONNECT) {
            net = TU_NET_CONNECT;
        }
    }
    esp_http_client_close(s_client);
    out->err = (err == ESP_OK) ? ESP_FAIL : err;
    out->net = net;
    out->cls = tu_classify(m, 0, NULL, 0);
    ESP_LOGW(TAG, "%s: transport error %s (net=%d, tls=0x%x flags=0x%x)", method_name(m), esp_err_to_name(out->err),
             (int)net, tls_code, tls_flags);
}

static void local_failure(esp_err_t err, tu_response_t *out)
{
    out->err = err;
    out->net = TU_NET_OK;
    out->cls.result = TU_RES_PERMANENT_ERROR;
    out->cls.http_status = 0;
    out->cls.retry_after_s = -1;
}

typedef struct {
    tu_method_t method;
    const char *path;          /* relative, unencoded */
    const char *depth;         /* PROPFIND */
    tu_precond_t precond;
    const char *etag;          /* IF_MATCH */
    const char *content_type;
    const char *body;          /* in-memory body ... */
    int body_len;
    FILE *file;                /* ... or streamed file */
    int file_len;
    char *resp_buf;            /* optional response body capture */
    int resp_buf_sz;
    int resp_len;
    bool resp_truncated;
} tu_req_t;

/* Caller holds the mutex. */
static esp_err_t do_request(tu_req_t *r, tu_response_t *out)
{
    memset(out, 0, sizeof *out);
    out->cls.retry_after_s = -1;
    r->resp_len = 0;
    r->resp_truncated = false;

    if (!s_client) {
        local_failure(ESP_ERR_INVALID_STATE, out);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_auth_blocked) {
        out->err = ESP_ERR_INVALID_STATE;
        out->cls.result = TU_RES_AUTH_FAILED;
        ESP_LOGW(TAG, "%s %s skipped: credentials rejected earlier", method_name(r->method), r->path);
        return ESP_ERR_INVALID_STATE;
    }

    char url[TU_URL_MAX];
    int rc = tu_url_build(s_base, r->path, url, sizeof url);
    if (rc != TU_OK) {
        ESP_LOGE(TAG, "bad remote path (rc=%d)", rc);
        local_failure(ESP_ERR_INVALID_ARG, out);
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_set_url(s_client, url);
    esp_http_client_set_method(s_client, esp_method(r->method));
    esp_http_client_delete_header(s_client, "Depth");
    esp_http_client_delete_header(s_client, "If-None-Match");
    esp_http_client_delete_header(s_client, "If-Match");
    esp_http_client_delete_header(s_client, "Content-Type");
    esp_http_client_set_header(s_client, "Authorization", s_auth);
    if (r->depth) esp_http_client_set_header(s_client, "Depth", r->depth);
    if (r->precond == TU_PRECOND_IF_NONE_MATCH_STAR) esp_http_client_set_header(s_client, "If-None-Match", "*");
    else if (r->precond == TU_PRECOND_IF_MATCH && r->etag) esp_http_client_set_header(s_client, "If-Match", r->etag);
    if (r->content_type) esp_http_client_set_header(s_client, "Content-Type", r->content_type);

    memset(&s_hdr, 0, sizeof s_hdr);
    int write_len = r->file ? r->file_len : r->body_len;

    esp_err_t err = esp_http_client_open(s_client, write_len);
    if (err != ESP_OK) {
        transport_failure(err, r->method, out);
        return out->err;
    }

    /* request body */
    if (r->body && r->body_len > 0) {
        int off = 0;
        while (off < r->body_len) {
            int w = esp_http_client_write(s_client, r->body + off, r->body_len - off);
            if (w <= 0) {
                transport_failure(ESP_ERR_HTTP_WRITE_DATA, r->method, out);
                return out->err;
            }
            off += w;
        }
    } else if (r->file) {
        int sent = 0;
        while (sent < r->file_len) {
            size_t want = (size_t)(r->file_len - sent) < sizeof s_io ? (size_t)(r->file_len - sent) : sizeof s_io;
            size_t got = fread(s_io, 1, want, r->file);
            if (got == 0) {
                /* file shrank or read error: abort the request mid-body */
                ESP_LOGE(TAG, "PUT %s: local read failed at %d/%d", r->path, sent, r->file_len);
                esp_http_client_close(s_client);
                out->err = ESP_ERR_INVALID_SIZE;
                out->cls = tu_classify(r->method, 0, NULL, 0); /* retry later */
                return out->err;
            }
            size_t off = 0;
            while (off < got) {
                int w = esp_http_client_write(s_client, s_io + off, (int)(got - off));
                if (w <= 0) {
                    transport_failure(ESP_ERR_HTTP_WRITE_DATA, r->method, out);
                    return out->err;
                }
                off += (size_t)w;
            }
            sent += (int)got;
        }
    }

    int64_t clen = esp_http_client_fetch_headers(s_client);
    if (clen < 0) {
        transport_failure((esp_err_t)clen, r->method, out);
        return out->err;
    }
    int status = esp_http_client_get_status_code(s_client);

    /* response body */
    bool reusable = true;
    if (r->resp_buf && r->resp_buf_sz > 1) {
        int n = esp_http_client_read_response(s_client, r->resp_buf, r->resp_buf_sz - 1);
        if (n < 0) {
            n = 0;
            reusable = false;
        }
        r->resp_buf[n] = '\0';
        r->resp_len = n;
        r->resp_truncated = !esp_http_client_is_complete_data_received(s_client);
    }
    if (reusable && esp_http_client_flush_response(s_client, NULL) != ESP_OK) reusable = false;
    if (!reusable || !esp_http_client_is_persistent_connection(s_client)) esp_http_client_close(s_client);

    out->err = ESP_OK;
    out->net = TU_NET_OK;
    out->cls = tu_classify(r->method, status, s_hdr.retry_after[0] ? s_hdr.retry_after : NULL, now_epoch());
    if (out->cls.result == TU_RES_AUTH_FAILED) {
        s_auth_blocked = true;
        esp_http_client_close(s_client);
        ESP_LOGE(TAG, "%s %s -> %d: credentials rejected, uploads stopped (no retry)", method_name(r->method),
                 r->path, status);
    } else {
        ESP_LOGI(TAG, "%s %s -> %d %s%s%s", method_name(r->method), r->path[0] ? r->path : "/", status,
                 tu_result_name(out->cls.result), s_hdr.retry_after[0] ? " retry-after=" : "", s_hdr.retry_after);
    }
    return ESP_OK;
}

/* ---- init ----------------------------------------------------------------- */

static void client_teardown(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    tu_secure_zero(s_auth, sizeof s_auth);
}

esp_err_t tu_client_init(const tu_config_t *cfg)
{
    if (!cfg || !cfg->app_password || !cfg->app_password[0]) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    const char *base = cfg->base_url ? cfg->base_url : TU_DEFAULT_BASE_URL;
    const char *user = cfg->user ? cfg->user : TU_DEFAULT_USER;
    if (strncmp(base, "https://", 8) != 0) {
        ESP_LOGE(TAG, "base URL must be https://");
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    client_teardown();
    esp_err_t ret = ESP_OK;
    if (tu_url_build(base, NULL, s_base, sizeof s_base) != TU_OK ||
        tu_basic_auth_header(user, cfg->app_password, s_auth, sizeof s_auth) != TU_OK) {
        ret = ESP_ERR_INVALID_ARG;
        goto out;
    }
    s_timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : TU_DEFAULT_TIMEOUT_MS;

    esp_http_client_config_t hc = {
        .url = s_base,
        .auth_type = HTTP_AUTH_TYPE_NONE,   /* we send Authorization ourselves */
        .max_authorization_retries = -1,    /* never re-send credentials on 401 */
        .disable_auto_redirect = true,      /* a redirect means a misconfigured base URL */
        .timeout_ms = s_timeout_ms,
        .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,          /* TCP keep-alive on the persistent connection */
        .buffer_size = 2048,
        .buffer_size_tx = 2048,             /* request line + headers (long encoded paths) */
        .user_agent = "TensileHMI/0.1 (ESP32-P4)",
    };
    s_client = esp_http_client_init(&hc);
    if (!s_client) {
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
    s_auth_blocked = false;
    ESP_LOGI(TAG, "client ready: %s (user %s, password ***)", s_base, user);
out:
    if (ret != ESP_OK) client_teardown();
    unlock();
    return ret;
}

void tu_client_deinit(void)
{
    if (!s_mutex) return;
    lock();
    client_teardown();
    unlock();
}

bool tu_client_auth_blocked(void) { return s_auth_blocked; }

/* ---- requests -------------------------------------------------------------- */

static esp_err_t propfind_locked(const char *path, int depth, tu_propfind_out_t *out)
{
    out->n_entries = 0;
    out->truncated = false;
    if (depth != 0 && depth != 1) {
        local_failure(ESP_ERR_INVALID_ARG, &out->resp);
        return ESP_ERR_INVALID_ARG;
    }
    char *buf = heap_caps_malloc(TU_PROPFIND_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(TU_PROPFIND_BUF);
    if (!buf) {
        local_failure(ESP_ERR_NO_MEM, &out->resp);
        return ESP_ERR_NO_MEM;
    }
    tu_req_t r = {
        .method = TU_METHOD_PROPFIND,
        .path = path ? path : "",
        .depth = depth ? "1" : "0",
        .content_type = "application/xml; charset=utf-8",
        .body = TU_PROPFIND_BODY,
        .body_len = (int)strlen(TU_PROPFIND_BODY),
        .resp_buf = buf,
        .resp_buf_sz = TU_PROPFIND_BUF,
    };
    esp_err_t err = do_request(&r, &out->resp);
    if (err == ESP_OK && out->resp.cls.http_status == 207) {
        size_t n = 0;
        if (tu_propfind_parse(buf, (size_t)r.resp_len, out->entries, out->entries ? out->max_entries : 0, &n) !=
            TU_OK) {
            ESP_LOGW(TAG, "PROPFIND: 207 body is not a multistatus");
            out->resp.cls.result = TU_RES_PERMANENT_ERROR;
        }
        out->n_entries = n;
        out->truncated = r.resp_truncated;
    }
    free(buf);
    return err;
}

esp_err_t tu_propfind(const char *path, int depth, tu_propfind_out_t *out)
{
    if (!out || !s_mutex) return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t err = propfind_locked(path, depth, out);
    unlock();
    return err;
}

static esp_err_t mkcol_locked(const char *path, tu_response_t *out)
{
    tu_req_t r = {.method = TU_METHOD_MKCOL, .path = path};
    return do_request(&r, out);
}

esp_err_t tu_mkcol(const char *path, tu_response_t *out)
{
    tu_response_t tmp;
    if (!path || !s_mutex) return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t err = mkcol_locked(path, out ? out : &tmp);
    unlock();
    return err;
}

static esp_err_t put_locked(const char *remote_path, const char *local_path, tu_precond_t precond, const char *etag,
                            char *out_etag, tu_response_t *out)
{
    if (out_etag) out_etag[0] = '\0';
    if (precond == TU_PRECOND_IF_MATCH && (!etag || !etag[0])) {
        local_failure(ESP_ERR_INVALID_ARG, out);
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "PUT: cannot open %s", local_path);
        local_failure(ESP_ERR_NOT_FOUND, out);
        return ESP_ERR_NOT_FOUND;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || size > INT_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        local_failure(ESP_ERR_INVALID_SIZE, out);
        return ESP_ERR_INVALID_SIZE;
    }
    tu_req_t r = {
        .method = TU_METHOD_PUT,
        .path = remote_path,
        .precond = precond,
        .etag = etag,
        .content_type = tu_content_type_for(remote_path),
        .file = f,
        .file_len = (int)size,
    };
    esp_err_t err = do_request(&r, out);
    fclose(f);
    if (err == ESP_OK && out_etag && tu_result_is_success(out->cls.result)) {
        const char *e = s_hdr.etag[0] ? s_hdr.etag : s_hdr.oc_etag;
        snprintf(out_etag, TU_ETAG_MAX, "%s", e);
    }
    return err;
}

esp_err_t tu_put_file(const char *remote_path, const char *local_path, tu_precond_t precond, const char *etag,
                      char *out_etag, tu_response_t *out)
{
    tu_response_t tmp;
    if (!remote_path || !local_path || !s_mutex) return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t err = put_locked(remote_path, local_path, precond, etag, out_etag, out ? out : &tmp);
    unlock();
    return err;
}

esp_err_t tu_upload_test_file(const char *remote_path, const char *local_path, const tu_plan_opts_t *opts,
                              tu_upload_result_t *out)
{
    if (!remote_path || !local_path || !out || !s_mutex) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->retry_after_s = -1;
    out->result = TU_RES_PERMANENT_ERROR;

    tu_plan_t *plan = malloc(sizeof *plan); /* ~1 KB, keep it off the task stack */
    if (!plan) return ESP_ERR_NO_MEM;
    if (tu_plan_init(plan, remote_path, opts) != TU_OK) {
        free(plan);
        ESP_LOGE(TAG, "upload: invalid remote path");
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    for (;;) {
        tu_step_t st = tu_plan_step(plan);
        if (st.kind == TU_STEP_DONE || st.kind == TU_STEP_FAILED) break;
        tu_response_t resp;
        if (st.kind == TU_STEP_MKCOL) {
            mkcol_locked(st.path, &resp);
        } else {
            char etag[TU_ETAG_MAX];
            put_locked(st.path, local_path, st.precond, st.etag, etag, &resp);
            if (tu_result_is_success(resp.cls.result)) snprintf(out->etag, sizeof out->etag, "%s", etag);
        }
        out->requests++;
        tu_plan_feed(plan, &resp.cls);
    }
    unlock();

    tu_step_t fin = tu_plan_step(plan);
    snprintf(out->remote_path, sizeof out->remote_path, "%s", plan->path);
    out->http_status = plan->last.http_status;
    if (fin.kind == TU_STEP_DONE) {
        out->result = plan->last.result;
        out->created = plan->created;
        ESP_LOGI(TAG, "uploaded %s (%u requests, etag %s)", out->remote_path, out->requests,
                 out->etag[0] ? out->etag : "-");
    } else {
        out->result = plan->fail.result;
        out->retry_after_s = plan->fail.retry_after_s;
        ESP_LOGW(TAG, "upload of %s failed: %s (HTTP %d)", remote_path, tu_result_name(out->result),
                 out->http_status);
    }
    free(plan);
    return tu_result_is_success(out->result) ? ESP_OK : ESP_FAIL;
}

esp_err_t tu_test_connection(tu_conn_test_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (!s_mutex || !s_client) {
        snprintf(out->message, sizeof out->message, "Upload not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_auth_blocked) {
        out->http_status = 401;
        snprintf(out->message, sizeof out->message, "App password rejected - enter a new one");
        return ESP_ERR_INVALID_STATE;
    }
    tu_dav_entry_t entry;
    tu_propfind_out_t pf = {.entries = &entry, .max_entries = 1};
    esp_err_t err = tu_propfind("", 0, &pf);
    out->http_status = pf.resp.cls.http_status;
    out->net = pf.resp.net;
    if (out->http_status == 207 && pf.n_entries == 0) {
        snprintf(out->message, sizeof out->message, "Invalid WebDAV response");
        out->ok = false;
    } else {
        bool is_coll = (pf.n_entries > 0) ? entry.is_collection : true;
        out->ok = tu_conn_test_message(s_base, out->net, out->http_status, is_coll, out->message, sizeof out->message);
    }
    ESP_LOGI(TAG, "connection test: %s", out->message);
    return (err == ESP_OK && out->ok) ? ESP_OK : (err != ESP_OK ? err : ESP_FAIL);
}
