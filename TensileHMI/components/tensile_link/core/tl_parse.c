/* tensile_link core: line classifier (see tl_parse.h). */
#include "tl_parse.h"

#include <string.h>

#define TL_NUM_MAX_INT_DIGITS  10
#define TL_NUM_MAX_FRAC_DIGITS 6
#define TL_TS_MAX_S            4294967.296 /* millis() wrap / 1000 */
#define TL_FORCE_MAX_ABS_KN    100000.0

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool eq(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len == n && memcmp(s, lit, n) == 0;
}

static bool starts(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len >= n && memcmp(s, lit, n) == 0;
}

bool tl_parse_number(const char *s, size_t len, double *out)
{
    size_t i = 0;
    bool neg = false;
    if (len == 0) {
        return false;
    }
    if (s[0] == '-') {
        neg = true;
        i = 1;
    }
    size_t int_start = i;
    double v = 0.0;
    while (i < len && is_digit(s[i])) {
        v = v * 10.0 + (double)(s[i] - '0');
        i++;
    }
    size_t n_int = i - int_start;
    if (n_int == 0 || n_int > TL_NUM_MAX_INT_DIGITS) {
        return false;
    }
    if (i < len) {
        if (s[i] != '.') {
            return false;
        }
        i++;
        size_t frac_start = i;
        double scale = 1.0;
        double frac = 0.0;
        while (i < len && is_digit(s[i])) {
            frac = frac * 10.0 + (double)(s[i] - '0');
            scale *= 10.0;
            i++;
        }
        size_t n_frac = i - frac_start;
        if (n_frac == 0 || n_frac > TL_NUM_MAX_FRAC_DIGITS || i != len) {
            return false;
        }
        v += frac / scale;
    }
    *out = neg ? -v : v;
    return true;
}

static void skip_ws(const char *s, size_t len, size_t *i)
{
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t')) {
        (*i)++;
    }
}

bool tl_parse_json_sample(const char *s, size_t len, double *ts, double *cur, double *peak)
{
    static const char *const keys[3] = {"timestamp", "current", "peak"};
    double vals[3] = {0};
    bool seen[3] = {false, false, false};
    size_t i = 0;

    skip_ws(s, len, &i);
    if (i >= len || s[i] != '{') {
        return false;
    }
    i++;
    for (int field = 0; field < 3; field++) {
        skip_ws(s, len, &i);
        if (i >= len || s[i] != '"') {
            return false;
        }
        i++;
        size_t k0 = i;
        while (i < len && s[i] != '"') {
            i++;
        }
        if (i >= len) {
            return false;
        }
        size_t klen = i - k0;
        i++; /* closing quote */
        int which = -1;
        for (int k = 0; k < 3; k++) {
            if (eq(s + k0, klen, keys[k])) {
                which = k;
            }
        }
        if (which < 0 || seen[which]) {
            return false;
        }
        skip_ws(s, len, &i);
        if (i >= len || s[i] != ':') {
            return false;
        }
        i++;
        skip_ws(s, len, &i);
        size_t v0 = i;
        while (i < len && (is_digit(s[i]) || s[i] == '-' || s[i] == '.')) {
            i++;
        }
        if (!tl_parse_number(s + v0, i - v0, &vals[which])) {
            return false;
        }
        seen[which] = true;
        skip_ws(s, len, &i);
        if (i >= len) {
            return false;
        }
        if (field < 2) {
            if (s[i] != ',') {
                return false;
            }
        } else if (s[i] != '}') {
            return false;
        }
        i++;
    }
    skip_ws(s, len, &i);
    if (i != len) {
        return false;
    }
    if (vals[0] < 0.0 || vals[0] > TL_TS_MAX_S) {
        return false;
    }
    for (int k = 1; k < 3; k++) {
        if (vals[k] > TL_FORCE_MAX_ABS_KN || vals[k] < -TL_FORCE_MAX_ABS_KN) {
            return false;
        }
    }
    *ts = vals[0];
    *cur = vals[1];
    *peak = vals[2];
    return true;
}

static bool parse_csv_sample(const char *s, size_t len, double *cur, double *peak)
{
    const char *comma = memchr(s, ',', len);
    if (!comma) {
        return false;
    }
    size_t a = (size_t)(comma - s);
    if (!tl_parse_number(s, a, cur) || !tl_parse_number(comma + 1, len - a - 1, peak)) {
        return false;
    }
    return *cur <= TL_FORCE_MAX_ABS_KN && *cur >= -TL_FORCE_MAX_ABS_KN &&
           *peak <= TL_FORCE_MAX_ABS_KN && *peak >= -TL_FORCE_MAX_ABS_KN;
}

/* Arduino printFloat specials or a numeric-looking token: used to tell a
 * broken CSV sample ("nan,0.00") from unrelated text. */
static bool csvish_token(const char *s, size_t len)
{
    double d;
    if (eq(s, len, "nan") || eq(s, len, "inf") || eq(s, len, "ovf")) {
        return true;
    }
    return tl_parse_number(s, len, &d);
}

tl_line_kind_t tl_classify(const char *line, size_t len, bool truncated, tl_line_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = TL_LINE_UNKNOWN;

    if (truncated) {
        out->kind = TL_LINE_OVERFLOW;
        return out->kind;
    }
    if (len == 0) {
        return out->kind;
    }

    /* Samples first: they are by far the most frequent lines. */
    if (line[0] == '{') {
        out->fmt = TL_FMT_JSON;
        if (tl_parse_json_sample(line, len, &out->timestamp_s, &out->current_kN, &out->peak_kN)) {
            out->kind = TL_LINE_SAMPLE_JSON;
        } else {
            out->kind = TL_LINE_SAMPLE_BAD;
        }
        return out->kind;
    }

    const char *comma = memchr(line, ',', len);
    if (comma && parse_csv_sample(line, len, &out->current_kN, &out->peak_kN)) {
        out->kind = TL_LINE_SAMPLE_CSV;
        out->fmt = TL_FMT_CSV;
        return out->kind;
    }
    if (comma) {
        size_t a = (size_t)(comma - line);
        if (csvish_token(line, a) && csvish_token(comma + 1, len - a - 1)) {
            out->kind = TL_LINE_SAMPLE_BAD;
            out->fmt = TL_FMT_CSV;
            return out->kind;
        }
    }

    /* Boot calibration dump: "<raw>\t<value>" */
    if (is_digit(line[0])) {
        const char *tab = memchr(line, '\t', len);
        if (tab) {
            size_t n = (size_t)(tab - line);
            bool digits = n > 0 && n <= 7;
            uint32_t raw = 0;
            for (size_t i = 0; digits && i < n; i++) {
                if (!is_digit(line[i])) {
                    digits = false;
                } else {
                    raw = raw * 10u + (uint32_t)(line[i] - '0');
                }
            }
            size_t rest = len - n - 1;
            if (digits && rest > 0) {
                out->kind = TL_LINE_CALIBRATION_ROW;
                out->calib_raw = raw;
                out->value_valid = tl_parse_number(tab + 1, rest, &out->calib_value);
                return out->kind;
            }
        }
    }

    if (starts(line, len, TL_FW_MENU_HEADER_PREFIX)) {
        static const char suffix[] = " kN";
        size_t p = strlen(TL_FW_MENU_HEADER_PREFIX);
        size_t sl = sizeof(suffix) - 1;
        out->kind = TL_LINE_MENU_HEADER;
        if (len >= p + sl && memcmp(line + len - sl, suffix, sl) == 0) {
            out->value_valid = tl_parse_number(line + p, len - p - sl, &out->peak_kN);
        }
        return out->kind;
    }

    static const struct {
        const char *text;
        tl_line_kind_t kind;
        char option;
        tl_fmt_t fmt;
    } table[] = {
        {TL_FW_MENU_SEP, TL_LINE_MENU_OPTION, '-', TL_FMT_UNKNOWN},
        {TL_FW_MENU_OPT_R, TL_LINE_MENU_OPTION, 'r', TL_FMT_UNKNOWN},
        {TL_FW_MENU_OPT_X, TL_LINE_MENU_OPTION, 'x', TL_FMT_UNKNOWN},
        {TL_FW_MENU_OPT_J_JSON, TL_LINE_MENU_OPTION, 'j', TL_FMT_JSON},
        {TL_FW_MENU_OPT_J_CSV, TL_LINE_MENU_OPTION, 'j', TL_FMT_CSV},
        {TL_FW_MENU_OPT_C, TL_LINE_MENU_OPTION, 'c', TL_FMT_UNKNOWN},
        {TL_FW_MODE_JSON, TL_LINE_MODE_CHANGED, 0, TL_FMT_JSON},
        {TL_FW_MODE_CSV, TL_LINE_MODE_CHANGED, 0, TL_FMT_CSV},
        {TL_FW_NEW_TEST, TL_LINE_NEW_TEST_ACK, 0, TL_FMT_UNKNOWN},
        {TL_FW_RESUMED, TL_LINE_RESUMED, 0, TL_FMT_UNKNOWN},
        {TL_FW_CALIB_NOT_IMPL, TL_LINE_CALIB_NOT_IMPL, 0, TL_FMT_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (eq(line, len, table[i].text)) {
            out->kind = table[i].kind;
            out->option = table[i].option;
            out->fmt = table[i].fmt;
            return out->kind;
        }
    }
    return out->kind;
}

const char *tl_line_kind_name(tl_line_kind_t k)
{
    switch (k) {
    case TL_LINE_UNKNOWN: return "UNKNOWN";
    case TL_LINE_OVERFLOW: return "OVERFLOW";
    case TL_LINE_MENU_HEADER: return "MENU_HEADER";
    case TL_LINE_MENU_OPTION: return "MENU_OPTION";
    case TL_LINE_MODE_CHANGED: return "MODE_CHANGED";
    case TL_LINE_NEW_TEST_ACK: return "NEW_TEST_ACK";
    case TL_LINE_RESUMED: return "RESUMED";
    case TL_LINE_CALIB_NOT_IMPL: return "CALIB_NOT_IMPL";
    case TL_LINE_CALIBRATION_ROW: return "CALIBRATION_ROW";
    case TL_LINE_SAMPLE_JSON: return "SAMPLE_JSON";
    case TL_LINE_SAMPLE_CSV: return "SAMPLE_CSV";
    case TL_LINE_SAMPLE_BAD: return "SAMPLE_BAD";
    }
    return "?";
}
