/* tensile_upload core: HTTP status classification, Retry-After, backoff. Pure C11. */
#include "tu_core.h"

#include <string.h>

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* ---- HTTP-date ---------------------------------------------------------- */

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int parse_month(const char *s)
{
    static const char *const M[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    char m[3];
    for (int i = 0; i < 3; i++) {
        char c = s[i];
        if (!is_alpha(c)) return -1;
        m[i] = (char)(c | 0x20);
    }
    for (int i = 0; i < 12; i++)
        if (memcmp(m, M[i], 3) == 0) return i + 1;
    return -1;
}

static const char *parse_uint(const char *p, int max_digits, int *out, int *ndig)
{
    int v = 0, n = 0;
    while (n < max_digits && is_digit(*p)) {
        v = v * 10 + (*p - '0');
        p++;
        n++;
    }
    *out = v;
    if (ndig) *ndig = n;
    return n ? p : NULL;
}

static const char *parse_hms(const char *p, int *h, int *mi, int *s)
{
    int n;
    if (!(p = parse_uint(p, 2, h, &n)) || n != 2 || *p++ != ':') return NULL;
    if (!(p = parse_uint(p, 2, mi, &n)) || n != 2 || *p++ != ':') return NULL;
    if (!(p = parse_uint(p, 2, s, &n)) || n != 2) return NULL;
    return p;
}

int tu_parse_http_date(const char *s, int64_t *out_epoch)
{
    if (!s || !out_epoch) return TU_ERR_ARG;
    const char *p = s;
    while (is_ws(*p)) p++;
    /* weekday name */
    if (!is_alpha(*p)) return TU_ERR_PARSE;
    while (is_alpha(*p)) p++;
    if (*p == ',') p++;
    while (is_ws(*p)) p++;

    int day = 0, mon = 0, year = 0, h = 0, mi = 0, sec = 0, nd = 0;
    if (is_digit(*p)) {
        /* IMF-fixdate "06 Nov 1994 08:49:37 GMT" or RFC 850 "06-Nov-94 08:49:37 GMT" */
        if (!(p = parse_uint(p, 2, &day, NULL))) return TU_ERR_PARSE;
        if (*p != ' ' && *p != '-') return TU_ERR_PARSE;
        p++;
        if ((mon = parse_month(p)) < 0) return TU_ERR_PARSE;
        p += 3;
        if (*p != ' ' && *p != '-') return TU_ERR_PARSE;
        p++;
        if (!(p = parse_uint(p, 4, &year, &nd))) return TU_ERR_PARSE;
        if (nd == 2) year += (year < 70) ? 2000 : 1900;
        else if (nd != 4) return TU_ERR_PARSE;
        if (*p++ != ' ') return TU_ERR_PARSE;
        if (!(p = parse_hms(p, &h, &mi, &sec))) return TU_ERR_PARSE;
        while (is_ws(*p)) p++;
        if (strncmp(p, "GMT", 3) != 0 && strncmp(p, "UTC", 3) != 0) return TU_ERR_PARSE;
    } else {
        /* asctime "Nov  6 08:49:37 1994" */
        if ((mon = parse_month(p)) < 0) return TU_ERR_PARSE;
        p += 3;
        while (*p == ' ') p++;
        if (!(p = parse_uint(p, 2, &day, NULL))) return TU_ERR_PARSE;
        if (*p++ != ' ') return TU_ERR_PARSE;
        if (!(p = parse_hms(p, &h, &mi, &sec))) return TU_ERR_PARSE;
        if (*p++ != ' ') return TU_ERR_PARSE;
        if (!(p = parse_uint(p, 4, &year, &nd)) || nd != 4) return TU_ERR_PARSE;
    }
    if (day < 1 || day > 31 || h > 23 || mi > 59 || sec > 60) return TU_ERR_PARSE;
    *out_epoch = days_from_civil(year, (unsigned)mon, (unsigned)day) * 86400 + h * 3600 + mi * 60 + sec;
    return TU_OK;
}

int32_t tu_parse_retry_after(const char *v, int64_t now_epoch)
{
    if (!v) return -1;
    const char *p = v;
    while (is_ws(*p)) p++;
    if (!*p) return -1;
    if (is_digit(*p)) {
        int64_t n = 0;
        while (is_digit(*p)) {
            if (n <= TU_RETRY_AFTER_MAX_S) n = n * 10 + (*p - '0');
            p++;
        }
        while (is_ws(*p)) p++;
        if (*p) return -1;
        return (int32_t)(n > TU_RETRY_AFTER_MAX_S ? TU_RETRY_AFTER_MAX_S : n);
    }
    int64_t when;
    if (now_epoch <= 0 || tu_parse_http_date(p, &when) != TU_OK) return -1;
    int64_t d = when - now_epoch;
    if (d < 0) d = 0;
    if (d > TU_RETRY_AFTER_MAX_S) d = TU_RETRY_AFTER_MAX_S;
    return (int32_t)d;
}

int tu_parse_status_line(const char *s)
{
    if (!s) return 0;
    while (is_ws(*s)) s++;
    if (strncmp(s, "HTTP/", 5) != 0) return 0;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    int code = 0, n = 0;
    if (!parse_uint(s, 3, &code, &n) || n != 3 || code < 100) return 0;
    return code;
}

/* ---- classification ---------------------------------------------------- */

tu_class_t tu_classify(tu_method_t method, int st, const char *retry_after, int64_t now_epoch)
{
    tu_class_t c = {TU_RES_PERMANENT_ERROR, st, -1};
    if (st <= 0) {
        c.http_status = 0;
        c.result = TU_RES_RETRY_LATER; /* timeout / DNS / TLS / reset: use backoff */
        return c;
    }
    if (st >= 200 && st < 300) {
        switch (method) {
        case TU_METHOD_PROPFIND:
            /* Only 207 proves WebDAV; a 200 is typically a captive portal or login page. */
            c.result = (st == 207) ? TU_RES_OK : TU_RES_PERMANENT_ERROR;
            break;
        case TU_METHOD_MKCOL:
            c.result = (st == 201) ? TU_RES_CREATED : TU_RES_PERMANENT_ERROR;
            break;
        case TU_METHOD_PUT:
            if (st == 201) c.result = TU_RES_CREATED;
            else if (st == 200 || st == 204) c.result = TU_RES_OK;
            else c.result = TU_RES_PERMANENT_ERROR;
            break;
        default: c.result = TU_RES_OK; break;
        }
        return c;
    }
    switch (st) {
    case 401:
    case 403: c.result = TU_RES_AUTH_FAILED; return c;
    case 404: c.result = TU_RES_NOT_FOUND; return c;
    case 405:
        c.result = (method == TU_METHOD_MKCOL) ? TU_RES_ALREADY_EXISTS : TU_RES_PERMANENT_ERROR;
        return c;
    case 409:
        c.result = (method == TU_METHOD_MKCOL || method == TU_METHOD_PUT) ? TU_RES_NOT_FOUND : TU_RES_PERMANENT_ERROR;
        return c;
    case 412: c.result = TU_RES_PRECONDITION_FAILED; return c;
    case 408:
    case 423: /* Locked (Nextcloud file locking) */
    case 425:
    case 429: c.result = TU_RES_RETRY_LATER; break;
    default:
        if (st >= 500 && st != 501 && st != 505) c.result = TU_RES_RETRY_LATER;
        else c.result = TU_RES_PERMANENT_ERROR; /* 1xx, 3xx (no redirects followed), other 4xx, 501, 505 */
        break;
    }
    if (c.result == TU_RES_RETRY_LATER) c.retry_after_s = tu_parse_retry_after(retry_after, now_epoch);
    return c;
}

bool tu_result_is_success(tu_result_t r)
{
    return r == TU_RES_OK || r == TU_RES_CREATED || r == TU_RES_ALREADY_EXISTS;
}

const char *tu_result_name(tu_result_t r)
{
    switch (r) {
    case TU_RES_OK: return "OK";
    case TU_RES_CREATED: return "CREATED";
    case TU_RES_ALREADY_EXISTS: return "ALREADY_EXISTS";
    case TU_RES_PRECONDITION_FAILED: return "PRECONDITION_FAILED";
    case TU_RES_AUTH_FAILED: return "AUTH_FAILED";
    case TU_RES_NOT_FOUND: return "NOT_FOUND";
    case TU_RES_RETRY_LATER: return "RETRY_LATER";
    case TU_RES_PERMANENT_ERROR: return "PERMANENT_ERROR";
    }
    return "?";
}

/* ---- backoff ------------------------------------------------------------ */

uint32_t tu_backoff_ms(uint32_t attempt, tu_rng_fn rng, void *ctx)
{
    uint64_t d = TU_BACKOFF_MIN_MS;
    for (uint32_t i = 0; i < attempt && d < TU_BACKOFF_MAX_MS; i++) d <<= 1;
    if (d > TU_BACKOFF_MAX_MS) d = TU_BACKOFF_MAX_MS;
    if (rng) {
        uint64_t span = d / 5; /* +/- 20 % */
        uint64_t r = rng(ctx) % (2 * span + 1);
        d = d - span + r;
    }
    if (d < TU_BACKOFF_MIN_MS) d = TU_BACKOFF_MIN_MS;
    if (d > TU_BACKOFF_MAX_MS) d = TU_BACKOFF_MAX_MS;
    return (uint32_t)d;
}

uint32_t tu_retry_delay_ms(const tu_class_t *c, uint32_t attempt, tu_rng_fn rng, void *ctx)
{
    if (c && c->retry_after_s >= 0) {
        uint64_t d = (uint64_t)c->retry_after_s * 1000u;
        if (rng && d >= 10) d += rng(ctx) % (d / 10 + 1);
        if (d < 1000) d = 1000;
        if (d > 3600000u) d = 3600000u;
        return (uint32_t)d;
    }
    return tu_backoff_ms(attempt, rng, ctx);
}
