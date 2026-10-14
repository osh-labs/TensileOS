/* tensile_report core: document structure, object/xref bookkeeping, graphics operators. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tr_internal.h"

/* ---- output ------------------------------------------------------------- */

static void flush_buf(tr_doc_t *doc)
{
    if (doc->buf_len == 0) {
        return;
    }
    if (doc->write && doc->err == TR_OK && doc->write(doc->write_ctx, doc->buf, doc->buf_len) != 0) {
        doc->err = TR_ERR_IO;
    }
    doc->buf_len = 0;
}

void tr__put(tr_doc_t *doc, const void *data, size_t len)
{
    if (doc->err != TR_OK) {
        return;
    }
    const uint8_t *p = (const uint8_t *)data;
    doc->offset += (uint32_t)len;
    if (!doc->write) {
        return; /* dry run: count only */
    }
    for (size_t i = 0; i < len; i++) {
        /* FNV-1a, two seeds, for the trailer /ID */
        doc->hash[0] = (doc->hash[0] ^ p[i]) * 0x100000001b3ULL;
        doc->hash[1] = (doc->hash[1] ^ p[i]) * 0x100000001b3ULL;
    }
    while (len > 0) {
        size_t room = sizeof(doc->buf) - doc->buf_len;
        size_t n = len < room ? len : room;
        memcpy(doc->buf + doc->buf_len, p, n);
        doc->buf_len = (uint16_t)(doc->buf_len + n);
        p += n;
        len -= n;
        if (doc->buf_len == sizeof(doc->buf)) {
            flush_buf(doc);
            if (doc->err != TR_OK) {
                return;
            }
        }
    }
}

void tr__puts(tr_doc_t *doc, const char *s) { tr__put(doc, s, strlen(s)); }

void tr__putc(tr_doc_t *doc, char c) { tr__put(doc, &c, 1); }

void tr__put_uint(tr_doc_t *doc, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[sizeof(tmp) - 1 - n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < (int)sizeof(tmp));
    tr__put(doc, tmp + sizeof(tmp) - n, (size_t)n);
}

void tr__put_num(tr_doc_t *doc, double v, char sep)
{
    char tmp[32];
    int n = 0;
    if (!isfinite(v)) {
        v = 0.0;
    }
    if (v > 1e7) {
        v = 1e7;
    } else if (v < -1e7) {
        v = -1e7;
    }
    long long m = llround(v * 1000.0);
    int neg = m < 0;
    unsigned long long a = (unsigned long long)(neg ? -m : m);
    unsigned long long ip = a / 1000u;
    unsigned frac = (unsigned)(a % 1000u);
    char digits[24];
    int nd = 0;
    do {
        digits[nd++] = (char)('0' + ip % 10u);
        ip /= 10u;
    } while (ip);
    if (neg && (a != 0)) {
        tmp[n++] = '-';
    }
    while (nd) {
        tmp[n++] = digits[--nd];
    }
    if (frac) {
        tmp[n++] = '.';
        tmp[n++] = (char)('0' + frac / 100u);
        frac %= 100u;
        if (frac) {
            tmp[n++] = (char)('0' + frac / 10u);
            frac %= 10u;
            if (frac) {
                tmp[n++] = (char)('0' + frac);
            }
        }
    }
    if (sep) {
        tmp[n++] = sep;
    }
    tr__put(doc, tmp, (size_t)n);
}

int tr__fail(tr_doc_t *doc, int err)
{
    if (doc->err == TR_OK) {
        doc->err = err;
    }
    return doc->err;
}

int tr__check_page(tr_doc_t *doc)
{
    if (doc->err != TR_OK) {
        return doc->err;
    }
    if (!doc->in_page) {
        return tr__fail(doc, TR_ERR_STATE);
    }
    return TR_OK;
}

/* ---- objects ------------------------------------------------------------ */

static void begin_obj(tr_doc_t *doc, uint32_t num)
{
    if (doc->err != TR_OK) {
        return;
    }
    if (num == 0 || num > doc->max_objects) {
        tr__fail(doc, TR_ERR_FULL);
        return;
    }
    doc->obj_offsets[num - 1] = doc->offset;
    tr__put_uint(doc, num);
    tr__puts(doc, " 0 obj\n");
}

static void end_obj(tr_doc_t *doc) { tr__puts(doc, "\nendobj\n"); }

static uint32_t alloc_obj(tr_doc_t *doc)
{
    if (doc->next_obj > doc->max_objects) {
        tr__fail(doc, TR_ERR_FULL);
        return 0;
    }
    return doc->next_obj++;
}

/* ---- time --------------------------------------------------------------- */

typedef struct {
    int y, mo, d, h, mi, s;
} civil_t;

static civil_t civil_from_unix(int64_t t)
{
    civil_t c;
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    c.h = (int)(rem / 3600);
    c.mi = (int)(rem % 3600 / 60);
    c.s = (int)(rem % 60);
    /* Howard Hinnant's civil_from_days */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    c.d = (int)(doy - (153 * mp + 2) / 5 + 1);
    c.mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    c.y = (int)(y + (c.mo <= 2));
    return c;
}

void tr_format_datetime(char *out, size_t out_size, int64_t t_utc, int tz_offset_min)
{
    civil_t c = civil_from_unix(t_utc + (int64_t)tz_offset_min * 60);
    if (out && out_size) {
        snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d", c.y % 10000, c.mo, c.d, c.h, c.mi,
                 c.s);
    }
}

static void put_pdf_date(tr_doc_t *doc, int64_t t_utc, int tz)
{
    char b[40];
    civil_t c = civil_from_unix(t_utc + (int64_t)tz * 60);
    if (tz == 0) {
        snprintf(b, sizeof b, "(D:%04d%02d%02d%02d%02d%02dZ)", c.y % 10000, c.mo, c.d, c.h, c.mi,
                 c.s);
    } else {
        int a = tz < 0 ? -tz : tz;
        snprintf(b, sizeof b, "(D:%04d%02d%02d%02d%02d%02d%c%02d'%02d')", c.y % 10000, c.mo, c.d,
                 c.h, c.mi, c.s, tz < 0 ? '-' : '+', (a / 60) % 100, a % 60);
    }
    tr__puts(doc, b);
}

/* /Info text string: literal when printable ASCII, else UTF-16BE hex with BOM. */
static void put_text_string(tr_doc_t *doc, const char *s)
{
    int ascii = 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 32 || *p > 126) {
            ascii = 0;
            break;
        }
    }
    if (ascii) {
        tr__putc(doc, '(');
        for (const char *p = s; *p; p++) {
            if (*p == '(' || *p == ')' || *p == '\\') {
                tr__putc(doc, '\\');
            }
            tr__putc(doc, *p);
        }
        tr__putc(doc, ')');
        return;
    }
    static const char hx[] = "0123456789ABCDEF";
    tr__puts(doc, "<FEFF");
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int extra;
        if (*p < 0x80) {
            cp = *p;
            extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            extra = 3;
        } else {
            cp = 0xFFFD;
            extra = 0;
        }
        p++;
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) {
                cp = 0xFFFD;
                break;
            }
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            cp = 0xFFFD;
        }
        uint16_t units[2];
        int nu = 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            units[0] = (uint16_t)(0xD800 | (cp >> 10));
            units[1] = (uint16_t)(0xDC00 | (cp & 0x3FF));
            nu = 2;
        } else {
            units[0] = (uint16_t)cp;
        }
        for (int i = 0; i < nu; i++) {
            char h[4] = {hx[units[i] >> 12], hx[(units[i] >> 8) & 15], hx[(units[i] >> 4) & 15],
                         hx[units[i] & 15]};
            tr__put(doc, h, 4);
        }
    }
    tr__putc(doc, '>');
}

static void put_info_entry(tr_doc_t *doc, const char *key, const char *val)
{
    if (val && *val) {
        tr__puts(doc, key);
        tr__putc(doc, ' ');
        put_text_string(doc, val);
        tr__putc(doc, '\n');
    }
}

/* ---- document ----------------------------------------------------------- */

int tr_doc_init(tr_doc_t *doc, const tr_config_t *cfg)
{
    if (!doc) {
        return TR_ERR_ARG;
    }
    memset(doc, 0, sizeof(*doc));
    if (!cfg || !cfg->obj_offsets || !cfg->page_objs || cfg->max_pages == 0 ||
        cfg->max_objects < TR_OBJECTS_FOR_PAGES(1)) {
        doc->err = TR_ERR_ARG;
        return doc->err;
    }
    doc->write = cfg->write;
    doc->write_ctx = cfg->write_ctx;
    doc->obj_offsets = cfg->obj_offsets;
    doc->max_objects = cfg->max_objects;
    doc->page_objs = cfg->page_objs;
    doc->max_pages = cfg->max_pages;
    doc->next_obj = TR_OBJ_FIRST_FREE;
    doc->on_page_begin = cfg->on_page_begin;
    doc->on_page_end = cfg->on_page_end;
    doc->hook_user = cfg->hook_user;
    doc->hash[0] = 0xcbf29ce484222325ULL;
    doc->hash[1] = 0x84222325cbf29ce4ULL;
    memset(doc->obj_offsets, 0, sizeof(uint32_t) * doc->max_objects);

    /* Header with a binary comment so transfer tools treat the file as binary. */
    tr__puts(doc, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    begin_obj(doc, TR_OBJ_INFO);
    tr__puts(doc, "<<\n/Producer (TensileOS HMI)\n");
    put_info_entry(doc, "/Title", cfg->title);
    put_info_entry(doc, "/Author", cfg->author);
    put_info_entry(doc, "/Subject", cfg->subject);
    put_info_entry(doc, "/Creator", cfg->creator);
    tr__puts(doc, "/CreationDate ");
    put_pdf_date(doc, cfg->created_utc, cfg->tz_offset_min);
    tr__puts(doc, "\n>>");
    end_obj(doc);

    begin_obj(doc, TR_OBJ_FONT_REG);
    tr__puts(doc, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    end_obj(doc);
    begin_obj(doc, TR_OBJ_FONT_BOLD);
    tr__puts(doc,
             "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
    end_obj(doc);
    begin_obj(doc, TR_OBJ_RESOURCES);
    tr__puts(doc, "<< /ProcSet [/PDF /Text] /Font << /F1 ");
    tr__put_uint(doc, TR_OBJ_FONT_REG);
    tr__puts(doc, " 0 R /F2 ");
    tr__put_uint(doc, TR_OBJ_FONT_BOLD);
    tr__puts(doc, " 0 R >> >>");
    end_obj(doc);
    return doc->err;
}

int tr_page_begin(tr_doc_t *doc)
{
    if (!doc) {
        return TR_ERR_ARG;
    }
    if (doc->err != TR_OK) {
        return doc->err;
    }
    if (doc->in_page || doc->finished) {
        return tr__fail(doc, TR_ERR_STATE);
    }
    if (doc->page_count >= doc->max_pages) {
        return tr__fail(doc, TR_ERR_FULL);
    }
    /* content stream, its /Length, and the page object */
    uint32_t content = alloc_obj(doc);
    uint32_t length = alloc_obj(doc);
    uint32_t page = alloc_obj(doc);
    if (doc->err != TR_OK) {
        return doc->err;
    }
    (void)length;
    doc->page_objs[doc->page_count++] = page;
    doc->content_obj = content;
    begin_obj(doc, content);
    tr__puts(doc, "<< /Length ");
    tr__put_uint(doc, content + 1);
    tr__puts(doc, " 0 R >>\nstream\n");
    doc->stream_start = doc->offset;
    doc->in_page = 1;
    if (doc->on_page_begin && !doc->in_hook) {
        doc->in_hook = 1;
        doc->on_page_begin(doc, doc->page_count, doc->hook_user);
        doc->in_hook = 0;
    }
    return doc->err;
}

int tr_page_end(tr_doc_t *doc)
{
    if (!doc) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (doc->on_page_end && !doc->in_hook) {
        doc->in_hook = 1;
        doc->on_page_end(doc, doc->page_count, doc->hook_user);
        doc->in_hook = 0;
    }
    uint32_t len = doc->offset - doc->stream_start;
    tr__puts(doc, "\nendstream");
    end_obj(doc);
    uint32_t content = doc->content_obj;
    begin_obj(doc, content + 1);
    tr__put_uint(doc, len);
    end_obj(doc);
    begin_obj(doc, content + 2);
    tr__puts(doc, "<< /Type /Page /Parent ");
    tr__put_uint(doc, TR_OBJ_PAGES);
    tr__puts(doc, " 0 R /MediaBox [0 0 612 792] /Resources ");
    tr__put_uint(doc, TR_OBJ_RESOURCES);
    tr__puts(doc, " 0 R /Contents ");
    tr__put_uint(doc, content);
    tr__puts(doc, " 0 R >>");
    end_obj(doc);
    doc->in_page = 0;
    return doc->err;
}

int tr_page_break(tr_doc_t *doc)
{
    if (tr_page_end(doc) != TR_OK) {
        return doc->err;
    }
    return tr_page_begin(doc);
}

int tr_doc_finish(tr_doc_t *doc)
{
    if (!doc) {
        return TR_ERR_ARG;
    }
    if (doc->err != TR_OK) {
        return doc->err;
    }
    if (doc->finished) {
        return tr__fail(doc, TR_ERR_STATE);
    }
    if (doc->in_page) {
        tr_page_end(doc);
    }
    if (doc->page_count == 0) { /* a PDF needs at least one page */
        tr_page_begin(doc);
        tr_page_end(doc);
    }
    begin_obj(doc, TR_OBJ_PAGES);
    tr__puts(doc, "<< /Type /Pages /Count ");
    tr__put_uint(doc, doc->page_count);
    tr__puts(doc, " /Kids [");
    for (uint32_t i = 0; i < doc->page_count; i++) {
        if (i) {
            tr__putc(doc, (i % 10u) ? ' ' : '\n');
        }
        tr__put_uint(doc, doc->page_objs[i]);
        tr__puts(doc, " 0 R");
    }
    tr__puts(doc, "] >>");
    end_obj(doc);
    begin_obj(doc, TR_OBJ_CATALOG);
    tr__puts(doc, "<< /Type /Catalog /Pages ");
    tr__put_uint(doc, TR_OBJ_PAGES);
    tr__puts(doc, " 0 R >>");
    end_obj(doc);

    uint32_t size = doc->next_obj; /* objects 1..next_obj-1 plus entry 0 */
    uint32_t xref_at = doc->offset;
    tr__puts(doc, "xref\n0 ");
    tr__put_uint(doc, size);
    tr__puts(doc, "\n0000000000 65535 f \n");
    for (uint32_t i = 1; i < size && doc->err == TR_OK; i++) {
        char e[24];
        snprintf(e, sizeof e, "%010lu 00000 n \n", (unsigned long)doc->obj_offsets[i - 1]);
        tr__put(doc, e, 20);
    }
    uint64_t h0 = doc->hash[0], h1 = doc->hash[1];
    char id[80];
    snprintf(id, sizeof id, "<%08lx%08lx%08lx%08lx>", (unsigned long)(h0 >> 32),
             (unsigned long)(h0 & 0xffffffffu), (unsigned long)(h1 >> 32),
             (unsigned long)(h1 & 0xffffffffu));
    tr__puts(doc, "trailer\n<< /Size ");
    tr__put_uint(doc, size);
    tr__puts(doc, " /Root ");
    tr__put_uint(doc, TR_OBJ_CATALOG);
    tr__puts(doc, " 0 R /Info ");
    tr__put_uint(doc, TR_OBJ_INFO);
    tr__puts(doc, " 0 R /ID [");
    tr__puts(doc, id);
    tr__puts(doc, id);
    tr__puts(doc, "] >>\nstartxref\n");
    tr__put_uint(doc, xref_at);
    tr__puts(doc, "\n%%EOF\n");
    flush_buf(doc);
    doc->finished = 1;
    return doc->err;
}

int tr_doc_error(const tr_doc_t *doc) { return doc ? doc->err : TR_ERR_ARG; }
uint32_t tr_doc_page_count(const tr_doc_t *doc) { return doc ? doc->page_count : 0; }
uint32_t tr_doc_replacements(const tr_doc_t *doc) { return doc ? doc->replaced : 0; }
uint32_t tr_doc_bytes(const tr_doc_t *doc) { return doc ? doc->offset : 0; }
int tr_doc_is_dry_run(const tr_doc_t *doc) { return doc && !doc->write; }

/* ---- graphics ----------------------------------------------------------- */

tr_rgb_t tr_rgb_hex(uint32_t rgb)
{
    tr_rgb_t c = {(float)((rgb >> 16) & 0xFF) / 255.0f, (float)((rgb >> 8) & 0xFF) / 255.0f,
                  (float)(rgb & 0xFF) / 255.0f};
    return c;
}

static int op(tr_doc_t *doc, const char *s)
{
    tr__puts(doc, s);
    return doc->err;
}

int tr_save(tr_doc_t *doc) { return tr__check_page(doc) ? doc->err : op(doc, "q\n"); }
int tr_restore(tr_doc_t *doc) { return tr__check_page(doc) ? doc->err : op(doc, "Q\n"); }

static int set_rgb(tr_doc_t *doc, tr_rgb_t c, const char *opname)
{
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    tr__put_num(doc, c.r, ' ');
    tr__put_num(doc, c.g, ' ');
    tr__put_num(doc, c.b, ' ');
    return op(doc, opname);
}

int tr_set_stroke_rgb(tr_doc_t *doc, tr_rgb_t c) { return set_rgb(doc, c, "RG\n"); }
int tr_set_fill_rgb(tr_doc_t *doc, tr_rgb_t c) { return set_rgb(doc, c, "rg\n"); }

int tr_set_line_width(tr_doc_t *doc, double w)
{
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (w < 0) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    tr__put_num(doc, w, ' ');
    return op(doc, "w\n");
}

int tr_set_dash(tr_doc_t *doc, const double *pattern, int n, double phase)
{
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (n < 0 || (n > 0 && !pattern)) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    tr__putc(doc, '[');
    for (int i = 0; i < n; i++) {
        tr__put_num(doc, pattern[i], i + 1 < n ? ' ' : 0);
    }
    tr__puts(doc, "] ");
    tr__put_num(doc, n ? phase : 0, ' ');
    return op(doc, "d\n");
}

int tr_set_line_style(tr_doc_t *doc, tr_line_style_t style)
{
    static const double dashed[] = {6, 3};
    static const double dotted[] = {1.5, 2};
    switch (style) {
    case TR_LINE_DASHED:
        return tr_set_dash(doc, dashed, 2, 0);
    case TR_LINE_DOTTED:
        return tr_set_dash(doc, dotted, 2, 0);
    case TR_LINE_SOLID:
    default:
        return tr_set_dash(doc, NULL, 0, 0);
    }
}

int tr_line(tr_doc_t *doc, double x1, double y1, double x2, double y2)
{
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    tr__put_num(doc, x1, ' ');
    tr__put_num(doc, y1, ' ');
    tr__puts(doc, "m ");
    tr__put_num(doc, x2, ' ');
    tr__put_num(doc, y2, ' ');
    return op(doc, "l S\n");
}

int tr_polyline(tr_doc_t *doc, const double *xy, int n)
{
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (!xy || n < 2) {
        return n < 2 && xy ? TR_OK : tr__fail(doc, TR_ERR_ARG);
    }
    for (int i = 0; i < n; i++) {
        tr__put_num(doc, xy[2 * i], ' ');
        tr__put_num(doc, xy[2 * i + 1], ' ');
        tr__puts(doc, i == 0 ? "m\n" : "l\n");
    }
    return op(doc, "S\n");
}

int tr_rect(tr_doc_t *doc, double x, double y, double w, double h, tr_paint_t paint)
{
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    const char *o = paint == TR_PAINT_FILL ? "f\n" : paint == TR_PAINT_FILL_STROKE ? "B\n" : "S\n";
    tr__put_num(doc, x, ' ');
    tr__put_num(doc, y, ' ');
    tr__put_num(doc, w, ' ');
    tr__put_num(doc, h, ' ');
    tr__puts(doc, "re ");
    return op(doc, o);
}

int tr_clip_rect(tr_doc_t *doc, double x, double y, double w, double h)
{
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    tr__put_num(doc, x, ' ');
    tr__put_num(doc, y, ' ');
    tr__put_num(doc, w, ' ');
    tr__put_num(doc, h, ' ');
    return op(doc, "re W n\n");
}

/* ---- ticks -------------------------------------------------------------- */

static double nice_num(double x, int round_it)
{
    double e = floor(log10(x));
    double f = x / pow(10.0, e);
    double nf;
    if (round_it) {
        nf = f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10;
    } else {
        nf = f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10;
    }
    return nf * pow(10.0, e);
}

int tr_nice_ticks(double lo, double hi, int max_ticks, tr_ticks_t *out)
{
    if (!out || !isfinite(lo) || !isfinite(hi)) {
        return TR_ERR_ARG;
    }
    if (max_ticks < 2) {
        max_ticks = 2;
    }
    if (hi < lo) {
        double t = lo;
        lo = hi;
        hi = t;
    }
    if (hi - lo < 1e-12) {
        double pad = fabs(lo) > 1e-12 ? fabs(lo) * 0.1 : 1.0;
        lo -= pad;
        hi += pad;
    }
    double step = nice_num(nice_num(hi - lo, 0) / (max_ticks - 1), 1);
    double start = floor(lo / step + 1e-9) * step;
    double end = ceil(hi / step - 1e-9) * step;
    int count = (int)llround((end - start) / step) + 1;
    while (count > max_ticks) { /* rounding pushed us over: widen the step */
        step = nice_num(step * 1.5, 0);
        start = floor(lo / step + 1e-9) * step;
        end = ceil(hi / step - 1e-9) * step;
        count = (int)llround((end - start) / step) + 1;
    }
    int dec = 0;
    while (dec < 6) {
        double s = step * pow(10.0, dec);
        if (fabs(s - round(s)) < 1e-6 * (s > 1 ? s : 1)) {
            break;
        }
        dec++;
    }
    out->start = start;
    out->step = step;
    out->count = count;
    out->decimals = dec;
    return TR_OK;
}
