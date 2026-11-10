/* How the display reacts to the compositor's preferred scale. display.c is included so the
 * event handler can be called directly: it only touches plain fields and a callback, so no
 * compositor is needed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/display.c"

static int rescales;
static void on_rescale(void) { rescales++; }

static Display *fresh(void) {
    Display *d = calloc(1, sizeof *d);
    assert(d);
    d->scale120 = SCALE_UNIT;
    d->rescale_cb = on_rescale;
    rescales = 0;
    return d;
}

int main(void) {
    Display *d = fresh();
    struct wl_surface *surface = (struct wl_surface *)d;   /* only compared with NULL below */

    /* No HUD on screen: the scale is remembered for the next one, nothing is redrawn */
    preferred_scale(d, NULL, 180);
    assert(d->scale120 == 180 && rescales == 0);
    assert(display_scale(d) == 180);

    /* A HUD on screen: it is redrawn, once per actual change */
    d->surface = surface;
    preferred_scale(d, NULL, 240);
    assert(d->scale120 == 240 && rescales == 1);
    preferred_scale(d, NULL, 240);                 /* the compositor repeats itself */
    assert(rescales == 1);
    preferred_scale(d, NULL, 150);                 /* moved to another monitor */
    assert(d->scale120 == 150 && rescales == 2);

    /* Nonsense is ignored */
    preferred_scale(d, NULL, 0);
    preferred_scale(d, NULL, 119);
    preferred_scale(d, NULL, 481);
    preferred_scale(d, NULL, 0xFFFFFFFFu);
    assert(d->scale120 == 150 && rescales == 2);

    /* The limits themselves are fine */
    preferred_scale(d, NULL, SCALE_MIN);
    assert(d->scale120 == 120 && rescales == 3);
    preferred_scale(d, NULL, SCALE_MAX);
    assert(d->scale120 == 480 && rescales == 4);

    /* PACHUD_SCALE pins the scale: the compositor's preference is ignored */
    d->scale_fixed = true;
    d->scale120 = 180;
    preferred_scale(d, NULL, 240);
    assert(d->scale120 == 180 && rescales == 4);

    /* No callback registered: still fine */
    d->scale_fixed = false;
    d->rescale_cb = NULL;
    preferred_scale(d, NULL, 200);
    assert(d->scale120 == 200);

    free(d);
    puts("display: ok");
    return 0;
}
