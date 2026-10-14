/* Host tests for tensile_report (PDF writer). Writes OUT_DIR/phase0_proof.pdf and
 * OUT_DIR/table_multipage.pdf; check them with TensileHMI/tools/check_pdf.sh. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tensile_report.h"
#include "tinytest.h"

#define MAX_PAGES 16
#define MAX_OBJS TR_OBJECTS_FOR_PAGES(MAX_PAGES)

/* 2026-01-20 21:06:45 EST (first golden test) */
#define T_GEN 1768961205LL
#define TZ_EST (-300)

/* ---- sinks -------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len, cap;
    int fail_after; /* >0: fail once len would exceed this */
} membuf_t;

static int mem_write(void *ctx, const void *buf, size_t len)
{
    membuf_t *m = (membuf_t *)ctx;
    if (m->fail_after > 0 && m->len + len > (size_t)m->fail_after) {
        return -1;
    }
    if (m->len + len + 1 > m->cap) {
        size_t cap = m->cap ? m->cap : 4096;
        while (cap < m->len + len + 1) {
            cap *= 2;
        }
        char *p = realloc(m->data, cap);
        if (!p) {
            return -1;
        }
        m->data = p;
        m->cap = cap;
    }
    memcpy(m->data + m->len, buf, len);
    m->len += len;
    m->data[m->len] = '\0';
    return 0;
}

static int file_write(void *ctx, const void *buf, size_t len)
{
    return fwrite(buf, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

typedef struct {
    uint32_t offs[MAX_OBJS];
    uint32_t pages[MAX_PAGES];
    tr_config_t cfg;
} doc_mem_t;

static void cfg_init(doc_mem_t *dm, tr_write_fn fn, void *ctx)
{
    memset(dm, 0, sizeof(*dm));
    dm->cfg.write = fn;
    dm->cfg.write_ctx = ctx;
    dm->cfg.obj_offsets = dm->offs;
    dm->cfg.max_objects = MAX_OBJS;
    dm->cfg.page_objs = dm->pages;
    dm->cfg.max_pages = MAX_PAGES;
    dm->cfg.title = "Unit test";
    dm->cfg.created_utc = T_GEN;
    dm->cfg.tz_offset_min = TZ_EST;
}

/* ---- escaping and mapping ---------------------------------------------- */

static void test_escape(void)
{
    char out[64];
    size_t n = tr_pdf_escape("a(b)c\\d", 7, out, sizeof out);
    TT_EQ_STR(out, "a\\(b\\)c\\\\d");
    TT_EQ_INT(n, 10);
    n = tr_pdf_escape("x\ny\x01", 4, out, sizeof out);
    TT_EQ_STR(out, "x\\012y\\001");
    TT_EQ_INT(n, 10);
    n = tr_pdf_escape("\xE9(", 2, out, sizeof out); /* high bytes pass through */
    TT_EQ_STR(out, "\xE9\\(");
    /* truncation: returns the full length, output NUL-terminated */
    n = tr_pdf_escape("((((", 4, out, 5);
    TT_EQ_INT(n, 8);
    TT_EQ_STR(out, "\\(\\(");
}

static void test_utf8_mapping(void)
{
    char out[64];
    uint32_t rep = 0;
    size_t n = tr_utf8_to_winansi("caf\xC3\xA9 \xC2\xB1\xC2\xB0", out, sizeof out, &rep);
    TT_EQ_INT(n, 7);
    TT_EQ_STR(out, "caf\xE9 \xB1\xB0");
    TT_EQ_INT(rep, 0);
    /* en dash, em dash, bullet, euro, curly quotes, ellipsis, trademark, OE, Z caron */
    n = tr_utf8_to_winansi("\xE2\x80\x93\xE2\x80\x94\xE2\x80\xA2\xE2\x82\xAC\xE2\x80\x9C\xE2\x80\x9D"
                           "\xE2\x80\x98\xE2\x80\x99\xE2\x80\xA6\xE2\x84\xA2\xC5\x92\xC5\xBD",
                           out, sizeof out, &rep);
    TT_EQ_INT(n, 12);
    TT_EQ_STR(out, "\x96\x97\x95\x80\x93\x94\x91\x92\x85\x99\x8C\x8E");
    TT_EQ_INT(rep, 0);
    /* sigma (U+03C3) is not in WinAnsi */
    n = tr_utf8_to_winansi("3\xCF\x83", out, sizeof out, &rep);
    TT_EQ_STR(out, "3?");
    TT_EQ_INT(rep, 1);
    /* emoji, invalid lead byte, overlong, truncated sequence, C1 control */
    rep = 0;
    n = tr_utf8_to_winansi("\xF0\x9F\x98\x80|\xFF|\xC0\xAF|\xC2\x85|\xE2\x82", out, sizeof out, &rep);
    TT_EQ_STR(out, "?|?|?|?|?");
    TT_EQ_INT(rep, 5);
    /* tabs/newlines become spaces */
    tr_utf8_to_winansi("a\tb\nc", out, sizeof out, NULL);
    TT_EQ_STR(out, "a b c");
    /* replacements are counted on the document while drawing */
    membuf_t m = {0};
    doc_mem_t dm;
    cfg_init(&dm, mem_write, &m);
    tr_doc_t doc;
    tr_doc_init(&doc, &dm.cfg);
    tr_page_begin(&doc);
    tr_text(&doc, 72, 700, TR_FONT_HELVETICA, 10, "\xCF\x83 = \xE2\x88\x9A" "2");
    TT_EQ_INT(tr_doc_replacements(&doc), 2);
    TT_EQ_INT(tr_doc_finish(&doc), TR_OK);
    TT_CHECK(strstr(m.data, "(? = ?2) Tj") != NULL);
    free(m.data);
}

/* ---- metrics ------------------------------------------------------------ */

static void test_width(void)
{
    /* reference values: reportlab.pdfbase.pdfmetrics.stringWidth */
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA, 10, "Hello World"), 51.67, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA_BOLD, 18, "Tensile Testing Statistical Analysis Report"),
            363.114, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA, 9, "SEG 5mm Dyneema Cord Tack"), 126.531, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA_BOLD, 9, "Peak (kN)"), 41.013, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA, 9, "3.379"), 22.518, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA, 12,
                          "caf\xC3\xA9 \xE2\x80\x93 \xE2\x82\xAC" "5 \xE2\x80\x9Cok\xE2\x80\x9D \xE2\x80\xA2"),
            80.904, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA_BOLD, 11, "WAVE ~ jumps!"), 81.598, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA, 10, "\xC3\x86\xC3\x98\xC3\xA5\xC3\x9F\xC2\xB1\xC2\xB0"),
            39.29, 1e-9);
    TT_NEAR(tr_text_width(TR_FONT_HELVETICA, 10, ""), 0.0, 1e-12);
    TT_NEAR(tr_text_width_n(TR_FONT_HELVETICA, 10, "Hello World", 5), 22.78, 1e-9);
}

/* ---- wrapping ----------------------------------------------------------- */

#define MAX_WRAP_LINES 16
typedef struct {
    int n;
    char lines[MAX_WRAP_LINES][128];
} wrap_cap_t;

static void cap_line(void *user, const char *s, size_t len)
{
    wrap_cap_t *w = (wrap_cap_t *)user;
    if (w->n < MAX_WRAP_LINES) {
        size_t k = len < 127 ? len : 127;
        memcpy(w->lines[w->n], s, k);
        w->lines[w->n][k] = '\0';
    }
    w->n++;
}

static void test_wrap(void)
{
    wrap_cap_t w;
    const char *s = "The quick brown fox jumps over the lazy dog";
    /* "The quick brown fox" = 86.71 pt at 10 pt; "The quick brown fox jumps" = 115.08 */
    memset(&w, 0, sizeof w);
    int n = tr_wrap(TR_FONT_HELVETICA, 10, s, 100, cap_line, &w);
    TT_EQ_INT(n, 3);
    TT_EQ_INT(w.n, 3);
    TT_EQ_STR(w.lines[0], "The quick brown fox");
    TT_EQ_STR(w.lines[1], "jumps over the lazy");
    TT_EQ_STR(w.lines[2], "dog");
    for (int i = 0; i < w.n; i++) {
        TT_CHECK(tr_text_width(TR_FONT_HELVETICA, 10, w.lines[i]) <= 100.0);
    }
    /* exact fit stays on one line */
    double full = tr_text_width(TR_FONT_HELVETICA, 10, s);
    TT_EQ_INT(tr_wrap(TR_FONT_HELVETICA, 10, s, full, NULL, NULL), 1);
    TT_EQ_INT(tr_wrap(TR_FONT_HELVETICA, 10, s, full - 0.01, NULL, NULL), 2);
    /* explicit newlines, blank line, trailing newline */
    memset(&w, 0, sizeof w);
    n = tr_wrap(TR_FONT_HELVETICA, 10, "a\n\nb  \n", 500, cap_line, &w);
    TT_EQ_INT(n, 3);
    TT_EQ_STR(w.lines[0], "a");
    TT_EQ_STR(w.lines[1], "");
    TT_EQ_STR(w.lines[2], "b");
    /* a word wider than the box is split */
    memset(&w, 0, sizeof w);
    n = tr_wrap(TR_FONT_HELVETICA, 10, "WWWWWWWWWW", 30, cap_line, &w); /* W = 9.44 pt */
    TT_EQ_INT(n, 4);
    TT_EQ_STR(w.lines[0], "WWW");
    TT_EQ_STR(w.lines[3], "W");
    /* multi-byte characters are never split */
    memset(&w, 0, sizeof w);
    n = tr_wrap(TR_FONT_HELVETICA, 10, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 12, cap_line, &w);
    TT_EQ_INT(n, 2);
    TT_EQ_STR(w.lines[0], "\xC3\xA9\xC3\xA9");
    /* empty text has no lines */
    TT_EQ_INT(tr_wrap(TR_FONT_HELVETICA, 10, "", 100, NULL, NULL), 0);
}

static void test_ticks(void)
{
    tr_ticks_t t;
    TT_EQ_INT(tr_nice_ticks(0, 4.295, 6, &t), TR_OK);
    TT_NEAR(t.start, 0, 1e-12);
    TT_NEAR(t.step, 1, 1e-12);
    TT_EQ_INT(t.count, 6);
    TT_EQ_INT(t.decimals, 0);
    tr_nice_ticks(2.83, 4.3, 6, &t);
    TT_NEAR(t.start, 2.5, 1e-9);
    TT_NEAR(t.step, 0.5, 1e-9);
    TT_EQ_INT(t.count, 5);
    TT_EQ_INT(t.decimals, 1);
    tr_nice_ticks(0, 0.037, 8, &t);
    TT_NEAR(t.step, 0.01, 1e-12);
    TT_EQ_INT(t.decimals, 2);
    TT_CHECK(t.count <= 8);
    tr_nice_ticks(5, 5, 6, &t); /* degenerate range */
    TT_CHECK(t.count >= 2 && t.start <= 5 && t.start + (t.count - 1) * t.step >= 5);
}

static void test_datetime(void)
{
    char b[24];
    tr_format_datetime(b, sizeof b, T_GEN, TZ_EST);
    TT_EQ_STR(b, "2026-01-20 21:06:45");
    tr_format_datetime(b, sizeof b, 0, 0);
    TT_EQ_STR(b, "1970-01-01 00:00:00");
    tr_format_datetime(b, sizeof b, 951782400LL, 0); /* leap day 2000 */
    TT_EQ_STR(b, "2000-02-29 00:00:00");
}

/* ---- structure ---------------------------------------------------------- */

/* Validates header, xref offsets, /Size, startxref and every stream /Length. */
static int verify_structure(const char *pdf, size_t len, int expect_pages)
{
    int ok = 1;
    TT_CHECK(len > 100 && memcmp(pdf, "%PDF-1.4\n", 9) == 0);
    const char *sx = NULL;
    for (const char *p = pdf; (p = strstr(p, "startxref\n")) != NULL; p++) {
        sx = p;
    }
    TT_CHECK(sx != NULL);
    if (!sx) {
        return 0;
    }
    unsigned long xref_at = strtoul(sx + 10, NULL, 10);
    TT_CHECK(xref_at < len && strncmp(pdf + xref_at, "xref\n0 ", 7) == 0);
    TT_CHECK(strcmp(pdf + len - 6, "%%EOF\n") == 0);
    char *endp;
    unsigned long size = strtoul(pdf + xref_at + 7, &endp, 10);
    const char *e = endp + 1;
    TT_CHECK(strncmp(e, "0000000000 65535 f \n", 20) == 0);
    for (unsigned long i = 1; i < size; i++) {
        const char *ent = e + 20 * i;
        unsigned long off = strtoul(ent, NULL, 10);
        char want[32];
        int wl = snprintf(want, sizeof want, "%lu 0 obj\n", i);
        int good = ent[10] == ' ' && ent[17] == 'n' && ent[19] == '\n' && off < len &&
                   strncmp(pdf + off, want, (size_t)wl) == 0;
        if (!good) {
            fprintf(stderr, "xref entry %lu bad (offset %lu)\n", i, off);
            ok = 0;
        }
    }
    TT_CHECK(ok);
    const char *tr = e + 20 * size;
    TT_CHECK(strncmp(tr, "trailer\n", 8) == 0);
    char want[64];
    snprintf(want, sizeof want, "/Size %lu ", size);
    TT_CHECK(strstr(tr, want) != NULL);
    TT_CHECK(strstr(tr, "/Root 1 0 R") && strstr(tr, "/Info 3 0 R") && strstr(tr, "/ID ["));
    /* stream lengths: "<< /Length N 0 R >>\nstream\n" ... "\nendstream" and object N holds it */
    int streams = 0;
    for (const char *p = pdf; (p = strstr(p, "/Length ")) != NULL; p++) {
        unsigned long lobj = strtoul(p + 8, NULL, 10);
        const char *st = strstr(p, "stream\n");
        const char *en = st ? strstr(st, "\nendstream") : NULL;
        if (!st || !en) {
            ok = 0;
            break;
        }
        size_t actual = (size_t)(en - (st + 7));
        unsigned long loff = strtoul(e + 20 * lobj, NULL, 10);
        char hdr[32];
        int hl = snprintf(hdr, sizeof hdr, "%lu 0 obj\n", lobj);
        unsigned long declared = strtoul(pdf + loff + hl, NULL, 10);
        if (declared != actual) {
            fprintf(stderr, "stream length %lu declared, %zu actual\n", declared, actual);
            ok = 0;
        }
        streams++;
    }
    TT_EQ_INT(streams, expect_pages);
    snprintf(want, sizeof want, "/Type /Pages /Count %d ", expect_pages);
    TT_CHECK(strstr(pdf, want) != NULL);
    TT_CHECK(ok);
    return ok;
}

static void draw_sample_page(tr_doc_t *doc, int i)
{
    char b[32];
    snprintf(b, sizeof b, "Page body %d (paren) \\", i);
    tr_text(doc, 72, 700, TR_FONT_HELVETICA, 12, b);
    tr_set_stroke_rgb(doc, tr_rgb_hex(0xFF0000));
    tr_set_line_width(doc, 1.5);
    tr_line(doc, 1.5, -0.25, 100, 0);
    static const double pts[] = {72, 600, 100, 620.5, 130, 590};
    tr_polyline(doc, pts, 3);
    tr_rect(doc, 72, 500, 50, 20, TR_PAINT_FILL_STROKE);
}

static void test_xref(void)
{
    membuf_t m = {0};
    doc_mem_t dm;
    cfg_init(&dm, mem_write, &m);
    dm.cfg.title = "Caf\xC3\xA9 report"; /* non-ASCII -> UTF-16BE hex */
    tr_doc_t doc;
    TT_EQ_INT(tr_doc_init(&doc, &dm.cfg), TR_OK);
    for (int i = 0; i < 3; i++) {
        TT_EQ_INT(tr_page_begin(&doc), TR_OK);
        draw_sample_page(&doc, i);
        TT_EQ_INT(tr_page_end(&doc), TR_OK);
    }
    TT_EQ_INT(tr_doc_finish(&doc), TR_OK);
    TT_EQ_INT(tr_doc_bytes(&doc), m.len);
    verify_structure(m.data, m.len, 3);
    TT_CHECK(strstr(m.data, "/Producer (TensileOS HMI)") != NULL);
    TT_CHECK(strstr(m.data, "/CreationDate (D:20260120210645-05'00')") != NULL);
    TT_CHECK(strstr(m.data, "/Title <FEFF00430061006600E90020007200650070006F00720074>") != NULL);
    TT_CHECK(strstr(m.data, "/BaseFont /Helvetica /Encoding /WinAnsiEncoding") != NULL);
    TT_CHECK(strstr(m.data, "/BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding") != NULL);
    TT_CHECK(strstr(m.data, "(Page body 0 \\(paren\\) \\\\) Tj") != NULL);
    TT_CHECK(strstr(m.data, "1.5 -0.25 m 100 0 l S") != NULL);
    TT_CHECK(strstr(m.data, "1 0 0 RG") != NULL);
    TT_CHECK(strstr(m.data, "100 620.5 l") != NULL);

    /* a dry run produces the same byte count and page count without writing */
    doc_mem_t dm2;
    cfg_init(&dm2, NULL, NULL);
    dm2.cfg.title = dm.cfg.title;
    tr_doc_t dry;
    tr_doc_init(&dry, &dm2.cfg);
    TT_CHECK(tr_doc_is_dry_run(&dry));
    for (int i = 0; i < 3; i++) {
        tr_page_begin(&dry);
        draw_sample_page(&dry, i);
        tr_page_end(&dry);
    }
    TT_EQ_INT(tr_doc_finish(&dry), TR_OK);
    TT_EQ_INT(tr_doc_page_count(&dry), 3);
    TT_EQ_INT(tr_doc_bytes(&dry), m.len);
    free(m.data);

    /* an empty document still gets one (blank) page */
    membuf_t m2 = {0};
    cfg_init(&dm, mem_write, &m2);
    tr_doc_init(&doc, &dm.cfg);
    TT_EQ_INT(tr_doc_finish(&doc), TR_OK);
    verify_structure(m2.data, m2.len, 1);
    free(m2.data);
}

static void test_errors(void)
{
    doc_mem_t dm;
    tr_doc_t doc;
    membuf_t m = {0};
    /* drawing outside a page is a sticky state error */
    cfg_init(&dm, mem_write, &m);
    tr_doc_init(&doc, &dm.cfg);
    TT_EQ_INT(tr_line(&doc, 0, 0, 1, 1), TR_ERR_STATE);
    TT_EQ_INT(tr_page_begin(&doc), TR_ERR_STATE);
    TT_EQ_INT(tr_doc_finish(&doc), TR_ERR_STATE);
    free(m.data);
    /* page limit */
    memset(&m, 0, sizeof m);
    cfg_init(&dm, mem_write, &m);
    dm.cfg.max_pages = 2;
    tr_doc_init(&doc, &dm.cfg);
    TT_EQ_INT(tr_page_break(&doc) == TR_ERR_STATE, 1); /* no page open yet */
    free(m.data);
    memset(&m, 0, sizeof m);
    cfg_init(&dm, mem_write, &m);
    dm.cfg.max_pages = 2;
    tr_doc_init(&doc, &dm.cfg);
    tr_page_begin(&doc);
    TT_EQ_INT(tr_page_break(&doc), TR_OK);
    TT_EQ_INT(tr_page_break(&doc), TR_ERR_FULL);
    free(m.data);
    /* object limit */
    memset(&m, 0, sizeof m);
    cfg_init(&dm, mem_write, &m);
    dm.cfg.max_objects = TR_OBJECTS_FOR_PAGES(1);
    tr_doc_init(&doc, &dm.cfg);
    tr_page_begin(&doc);
    TT_EQ_INT(tr_page_break(&doc), TR_ERR_FULL);
    free(m.data);
    /* sink failure */
    memset(&m, 0, sizeof m);
    m.fail_after = 600;
    cfg_init(&dm, mem_write, &m);
    tr_doc_init(&doc, &dm.cfg);
    tr_page_begin(&doc);
    for (int i = 0; i < 50; i++) {
        tr_text(&doc, 72, 700 - i * 12, TR_FONT_HELVETICA, 10, "filling the buffer");
    }
    TT_EQ_INT(tr_doc_finish(&doc), TR_ERR_IO);
    free(m.data);
    /* bad config */
    cfg_init(&dm, mem_write, &m);
    dm.cfg.obj_offsets = NULL;
    TT_EQ_INT(tr_doc_init(&doc, &dm.cfg), TR_ERR_ARG);
}

/* ---- fixtures ----------------------------------------------------------- */

typedef struct {
    char name[64], project[128], technician[64];
    double peak;
} fixture_t;

static void trim_eol(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

static int load_fixture(const char *file, fixture_t *f)
{
    char path[512], line[256];
    snprintf(path, sizeof path, "%s/%s", FIXTURE_DIR, file);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    memset(f, 0, sizeof(*f));
    int got_peak = 0;
    while (fgets(line, sizeof line, fp) && line[0] == '#') {
        trim_eol(line);
        if (strncmp(line, "# Test Name: ", 13) == 0) {
            snprintf(f->name, sizeof f->name, "%s", line + 13);
        } else if (strncmp(line, "# Project: ", 11) == 0) {
            snprintf(f->project, sizeof f->project, "%s", line + 11);
        } else if (strncmp(line, "# Technician: ", 14) == 0) {
            snprintf(f->technician, sizeof f->technician, "%s", line + 14);
        } else if (strncmp(line, "# Peak Force: ", 14) == 0) {
            f->peak = strtod(line + 14, NULL);
            got_peak = 1;
        }
    }
    fclose(fp);
    return got_peak;
}

static const char *const k_fixtures[5] = {"Test_1_210645.csv", "Test_2_211047.csv",
                                          "Test_3_211234.csv", "Test_4_211401.csv",
                                          "Test_5_211540.csv"};

static void test_phase0_proof(void)
{
    fixture_t fx[5];
    tr_test_row_t rows[5];
    for (int i = 0; i < 5; i++) {
        TT_CHECK(load_fixture(k_fixtures[i], &fx[i]));
        rows[i].name = fx[i].name;
        rows[i].project = fx[i].project;
        rows[i].technician = fx[i].technician;
        rows[i].peak_kn = fx[i].peak;
    }
    TT_NEAR(fx[0].peak, 3.379, 1e-9);
    TT_NEAR(fx[1].peak, 3.841, 1e-9);
    TT_NEAR(fx[4].peak, 3.828, 1e-9);
    TT_EQ_STR(fx[2].project, "SEG 5mm Dyneema Cord Tack");

    /* into memory for structural checks */
    membuf_t m = {0};
    doc_mem_t dm;
    cfg_init(&dm, mem_write, &m);
    dm.cfg.title = "Tensile Testing Statistical Analysis Report";
    dm.cfg.creator = "tensile_report host test";
    tr_doc_t doc;
    tr_doc_init(&doc, &dm.cfg);
    TT_EQ_INT(tr_phase0_proof(&doc, rows, 5, T_GEN, TZ_EST), TR_OK);
    TT_EQ_INT(tr_doc_finish(&doc), TR_OK);
    TT_EQ_INT(tr_doc_page_count(&doc), 1);
    TT_EQ_INT(tr_doc_replacements(&doc), 0);
    verify_structure(m.data, m.len, 1);
    TT_CHECK(strstr(m.data, "Mean 3.567") != NULL);
    TT_CHECK(strstr(m.data, "(+0.274) Tj") != NULL); /* 3.841 - 3.5666 */
    TT_CHECK(strstr(m.data, "(Page 1 of 1) Tj") != NULL);
    TT_CHECK(strstr(m.data, "(Created via TensileOS) Tj") != NULL);
    TT_CHECK(m.len < 100000);

    /* and to a file for qpdf / pdftotext */
    FILE *fp = fopen(OUT_DIR "/phase0_proof.pdf", "wb");
    TT_CHECK(fp != NULL);
    if (fp) {
        cfg_init(&dm, file_write, fp);
        dm.cfg.title = "Tensile Testing Statistical Analysis Report";
        dm.cfg.creator = "tensile_report host test";
        tr_doc_init(&doc, &dm.cfg);
        tr_phase0_proof(&doc, rows, 5, T_GEN, TZ_EST);
        TT_EQ_INT(tr_doc_finish(&doc), TR_OK);
        TT_EQ_INT(fclose(fp), 0);
        TT_EQ_INT(tr_doc_bytes(&doc), m.len); /* deterministic output */
        fprintf(stderr, "wrote %s (%lu bytes)\n", OUT_DIR "/phase0_proof.pdf",
                (unsigned long)tr_doc_bytes(&doc));
    }
    free(m.data);
}

/* ---- multi-page table (header repetition, page n of N via dry run) ------ */

typedef struct {
    uint32_t total;
} footer_ctx_t;

static void footer_hook(tr_doc_t *doc, uint32_t page, void *user)
{
    footer_ctx_t *f = (footer_ctx_t *)user;
    char b[48];
    snprintf(b, sizeof b, "Page %lu of %lu", (unsigned long)page, (unsigned long)f->total);
    tr_set_fill_rgb(doc, tr_rgb_hex(0x808080));
    tr_text_aligned(doc, TR_PAGE_W / 2, 36, TR_ALIGN_CENTER, TR_FONT_HELVETICA, 9,
                    "Created via TensileOS");
    tr_text_aligned(doc, TR_PAGE_W - 72, 36, TR_ALIGN_RIGHT, TR_FONT_HELVETICA, 9, b);
}

static int render_table_doc(tr_write_fn fn, void *ctx, uint32_t total, uint32_t *pages_out)
{
    static const double col_w[6] = {21.6, 54, 172.8, 64.8, 50.4, 64.8};
    static const char *const hdr[6] = {"#", "Test Name", "Project", "Technician", "Peak (kN)",
                                       "Deviation"};
    footer_ctx_t fc = {total};
    doc_mem_t dm;
    cfg_init(&dm, fn, ctx);
    dm.cfg.on_page_end = footer_hook;
    dm.cfg.hook_user = &fc;
    tr_doc_t doc;
    tr_doc_init(&doc, &dm.cfg);
    tr_page_begin(&doc);
    tr_table_t t;
    memset(&t, 0, sizeof t);
    tr_table_defaults(&t);
    t.ncols = 6;
    t.col_w = col_w;
    t.header = hdr;
    t.x = 91.8;
    t.y_top = 792 - 36;
    t.y_bottom = 60;
    double y = 792 - 36;
    tr_set_fill_rgb(&doc, tr_rgb_hex(0x000000));
    tr_text(&doc, 72, y - 14, TR_FONT_HELVETICA_BOLD, 12, "Individual Test Results");
    y -= 22;
    tr_table_header(&doc, &t, &y);
    for (int i = 0; i < 60; i++) {
        char idx[8], name[16], peak[16], dev[16];
        snprintf(idx, sizeof idx, "%d", i + 1);
        snprintf(name, sizeof name, "Test %d", i + 1);
        snprintf(peak, sizeof peak, "%.3f", 3.3 + 0.01 * (i % 50));
        snprintf(dev, sizeof dev, "%+.3f", 0.01 * (i % 50) - 0.25);
        const char *proj = (i % 7 == 3)
                               ? "SEG 5mm Dyneema Cord Tack \xE2\x80\x93 long project name that has "
                                 "to wrap onto several lines inside its cell"
                               : "SEG 5mm Dyneema Cord Tack";
        const char *cells[6] = {idx, name, proj, "Chris Lee", peak, dev};
        tr_table_row(&doc, &t, cells, &y);
        TT_CHECK(y >= t.y_bottom - 1e-9);
    }
    int rc = tr_doc_finish(&doc);
    *pages_out = tr_doc_page_count(&doc);
    return rc;
}

static void test_table_multipage(void)
{
    uint32_t pages = 0, pages2 = 0;
    TT_EQ_INT(render_table_doc(NULL, NULL, 0, &pages), TR_OK); /* dry run */
    TT_CHECK(pages >= 2);
    membuf_t m = {0};
    TT_EQ_INT(render_table_doc(mem_write, &m, pages, &pages2), TR_OK);
    TT_EQ_INT(pages2, pages);
    verify_structure(m.data, m.len, (int)pages);
    /* header drawn once per page */
    int headers = 0;
    for (const char *p = m.data; (p = strstr(p, "(Peak \\(kN\\)) Tj")) != NULL; p++) {
        headers++;
    }
    TT_EQ_INT(headers, (int)pages);
    char last[64];
    snprintf(last, sizeof last, "(Page %lu of %lu) Tj", (unsigned long)pages, (unsigned long)pages);
    TT_CHECK(strstr(m.data, last) != NULL);
    FILE *fp = fopen(OUT_DIR "/table_multipage.pdf", "wb");
    TT_CHECK(fp && fwrite(m.data, 1, m.len, fp) == m.len);
    if (fp) {
        fclose(fp);
    }
    free(m.data);
}

/* ---- chart helper ------------------------------------------------------- */

static void test_chart_line(void)
{
    membuf_t m = {0};
    doc_mem_t dm;
    cfg_init(&dm, mem_write, &m);
    tr_doc_t doc;
    tr_doc_init(&doc, &dm.cfg);
    tr_page_begin(&doc);
    tr_chart_cfg_t cc;
    memset(&cc, 0, sizeof cc);
    cc.x = 72;
    cc.y = 400;
    cc.w = 432;
    cc.h = 200;
    cc.title = "Force vs. Time";
    cc.x_label = "Time (s)";
    cc.y_label = "Force (kN)";
    cc.x_min = 0;
    cc.x_max = 10;
    cc.y_min = 0;
    cc.y_max = 3.9;
    cc.legend_w = 70;
    cc.grid = 1;
    tr_chart_t c;
    TT_EQ_INT(tr_chart_begin(&doc, &cc, &c), TR_OK);
    TT_NEAR(c.y_lo, 0, 1e-12);
    TT_NEAR(c.y_hi, 4, 1e-12);
    TT_NEAR(tr_chart_map_y(&c, 0), c.py, 1e-9);
    TT_NEAR(tr_chart_map_y(&c, 4), c.py + c.ph, 1e-9);
    TT_NEAR(tr_chart_map_x(&c, 10), c.px + c.pw, 1e-9);
    double xs[50], ys[50];
    for (int i = 0; i < 50; i++) {
        xs[i] = i * 0.2;
        ys[i] = 3.8 * sin(i * 0.06);
    }
    TT_EQ_INT(tr_chart_line(&doc, &c, xs, ys, 50, tr_rgb_hex(0x2196F3), 1.2, "Force"), TR_OK);
    TT_EQ_INT(tr_chart_hline(&doc, &c, 3.8, tr_rgb_hex(0xFF5722), 1, TR_LINE_DASHED, "Peak",
                             "3.800"),
              TR_OK);
    TT_EQ_INT(tr_chart_hline(&doc, &c, 9.0, tr_rgb_hex(0x000000), 1, TR_LINE_SOLID, "Off-scale",
                             NULL),
              TR_OK);
    TT_EQ_INT(c.nlegend, 3);
    TT_EQ_INT(tr_chart_end(&doc, &c), TR_OK);
    TT_EQ_INT(tr_doc_finish(&doc), TR_OK);
    TT_CHECK(strstr(m.data, "[6 3] 0 d") != NULL);
    TT_CHECK(strstr(m.data, "re W n") != NULL);
    TT_CHECK(strstr(m.data, "0 1 -1 0 ") != NULL); /* rotated y label */
    verify_structure(m.data, m.len, 1);
    free(m.data);
}

int main(void)
{
    TT_RUN(test_escape);
    TT_RUN(test_utf8_mapping);
    TT_RUN(test_width);
    TT_RUN(test_wrap);
    TT_RUN(test_ticks);
    TT_RUN(test_datetime);
    TT_RUN(test_xref);
    TT_RUN(test_errors);
    TT_RUN(test_chart_line);
    TT_RUN(test_table_multipage);
    TT_RUN(test_phase0_proof);
    return tt_summary();
}
