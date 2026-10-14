/*
 * Host-side replay driver for the tensile_upload planner against tools/webdav_mock.py.
 * Plain HTTP over POSIX sockets (one connection per request), core/ logic unchanged.
 * Built and run by `python3 tools/webdav_mock.py --replay`; not part of the firmware.
 *
 *   tu_replay <http://127.0.0.1:PORT/remote.php/dav/files/USER/TensileOS/> USER PASSWORD REMOTE_PATH LOCAL_FILE
 *
 * Exit: 0 DONE, 2 AUTH_FAILED, 3 RETRY_LATER, 1 other failure.
 * Prints each request and the final outcome; then lists the target folder via PROPFIND.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tu_core.h"

static char g_host[64];
static int g_port;
static char g_base[TU_URL_MAX];
static char g_auth[TU_AUTH_HDR_MAX];

typedef struct {
    int status;
    char etag[TU_ETAG_MAX];
    char retry_after[64];
    char *body;
    size_t body_len;
} resp_t;

static int http(const char *method, const char *rel, const char *extra_hdrs, const char *body, size_t body_len,
                resp_t *r)
{
    memset(r, 0, sizeof *r);
    char url[TU_URL_MAX];
    if (tu_url_build(g_base, rel, url, sizeof url) != TU_OK) return -1;
    const char *target = strchr(url + strlen("http://"), '/');

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_port)};
    inet_pton(AF_INET, g_host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    char hdr[2048];
    int n = snprintf(hdr, sizeof hdr,
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\nAuthorization: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n%s\r\n",
                     method, target, g_host, g_port, g_auth, body_len, extra_hdrs ? extra_hdrs : "");
    if (write(fd, hdr, (size_t)n) != n || (body_len && write(fd, body, body_len) != (ssize_t)body_len)) {
        close(fd);
        return -1;
    }
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap + 1);
    ssize_t k;
    while ((k = read(fd, buf + len, cap - len)) > 0) {
        len += (size_t)k;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
    }
    close(fd);
    buf[len] = '\0';
    r->status = tu_parse_status_line(buf);
    char *hend = strstr(buf, "\r\n\r\n");
    if (hend) {
        *hend = '\0';
        for (char *line = strstr(buf, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
            char *l = line + 2;
            if (strncasecmp(l, "ETag:", 5) == 0) sscanf(l + 5, " %95[^\r\n]", r->etag);
            if (strncasecmp(l, "Retry-After:", 12) == 0) sscanf(l + 12, " %63[^\r\n]", r->retry_after);
        }
        r->body = hend + 4;
        r->body_len = len - (size_t)(hend + 4 - buf);
        memmove(buf, r->body, r->body_len + 1);
        r->body = buf;
    } else {
        free(buf);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s BASE_URL USER PASSWORD REMOTE_PATH LOCAL_FILE\n", argv[0]);
        return 1;
    }
    snprintf(g_base, sizeof g_base, "%s", argv[1]);
    if (sscanf(argv[1], "http://%63[^:]:%d", g_host, &g_port) != 2) return 1;
    if (tu_basic_auth_header(argv[2], argv[3], g_auth, sizeof g_auth) != TU_OK) return 1;
    tu_secure_zero(argv[3], strlen(argv[3]));

    FILE *f = fopen(argv[5], "rb");
    if (!f) return 1;
    static char data[1 << 20];
    size_t dlen = fread(data, 1, sizeof data, f);
    fclose(f);

    tu_plan_t plan;
    if (tu_plan_init(&plan, argv[4], NULL) != TU_OK) return 1;
    for (;;) {
        tu_step_t st = tu_plan_step(&plan);
        if (st.kind == TU_STEP_DONE || st.kind == TU_STEP_FAILED) break;
        resp_t r;
        tu_class_t c;
        if (st.kind == TU_STEP_MKCOL) {
            int rc = http("MKCOL", st.path, NULL, NULL, 0, &r);
            c = tu_classify(TU_METHOD_MKCOL, rc ? 0 : r.status, r.retry_after, 0);
        } else {
            char h[256];
            if (st.precond == TU_PRECOND_IF_NONE_MATCH_STAR) snprintf(h, sizeof h, "If-None-Match: *\r\n");
            else if (st.precond == TU_PRECOND_IF_MATCH) snprintf(h, sizeof h, "If-Match: %s\r\n", st.etag);
            else h[0] = '\0';
            strncat(h, "Content-Type: ", sizeof h - strlen(h) - 1);
            strncat(h, tu_content_type_for(st.path), sizeof h - strlen(h) - 1);
            strncat(h, "\r\n", sizeof h - strlen(h) - 1);
            int rc = http("PUT", st.path, h, data, dlen, &r);
            c = tu_classify(TU_METHOD_PUT, rc ? 0 : r.status, r.retry_after, 0);
            if (r.etag[0]) printf("  etag %s\n", r.etag);
        }
        printf("%s %s -> %d %s\n", st.kind == TU_STEP_MKCOL ? "MKCOL" : "PUT", st.path, c.http_status,
               tu_result_name(c.result));
        free(r.body);
        tu_plan_feed(&plan, &c);
    }
    tu_step_t fin = tu_plan_step(&plan);
    int rc;
    if (fin.kind == TU_STEP_DONE) {
        printf("DONE %s created=%d\n", plan.path, plan.created);
        rc = 0;
    } else {
        printf("FAILED %s retry_after=%d\n", tu_result_name(plan.fail.result), (int)plan.fail.retry_after_s);
        rc = plan.fail.result == TU_RES_AUTH_FAILED ? 2 : plan.fail.result == TU_RES_RETRY_LATER ? 3 : 1;
    }
    if (rc == 2) return rc; /* never send anything after a 401 */

    /* list the folder */
    char dir[TU_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", plan.orig_path);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = '\0';
    resp_t r;
    char hdr[128];
    snprintf(hdr, sizeof hdr, "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n");
    if (http("PROPFIND", dir, hdr, TU_PROPFIND_BODY, strlen(TU_PROPFIND_BODY), &r) == 0 && r.status == 207) {
        tu_dav_entry_t e[32];
        size_t n = 0;
        tu_propfind_parse(r.body, r.body_len, e, 32, &n);
        printf("entries=%zu\n", n);
        for (size_t i = 0; i < n && i < 32; i++) {
            char dec[TU_HREF_MAX];
            tu_percent_decode(e[i].href, dec, sizeof dec);
            printf("  %s %s len=%lld etag=%s status=%d\n", e[i].is_collection ? "dir " : "file", dec,
                   (long long)e[i].content_length, e[i].etag, e[i].status);
        }
    }
    free(r.body);
    return rc;
}
