/*
 * tensile_report - streaming PDF 1.4 writer for on-device reports (prefix tr_).
 *
 * Everything declared here except the "ESP glue" section is implemented in
 * core/ (pure C11, host-testable). The writer streams every object to a
 * caller-supplied sink as soon as it is complete; the only per-document RAM is
 * the xref offset table and the page object list (caller-provided buffers) plus
 * a small output buffer inside tr_doc_t.
 *
 * Coordinates are PDF points (1/72 in), origin bottom-left, US Letter page.
 * Text is UTF-8; it is mapped to WinAnsiEncoding (CP1252) for the base-14
 * Helvetica fonts. Unmappable characters become '?' and are counted
 * (tr_doc_replacements()).
 *
 * Errors are sticky: once a call fails, later calls are no-ops and return the
 * same error, so a caller may draw a whole page and check tr_doc_finish() only.
 *
 * Typical use:
 *   uint32_t offs[256], pages[32];
 *   tr_config_t cfg = { .write = my_write, .write_ctx = f,
 *                       .obj_offsets = offs, .max_objects = 256,
 *                       .page_objs = pages, .max_pages = 32,
 *                       .title = "Statistics Report", .created_utc = now };
 *   tr_doc_t doc;
 *   tr_doc_init(&doc, &cfg);
 *   tr_page_begin(&doc);
 *   tr_text(&doc, 72, 700, TR_FONT_HELVETICA, 12, "Hello");
 *   tr_page_end(&doc);
 *   int err = tr_doc_finish(&doc);
 *
 * "Page n of N": lay the document out once with cfg.write == NULL (dry run:
 * nothing is written, pages are counted), then again for real with the total.
 */
#ifndef TENSILE_REPORT_H
#define TENSILE_REPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants ---------------------------------------------------------- */

#define TR_PAGE_W 612.0 /* US Letter, points */
#define TR_PAGE_H 792.0

#define TR_OK 0
#define TR_ERR_ARG (-1)   /* bad argument */
#define TR_ERR_IO (-2)    /* sink write failed */
#define TR_ERR_FULL (-3)  /* max_objects or max_pages exceeded */
#define TR_ERR_STATE (-4) /* call not valid in this state (e.g. draw outside a page) */
#define TR_ERR_NOMEM (-5) /* allocation failed (ESP glue only) */

/* Objects used by a document: 6 fixed + 3 per page. */
#define TR_OBJECTS_FOR_PAGES(n) (6u + 3u * (uint32_t)(n))

/* Sink: write len bytes, return 0 on success, nonzero on failure. */
typedef int (*tr_write_fn)(void *ctx, const void *buf, size_t len);

typedef enum { TR_FONT_HELVETICA = 0, TR_FONT_HELVETICA_BOLD = 1, TR_FONT_COUNT = 2 } tr_font_t;
typedef enum { TR_ALIGN_LEFT = 0, TR_ALIGN_CENTER = 1, TR_ALIGN_RIGHT = 2 } tr_align_t;
typedef enum { TR_PAINT_STROKE = 1, TR_PAINT_FILL = 2, TR_PAINT_FILL_STROKE = 3 } tr_paint_t;
typedef enum { TR_LINE_SOLID = 0, TR_LINE_DASHED = 1, TR_LINE_DOTTED = 2 } tr_line_style_t;

typedef struct {
    float r, g, b; /* 0..1 */
} tr_rgb_t;

/* 0xRRGGBB -> tr_rgb_t */
tr_rgb_t tr_rgb_hex(uint32_t rgb);

/* ---- document ----------------------------------------------------------- */

struct tr_doc;
/* Called right after a page starts (e.g. header) and right before it ends
 * (e.g. footer). page_no is 1-based. */
typedef void (*tr_page_hook_fn)(struct tr_doc *doc, uint32_t page_no, void *user);

typedef struct {
    tr_write_fn write; /* NULL = dry run (layout/page count only) */
    void *write_ctx;
    uint32_t *obj_offsets; /* xref table storage, max_objects entries */
    uint32_t max_objects;  /* >= TR_OBJECTS_FOR_PAGES(max_pages) */
    uint32_t *page_objs;   /* page object numbers, max_pages entries */
    uint32_t max_pages;
    /* /Info (UTF-8, optional except the time). Producer is always "TensileOS HMI". */
    const char *title;
    const char *author;
    const char *subject;
    const char *creator;
    int64_t created_utc; /* CreationDate, seconds since 1970-01-01 UTC */
    int tz_offset_min;   /* local offset for CreationDate, e.g. -300 for EST */
    tr_page_hook_fn on_page_begin, on_page_end;
    void *hook_user;
} tr_config_t;

/* Treat as opaque; it is public only so callers can allocate it. */
typedef struct tr_doc {
    tr_write_fn write;
    void *write_ctx;
    uint32_t *obj_offsets;
    uint32_t max_objects;
    uint32_t next_obj;
    uint32_t *page_objs;
    uint32_t max_pages;
    uint32_t page_count;
    uint32_t offset; /* bytes emitted so far */
    uint32_t content_obj;
    uint32_t stream_start;
    uint32_t replaced; /* unmappable characters replaced with '?' */
    uint64_t hash[2];  /* running hash for the trailer /ID */
    tr_page_hook_fn on_page_begin, on_page_end;
    void *hook_user;
    int err;
    uint8_t in_page;
    uint8_t in_hook;
    uint8_t finished;
    uint16_t buf_len;
    uint8_t buf[256];
} tr_doc_t;

int tr_doc_init(tr_doc_t *doc, const tr_config_t *cfg);
/* Ends an open page, writes page tree, catalog, xref and trailer, flushes. */
int tr_doc_finish(tr_doc_t *doc);
int tr_doc_error(const tr_doc_t *doc);
uint32_t tr_doc_page_count(const tr_doc_t *doc); /* pages begun so far */
uint32_t tr_doc_replacements(const tr_doc_t *doc);
uint32_t tr_doc_bytes(const tr_doc_t *doc);
int tr_doc_is_dry_run(const tr_doc_t *doc);

int tr_page_begin(tr_doc_t *doc);
int tr_page_end(tr_doc_t *doc);
int tr_page_break(tr_doc_t *doc); /* end + begin (hooks run) */

/* ---- graphics ----------------------------------------------------------- */

int tr_save(tr_doc_t *doc);    /* q */
int tr_restore(tr_doc_t *doc); /* Q */
int tr_set_stroke_rgb(tr_doc_t *doc, tr_rgb_t c);
int tr_set_fill_rgb(tr_doc_t *doc, tr_rgb_t c); /* also the text color */
int tr_set_line_width(tr_doc_t *doc, double w);
/* pattern of n on/off lengths (n == 0: solid). */
int tr_set_dash(tr_doc_t *doc, const double *pattern, int n, double phase);
int tr_set_line_style(tr_doc_t *doc, tr_line_style_t style);
int tr_line(tr_doc_t *doc, double x1, double y1, double x2, double y2);
/* xy = x0,y0,x1,y1,... (n points) */
int tr_polyline(tr_doc_t *doc, const double *xy, int n);
int tr_rect(tr_doc_t *doc, double x, double y, double w, double h, tr_paint_t paint);
/* Intersect the clip with a rectangle; wrap in tr_save/tr_restore. */
int tr_clip_rect(tr_doc_t *doc, double x, double y, double w, double h);

/* ---- text --------------------------------------------------------------- */

/* Width in points of UTF-8 text (no kerning, same as reportlab stringWidth). */
double tr_text_width(tr_font_t font, double size, const char *utf8);
double tr_text_width_n(tr_font_t font, double size, const char *utf8, size_t len);

/* Baseline at (x, y); x is the left edge, centre or right edge per align. */
int tr_text(tr_doc_t *doc, double x, double y, tr_font_t font, double size, const char *utf8);
int tr_text_aligned(tr_doc_t *doc, double x, double y, tr_align_t align, tr_font_t font,
                    double size, const char *utf8);
int tr_text_n(tr_doc_t *doc, double x, double y, tr_align_t align, tr_font_t font, double size,
              const char *utf8, size_t len);
/* Rotated counter-clockwise by deg about the anchor (x, y). */
int tr_text_rotated(tr_doc_t *doc, double x, double y, double deg, tr_align_t align,
                    tr_font_t font, double size, const char *utf8);

/* Word wrap. For each line cb(user, start, len) gets a slice of utf8 (trailing
 * spaces trimmed). Breaks at spaces, honours '\n', splits words wider than
 * max_w. cb may be NULL (count only). Returns the line count. */
typedef void (*tr_line_fn)(void *user, const char *start, size_t len);
int tr_wrap(tr_font_t font, double size, const char *utf8, double max_w, tr_line_fn cb, void *user);

/* Wrapped text in a box of width w whose top edge is y_top. Each line box is
 * `leading` tall with the text vertically centred in it. max_lines 0 = no
 * limit. Returns the number of lines drawn (height = lines * leading) or a
 * negative error. */
int tr_text_box(tr_doc_t *doc, double x, double y_top, double w, tr_font_t font, double size,
                double leading, tr_align_t align, const char *utf8, int max_lines);

/* UTF-8 -> WinAnsi (CP1252). Writes at most out_size bytes including NUL;
 * returns the full output length (like snprintf). *replaced (optional) is
 * incremented per unmappable character. Tab/CR/LF become spaces. */
size_t tr_utf8_to_winansi(const char *utf8, char *out, size_t out_size, uint32_t *replaced);
/* Escape bytes for a PDF literal string body: ( ) \ get a backslash, control
 * bytes become \ddd. Same return convention as above. */
size_t tr_pdf_escape(const char *in, size_t len, char *out, size_t out_size);

/* ---- helpers ------------------------------------------------------------ */

/* "YYYY-MM-DD HH:MM:SS" for t + tz_offset_min (needs 20 bytes). */
void tr_format_datetime(char *out, size_t out_size, int64_t t_utc, int tz_offset_min);

typedef struct {
    double start, step;
    int count;    /* ticks: start + i*step, i < count */
    int decimals; /* digits needed to print the step */
} tr_ticks_t;
/* "Nice" axis ticks (1/2/5 x 10^k) covering [lo, hi] with <= max_ticks ticks. */
int tr_nice_ticks(double lo, double hi, int max_ticks, tr_ticks_t *out);

/* ---- table -------------------------------------------------------------- */

#define TR_TABLE_MAX_COLS 12

typedef struct {
    /* layout */
    double x;        /* left edge */
    double y_top;    /* table top after a page break */
    double y_bottom; /* rows must stay above this; otherwise page break */
    int ncols;
    const double *col_w;
    const char *const *header; /* ncols strings or NULL */
    const tr_align_t *col_align; /* NULL: all columns use align */
    tr_align_t align;
    int repeat_header; /* redraw header after a page break */
    /* style */
    tr_font_t header_font, body_font;
    double font_size, leading, pad_x, pad_y;
    tr_rgb_t text_color, header_bg, row_bg[2], grid_color;
    double grid_w; /* 0: no grid */
    /* state */
    int row_index;
} tr_table_t;

/* Fills style fields matching the desktop report (9 pt, grey grid, #E3F2FD header, zebra). */
void tr_table_defaults(tr_table_t *t);
double tr_table_row_height(const tr_table_t *t, const char *const *cells, tr_font_t font);
/* *y is the current top; updated to the bottom of what was drawn. */
int tr_table_header(tr_doc_t *doc, tr_table_t *t, double *y);
int tr_table_row(tr_doc_t *doc, tr_table_t *t, const char *const *cells, double *y);

/* ---- chart -------------------------------------------------------------- */

#define TR_CHART_MAX_LEGEND 8

typedef struct {
    double x, y, w, h; /* whole chart box including labels and legend */
    const char *title, *x_label, *y_label;
    double y_min, y_max; /* data extent; ticks are made "nice" around it */
    double x_min, x_max; /* for tr_chart_line; ignored by bars */
    int max_ticks;       /* 0: 6 */
    double legend_w;     /* space reserved at the right for the legend (0: none) */
    double x_label_angle; /* category label rotation in degrees (e.g. 45) */
    double margin_left, margin_bottom, margin_top; /* 0: automatic */
    int grid;
} tr_chart_cfg_t;

typedef struct {
    const char *label;
    tr_rgb_t color;
    double width;
    tr_line_style_t style;
    int is_box; /* filled square (bars) instead of a line sample */
} tr_legend_entry_t;

typedef struct {
    tr_chart_cfg_t cfg;
    double px, py, pw, ph; /* plot area */
    tr_ticks_t yt;
    double y_lo, y_hi;
    int nlegend;
    tr_legend_entry_t legend[TR_CHART_MAX_LEGEND];
} tr_chart_t;

/* Computes the plot area and draws title, axis labels, y ticks, grid and frame. */
int tr_chart_begin(tr_doc_t *doc, const tr_chart_cfg_t *cfg, tr_chart_t *c);
double tr_chart_map_y(const tr_chart_t *c, double v);
double tr_chart_map_x(const tr_chart_t *c, double v);
/* One bar per value, labels (may be NULL) under each bar; legend_label may be NULL. */
int tr_chart_bars(tr_doc_t *doc, tr_chart_t *c, const double *values, const char *const *labels,
                  int n, tr_rgb_t color, const char *legend_label);
/* Horizontal reference line across the plot; inline_label (may be NULL) is
 * printed above the line at the right end, legend_label adds a legend entry. */
int tr_chart_hline(tr_doc_t *doc, tr_chart_t *c, double value, tr_rgb_t color, double width,
                   tr_line_style_t style, const char *legend_label, const char *inline_label);
/* Polyline in data coordinates (x range from cfg.x_min/x_max), clipped to the plot. */
int tr_chart_line(tr_doc_t *doc, tr_chart_t *c, const double *xs, const double *ys, int n,
                  tr_rgb_t color, double width, const char *legend_label);
/* Draws the legend (if legend_w > 0 and entries exist). */
int tr_chart_end(tr_doc_t *doc, tr_chart_t *c);

/* ---- Phase 0 proof page ------------------------------------------------- */

typedef struct {
    const char *name, *project, *technician;
    double peak_kn;
} tr_test_row_t;

/* One complete Letter page: title, wrapped paragraph, 6-column results table,
 * bar chart with mean and +/-3 sigma lines, footer. Caller does init/finish. */
int tr_phase0_proof(tr_doc_t *doc, const tr_test_row_t *tests, int n, int64_t generated_utc,
                    int tz_offset_min);

/* ---- ESP glue (tensile_report_esp.c, not built on host) ----------------- */

typedef struct tr_file tr_file_t;
/* Opens "<path>.tmp" for writing. Returns NULL on failure. */
tr_file_t *tr_open_file(const char *path);
/* tr_write_fn for a tr_file_t (pass the handle as write_ctx). */
int tr_file_write(void *ctx, const void *buf, size_t len);
/* fflush + fsync + fclose, then rename "<path>.tmp" -> path. Frees f. */
int tr_file_commit(tr_file_t *f);
/* Closes and deletes the temp file. Frees f. */
void tr_file_abort(tr_file_t *f);
/* Logs replacement count / size of a finished doc (ESP_LOGx). */
void tr_log_doc_summary(const tr_doc_t *doc, const char *path);

#ifdef __cplusplus
}
#endif
#endif /* TENSILE_REPORT_H */
