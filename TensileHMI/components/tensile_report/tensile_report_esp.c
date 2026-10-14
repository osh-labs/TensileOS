/* tensile_report ESP-IDF glue: SD-card file sink with atomic replace (write <path>.tmp, fsync, rename). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "tensile_report.h"

static const char *TAG = "tensile_report";

#define TR_FILE_PATH_MAX 128
#define TR_FILE_VBUF 4096 /* stdio buffer: fewer, larger SD writes */

struct tr_file {
    FILE *fp;
    int failed;
    char path[TR_FILE_PATH_MAX];
    char tmp[TR_FILE_PATH_MAX + 4];
    char *vbuf;
};

tr_file_t *tr_open_file(const char *path)
{
    if (!path || strlen(path) >= TR_FILE_PATH_MAX) {
        ESP_LOGE(TAG, "bad path");
        return NULL;
    }
    tr_file_t *f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    snprintf(f->path, sizeof f->path, "%s", path);
    snprintf(f->tmp, sizeof f->tmp, "%s.tmp", path);
    f->fp = fopen(f->tmp, "wb");
    if (!f->fp) {
        ESP_LOGE(TAG, "open %s failed: errno %d", f->tmp, errno);
        free(f);
        return NULL;
    }
    f->vbuf = malloc(TR_FILE_VBUF);
    if (f->vbuf) {
        setvbuf(f->fp, f->vbuf, _IOFBF, TR_FILE_VBUF);
    }
    return f;
}

int tr_file_write(void *ctx, const void *buf, size_t len)
{
    tr_file_t *f = (tr_file_t *)ctx;
    if (!f || !f->fp || f->failed) {
        return -1;
    }
    if (len && fwrite(buf, 1, len, f->fp) != len) {
        ESP_LOGE(TAG, "write %s failed: errno %d", f->tmp, errno);
        f->failed = 1;
        return -1;
    }
    return 0;
}

static void release(tr_file_t *f)
{
    free(f->vbuf);
    free(f);
}

int tr_file_commit(tr_file_t *f)
{
    if (!f) {
        return TR_ERR_ARG;
    }
    int rc = TR_OK;
    if (f->failed || fflush(f->fp) != 0 || fsync(fileno(f->fp)) != 0) {
        ESP_LOGE(TAG, "flush %s failed: errno %d", f->tmp, errno);
        rc = TR_ERR_IO;
    }
    if (fclose(f->fp) != 0 && rc == TR_OK) {
        rc = TR_ERR_IO;
    }
    f->fp = NULL;
    if (rc == TR_OK) {
        /* FAT rename() does not replace an existing file. */
        unlink(f->path);
        if (rename(f->tmp, f->path) != 0) {
            /* Keep the complete .tmp: it is the only copy left after the unlink above. */
            ESP_LOGE(TAG, "rename %s failed: errno %d; complete file kept as %s", f->tmp, errno, f->tmp);
            release(f);
            return TR_ERR_IO;
        }
    }
    if (rc != TR_OK) {
        unlink(f->tmp);
    }
    release(f);
    return rc;
}

void tr_file_abort(tr_file_t *f)
{
    if (!f) {
        return;
    }
    if (f->fp) {
        fclose(f->fp);
    }
    unlink(f->tmp);
    release(f);
}

void tr_log_doc_summary(const tr_doc_t *doc, const char *path)
{
    if (!doc) {
        return;
    }
    if (tr_doc_replacements(doc) > 0) {
        ESP_LOGW(TAG, "%s: %lu unmappable character(s) replaced with '?'", path ? path : "pdf",
                 (unsigned long)tr_doc_replacements(doc));
    }
    ESP_LOGI(TAG, "%s: %lu page(s), %lu bytes, status %d", path ? path : "pdf",
             (unsigned long)tr_doc_page_count(doc), (unsigned long)tr_doc_bytes(doc),
             tr_doc_error(doc));
}
