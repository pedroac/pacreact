#ifndef SCALE_H
#define SCALE_H

#include <stdlib.h>
#include "tunables.h"

/* Scales are kept as integers in 120ths, as the fractional-scale protocol sends them:
 * 120 is 1x, 180 is 1.5x, 240 is 2x. */
#define SCALE_UNIT 120

/* Buffer pixels needed for `logical` pixels at the given scale (rounded up, so the content
 * never ends up smaller than the surface it fills). */
static inline int scaled_px(int logical, int scale120) {
    return (int)(((long long)logical * scale120 + SCALE_UNIT - 1) / SCALE_UNIT);
}

/* "1.5" or "2" as a scale in 120ths, or 0 if it is not a number between 1 and 4. */
static inline int parse_scale120(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    double v = strtod(s, &end);
    if (*end != '\0' || !(v >= 1.0) || v > 4.0) return 0;
    return (int)(v * SCALE_UNIT + 0.5);
}

/* The nearest whole scale, for compositors without fractional scaling. */
static inline int whole_scale(int scale120) {
    int n = (scale120 + SCALE_UNIT / 2) / SCALE_UNIT;
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

#endif
