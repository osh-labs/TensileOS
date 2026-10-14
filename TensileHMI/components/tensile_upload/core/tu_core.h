/*
 * tensile_upload core: pure C11 WebDAV helpers for the Nextcloud upload path.
 *
 * Nothing in here touches the network, the filesystem or ESP-IDF. Everything is
 * host-testable (see TensileHMI/test/host/test_tensile_upload.c).
 *
 * Contents
 *   - URL builder (RFC 3986 per-segment percent-encoding)
 *   - Basic auth header builder, secure zero
 *   - HTTP status classification (+ Retry-After parsing)
 *   - Minimal PROPFIND 207 multistatus parser
 *   - Backoff calculator with injectable RNG
 *   - Collision renamer (name.csv -> name_TAB5.csv -> name_TAB5_2.csv)
 *   - Upload request planner (MKCOL ancestors, PUT If-None-Match: *)
 *   - Connection-test message formatter (build plan Section 6.2)
 */
#ifndef TU_CORE_H
#define TU_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Return codes (0 = OK, negative = error) -------------------------- */
#define TU_OK            0
#define TU_ERR_ARG      (-1)  /* NULL / malformed argument */
#define TU_ERR_OVERFLOW (-2)  /* output buffer too small (output is still NUL-terminated, truncated) */
#define TU_ERR_PATH     (-3)  /* rejected path: '..' or '.' segment, control char, backslash */
#define TU_ERR_PARSE    (-4)  /* unparseable input */
#define TU_ERR_STATE    (-5)  /* planner used in wrong state */
#define TU_ERR_NOMEM    (-6)  /* allocation failed */

/* ---- Sizes ------------------------------------------------------------ */
#define TU_URL_MAX       512  /* full URL incl. NUL */
#define TU_PATH_MAX      256  /* relative remote path (unencoded) incl. NUL */
#define TU_HREF_MAX      384  /* href as returned by server (still percent-encoded) */
#define TU_ETAG_MAX       96  /* ETag incl. quotes, incl. NUL */
#define TU_AUTH_HDR_MAX  256  /* "Basic <b64>" incl. NUL */

/* ---- URL building ----------------------------------------------------- */

/* Percent-encode a relative path: each '/'-separated segment is encoded so only
 * RFC 3986 unreserved characters (ALPHA DIGIT - . _ ~) remain literal; '/' is
 * preserved; UTF-8 bytes become %XX (uppercase hex). Leading '/' is dropped,
 * empty segments ("a//b") are collapsed, a trailing '/' is preserved.
 * Rejects "." and ".." segments, control characters (< 0x20, 0x7F) and '\\'
 * with TU_ERR_PATH. */
int tu_path_encode(const char *rel_path, char *out, size_t out_sz);

/* base_url (already encoded, e.g. ".../files/tensile-hmi/TensileOS/") + encoded
 * rel_path. A missing '/' at the end of base_url is added. rel_path may be ""
 * or NULL to address the base collection itself. */
int tu_url_build(const char *base_url, const char *rel_path, char *out, size_t out_sz);

/* Decode %XX escapes in place-safe manner into out (for comparing hrefs). */
int tu_percent_decode(const char *in, char *out, size_t out_sz);

/* ---- Auth ------------------------------------------------------------- */

/* Standard base64 (RFC 4648, with '=' padding). Returns chars written (excl
 * NUL) or a negative TU_ERR_*. */
int tu_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz);

/* Writes "Basic base64(user ':' password)" into out. user must not contain ':'
 * (RFC 7617). The concatenation is never materialised in a temporary buffer.
 * On error out is zeroed. */
int tu_basic_auth_header(const char *user, const char *password, char *out, size_t out_sz);

/* memset that the optimiser may not remove. */
void tu_secure_zero(void *p, size_t n);

/* ---- HTTP classification --------------------------------------------- */

typedef enum {
    TU_METHOD_PROPFIND = 0,
    TU_METHOD_MKCOL,
    TU_METHOD_PUT,
    TU_METHOD_GET,
    TU_METHOD_DELETE,
} tu_method_t;

typedef enum {
    TU_RES_OK = 0,             /* 2xx success (PROPFIND 207, PUT 200/204) */
    TU_RES_CREATED,            /* 201 (MKCOL, PUT) */
    TU_RES_ALREADY_EXISTS,     /* MKCOL 405: collection exists -> success */
    TU_RES_PRECONDITION_FAILED,/* 412: If-None-Match/If-Match failed */
    TU_RES_AUTH_FAILED,        /* 401/403: STOP the queue, never retry */
    TU_RES_NOT_FOUND,          /* 404, or 409 for MKCOL/PUT (parent missing) */
    TU_RES_RETRY_LATER,        /* 408/423/425/429/5xx or transport error */
    TU_RES_PERMANENT_ERROR,    /* other 4xx, unexpected 1xx/2xx/3xx */
} tu_result_t;

typedef struct {
    tu_result_t result;
    int http_status;       /* 0 = transport error (no response) */
    int32_t retry_after_s; /* >= 0 when the server sent a usable Retry-After, else -1 */
} tu_class_t;

#define TU_RETRY_AFTER_MAX_S 86400 /* clamp absurd Retry-After values to 24 h */

/* Classify a response. retry_after may be NULL. now_epoch (UTC seconds) is used
 * to convert an HTTP-date Retry-After into delta seconds; pass 0 if the clock
 * is not synced (HTTP-date form is then ignored). http_status <= 0 means a
 * transport-level failure (timeout, DNS, TLS) -> RETRY_LATER. */
tu_class_t tu_classify(tu_method_t method, int http_status, const char *retry_after, int64_t now_epoch);

/* True for results the caller treats as success of the request. */
bool tu_result_is_success(tu_result_t r);
const char *tu_result_name(tu_result_t r);

/* Retry-After: delta-seconds or HTTP-date. Returns seconds (clamped to
 * [0, TU_RETRY_AFTER_MAX_S]) or -1 if absent/invalid. */
int32_t tu_parse_retry_after(const char *value, int64_t now_epoch);

/* IMF-fixdate, RFC 850 and asctime forms (RFC 9110 5.6.7). */
int tu_parse_http_date(const char *s, int64_t *out_epoch);

/* Parse "HTTP/1.1 207 Multi-Status" -> 207, or 0 when malformed. */
int tu_parse_status_line(const char *s);

/* ---- PROPFIND multistatus -------------------------------------------- */

typedef struct {
    char href[TU_HREF_MAX];  /* XML-entity-decoded, still percent-encoded */
    int status;              /* response <status>, else status of the 2xx propstat, else first propstat */
    bool has_etag;
    char etag[TU_ETAG_MAX];  /* as sent, incl. quotes: "\"6436d084d4805\"" */
    int64_t content_length;  /* -1 when absent (collections) */
    bool is_collection;
} tu_dav_entry_t;

/* Parse a 207 body. Fills up to max_entries; *n_total receives the number of
 * <response> elements seen (may exceed max_entries). Returns TU_OK, or
 * TU_ERR_PARSE if no <multistatus> element was found. Tolerates any prefix
 * bound to "DAV:" (d:, D:, default namespace), whitespace, comments, CDATA,
 * and ignores elements in non-DAV namespaces (oc:, nc:, s:). */
int tu_propfind_parse(const char *xml, size_t len, tu_dav_entry_t *entries, size_t max_entries, size_t *n_total);

/* The request body the client sends with PROPFIND. */
extern const char TU_PROPFIND_BODY[];

/* ---- Backoff ---------------------------------------------------------- */

#define TU_BACKOFF_MIN_MS   5000u
#define TU_BACKOFF_MAX_MS 900000u  /* 15 min */

typedef uint32_t (*tu_rng_fn)(void *ctx);

/* Delay before retry number `attempt` (0 = first retry): 5 s * 2^attempt,
 * capped at 15 min, with +/-20 % uniform jitter, result clamped to
 * [5 s, 15 min]. rng may be NULL (no jitter). */
uint32_t tu_backoff_ms(uint32_t attempt, tu_rng_fn rng, void *rng_ctx);

/* Delay for a RETRY_LATER classification: honours Retry-After (plus up to
 * 10 % jitter, clamped to [1 s, 1 h]) when present, otherwise tu_backoff_ms. */
uint32_t tu_retry_delay_ms(const tu_class_t *c, uint32_t attempt, tu_rng_fn rng, void *rng_ctx);

/* ---- Collision renamer ----------------------------------------------- */

/* attempt 0 -> path unchanged; 1 -> "<stem>_<suffix><ext>";
 * n >= 2 -> "<stem>_<suffix>_<n><ext>". Only the final path segment is
 * changed; the extension is the text from the last '.' of that segment
 * (a leading '.' does not count). */
int tu_collision_name(const char *path, const char *suffix, unsigned attempt, char *out, size_t out_sz);

#define TU_COLLISION_SUFFIX "TAB5"
#define TU_EDIT_SUFFIX      "hmi-edit"

/* Content-Type for a file name: text/csv, application/pdf, application/json,
 * else application/octet-stream. */
const char *tu_content_type_for(const char *path);

/* ---- Upload planner --------------------------------------------------- */

typedef enum {
    TU_PRECOND_NONE = 0,
    TU_PRECOND_IF_NONE_MATCH_STAR, /* create-only */
    TU_PRECOND_IF_MATCH,           /* replace only if ETag still matches */
} tu_precond_t;

typedef enum {
    TU_STEP_MKCOL = 0,
    TU_STEP_PUT,
    TU_STEP_DONE,   /* success: see tu_plan_t.path / .created */
    TU_STEP_FAILED, /* see tu_plan_t.fail */
} tu_step_kind_t;

typedef struct {
    tu_step_kind_t kind;
    const char *path;      /* relative, unencoded; MKCOL paths end in '/' */
    tu_precond_t precond;  /* PUT only */
    const char *etag;      /* PUT with IF_MATCH only */
} tu_step_t;

#define TU_PLAN_MAX_DEPTH      8
#define TU_PLAN_MAX_COLLISIONS 20
#define TU_PLAN_MAX_STEPS      48

typedef struct {
    tu_precond_t precond;   /* default IF_NONE_MATCH_STAR when opts == NULL */
    const char *etag;       /* required for IF_MATCH */
    bool dirs_known;        /* skip MKCOL and PUT straight away; MKCOL only on 404/409 */
    unsigned max_collisions;/* 0 -> TU_PLAN_MAX_COLLISIONS */
} tu_plan_opts_t;

typedef enum {
    TU_PS_MKCOL = 0, /* MKCOL dirs[dir_idx]: success -> deeper/PUT, 409 -> parent */
    TU_PS_PUT,
    TU_PS_DONE,
    TU_PS_FAILED,
} tu_plan_state_t;

typedef struct {
    tu_plan_state_t state;
    char orig_path[TU_PATH_MAX];  /* file path as requested */
    char path[TU_PATH_MAX];       /* current/final PUT path (after renames) */
    char dir_buf[TU_PATH_MAX];    /* MKCOL path of current step */
    uint16_t dir_end[TU_PLAN_MAX_DEPTH]; /* length of each ancestor prefix in orig_path */
    int n_dirs;
    int dir_idx;
    int dir_ok;                   /* highest ancestor index confirmed to exist (-1 none) */
    bool mkcol_done;
    tu_precond_t precond;
    char etag[TU_ETAG_MAX];
    const char *suffix;
    unsigned collision;
    unsigned max_collisions;
    unsigned steps;
    bool created;                 /* final PUT returned 201 */
    tu_class_t last;              /* last classified response */
    tu_class_t fail;              /* classification that caused FAILED */
} tu_plan_t;

/* remote_path like "Tests/2026-10-14/name.csv" (relative to base, no leading
 * '/'). Ancestors are MKCOL'd deepest-first; on 409 the planner walks up until
 * a MKCOL succeeds, then back down, then PUTs. The base collection itself is
 * assumed to exist. */
int tu_plan_init(tu_plan_t *p, const char *remote_path, const tu_plan_opts_t *opts);

/* Current request to perform (or DONE/FAILED). */
tu_step_t tu_plan_step(const tu_plan_t *p);

/* Feed the classified response of the current step. */
int tu_plan_feed(tu_plan_t *p, const tu_class_t *c);

/* ---- Connection test --------------------------------------------------- */

typedef enum {
    TU_NET_OK = 0,      /* an HTTP response was received */
    TU_NET_DNS,
    TU_NET_CONNECT,
    TU_NET_TLS,         /* handshake / certificate verification */
    TU_NET_TIMEOUT,
    TU_NET_OTHER,
} tu_net_err_t;

/* Human-readable result of PROPFIND Depth:0 on the base URL. is_collection is
 * the parsed resourcetype of the 207 entry (pass true if not parsed). User and
 * folder names for the 404 message are taken from base_url
 * (".../files/<user>/<folder>/"). Returns true if "Connected". */
bool tu_conn_test_message(const char *base_url, tu_net_err_t net, int http_status, bool is_collection,
                          char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
#endif /* TU_CORE_H */
