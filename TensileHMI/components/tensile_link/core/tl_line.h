/*
 * tensile_link core: line assembler.
 *
 * Turns an arbitrary-chunked byte stream from the OpenScale (FT231X, 115200 8N1)
 * into complete lines. Accepts CR, LF or CRLF terminators (Arduino println
 * sends CRLF). Empty lines are swallowed. Lines longer than TL_LINE_MAX-1 are
 * truncated, flagged, and the remainder up to the next terminator is discarded.
 * NUL bytes are replaced by '?' so the result is always a valid C string.
 *
 * Pure C11, no ESP-IDF headers, no allocation.
 */
#ifndef TL_LINE_H
#define TL_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest v1.6 line is the menu option "x) Start new test (reset peak and timestamp)"
 * (44 chars) or a JSON sample (~60 chars). 128 leaves margin. */
#define TL_LINE_MAX 128

typedef struct {
    char buf[TL_LINE_MAX];
    size_t len;
    bool overflow;          /* current line exceeded the buffer */
    bool last_was_cr;       /* swallow the LF of a CRLF pair */
    bool ready;             /* buf holds a completed line (cleared on next put) */
    uint32_t lines;         /* completed lines */
    uint32_t overflows;     /* truncated lines */
} tl_line_asm_t;

void tl_line_init(tl_line_asm_t *a);

/* Drop any partial line (e.g. after a device reset). Counters are kept. */
void tl_line_reset(tl_line_asm_t *a);

/*
 * Feed one byte. Returns a pointer to the completed, NUL-terminated line when
 * this byte terminates a non-empty line, else NULL. The pointer stays valid
 * until the next call. *len receives the length, *truncated whether the line
 * overflowed (either may be NULL).
 */
const char *tl_line_put(tl_line_asm_t *a, uint8_t c, size_t *len, bool *truncated);

#ifdef __cplusplus
}
#endif

#endif /* TL_LINE_H */
