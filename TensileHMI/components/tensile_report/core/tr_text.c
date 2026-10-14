/* tensile_report core: UTF-8 -> WinAnsi mapping, string escaping, metrics, wrapping, text output. */
#include <math.h>
#include <string.h>

#include "tr_internal.h"

/* Unicode code points that WinAnsi (CP1252) places in 0x80..0x9F. */
static const struct {
    uint16_t cp;
    uint8_t code;
} k_cp1252_specials[] = {
    {0x0152, 0x8C}, {0x0153, 0x9C}, {0x0160, 0x8A}, {0x0161, 0x9A}, {0x0178, 0x9F},
    {0x017D, 0x8E}, {0x017E, 0x9E}, {0x0192, 0x83}, {0x02C6, 0x88}, {0x02DC, 0x98},
    {0x2013, 0x96}, {0x2014, 0x97}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201A, 0x82},
    {0x201C, 0x93}, {0x201D, 0x94}, {0x201E, 0x84}, {0x2020, 0x86}, {0x2021, 0x87},
    {0x2022, 0x95}, {0x2026, 0x85}, {0x2030, 0x89}, {0x2039, 0x8B}, {0x203A, 0x9B},
    {0x20AC, 0x80}, {0x2122, 0x99},
};

static uint8_t map_cp(uint32_t cp, uint32_t *replaced)
{
    if (cp == '\t' || cp == '\n' || cp == '\r') {
        return ' ';
    }
    if ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF)) {
        return (uint8_t)cp;
    }
    for (size_t i = 0; i < sizeof(k_cp1252_specials) / sizeof(k_cp1252_specials[0]); i++) {
        if (k_cp1252_specials[i].cp == cp) {
            return k_cp1252_specials[i].code;
        }
    }
    if (replaced) {
        (*replaced)++;
    }
    return '?';
}

uint8_t tr__next_winansi(const char **pp, const char *end, uint32_t *replaced)
{
    const unsigned char *p = (const unsigned char *)*pp;
    const unsigned char *e = (const unsigned char *)end;
    uint32_t cp;
    int extra;
    uint32_t min;
    unsigned char c = *p++;
    if (c < 0x80) {
        *pp = (const char *)p;
        return map_cp(c, replaced);
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1Fu;
        extra = 1;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0Fu;
        extra = 2;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07u;
        extra = 3;
        min = 0x10000;
    } else { /* stray continuation or invalid lead byte: consume one byte */
        *pp = (const char *)p;
        if (replaced) {
            (*replaced)++;
        }
        return '?';
    }
    for (int i = 0; i < extra; i++) {
        if (p >= e || (*p & 0xC0) != 0x80) { /* truncated: resume at this byte */
            *pp = (const char *)p;
            if (replaced) {
                (*replaced)++;
            }
            return '?';
        }
        cp = (cp << 6) | (*p++ & 0x3Fu);
    }
    *pp = (const char *)p;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        if (replaced) {
            (*replaced)++;
        }
        return '?';
    }
    return map_cp(cp, replaced);
}

size_t tr_utf8_to_winansi(const char *utf8, char *out, size_t out_size, uint32_t *replaced)
{
    size_t n = 0;
    if (!utf8) {
        utf8 = "";
    }
    const char *end = utf8 + strlen(utf8);
    const char *p = utf8;
    while (p < end) {
        uint8_t b = tr__next_winansi(&p, end, replaced);
        if (out && n + 1 < out_size) {
            out[n] = (char)b;
        }
        n++;
    }
    if (out && out_size) {
        out[n < out_size ? n : out_size - 1] = '\0';
    }
    return n;
}

/* Appends the escaped form of byte b to e (needs 4 bytes); returns its length. */
static size_t escape_byte(uint8_t b, char *e)
{
    if (b == '(' || b == ')' || b == '\\') {
        e[0] = '\\';
        e[1] = (char)b;
        return 2;
    }
    if (b < 32 || b == 127) {
        e[0] = '\\';
        e[1] = (char)('0' + ((b >> 6) & 7));
        e[2] = (char)('0' + ((b >> 3) & 7));
        e[3] = (char)('0' + (b & 7));
        return 4;
    }
    e[0] = (char)b;
    return 1;
}

size_t tr_pdf_escape(const char *in, size_t len, char *out, size_t out_size)
{
    size_t n = 0;
    for (size_t i = 0; in && i < len; i++) {
        char e[4];
        size_t k = escape_byte((uint8_t)in[i], e);
        for (size_t j = 0; j < k; j++, n++) {
            if (out && n + 1 < out_size) {
                out[n] = e[j];
            }
        }
    }
    if (out && out_size) {
        out[n < out_size ? n : out_size - 1] = '\0';
    }
    return n;
}

/* ---- metrics ------------------------------------------------------------ */

unsigned tr__glyph_w(tr_font_t font, uint8_t code)
{
    if ((unsigned)font >= TR_FONT_COUNT || code < TR_WIDTH_FIRST) {
        return 0;
    }
    return tr_font_widths[font][code - TR_WIDTH_FIRST];
}

static unsigned long width_units(tr_font_t font, const char *s, size_t len)
{
    unsigned long w = 0;
    const char *end = s + len;
    while (s < end) {
        w += tr__glyph_w(font, tr__next_winansi(&s, end, NULL));
    }
    return w;
}

double tr_text_width_n(tr_font_t font, double size, const char *utf8, size_t len)
{
    if (!utf8) {
        return 0.0;
    }
    return (double)width_units(font, utf8, len) * size / 1000.0;
}

double tr_text_width(tr_font_t font, double size, const char *utf8)
{
    return utf8 ? tr_text_width_n(font, size, utf8, strlen(utf8)) : 0.0;
}

double tr__baseline_in_box(double size, double leading)
{
    /* centre the cap height (718/1000 em for Helvetica and Helvetica-Bold) */
    return leading / 2.0 + 0.718 * size / 2.0;
}

/* ---- output ------------------------------------------------------------- */

static void put_string(tr_doc_t *doc, const char *s, size_t len)
{
    const char *end = s + len;
    tr__putc(doc, '(');
    while (s < end) {
        char e[4];
        size_t k = escape_byte(tr__next_winansi(&s, end, &doc->replaced), e);
        tr__put(doc, e, k);
    }
    tr__puts(doc, ") Tj ET\n");
}

static void put_font(tr_doc_t *doc, tr_font_t font, double size)
{
    tr__puts(doc, font == TR_FONT_HELVETICA_BOLD ? "BT /F2 " : "BT /F1 ");
    tr__put_num(doc, size, ' ');
    tr__puts(doc, "Tf ");
}

int tr_text_n(tr_doc_t *doc, double x, double y, tr_align_t align, tr_font_t font, double size,
              const char *utf8, size_t len)
{
    if (!doc) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (!utf8 || (unsigned)font >= TR_FONT_COUNT || size <= 0) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    if (len == 0) {
        return TR_OK;
    }
    if (align != TR_ALIGN_LEFT) {
        double w = tr_text_width_n(font, size, utf8, len);
        x -= align == TR_ALIGN_CENTER ? w / 2.0 : w;
    }
    put_font(doc, font, size);
    tr__put_num(doc, x, ' ');
    tr__put_num(doc, y, ' ');
    tr__puts(doc, "Td ");
    put_string(doc, utf8, len);
    return doc->err;
}

int tr_text_aligned(tr_doc_t *doc, double x, double y, tr_align_t align, tr_font_t font,
                    double size, const char *utf8)
{
    return tr_text_n(doc, x, y, align, font, size, utf8, utf8 ? strlen(utf8) : 0);
}

int tr_text(tr_doc_t *doc, double x, double y, tr_font_t font, double size, const char *utf8)
{
    return tr_text_aligned(doc, x, y, TR_ALIGN_LEFT, font, size, utf8);
}

int tr_text_rotated(tr_doc_t *doc, double x, double y, double deg, tr_align_t align,
                    tr_font_t font, double size, const char *utf8)
{
    if (!doc) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (!utf8 || (unsigned)font >= TR_FONT_COUNT || size <= 0) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    size_t len = strlen(utf8);
    if (len == 0) {
        return TR_OK;
    }
    double a = deg * 3.14159265358979323846 / 180.0;
    double c = cos(a), s = sin(a);
    if (align != TR_ALIGN_LEFT) {
        double w = tr_text_width_n(font, size, utf8, len);
        double back = align == TR_ALIGN_CENTER ? w / 2.0 : w;
        x -= back * c;
        y -= back * s;
    }
    put_font(doc, font, size);
    tr__put_num(doc, c, ' ');
    tr__put_num(doc, s, ' ');
    tr__put_num(doc, -s, ' ');
    tr__put_num(doc, c, ' ');
    tr__put_num(doc, x, ' ');
    tr__put_num(doc, y, ' ');
    tr__puts(doc, "Tm ");
    put_string(doc, utf8, len);
    return doc->err;
}

/* ---- wrapping ----------------------------------------------------------- */

static void emit_line(tr_line_fn cb, void *user, const char *s, const char *e)
{
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
        e--;
    }
    if (cb) {
        cb(user, s, (size_t)(e - s));
    }
}

int tr_wrap(tr_font_t font, double size, const char *utf8, double max_w, tr_line_fn cb, void *user)
{
    if (!utf8 || size <= 0) {
        return 0;
    }
    const char *end = utf8 + strlen(utf8);
    const char *p = utf8;
    /* compare in font units to avoid accumulating float error */
    double limit = max_w * 1000.0 / size + 1e-6;
    int lines = 0;
    while (p < end) {
        const char *ls = p;
        const char *brk_end = NULL, *brk_next = NULL;
        unsigned long w = 0;
        int done = 0;
        while (p < end) {
            if (*p == '\n') {
                emit_line(cb, user, ls, p);
                p++;
                done = 1;
                break;
            }
            const char *cs = p;
            uint8_t b = tr__next_winansi(&p, end, NULL);
            if (b == ' ') {
                if (cs > ls) {
                    brk_end = cs;
                    brk_next = p;
                }
                w += tr__glyph_w(font, b);
                continue; /* spaces may hang past the edge; they are trimmed */
            }
            unsigned cw = tr__glyph_w(font, b);
            if ((double)(w + cw) > limit && cs > ls) {
                if (brk_end) {
                    emit_line(cb, user, ls, brk_end);
                    p = brk_next;
                } else { /* single word wider than the box: hard break */
                    emit_line(cb, user, ls, cs);
                    p = cs;
                }
                while (p < end && (*p == ' ' || *p == '\t')) {
                    p++;
                }
                done = 1;
                break;
            }
            w += cw;
        }
        if (!done) {
            emit_line(cb, user, ls, end);
        }
        lines++;
    }
    return lines;
}

typedef struct {
    tr_doc_t *doc;
    double x, y_top, w, size, leading;
    tr_font_t font;
    tr_align_t align;
    int idx, max_lines;
} box_ctx_t;

static void box_line(void *user, const char *s, size_t len)
{
    box_ctx_t *b = (box_ctx_t *)user;
    if (b->max_lines > 0 && b->idx >= b->max_lines) {
        return;
    }
    double x = b->align == TR_ALIGN_CENTER ? b->x + b->w / 2.0
               : b->align == TR_ALIGN_RIGHT ? b->x + b->w
                                            : b->x;
    double y = b->y_top - b->idx * b->leading - tr__baseline_in_box(b->size, b->leading);
    tr_text_n(b->doc, x, y, b->align, b->font, b->size, s, len);
    b->idx++;
}

int tr_text_box(tr_doc_t *doc, double x, double y_top, double w, tr_font_t font, double size,
                double leading, tr_align_t align, const char *utf8, int max_lines)
{
    if (!doc) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (!utf8 || w <= 0 || size <= 0 || (unsigned)font >= TR_FONT_COUNT) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    if (leading <= 0) {
        leading = size * 1.2;
    }
    box_ctx_t b = {doc, x, y_top, w, size, leading, font, align, 0, max_lines};
    tr_wrap(font, size, utf8, w, box_line, &b);
    return doc->err != TR_OK ? doc->err : b.idx;
}
