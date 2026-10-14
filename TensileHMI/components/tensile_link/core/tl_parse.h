/*
 * tensile_link core: line classifier for TensileOS firmware v1.6 (src/main.cpp).
 *
 * Every string below is matched against the exact text the firmware prints.
 * Number grammar is the Arduino Print::printFloat output: -?[0-9]{1,10}(\.[0-9]{1,6})?
 * "nan", "inf" and "ovf" are rejected wherever a sample value is expected.
 *
 * Pure C11, no ESP-IDF headers, no allocation.
 */
#ifndef TL_PARSE_H
#define TL_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TL_LINE_UNKNOWN = 0,     /* anything else (garbage, partial lines, other devices) */
    TL_LINE_OVERFLOW,        /* line was truncated by the assembler */
    TL_LINE_MENU_HEADER,     /* "Measurement Paused. Peak: 3.38 kN" */
    TL_LINE_MENU_OPTION,     /* "--------", "r) ...", "x) ...", "j) ... (current: JSON)", "c) ..." */
    TL_LINE_MODE_CHANGED,    /* "Output format changed to: JSON" / "CSV" */
    TL_LINE_NEW_TEST_ACK,    /* "Starting new test..." */
    TL_LINE_RESUMED,         /* "Resuming measurements..." */
    TL_LINE_CALIB_NOT_IMPL,  /* "Calibration mode not yet implemented." */
    TL_LINE_CALIBRATION_ROW, /* boot dump "<raw>\t<value>", raw 0..7000 step 20 */
    TL_LINE_SAMPLE_JSON,     /* {"timestamp":12.345,"current":1.234,"peak":5.678} */
    TL_LINE_SAMPLE_CSV,      /* "1.23,5.68" */
    TL_LINE_SAMPLE_BAD,      /* looks like a sample but fails strict parsing (nan, garbage) */
} tl_line_kind_t;

typedef enum {
    TL_FMT_UNKNOWN = 0,
    TL_FMT_CSV,
    TL_FMT_JSON,
} tl_fmt_t;

typedef struct {
    tl_line_kind_t kind;
    tl_fmt_t fmt;        /* MODE_CHANGED, MENU_OPTION 'j', SAMPLE_* */
    char option;         /* MENU_OPTION: '-', 'r', 'x', 'j', 'c' */
    bool value_valid;    /* MENU_HEADER peak / CALIBRATION_ROW value parsed */
    double peak_kN;      /* MENU_HEADER, SAMPLE_JSON, SAMPLE_CSV */
    double current_kN;   /* SAMPLE_JSON, SAMPLE_CSV */
    double timestamp_s;  /* SAMPLE_JSON only */
    uint32_t calib_raw;  /* CALIBRATION_ROW */
    double calib_value;  /* CALIBRATION_ROW */
} tl_line_info_t;

/* Exact firmware strings (v1.6). Exposed so tests and transcripts share them. */
#define TL_FW_MENU_HEADER_PREFIX "Measurement Paused. Peak: "
#define TL_FW_MENU_SEP           "--------"
#define TL_FW_MENU_OPT_R         "r) Resume measurement"
#define TL_FW_MENU_OPT_X         "x) Start new test (reset peak and timestamp)"
#define TL_FW_MENU_OPT_J_JSON    "j) Toggle output format (current: JSON)"
#define TL_FW_MENU_OPT_J_CSV     "j) Toggle output format (current: CSV)"
#define TL_FW_MENU_OPT_C         "c) Enter calibration mode (future feature)"
#define TL_FW_MODE_JSON          "Output format changed to: JSON"
#define TL_FW_MODE_CSV           "Output format changed to: CSV"
#define TL_FW_NEW_TEST           "Starting new test..."
#define TL_FW_RESUMED            "Resuming measurements..."
#define TL_FW_CALIB_NOT_IMPL     "Calibration mode not yet implemented."

/* Highest raw value in the boot calibration dump (for progress display). */
#define TL_FW_CALIB_RAW_LAST 7000u

/*
 * Classify one line (without terminator). `truncated` comes from the line
 * assembler; truncated lines are always TL_LINE_OVERFLOW. Never fails:
 * unrecognised input is TL_LINE_UNKNOWN. Returns out->kind.
 */
tl_line_kind_t tl_classify(const char *line, size_t len, bool truncated, tl_line_info_t *out);

/*
 * Strict number parser for Arduino float output. Parses exactly `len` chars.
 * Returns true on success.
 */
bool tl_parse_number(const char *s, size_t len, double *out);

/* Strict JSON sample parser; exposed for tests. */
bool tl_parse_json_sample(const char *s, size_t len, double *ts, double *cur, double *peak);

const char *tl_line_kind_name(tl_line_kind_t k);

#ifdef __cplusplus
}
#endif

#endif /* TL_PARSE_H */
