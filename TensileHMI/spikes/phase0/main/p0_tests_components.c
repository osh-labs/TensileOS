/*
 * Phase 0 tests built on project components: Nextcloud WebDAV (tensile_upload)
 * and the PDF writer (tensile_report).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "tensile_report.h"
#include "tensile_upload.h"

#include "p0.h"

#define P0_REMOTE_DIR "Phase0"

static bool s_dav_ready;

static void device_suffix(char *out, size_t len)
{
    extern esp_err_t esp_read_mac(uint8_t *mac, int type);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, 0 /* ESP_MAC_BASE */);
    snprintf(out, len, "TAB5-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static bool dav_init(void)
{
    if (tu_client_auth_blocked()) {
        p0_log("WebDAV is blocked after an auth failure. Fix the app password and reboot.");
        return false;
    }
    if (s_dav_ready) {
        return true;
    }
    if (!g_p0_cfg.dav_app_password[0]) {
        p0_log("No dav_app_password in %s", P0_CONFIG_PATH);
        return false;
    }
    tu_config_t cfg = {
        .base_url = g_p0_cfg.dav_base_url,
        .user = g_p0_cfg.dav_user,
        .app_password = g_p0_cfg.dav_app_password,
    };
    s_dav_ready = (tu_client_init(&cfg) == ESP_OK);
    return s_dav_ready;
}

/* Uploads one local file into TensileOS/Phase0/ with create-only semantics. */
static bool upload_one(const char *local_path, const char *remote_name, tu_upload_result_t *res)
{
    char remote[160];
    snprintf(remote, sizeof(remote), P0_REMOTE_DIR "/%s", remote_name);
    int64_t t0 = esp_timer_get_time();
    tu_upload_test_file(remote, local_path, NULL, res);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    p0_log("  PUT %s -> %s: %s (HTTP %d, %u requests, %lld ms) etag %s", local_path, res->remote_path,
           tu_result_name(res->result), res->http_status, res->requests, ms, res->etag[0] ? res->etag : "-");
    return res->result == TU_RES_OK || res->result == TU_RES_CREATED;
}

p0_result_t p0_test_webdav(void)
{
    if (!p0_wifi_is_up()) {
        p0_record("webdav", P0_FAIL, "Wi-Fi not connected; run the Wi-Fi test first");
        return P0_FAIL;
    }
    if (!dav_init()) {
        p0_record("webdav", P0_FAIL, "client not initialised");
        return P0_FAIL;
    }

    /* 1. Connection test (PROPFIND Depth 0 on the base). */
    tu_conn_test_t ct;
    int64_t t0 = esp_timer_get_time();
    tu_test_connection(&ct);
    p0_log("PROPFIND base: HTTP %d in %lld ms -> %s", ct.http_status, (esp_timer_get_time() - t0) / 1000,
           ct.message);
    if (!ct.ok) {
        p0_record("webdav", P0_FAIL, "connection test: %s", ct.message);
        return P0_FAIL;
    }

    /* 2. Create-only upload of a small marker file (exercises MKCOL + PUT If-None-Match). */
    char dev[16], name[64], local[64];
    device_suffix(dev, sizeof(dev));
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(name, sizeof(name), "webdav_test_%Y%m%d_%H%M%S.txt", &lt);
    snprintf(local, sizeof(local), P0_OUT_DIR "/webdav_test.txt");
    FILE *f = fopen(local, "w");
    if (!f) {
        p0_record("webdav", P0_FAIL, "cannot write %s", local);
        return P0_FAIL;
    }
    fprintf(f, "TensileOS HMI Phase 0 WebDAV test from %s\n", dev);
    fclose(f);

    tu_upload_result_t r1;
    bool ok1 = upload_one(local, name, &r1);

    /* 3. Same name again: must NOT overwrite; expect 412 then rename to *_TAB5. */
    tu_upload_result_t r2;
    bool ok2 = upload_one(local, name, &r2);
    bool renamed = ok2 && strcmp(r1.remote_path, r2.remote_path) != 0;
    p0_log("Overwrite protection: second upload went to %s (%s)", r2.remote_path,
           renamed ? "renamed, original kept" : "NOT renamed - check If-None-Match handling");

    /* 4. Listing of Phase0/ to confirm both files and ETags. */
    tu_dav_entry_t entries[8];
    tu_propfind_out_t pf = { .entries = entries, .max_entries = 8 };
    tu_propfind(P0_REMOTE_DIR "/", 1, &pf);
    p0_log("PROPFIND Phase0/ depth 1: HTTP %d, %u entries%s", pf.resp.cls.http_status, (unsigned)pf.n_entries,
           pf.truncated ? " (truncated)" : "");

    bool ok = ok1 && renamed && !tu_client_auth_blocked();
    p0_record("webdav", ok ? P0_PASS : P0_FAIL, "PROPFIND %d, PUT %d, re-PUT %s", ct.http_status, r1.http_status,
              renamed ? "renamed (no overwrite)" : "unexpected");
    return ok ? P0_PASS : P0_FAIL;
}

/* -------------------------------------------------------------- PDF proof */

p0_result_t p0_test_pdf(void)
{
    /* The five golden tests from TensileCompanion/Tests/2026-01-20. */
    static const tr_test_row_t rows[] = {
        { "Test 1", "SEG 5mm Dyneema Cord Tack", "Chris Lee", 3.379 },
        { "Test 2", "SEG 5mm Dyneema Cord Tack", "Chris Lee", 3.841 },
        { "Test 3", "SEG 5mm Dyneema Cord Tack", "Chris Lee", 3.440 },
        { "Test 4", "SEG 5mm Dyneema Cord Tack", "Chris Lee", 3.345 },
        { "Test 5", "SEG 5mm Dyneema Cord Tack", "Chris Lee", 3.828 },
    };
    const char *path = P0_OUT_DIR "/phase0_proof.pdf";
    enum { MAX_PAGES = 4 };
    uint32_t offsets[TR_OBJECTS_FOR_PAGES(MAX_PAGES)];
    uint32_t pages[MAX_PAGES];

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    /* newlib has no tm_gmtoff: derive the offset by reinterpreting UTC fields as local time. */
    struct tm g;
    gmtime_r(&now, &g);
    g.tm_isdst = lt.tm_isdst;
    int tz_off_min = (int)(difftime(now, mktime(&g)) / 60);

    int64_t t0 = esp_timer_get_time();
    tr_file_t *f = tr_open_file(path);
    if (!f) {
        p0_record("pdf", P0_FAIL, "cannot open %s", path);
        return P0_FAIL;
    }
    tr_doc_t *doc = heap_caps_calloc(1, sizeof(*doc), MALLOC_CAP_DEFAULT);
    tr_config_t cfg = {
        .write = tr_file_write,
        .write_ctx = f,
        .obj_offsets = offsets,
        .max_objects = TR_OBJECTS_FOR_PAGES(MAX_PAGES),
        .page_objs = pages,
        .max_pages = MAX_PAGES,
        .title = "TensileOS HMI Phase 0 PDF proof",
        .creator = "TensileOS HMI Phase 0",
        .created_utc = (int64_t)now,
        .tz_offset_min = tz_off_min,
    };
    int rc = doc ? tr_doc_init(doc, &cfg) : -1;
    if (rc == TR_OK) {
        rc = tr_phase0_proof(doc, rows, 5, (int64_t)now, tz_off_min);
    }
    if (rc == TR_OK) {
        rc = tr_doc_finish(doc);
    }
    if (rc == TR_OK) {
        rc = tr_file_commit(f);
    } else {
        tr_file_abort(f);
    }
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (rc != TR_OK) {
        free(doc);
        p0_record("pdf", P0_FAIL, "writer error %d", rc);
        return P0_FAIL;
    }
    tr_log_doc_summary(doc, path);
    p0_log("PDF: %u bytes, %u page(s), %u replaced chars, %lld ms", (unsigned)tr_doc_bytes(doc),
           (unsigned)tr_doc_page_count(doc), (unsigned)tr_doc_replacements(doc), ms);
    free(doc);

    if (p0_wifi_is_up() && dav_init()) {
        char name[64];
        strftime(name, sizeof(name), "phase0_proof_%Y%m%d_%H%M%S.pdf", &lt);
        tu_upload_result_t r;
        upload_one(path, name, &r);
    } else {
        p0_log("Not uploaded (Wi-Fi or WebDAV not ready); copy it from the SD card");
    }
    p0_record("pdf", P0_PASS, "%s written in %lld ms", path, ms);
    return P0_PASS;
}

/* ---------------------------------------------------------- report upload */

p0_result_t p0_upload_report(void)
{
    if (!p0_wifi_is_up() || !dav_init()) {
        p0_record("upload", P0_FAIL, "Wi-Fi or WebDAV not ready");
        return P0_FAIL;
    }
    char dev[16], stamp[32], name[80];
    device_suffix(dev, sizeof(dev));
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &lt);

    /* Record the result first so it is part of the uploaded file. */
    p0_log("Uploading report and battery log as %s", dev);

    const char *files[][2] = {
        { P0_REPORT_PATH, "report" },
        { P0_OUT_DIR "/battery.csv", "battery" },
        { P0_OUT_DIR "/scale_transcript.txt", "scale_transcript" },
    };
    int ok = 0, tried = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (access(files[i][0], F_OK) != 0) {
            continue;
        }
        const char *ext = strrchr(files[i][0], '.');
        snprintf(name, sizeof(name), "%s_%s_%s%s", files[i][1], dev, stamp, ext ? ext : "");
        tu_upload_result_t r;
        tried++;
        ok += upload_one(files[i][0], name, &r) ? 1 : 0;
    }
    p0_record("upload", ok == tried && tried > 0 ? P0_PASS : P0_FAIL, "%d of %d files uploaded to %s/", ok, tried,
              P0_REMOTE_DIR);
    return ok == tried && tried > 0 ? P0_PASS : P0_FAIL;
}
