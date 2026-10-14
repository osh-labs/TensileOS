/* tensile_link core: line assembler (see tl_line.h). */
#include "tl_line.h"

#include <string.h>

void tl_line_init(tl_line_asm_t *a)
{
    memset(a, 0, sizeof(*a));
}

void tl_line_reset(tl_line_asm_t *a)
{
    a->len = 0;
    a->overflow = false;
    a->last_was_cr = false;
    a->ready = false;
    a->buf[0] = '\0';
}

const char *tl_line_put(tl_line_asm_t *a, uint8_t c, size_t *len, bool *truncated)
{
    if (a->ready) {
        /* Previous line was consumed by the caller; start a new one. */
        a->ready = false;
        a->len = 0;
        a->overflow = false;
    }

    if (c == '\r' || c == '\n') {
        bool swallow = (c == '\n' && a->last_was_cr);
        a->last_was_cr = (c == '\r');
        if (swallow || (a->len == 0 && !a->overflow)) {
            return NULL; /* LF of CRLF, or empty line */
        }
        a->buf[a->len] = '\0';
        a->ready = true;
        a->lines++;
        if (a->overflow) {
            a->overflows++;
        }
        if (len) {
            *len = a->len;
        }
        if (truncated) {
            *truncated = a->overflow;
        }
        return a->buf;
    }

    a->last_was_cr = false;
    if (a->len >= TL_LINE_MAX - 1) {
        a->overflow = true; /* discard until terminator */
        return NULL;
    }
    a->buf[a->len++] = (c == 0) ? '?' : (char)c;
    return NULL;
}
