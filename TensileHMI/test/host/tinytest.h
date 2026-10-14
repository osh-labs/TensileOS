/* Minimal host test harness. Usage: TT_CHECK(cond); TT_EQ_INT(a,b); TT_EQ_STR(a,b); TT_RUN(fn); return tt_summary(); */
#ifndef TINYTEST_H
#define TINYTEST_H
#include <stdio.h>
#include <string.h>
static int tt_fail = 0, tt_pass = 0;
#define TT_CHECK(c) do { if (c) tt_pass++; else { tt_fail++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define TT_EQ_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a == _b) tt_pass++; else { tt_fail++; fprintf(stderr, "%s:%d: %s == %s failed (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define TT_EQ_STR(a, b) do { const char *_a = (a), *_b = (b); if (_a && _b && strcmp(_a, _b) == 0) tt_pass++; else { tt_fail++; fprintf(stderr, "%s:%d: %s == %s failed (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)
#define TT_NEAR(a, b, eps) do { double _a = (a), _b = (b); if ((_a - _b) <= (eps) && (_b - _a) <= (eps)) tt_pass++; else { tt_fail++; fprintf(stderr, "%s:%d: %s ~= %s failed (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define TT_RUN(fn) do { fprintf(stderr, "-- %s\n", #fn); fn(); } while (0)
static inline int tt_summary(void) { fprintf(stderr, "%d passed, %d failed\n", tt_pass, tt_fail); return tt_fail ? 1 : 0; }
#endif
