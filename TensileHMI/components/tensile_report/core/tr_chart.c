/* tensile_report core: vector bar/line chart helper (axes, nice ticks, bars, reference lines, legend). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tr_internal.h"

#define TICK_FONT_SIZE 7.5
#define LABEL_FONT_SIZE 9.0
#define TITLE_FONT_SIZE 10.0
#define LEGEND_FONT_SIZE 8.0
#define LEGEND_ROW_H 12.0

static tr_rgb_t ink(void) { return tr_rgb_hex(0x212121); }

static void fmt_tick(char *buf, size_t n, double v, int decimals)
{
    if (fabs(v) < 1e-12) {
        v = 0.0; /* avoid "-0.0" */
    }
    snprintf(buf, n, "%.*f", decimals, v);
}

static void add_legend(tr_chart_t *c, const char *label, tr_rgb_t color, double width,
                       tr_line_style_t style, int is_box)
{
    if (!label || !*label || c->nlegend >= TR_CHART_MAX_LEGEND) {
        return;
    }
    tr_legend_entry_t *e = &c->legend[c->nlegend++];
    e->label = label;
    e->color = color;
    e->width = width;
    e->style = style;
    e->is_box = is_box;
}

double tr_chart_map_y(const tr_chart_t *c, double v)
{
    double span = c->y_hi - c->y_lo;
    return c->py + (span > 0 ? (v - c->y_lo) / span * c->ph : 0);
}

double tr_chart_map_x(const tr_chart_t *c, double v)
{
    double span = c->cfg.x_max - c->cfg.x_min;
    return c->px + (span > 0 ? (v - c->cfg.x_min) / span * c->pw : 0);
}

int tr_chart_begin(tr_doc_t *doc, const tr_chart_cfg_t *cfg, tr_chart_t *c)
{
    if (!doc || !c) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (!cfg || cfg->w <= 0 || cfg->h <= 0) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    if (tr_nice_ticks(cfg->y_min, cfg->y_max, cfg->max_ticks > 0 ? cfg->max_ticks : 6, &c->yt) !=
        TR_OK) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    c->y_lo = c->yt.start;
    c->y_hi = c->yt.start + (c->yt.count - 1) * c->yt.step;

    /* widest y tick label decides the left margin */
    double tick_w = 0;
    char buf[32];
    for (int i = 0; i < c->yt.count; i++) {
        fmt_tick(buf, sizeof buf, c->yt.start + i * c->yt.step, c->yt.decimals);
        double w = tr_text_width(TR_FONT_HELVETICA, TICK_FONT_SIZE, buf);
        tick_w = w > tick_w ? w : tick_w;
    }
    double ml = cfg->margin_left > 0 ? cfg->margin_left
                                     : tick_w + 8 + (cfg->y_label ? LABEL_FONT_SIZE + 6 : 0);
    double mb = cfg->margin_bottom > 0 ? cfg->margin_bottom : (cfg->x_label ? 34 : 20);
    double mt = cfg->margin_top > 0 ? cfg->margin_top : (cfg->title ? TITLE_FONT_SIZE + 10 : 6);
    double mr = cfg->legend_w > 0 ? cfg->legend_w + 8 : 6;
    c->px = cfg->x + ml;
    c->py = cfg->y + mb;
    c->pw = cfg->w - ml - mr;
    c->ph = cfg->h - mb - mt;
    if (c->pw <= 10 || c->ph <= 10) {
        return tr__fail(doc, TR_ERR_ARG);
    }

    tr_save(doc);
    /* plot background + grid */
    tr_set_line_style(doc, TR_LINE_SOLID);
    if (cfg->grid) {
        tr_set_stroke_rgb(doc, tr_rgb_hex(0xDDDDDD));
        tr_set_line_width(doc, 0.5);
        for (int i = 0; i < c->yt.count; i++) {
            double y = tr_chart_map_y(c, c->yt.start + i * c->yt.step);
            tr_line(doc, c->px, y, c->px + c->pw, y);
        }
    }
    /* y ticks and labels */
    tr_set_stroke_rgb(doc, ink());
    tr_set_fill_rgb(doc, ink());
    tr_set_line_width(doc, 0.6);
    for (int i = 0; i < c->yt.count; i++) {
        double v = c->yt.start + i * c->yt.step;
        double y = tr_chart_map_y(c, v);
        tr_line(doc, c->px - 3, y, c->px, y);
        fmt_tick(buf, sizeof buf, v, c->yt.decimals);
        tr_text_aligned(doc, c->px - 5, y - TICK_FONT_SIZE * 0.35, TR_ALIGN_RIGHT,
                        TR_FONT_HELVETICA, TICK_FONT_SIZE, buf);
    }
    if (cfg->title) {
        tr_text_aligned(doc, c->px + c->pw / 2, c->py + c->ph + 7, TR_ALIGN_CENTER,
                        TR_FONT_HELVETICA_BOLD, TITLE_FONT_SIZE, cfg->title);
    }
    if (cfg->y_label) {
        tr_text_rotated(doc, cfg->x + LABEL_FONT_SIZE, c->py + c->ph / 2, 90, TR_ALIGN_CENTER,
                        TR_FONT_HELVETICA, LABEL_FONT_SIZE, cfg->y_label);
    }
    if (cfg->x_label) {
        tr_text_aligned(doc, c->px + c->pw / 2, cfg->y + 3, TR_ALIGN_CENTER, TR_FONT_HELVETICA,
                        LABEL_FONT_SIZE, cfg->x_label);
    }
    tr_restore(doc);
    return doc->err;
}

int tr_chart_bars(tr_doc_t *doc, tr_chart_t *c, const double *values, const char *const *labels,
                  int n, tr_rgb_t color, const char *legend_label)
{
    if (!doc || !c) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (n < 0 || (n > 0 && !values)) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    if (n == 0) {
        return TR_OK;
    }
    double slot = c->pw / n;
    double bw = slot * 0.7;
    double base = c->y_lo > 0 ? c->y_lo : (c->y_hi < 0 ? c->y_hi : 0);
    double y0 = tr_chart_map_y(c, base);
    tr_save(doc);
    tr_clip_rect(doc, c->px, c->py, c->pw, c->ph);
    tr_set_fill_rgb(doc, color);
    for (int i = 0; i < n; i++) {
        double cx = c->px + slot * (i + 0.5);
        double y1 = tr_chart_map_y(c, values[i]);
        double lo = y1 < y0 ? y1 : y0;
        tr_rect(doc, cx - bw / 2, lo, bw, fabs(y1 - y0), TR_PAINT_FILL);
    }
    tr_restore(doc);
    if (labels) {
        tr_save(doc);
        tr_set_stroke_rgb(doc, ink());
        tr_set_fill_rgb(doc, ink());
        tr_set_line_width(doc, 0.6);
        tr_set_line_style(doc, TR_LINE_SOLID);
        for (int i = 0; i < n; i++) {
            double cx = c->px + slot * (i + 0.5);
            tr_line(doc, cx, c->py, cx, c->py - 3);
            if (!labels[i]) {
                continue;
            }
            if (fabs(c->cfg.x_label_angle) > 0.1) {
                tr_text_rotated(doc, cx + 2, c->py - 6, c->cfg.x_label_angle, TR_ALIGN_RIGHT,
                                TR_FONT_HELVETICA, TICK_FONT_SIZE, labels[i]);
            } else {
                tr_text_aligned(doc, cx, c->py - 4 - TICK_FONT_SIZE, TR_ALIGN_CENTER,
                                TR_FONT_HELVETICA, TICK_FONT_SIZE, labels[i]);
            }
        }
        tr_restore(doc);
    }
    add_legend(c, legend_label, color, 0, TR_LINE_SOLID, 1);
    return doc->err;
}

int tr_chart_hline(tr_doc_t *doc, tr_chart_t *c, double value, tr_rgb_t color, double width,
                   tr_line_style_t style, const char *legend_label, const char *inline_label)
{
    if (!doc || !c) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    add_legend(c, legend_label, color, width, style, 0);
    if (value < c->y_lo || value > c->y_hi) {
        return TR_OK; /* outside the axis range: legend only */
    }
    double y = tr_chart_map_y(c, value);
    tr_save(doc);
    tr_set_stroke_rgb(doc, color);
    tr_set_line_width(doc, width > 0 ? width : 1);
    tr_set_line_style(doc, style);
    tr_line(doc, c->px, y, c->px + c->pw, y);
    if (inline_label && *inline_label) {
        /* white backing so the label stays readable over bars */
        double tw = tr_text_width(TR_FONT_HELVETICA, 7, inline_label);
        double rx = c->px + c->pw - 4 - tw;
        tr_set_fill_rgb(doc, tr_rgb_hex(0xFFFFFF));
        tr_rect(doc, rx - 1.5, y + 1.5, tw + 3, 8, TR_PAINT_FILL);
        tr_set_fill_rgb(doc, color);
        tr_text(doc, rx, y + 3, TR_FONT_HELVETICA, 7, inline_label);
    }
    tr_restore(doc);
    return doc->err;
}

int tr_chart_line(tr_doc_t *doc, tr_chart_t *c, const double *xs, const double *ys, int n,
                  tr_rgb_t color, double width, const char *legend_label)
{
    if (!doc || !c) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    if (n < 0 || (n > 0 && (!xs || !ys))) {
        return tr__fail(doc, TR_ERR_ARG);
    }
    add_legend(c, legend_label, color, width, TR_LINE_SOLID, 0);
    if (n < 2) {
        return TR_OK;
    }
    tr_save(doc);
    tr_clip_rect(doc, c->px, c->py, c->pw, c->ph);
    tr_set_stroke_rgb(doc, color);
    tr_set_line_width(doc, width > 0 ? width : 1);
    tr_set_line_style(doc, TR_LINE_SOLID);
    tr__puts(doc, "1 j\n"); /* round joins */
    /* streamed point by point: no temporary arrays */
    for (int i = 0; i < n; i++) {
        tr__put_num(doc, tr_chart_map_x(c, xs[i]), ' ');
        tr__put_num(doc, tr_chart_map_y(c, ys[i]), ' ');
        tr__puts(doc, i == 0 ? "m\n" : "l\n");
    }
    tr__puts(doc, "S\n");
    tr_restore(doc);
    return doc->err;
}

int tr_chart_end(tr_doc_t *doc, tr_chart_t *c)
{
    if (!doc || !c) {
        return TR_ERR_ARG;
    }
    if (tr__check_page(doc) != TR_OK) {
        return doc->err;
    }
    tr_save(doc);
    /* frame on top of the data */
    tr_set_line_style(doc, TR_LINE_SOLID);
    tr_set_stroke_rgb(doc, ink());
    tr_set_line_width(doc, 0.8);
    tr_rect(doc, c->px, c->py, c->pw, c->ph, TR_PAINT_STROKE);

    if (c->cfg.legend_w > 0 && c->nlegend > 0) {
        double lx = c->px + c->pw + 8;
        double lw = c->cfg.legend_w;
        double lh = c->nlegend * LEGEND_ROW_H + 6;
        double ly = c->py + c->ph / 2 + lh / 2; /* top */
        tr_set_fill_rgb(doc, tr_rgb_hex(0xFFFFFF));
        tr_set_stroke_rgb(doc, tr_rgb_hex(0xCCCCCC));
        tr_set_line_width(doc, 0.6);
        tr_rect(doc, lx, ly - lh, lw, lh, TR_PAINT_FILL_STROKE);
        for (int i = 0; i < c->nlegend; i++) {
            const tr_legend_entry_t *e = &c->legend[i];
            double cy = ly - 3 - LEGEND_ROW_H * (i + 0.5);
            if (e->is_box) {
                tr_set_fill_rgb(doc, e->color);
                tr_rect(doc, lx + 7, cy - 4, 12, 8, TR_PAINT_FILL);
            } else {
                tr_set_stroke_rgb(doc, e->color);
                tr_set_line_width(doc, e->width > 0 ? e->width : 1);
                tr_set_line_style(doc, e->style);
                tr_line(doc, lx + 5, cy, lx + 21, cy);
            }
            tr_set_fill_rgb(doc, ink());
            tr_text(doc, lx + 26, cy - LEGEND_FONT_SIZE * 0.35, TR_FONT_HELVETICA,
                    LEGEND_FONT_SIZE, e->label);
        }
    }
    tr_restore(doc);
    return doc->err;
}
