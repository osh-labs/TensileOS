/*
 * tensile_upload: Nextcloud WebDAV upload client (build plan Section 6).
 *
 * Phase 0 scope: a synchronous WebDAV client over esp_http_client (TLS via the
 * certificate bundle, HTTP keep-alive across requests). The Phase 2 persistent
 * queue drives it through tu_upload_test_file(), whose result carries
 * everything a queue entry needs (final remote path, ETag, classification,
 * Retry-After).
 *
 * Return convention for request functions:
 *   ESP_OK      an HTTP response was received (ANY status). Inspect
 *               out->resp.cls.result - success is tu_result_is_success().
 *   ESP_ERR_*   no HTTP response (DNS/TLS/timeout, bad argument, local file
 *               error); out->resp.cls is still filled (RETRY_LATER for
 *               network failures).
 *   ESP_ERR_INVALID_STATE  client not initialised, or blocked after a
 *               401/403 (no request is sent). Call tu_client_init() with new
 *               credentials to unblock.
 *
 * Thread safety: all functions serialise on an internal mutex.
 * Secrets: the app password is never logged and is not retained; only the
 * derived Authorization header is kept in RAM (zeroed on deinit).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TU_DEFAULT_BASE_URL "https://nx80213.your-storageshare.de/remote.php/dav/files/tensile-hmi/TensileOS/"
#define TU_DEFAULT_USER     "tensile-hmi"
#define TU_DEFAULT_TIMEOUT_MS 15000

typedef struct {
    const char *base_url;     /* already percent-encoded; NULL -> TU_DEFAULT_BASE_URL */
    const char *user;         /* NULL -> TU_DEFAULT_USER */
    const char *app_password; /* required; copied into the auth header only */
    int timeout_ms;           /* 0 -> TU_DEFAULT_TIMEOUT_MS */
} tu_config_t;

/* Outcome of one HTTP exchange. */
typedef struct {
    tu_class_t cls;           /* result + http_status + retry_after_s */
    tu_net_err_t net;         /* TU_NET_OK when a response was received */
    esp_err_t err;            /* transport-level error, ESP_OK otherwise */
} tu_response_t;

typedef struct {
    tu_response_t resp;
    tu_dav_entry_t *entries;  /* caller-provided array (may be NULL) */
    size_t max_entries;
    size_t n_entries;         /* number of <response> elements seen */
    bool truncated;           /* body exceeded the receive buffer */
} tu_propfind_out_t;

typedef struct {
    tu_result_t result;       /* final classification */
    int http_status;          /* status of the last request */
    int32_t retry_after_s;    /* -1 if none */
    bool created;             /* PUT returned 201 (new file) */
    char remote_path[TU_PATH_MAX]; /* final path after collision renames */
    char etag[TU_ETAG_MAX];   /* ETag of the uploaded file ("" if not sent) */
    unsigned requests;        /* number of HTTP requests issued */
} tu_upload_result_t;

typedef struct {
    bool ok;                  /* "Connected" */
    int http_status;
    tu_net_err_t net;
    char message[96];         /* human-readable, for Settings -> Upload */
} tu_conn_test_t;

/* (Re)initialise. Safe to call again with new credentials; clears the
 * auth-blocked latch. */
esp_err_t tu_client_init(const tu_config_t *cfg);
void tu_client_deinit(void);

/* True after any 401/403 until tu_client_init() is called again. */
bool tu_client_auth_blocked(void);

/* PROPFIND path (relative to base; "" = base) with Depth 0 or 1. */
esp_err_t tu_propfind(const char *path, int depth, tu_propfind_out_t *out);

/* MKCOL path (relative). out may be NULL. */
esp_err_t tu_mkcol(const char *path, tu_response_t *out);

/* PUT local file to remote_path, streaming in chunks. etag is used with
 * TU_PRECOND_IF_MATCH. out_etag (TU_ETAG_MAX bytes, may be NULL) receives the
 * ETag (or OC-ETag) response header. */
esp_err_t tu_put_file(const char *remote_path, const char *local_path, tu_precond_t precond, const char *etag,
                      char *out_etag, tu_response_t *out);

/* Run the planner: MKCOL missing ancestors, PUT with If-None-Match: * (or
 * If-Match when opts->precond says so), collision renames. opts may be NULL.
 * Returns ESP_OK iff out->result is a success. */
esp_err_t tu_upload_test_file(const char *remote_path, const char *local_path, const tu_plan_opts_t *opts,
                              tu_upload_result_t *out);

/* PROPFIND Depth: 0 on the base URL; message per build plan 6.2. */
esp_err_t tu_test_connection(tu_conn_test_t *out);

#ifdef __cplusplus
}
#endif
