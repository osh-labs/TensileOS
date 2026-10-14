/* tensile_upload core: URL building, auth header, renaming helpers. Pure C11. */
#include "tu_core.h"

#include <stdio.h>
#include <string.h>

/* ---- small output buffer helper --------------------------------------- */
typedef struct {
    char *buf;
    size_t sz;
    size_t len;
    bool overflow;
} sbuf_t;

static void sb_init(sbuf_t *b, char *buf, size_t sz)
{
    b->buf = buf;
    b->sz = sz;
    b->len = 0;
    b->overflow = false;
    if (sz) buf[0] = '\0';
}

static void sb_putc(sbuf_t *b, char c)
{
    if (b->len + 1 < b->sz) {
        b->buf[b->len++] = c;
        b->buf[b->len] = '\0';
    } else {
        b->overflow = true;
    }
}

static void sb_puts(sbuf_t *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) sb_putc(b, s[i]);
}

static const char HEX[] = "0123456789ABCDEF";

static bool is_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

/* ---- path / URL -------------------------------------------------------- */

int tu_path_encode(const char *rel, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return TU_ERR_ARG;
    out[0] = '\0';
    if (!rel) return TU_OK;
    sbuf_t b;
    sb_init(&b, out, out_sz);

    const char *p = rel;
    while (*p == '/') p++;
    bool wrote_segment = false;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - seg);
        if (n == 0) { /* empty segment from "//" */
            while (*p == '/') p++;
            continue;
        }
        if ((n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) {
            out[0] = '\0';
            return TU_ERR_PATH;
        }
        if (wrote_segment) sb_putc(&b, '/');
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)seg[i];
            if (c < 0x20 || c == 0x7F || c == '\\') {
                out[0] = '\0';
                return TU_ERR_PATH;
            }
            if (is_unreserved(c)) {
                sb_putc(&b, (char)c);
            } else {
                sb_putc(&b, '%');
                sb_putc(&b, HEX[c >> 4]);
                sb_putc(&b, HEX[c & 0x0F]);
            }
        }
        wrote_segment = true;
        if (*p == '/') {
            while (*p == '/') p++;
            if (!*p) sb_putc(&b, '/'); /* preserve trailing slash */
        }
    }
    return b.overflow ? TU_ERR_OVERFLOW : TU_OK;
}

int tu_url_build(const char *base_url, const char *rel_path, char *out, size_t out_sz)
{
    if (!base_url || !out || out_sz == 0) return TU_ERR_ARG;
    out[0] = '\0';
    size_t blen = strlen(base_url);
    if (blen == 0) return TU_ERR_ARG;
    sbuf_t b;
    sb_init(&b, out, out_sz);
    sb_puts(&b, base_url, blen);
    if (base_url[blen - 1] != '/') sb_putc(&b, '/');
    if (b.overflow) return TU_ERR_OVERFLOW;
    if (rel_path && *rel_path) {
        int rc = tu_path_encode(rel_path, out + b.len, out_sz - b.len);
        if (rc == TU_ERR_PATH) {
            out[0] = '\0';
            return rc;
        }
        if (rc != TU_OK) return rc;
    }
    return TU_OK;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tu_percent_decode(const char *in, char *out, size_t out_sz)
{
    if (!in || !out || out_sz == 0) return TU_ERR_ARG;
    sbuf_t b;
    sb_init(&b, out, out_sz);
    for (const char *p = in; *p; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            sb_putc(&b, (char)(hexval(p[1]) * 16 + hexval(p[2])));
            p += 2;
        } else {
            sb_putc(&b, *p);
        }
    }
    return b.overflow ? TU_ERR_OVERFLOW : TU_OK;
}

/* ---- base64 / auth ------------------------------------------------------ */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encodes the virtual concatenation of up to three byte ranges. */
typedef struct {
    const uint8_t *p[3];
    size_t n[3];
} segs_t;

static int b64_segs(const segs_t *s, char *out, size_t out_sz)
{
    size_t total = s->n[0] + s->n[1] + s->n[2];
    size_t need = 4 * ((total + 2) / 3);
    if (need + 1 > out_sz) return TU_ERR_OVERFLOW;
    size_t seg = 0, idx = 0, o = 0;
    uint8_t tri[3];
    size_t got = 0;
    for (size_t k = 0; k < total; k++) {
        while (idx >= s->n[seg]) {
            seg++;
            idx = 0;
        }
        tri[got++] = s->p[seg][idx++];
        if (got == 3) {
            out[o++] = B64[tri[0] >> 2];
            out[o++] = B64[((tri[0] & 0x03) << 4) | (tri[1] >> 4)];
            out[o++] = B64[((tri[1] & 0x0F) << 2) | (tri[2] >> 6)];
            out[o++] = B64[tri[2] & 0x3F];
            got = 0;
        }
    }
    if (got == 1) {
        out[o++] = B64[tri[0] >> 2];
        out[o++] = B64[(tri[0] & 0x03) << 4];
        out[o++] = '=';
        out[o++] = '=';
    } else if (got == 2) {
        out[o++] = B64[tri[0] >> 2];
        out[o++] = B64[((tri[0] & 0x03) << 4) | (tri[1] >> 4)];
        out[o++] = B64[(tri[1] & 0x0F) << 2];
        out[o++] = '=';
    }
    out[o] = '\0';
    tu_secure_zero(tri, sizeof tri);
    return (int)o;
}

int tu_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz)
{
    if ((!in && in_len) || !out || out_sz == 0) return TU_ERR_ARG;
    segs_t s = {{in, NULL, NULL}, {in_len, 0, 0}};
    return b64_segs(&s, out, out_sz);
}

int tu_basic_auth_header(const char *user, const char *password, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return TU_ERR_ARG;
    out[0] = '\0';
    if (!user || !password || !*user || strchr(user, ':')) return TU_ERR_ARG;
    static const char prefix[] = "Basic ";
    const size_t plen = sizeof prefix - 1;
    if (out_sz <= plen) return TU_ERR_OVERFLOW;
    memcpy(out, prefix, plen);
    segs_t s = {{(const uint8_t *)user, (const uint8_t *)":", (const uint8_t *)password},
                {strlen(user), 1, strlen(password)}};
    int n = b64_segs(&s, out + plen, out_sz - plen);
    if (n < 0) {
        tu_secure_zero(out, out_sz);
        return n;
    }
    return TU_OK;
}

void tu_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* ---- collision renamer ------------------------------------------------- */

int tu_collision_name(const char *path, const char *suffix, unsigned attempt, char *out, size_t out_sz)
{
    if (!path || !out || out_sz == 0) return TU_ERR_ARG;
    out[0] = '\0';
    if (!suffix || !*suffix) suffix = TU_COLLISION_SUFFIX;
    size_t len = strlen(path);
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (!*name) return TU_ERR_ARG; /* path ends in '/' */
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL; /* ".hidden" has no extension */
    size_t stem_end = dot ? (size_t)(dot - path) : len;

    char tail[16] = "";
    if (attempt >= 2) snprintf(tail, sizeof tail, "_%u", attempt);

    sbuf_t b;
    sb_init(&b, out, out_sz);
    sb_puts(&b, path, stem_end);
    if (attempt >= 1) {
        sb_putc(&b, '_');
        sb_puts(&b, suffix, strlen(suffix));
        if (attempt >= 2) sb_puts(&b, tail, strlen(tail));
    }
    if (dot) sb_puts(&b, dot, len - stem_end);
    return b.overflow ? TU_ERR_OVERFLOW : TU_OK;
}

static bool ends_with_ci(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    if (m > n) return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], c = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != c) return false;
    }
    return true;
}

const char *tu_content_type_for(const char *path)
{
    if (!path) return "application/octet-stream";
    if (ends_with_ci(path, ".csv")) return "text/csv";
    if (ends_with_ci(path, ".pdf")) return "application/pdf";
    if (ends_with_ci(path, ".json")) return "application/json";
    if (ends_with_ci(path, ".txt")) return "text/plain";
    return "application/octet-stream";
}

/* ---- connection test message ----------------------------------------- */

/* Extract "<user>" and "<folder>" from ".../files/<user>/<folder>/". */
static void base_names(const char *base, char *user, size_t usz, char *folder, size_t fsz)
{
    snprintf(user, usz, "%s", "the account");
    snprintf(folder, fsz, "%s", "root folder");
    if (!base) return;
    const char *f = strstr(base, "/files/");
    if (!f) return;
    f += 7;
    const char *e = strchr(f, '/');
    if (!e) e = f + strlen(f);
    char enc[96];
    size_t n = (size_t)(e - f);
    if (n == 0 || n >= sizeof enc) return;
    memcpy(enc, f, n);
    enc[n] = '\0';
    tu_percent_decode(enc, user, usz);
    if (!*e) return;
    const char *g = e + 1;
    /* folder = remainder without trailing '/' (may be nested) */
    size_t m = strlen(g);
    while (m && g[m - 1] == '/') m--;
    if (m == 0 || m >= sizeof enc) {
        snprintf(folder, fsz, "%s", "(root)");
        return;
    }
    memcpy(enc, g, m);
    enc[m] = '\0';
    tu_percent_decode(enc, folder, fsz);
}

bool tu_conn_test_message(const char *base_url, tu_net_err_t net, int http_status, bool is_collection, char *out,
                          size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    char user[48], folder[64];
    base_names(base_url, user, sizeof user, folder, sizeof folder);
    switch (net) {
    case TU_NET_OK: break;
    case TU_NET_DNS: snprintf(out, out_sz, "Server not found (DNS) - check host and Wi-Fi"); return false;
    case TU_NET_CONNECT: snprintf(out, out_sz, "Cannot reach server - check network"); return false;
    case TU_NET_TLS: snprintf(out, out_sz, "Secure connection failed (TLS/certificate) - check clock and host"); return false;
    case TU_NET_TIMEOUT: snprintf(out, out_sz, "Server did not respond (timeout)"); return false;
    default: snprintf(out, out_sz, "Connection error"); return false;
    }
    if (http_status == 207) {
        if (!is_collection) {
            snprintf(out, out_sz, "%s is a file, not a folder", folder);
            return false;
        }
        snprintf(out, out_sz, "Connected");
        return true;
    }
    if (http_status == 401) {
        snprintf(out, out_sz, "App password rejected");
        return false;
    }
    if (http_status == 403) {
        snprintf(out, out_sz, "Access denied (HTTP 403) - account disabled or blocked");
        return false;
    }
    if (http_status == 404) {
        snprintf(out, out_sz, "Folder %s not found under %s", folder, user);
        return false;
    }
    if (http_status >= 300 && http_status < 400) {
        snprintf(out, out_sz, "Unexpected redirect (HTTP %d) - check server URL", http_status);
        return false;
    }
    if (http_status == 429 || http_status >= 500) {
        snprintf(out, out_sz, "Server busy or failing (HTTP %d) - try again later", http_status);
        return false;
    }
    if (http_status >= 200 && http_status < 300) {
        snprintf(out, out_sz, "Not a WebDAV server (HTTP %d) - captive portal or wrong URL?", http_status);
        return false;
    }
    snprintf(out, out_sz, "Unexpected server response (HTTP %d)", http_status);
    return false;
}
