/* tensile_report core: streaming table helper (row by row, header repeated after page breaks). */
#include <string.h>

#include "tr_internal.h"

void tr_table_defaults(tr_table_t *t)
{
    if (!t) {
        return;
    }
    t->align = TR_ALIGN_CENTER;
    t->repeat_header = 1;
    t->header_font = TR_FONT_HELVETICA_BOLD;
    t->body_font = TR_FONT_HELVETICA;
    t->font_size = 9;
    t->leading = 10;
    t->pad_x = 4;
    t->pad_y = 4;
    t->text_color = tr_rgb_hex(0x000000);
    t->header_bg = tr_rgb_hex(0xE3F2FD);
    t->row_bg[0] = tr_rgb_hex(0xFFFFFF);
    t->row_bg[1] = tr_rgb_hex(0xF5F5F5);
    t->grid_color = tr_rgb_hex(0x808080);
    t->grid_w = 0.5;
    t->row_index = 0;
}

static int table_ok(const tr_table_t *t)
{
    return t && t->ncols > 0 && t->ncols <= TR_TABLE_MAX_COLS && t->col_w && t->font_size > 0 &&
           t->leading > 0;
}

static double table_width(const tr_table_t *t)
{
    double w = 0;
    for (int i = 0; i < t->ncols; i++) {
        w += t->col_w[i];
    }
    return w;
}

double tr_table_row_height(const tr_table_t *t, const char *const *cells, tr_font_t font)
{
    if (!table_ok(t) || !cells) {
        return 0;
    }
    int max_lines = 1;
    for (int i = 0; i < t->ncols; i++) {
        double w = t->col_w[i] - 2 * t->pad_x;
        int n = tr_wrap(font, t->font_size, cells[i] ? cells[i] : "", w > 1 ? w : 1, NULL, NULL);
        if (n > max_lines) {
            max_lines = n;
        }
    }
    return max_lines * t->leading + 2 * t->pad_y;
}

static void draw_row(tr_doc_t *doc, const tr_table_t *t, const char *const *cells, tr_font_t font,
                     tr_rgb_t bg, double top, double h)
{
    double tw = table_width(t);
    tr_set_fill_rgb(doc, bg);
    tr_rect(doc, t->x, top - h, tw, h, TR_PAINT_FILL);
    tr_set_fill_rgb(doc, t->text_color);
    double x = t->x;
    for (int i = 0; i < t->ncols; i++) {
        tr_align_t a = t->col_align ? t->col_align[i] : t->align;
        double w = t->col_w[i] - 2 * t->pad_x;
        if (cells[i] && cells[i][0] && w > 0) {
            tr_text_box(doc, x + t->pad_x, top - t->pad_y, w, font, t->font_size, t->leading, a,
                        cells[i], 0);
        }
        x += t->col_w[i];
    }
    if (t->grid_w > 0) {
        tr_set_stroke_rgb(doc, t->grid_color);
        tr_set_line_width(doc, t->grid_w);
        tr_set_dash(doc, NULL, 0, 0);
        x = t->x;
        for (int i = 0; i < t->ncols; i++) {
            tr_rect(doc, x, top - h, t->col_w[i], h, TR_PAINT_STROKE);
            x += t->col_w[i];
        }
    }
}

int tr_table_header(tr_doc_t *doc, tr_table_t *t, double *y)
{
    if (!doc || !y) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (!table_ok(t)) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    if (!t->header) {
        return TR_OK;
    }
    double h = tr_table_row_height(t, t->header, t->header_font);
    draw_row(doc, t, t->header, t->header_font, t->header_bg, *y, h);
    *y -= h;
    return doc->err;
}

int tr_table_row(tr_doc_t *doc, tr_table_t *t, const char *const *cells, double *y)
{
    if (!doc || !y) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (!table_ok(t) || !cells) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    double h = tr_table_row_height(t, cells, t->body_font);
    if (*y - h < t->y_bottom) {
        if (tr_page_break(doc) != TR_OK) {
            return doc->err;
        }
        *y = t->y_top;
        if (t->repeat_header && t->header) {
            tr_table_header(doc, t, y);
        }
    }
    draw_row(doc, t, cells, t->body_font, t->row_bg[t->row_index & 1], *y, h);
    *y -= h;
    t->row_index++;
    return doc->err;
}
