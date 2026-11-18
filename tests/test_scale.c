/* The scale arithmetic: buffer sizes, parsing scale, whole-number fallbacks. */
#include "../src/scale.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* Buffer pixels: exact where it divides, rounded up where it does not */
    assert(scaled_px(100, 120) == 100);
    assert(scaled_px(100, 180) == 150);
    assert(scaled_px(101, 180) == 152);      /* 151.5 */
    assert(scaled_px(7, 150) == 9);          /* 8.75 */
    assert(scaled_px(300, 240) == 600);
    assert(scaled_px(1, 120) == 1);
    assert(scaled_px(64, 125) == 67);        /* 66.67: the content must not end up smaller */
    for (int logical = 1; logical < 2000; logical += 37)
        for (int s = SCALE_MIN; s <= SCALE_MAX; s += 7)
            assert(scaled_px(logical, s) * SCALE_UNIT >= logical * s &&              /* never smaller... */
                   (scaled_px(logical, s) - 1) * SCALE_UNIT < logical * s);          /* ...and no bigger than needed */

    /* scale */
    assert(parse_scale120("1") == 120);
    assert(parse_scale120("1.5") == 180);
    assert(parse_scale120("2") == 240);
    assert(parse_scale120("1.25") == 150);
    assert(parse_scale120("1.333") == 160);  /* 159.96, rounded */
    assert(parse_scale120("4") == 480);
    assert(parse_scale120("4.01") == 0);     /* above 4x */
    assert(parse_scale120("0.5") == 0);      /* below 1x */
    assert(parse_scale120("0") == 0);
    assert(parse_scale120("-2") == 0);
    assert(parse_scale120("abc") == 0);
    assert(parse_scale120("1.5x") == 0);     /* trailing junk */
    assert(parse_scale120("") == 0);
    assert(parse_scale120(NULL) == 0);
    assert(parse_scale120("nan") == 0);

    /* Compositors without fractional scaling get the nearest whole scale */
    assert(whole_scale(120) == 1);
    assert(whole_scale(150) == 1 && whole_scale(179) == 1);   /* just under 1.5x: down */
    assert(whole_scale(180) == 2);                            /* the half-way point rounds up */
    assert(whole_scale(240) == 2);
    assert(whole_scale(300) == 3);
    assert(whole_scale(480) == 4);
    assert(whole_scale(100) == 1 && whole_scale(600) == 4);   /* clamped */

    puts("scale: ok");
    return 0;
}
