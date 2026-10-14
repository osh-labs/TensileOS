/* tensile_upload core: minimal WebDAV PROPFIND (207 multistatus) parser. Pure C11.
 *
 * Not a general XML parser: it tokenises tags, tracks which prefixes are bound
 * to "DAV:" and extracts href/status/getetag/getcontentlength/resourcetype per
 * <response>. Namespace scoping is flattened (last declaration of a prefix
 * wins), which is sufficient for Sabre/Nextcloud output where all xmlns
 * declarations sit on <multistatus>.
 */
#include "tu_core.h"

#include <stdlib.h>
#include <string.h>

const char TU_PROPFIND_BODY[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
    "<d:resourcetype/><d:getetag/><d:getcontentlength/><d:getlastmodified/>"
    "</d:prop></d:propfind>\n";

#define NS_MAX 12
#define NAME_MAX_LEN 32
#define CAP_MAX 512

typedef enum { CAP_NONE = 0, CAP_HREF, CAP_RESP_STATUS, CAP_PS_STATUS, CAP_ETAG, CAP_CLEN } cap_t;

typedef struct {
    char prefix[NAME_MAX_LEN]; /* "" = default namespace */
    bool is_dav;
} ns_t;

typedef struct {
    ns_t ns[NS_MAX];
    int n_ns;
    bool seen_multistatus;
    bool in_response, in_propstat, in_prop, in_resourcetype;
    cap_t cap;
    char cap_buf[CAP_MAX];
    size_t cap_len;
    /* current response */
    tu_dav_entry_t cur;
    int resp_status;     /* <status> directly under <response> */
    int first_ps_status;
    int ok_ps_status;
    /* current propstat */
    int ps_status;
    bool ps_has_etag;
    char ps_etag[TU_ETAG_MAX];
    int64_t ps_clen;
    bool ps_collection;
    bool ps_has_rtype;
} pf_t;

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static void ns_set(pf_t *s, const char *prefix, size_t plen, const char *uri, size_t ulen)
{
    if (plen >= NAME_MAX_LEN) return;
    bool dav = (ulen == 4 && memcmp(uri, "DAV:", 4) == 0);
    for (int i = 0; i < s->n_ns; i++) {
        if (strlen(s->ns[i].prefix) == plen && memcmp(s->ns[i].prefix, prefix, plen) == 0) {
            s->ns[i].is_dav = dav;
            return;
        }
    }
    if (s->n_ns >= NS_MAX) return;
    memcpy(s->ns[s->n_ns].prefix, prefix, plen);
    s->ns[s->n_ns].prefix[plen] = '\0';
    s->ns[s->n_ns].is_dav = dav;
    s->n_ns++;
}

/* Unknown prefixes are treated leniently as DAV:. */
static bool ns_is_dav(const pf_t *s, const char *prefix, size_t plen)
{
    for (int i = 0; i < s->n_ns; i++)
        if (strlen(s->ns[i].prefix) == plen && memcmp(s->ns[i].prefix, prefix, plen) == 0) return s->ns[i].is_dav;
    return true;
}

/* Decode XML entities from src[0..n) into dst (NUL-terminated), trimming whitespace. */
static void xml_text(const char *src, size_t n, char *dst, size_t dsz)
{
    size_t a = 0, b = n;
    while (a < b && is_ws(src[a])) a++;
    while (b > a && is_ws(src[b - 1])) b--;
    size_t o = 0;
    for (size_t i = a; i < b && o + 1 < dsz;) {
        if (src[i] == '&') {
            const char *semi = memchr(src + i, ';', b - i);
            if (semi && semi - (src + i) <= 10) {
                size_t el = (size_t)(semi - (src + i)) + 1;
                const char *e = src + i;
                unsigned long cp = 0;
                bool ok = true;
                if (el == 5 && memcmp(e, "&amp;", 5) == 0) cp = '&';
                else if (el == 4 && memcmp(e, "&lt;", 4) == 0) cp = '<';
                else if (el == 4 && memcmp(e, "&gt;", 4) == 0) cp = '>';
                else if (el == 6 && memcmp(e, "&quot;", 6) == 0) cp = '"';
                else if (el == 6 && memcmp(e, "&apos;", 6) == 0) cp = '\'';
                else if (el > 3 && e[1] == '#') {
                    char num[12];
                    memcpy(num, e + 2, el - 3);
                    num[el - 3] = '\0';
                    char *end;
                    cp = (num[0] == 'x' || num[0] == 'X') ? strtoul(num + 1, &end, 16) : strtoul(num, &end, 10);
                    ok = (*end == '\0' && cp > 0 && cp <= 0x10FFFF);
                } else {
                    ok = false;
                }
                if (ok) {
                    /* UTF-8 encode */
                    char u[4];
                    size_t ul;
                    if (cp < 0x80) { u[0] = (char)cp; ul = 1; }
                    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); ul = 2; }
                    else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); ul = 3; }
                    else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); ul = 4; }
                    if (o + ul >= dsz) break;
                    memcpy(dst + o, u, ul);
                    o += ul;
                    i += el;
                    continue;
                }
            }
        }
        dst[o++] = src[i++];
    }
    dst[o] = '\0';
}

static void cap_append(pf_t *s, const char *p, size_t n)
{
    if (s->cap == CAP_NONE) return;
    size_t room = CAP_MAX - 1 - s->cap_len;
    if (n > room) n = room;
    memcpy(s->cap_buf + s->cap_len, p, n);
    s->cap_len += n;
}

static void cap_begin(pf_t *s, cap_t c)
{
    s->cap = c;
    s->cap_len = 0;
}

static void cap_end(pf_t *s)
{
    char tmp[CAP_MAX];
    xml_text(s->cap_buf, s->cap_len, tmp, sizeof tmp);
    switch (s->cap) {
    case CAP_HREF:
        strncpy(s->cur.href, tmp, TU_HREF_MAX - 1);
        s->cur.href[TU_HREF_MAX - 1] = '\0';
        break;
    case CAP_RESP_STATUS: s->resp_status = tu_parse_status_line(tmp); break;
    case CAP_PS_STATUS: s->ps_status = tu_parse_status_line(tmp); break;
    case CAP_ETAG:
        strncpy(s->ps_etag, tmp, TU_ETAG_MAX - 1);
        s->ps_etag[TU_ETAG_MAX - 1] = '\0';
        s->ps_has_etag = tmp[0] != '\0';
        break;
    case CAP_CLEN: {
        char *end;
        long long v = strtoll(tmp, &end, 10);
        if (end != tmp && *end == '\0' && v >= 0) s->ps_clen = v;
        break;
    }
    default: break;
    }
    s->cap = CAP_NONE;
}

static void propstat_reset(pf_t *s)
{
    s->ps_status = 0;
    s->ps_has_etag = false;
    s->ps_etag[0] = '\0';
    s->ps_clen = -1;
    s->ps_collection = false;
    s->ps_has_rtype = false;
}

static void propstat_commit(pf_t *s)
{
    int st = s->ps_status ? s->ps_status : 200; /* missing status: assume OK */
    if (!s->first_ps_status) s->first_ps_status = st;
    if (st < 200 || st >= 300) return;
    if (!s->ok_ps_status) s->ok_ps_status = st;
    if (s->ps_has_etag) {
        s->cur.has_etag = true;
        memcpy(s->cur.etag, s->ps_etag, TU_ETAG_MAX);
    }
    if (s->ps_clen >= 0) s->cur.content_length = s->ps_clen;
    if (s->ps_collection) s->cur.is_collection = true;
}

static void response_reset(pf_t *s)
{
    memset(&s->cur, 0, sizeof s->cur);
    s->cur.content_length = -1;
    s->resp_status = 0;
    s->first_ps_status = 0;
    s->ok_ps_status = 0;
}

/* Handle one element event. local = local name ("" for non-DAV elements). */
static void on_element(pf_t *s, const char *local, bool closing, bool self_closing, tu_dav_entry_t *entries,
                       size_t max, size_t *n)
{
    if (!closing) {
        if (strcmp(local, "multistatus") == 0) {
            s->seen_multistatus = true;
        } else if (strcmp(local, "response") == 0 && s->seen_multistatus) {
            s->in_response = true;
            response_reset(s);
        } else if (!s->in_response) {
            /* ignore */
        } else if (strcmp(local, "propstat") == 0) {
            s->in_propstat = true;
            propstat_reset(s);
        } else if (strcmp(local, "prop") == 0 && s->in_propstat) {
            s->in_prop = true;
        } else if (strcmp(local, "href") == 0 && !s->in_propstat) {
            if (!self_closing) cap_begin(s, CAP_HREF);
        } else if (strcmp(local, "status") == 0) {
            if (!self_closing) cap_begin(s, s->in_propstat ? CAP_PS_STATUS : CAP_RESP_STATUS);
        } else if (s->in_prop && strcmp(local, "getetag") == 0) {
            if (!self_closing) cap_begin(s, CAP_ETAG);
        } else if (s->in_prop && strcmp(local, "getcontentlength") == 0) {
            if (!self_closing) cap_begin(s, CAP_CLEN);
        } else if (s->in_prop && strcmp(local, "resourcetype") == 0) {
            s->ps_has_rtype = true;
            if (!self_closing) s->in_resourcetype = true;
        } else if (s->in_resourcetype && strcmp(local, "collection") == 0) {
            s->ps_collection = true;
        }
        if (!self_closing) return;
        /* self-closing elements that open a scope close it immediately */
        if (strcmp(local, "propstat") == 0 && s->in_propstat) {
            propstat_commit(s);
            s->in_propstat = false;
        }
        return;
    }
    /* closing tag */
    if (s->cap != CAP_NONE) {
        bool match = (s->cap == CAP_HREF && strcmp(local, "href") == 0) ||
                     ((s->cap == CAP_RESP_STATUS || s->cap == CAP_PS_STATUS) && strcmp(local, "status") == 0) ||
                     (s->cap == CAP_ETAG && strcmp(local, "getetag") == 0) ||
                     (s->cap == CAP_CLEN && strcmp(local, "getcontentlength") == 0);
        if (match) cap_end(s);
        return; /* foreign closing tag inside a text element: ignore */
    }
    if (strcmp(local, "resourcetype") == 0) {
        s->in_resourcetype = false;
    } else if (strcmp(local, "prop") == 0) {
        s->in_prop = false;
        s->in_resourcetype = false;
    } else if (strcmp(local, "propstat") == 0 && s->in_propstat) {
        propstat_commit(s);
        s->in_propstat = false;
        s->in_prop = false;
    } else if (strcmp(local, "response") == 0 && s->in_response) {
        s->cur.status = s->resp_status ? s->resp_status : (s->ok_ps_status ? s->ok_ps_status : s->first_ps_status);
        if (*n < max) entries[*n] = s->cur;
        (*n)++;
        s->in_response = s->in_propstat = s->in_prop = s->in_resourcetype = false;
    }
}

static const char *find_str(const char *p, const char *end, const char *needle)
{
    size_t nl = strlen(needle);
    for (; p + nl <= end; p++)
        if (memcmp(p, needle, nl) == 0) return p;
    return NULL;
}

int tu_propfind_parse(const char *xml, size_t len, tu_dav_entry_t *entries, size_t max_entries, size_t *n_total)
{
    if (!xml || (max_entries && !entries)) return TU_ERR_ARG;
    pf_t *s = calloc(1, sizeof *s); /* ~1.5 KB; not a hot path */
    if (!s) return TU_ERR_NOMEM;
    size_t n = 0;
    const char *p = xml, *end = xml + len;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) {
            cap_append(s, p, (size_t)(end - p));
            break;
        }
        if (lt > p) cap_append(s, p, (size_t)(lt - p));
        p = lt;
        if (end - p >= 4 && memcmp(p, "<!--", 4) == 0) {
            const char *e = find_str(p + 4, end, "-->");
            if (!e) break;
            p = e + 3;
            continue;
        }
        if (end - p >= 9 && memcmp(p, "<![CDATA[", 9) == 0) {
            const char *e = find_str(p + 9, end, "]]>");
            if (!e) break;
            cap_append(s, p + 9, (size_t)(e - (p + 9)));
            p = e + 3;
            continue;
        }
        if (end - p >= 2 && (p[1] == '?' || p[1] == '!')) {
            const char *e = memchr(p, '>', (size_t)(end - p));
            if (!e) break;
            p = e + 1;
            continue;
        }
        /* element tag */
        const char *q = p + 1;
        bool closing = false;
        if (q < end && *q == '/') {
            closing = true;
            q++;
        }
        const char *name = q;
        while (q < end && !is_ws(*q) && *q != '>' && *q != '/') q++;
        size_t name_len = (size_t)(q - name);
        /* attributes, until '>' (respecting quotes) */
        bool self_closing = false;
        bool complete = false;
        while (q < end) {
            while (q < end && is_ws(*q)) q++;
            if (q >= end) break;
            if (*q == '>') {
                complete = true;
                q++;
                break;
            }
            if (*q == '/') {
                self_closing = true;
                q++;
                continue;
            }
            const char *an = q;
            while (q < end && !is_ws(*q) && *q != '=' && *q != '>' && *q != '/') q++;
            size_t an_len = (size_t)(q - an);
            while (q < end && is_ws(*q)) q++;
            if (q < end && *q == '=') {
                q++;
                while (q < end && is_ws(*q)) q++;
                if (q < end && (*q == '"' || *q == '\'')) {
                    char quote = *q++;
                    const char *av = q;
                    while (q < end && *q != quote) q++;
                    size_t av_len = (size_t)(q - av);
                    if (q < end) q++;
                    if (an_len == 5 && memcmp(an, "xmlns", 5) == 0) ns_set(s, "", 0, av, av_len);
                    else if (an_len > 6 && memcmp(an, "xmlns:", 6) == 0) ns_set(s, an + 6, an_len - 6, av, av_len);
                }
            } else if (an_len == 0) {
                q++; /* stray char */
            }
        }
        if (!complete) break; /* truncated document */
        p = q;

        /* split prefix:local */
        const char *colon = memchr(name, ':', name_len);
        const char *lname = colon ? colon + 1 : name;
        size_t llen = colon ? name_len - (size_t)(lname - name) : name_len;
        size_t plen = colon ? (size_t)(colon - name) : 0;
        char local[NAME_MAX_LEN];
        if (llen >= NAME_MAX_LEN || !ns_is_dav(s, name, plen)) {
            local[0] = '\0';
        } else {
            memcpy(local, lname, llen);
            local[llen] = '\0';
        }
        on_element(s, local, closing, self_closing, entries, max_entries, &n);
    }

    bool ok = s->seen_multistatus;
    free(s);
    if (n_total) *n_total = n;
    return ok ? TU_OK : TU_ERR_PARSE;
}
