/* Internal helpers shared by the tensile_report core files. Not public API. */
#ifndef TR_INTERNAL_H
#define TR_INTERNAL_H

#include "tensile_report.h"

#define TR_WIDTH_FIRST 32
#define TR_WIDTH_COUNT 224

extern const uint16_t tr_font_widths[TR_FONT_COUNT][TR_WIDTH_COUNT];

/* Fixed object numbers. */
#define TR_OBJ_CATALOG 1u
#define TR_OBJ_PAGES 2u
#define TR_OBJ_INFO 3u
#define TR_OBJ_FONT_REG 4u
#define TR_OBJ_FONT_BOLD 5u
#define TR_OBJ_RESOURCES 6u
#define TR_OBJ_FIRST_FREE 7u

/* Output (tr_pdf.c). All are no-ops once doc->err is set. */
void tr__put(tr_doc_t *doc, const void *data, size_t len);
void tr__puts(tr_doc_t *doc, const char *s);
void tr__putc(tr_doc_t *doc, char c);
void tr__put_uint(tr_doc_t *doc, uint32_t v);
/* Number with up to 3 decimals, trailing zeros trimmed, followed by sep (if nonzero). */
void tr__put_num(tr_doc_t *doc, double v, char sep);
int tr__fail(tr_doc_t *doc, int err);
/* Returns doc->err, or TR_ERR_STATE if no page is open. */
int tr__check_page(tr_doc_t *doc);

/* Text (tr_text.c). Decodes one UTF-8 char from *p (< end), returns the
 * WinAnsi byte (or '?' with ++*replaced when unmappable). */
uint8_t tr__next_winansi(const char **p, const char *end, uint32_t *replaced);
/* Width of one WinAnsi byte in 1/1000 em. */
unsigned tr__glyph_w(tr_font_t font, uint8_t code);
/* Vertical offset from line-box top to baseline for text centred in a line box. */
double tr__baseline_in_box(double size, double leading);

#endif
