/* How the display reacts to the compositor's preferred scale. display.c is included so the
 * event handler can be called directly: it only touches plain fields and a callback, so no
 * compositor is needed. */
#include <assert.h>
#include "setting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the daemon is told of the monitors: a device each, taken back when it goes */
static char said_key[8][24], said_value[8][64];
void daemon_publish(const char *key, const char *value) {
    int i = 0, free_slot = -1;
    for (int j = 0; j < 8; j++) {
        if (!strcmp(said_key[j], key)) i = j + 1;
        else if (!said_key[j][0] && free_slot < 0) free_slot = j;
    }
    i = i ? i - 1 : free_slot;
    assert(i >= 0);
    if (!value) { said_key[i][0] = '\0'; return; }
    snprintf(said_key[i], sizeof said_key[0], "%s", key);
    snprintf(said_value[i], sizeof said_value[0], "%s", value);
}
static const char *said(const char *key) {
    for (int i = 0; i < 8; i++) if (!strcmp(said_key[i], key)) return said_value[i];
    return NULL;
}

#include "../src/display.c"

static int rescales;
static void on_rescale(void) { rescales++; }

static Display *fresh(void) {
    Display *d = calloc(1, sizeof *d);
    assert(d);
    d->scale120 = SCALE_UNIT;
    d->margin = DEFAULT_MARGIN;
    d->rescale_cb = on_rescale;
    rescales = 0;
    return d;
}

/* margin: pixels from the edge */
static long margin_of(const char *v) {
    if (v) pac_setting_set("margin", v); else pac_setting_set("margin", NULL);
    return setting_int("margin", DEFAULT_MARGIN, 0, MARGIN_MAX, "pixels");
}

static void test_margin(void) {
    assert(margin_of(NULL) == 16 && margin_of("") == 16);
    assert(margin_of("30") == 30);
    assert(margin_of("0") == 0);                /* flush with the edge is allowed */
    assert(margin_of("200") == 200);
    assert(margin_of("999") == 200);            /* too much: the most */
    assert(margin_of("-5") == 0);               /* below the range: the least */
    const char *bad[] = { "abc", "1.5", "5 " };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        assert(margin_of(bad[i]) == 16);        /* said so, and the default stays */
    assert(margin_of(" 12") == 12);             /* a stray space in front is forgiven */
    pac_setting_set("margin", NULL);
}

/* margin with a unit: a length that is the same on every screen */
static void test_margin_length(void) {
    Display *d = fresh();
    d->outputs[0] = (Output){ .proxy = (struct wl_output *)1, .logical_w = 1920, .logical_h = 1080, .physical_mm = 508, .mode_px = 1920, .scale = 1 };
    d->last_out = 1;
    double ppm = 1920.0 / 508;
    pac_setting_set("margin", "5mm");
    d->margin_size = setting_size("margin", 0, MARGIN_MAX);
    assert(d->margin_size.unit && margin_px(d) == (int)lround(5 * ppm));
    d->outputs[0].scale = 2; d->outputs[0].mode_px = 3840;       /* a sharper panel of the same size: the same logical size */
    assert(margin_px(d) == (int)lround(5 * ppm));
    d->outputs[0].physical_mm = 0;                               /* not told: 96 dpi */
    assert(margin_px(d) == (int)lround(5 * 96 / 25.4 / 2));
    pac_setting_set("margin", "10%");
    d->margin_size = setting_size("margin", 0, MARGIN_MAX);
    assert(margin_px(d) == 108);                                 /* of the shorter side, 1080 */
    pac_setting_set("margin", "100in");                          /* too much: the most */
    d->margin_size = setting_size("margin", 0, MARGIN_MAX);
    assert(margin_px(d) == MARGIN_MAX);
    pac_setting_set("margin", "3cm");                            /* not a unit: ignored, the default stays */
    d->margin_size = setting_size("margin", 0, MARGIN_MAX);
    assert(!d->margin_size.set && margin_px(d) == DEFAULT_MARGIN);
    pac_setting_set("margin", NULL);
    free(d);
}

static void test_deck_apart(void) {
    Display *d = fresh();
    int out[3];
    d->anchor = EDGE_T | EDGE_R;

    /* Nothing known about the screen: assume it is enough */
    assert(display_output_width(d) == 0 && display_deck_apart(d, 380));

    d->outputs[0].proxy = (struct wl_output *)&out[0];
    snprintf(d->outputs[0].name, sizeof d->outputs[0].name, "BIG");
    d->outputs[0].logical_w = 1920;
    d->outputs[1].proxy = (struct wl_output *)&out[1];
    snprintf(d->outputs[1].name, sizeof d->outputs[1].name, "SMALL");
    d->outputs[1].logical_w = 1000;

    /* Not shown anywhere yet, and not told where it will be: the narrowest is what counts */
    assert(display_output_width(d) == 1000 && !display_deck_apart(d, 380));

    /* Once the HUD has appeared on an output, that is the one that counts */
    Surface sf = { .d = d };
    surface_enter(&sf, NULL, (struct wl_output *)&out[0]);
    assert(d->last_out == 1 && display_output_width(d) == 1920 && display_deck_apart(d, 380));
    surface_enter(&sf, NULL, (struct wl_output *)&out[1]);
    assert(d->last_out == 2 && display_output_width(d) == 1000 && !display_deck_apart(d, 380));
    surface_enter(&sf, NULL, (struct wl_output *)&out[2]);          /* an output we do not know: ignored */
    assert(d->last_out == 2);

    /* output decides, whatever happened before; an unknown name tells nothing */
    d->want_output = "BIG";
    assert(display_output_width(d) == 1920 && display_deck_apart(d, 380));
    d->want_output = "SMALL";
    assert(display_output_width(d) == 1000 && !display_deck_apart(d, 380));
    d->want_output = "NOPE";
    assert(display_output_width(d) == 0 && display_deck_apart(d, 380));

    /* The line is exactly three items, two margins and two gaps: 380 * 3 + 32 + 32 */
    d->want_output = "BIG";
    d->margin = DEFAULT_MARGIN;
    d->outputs[0].logical_w = 3 * 380 + 2 * DEFAULT_MARGIN + 2 * DECK_GAP;
    assert(d->outputs[0].logical_w == 1204 && display_deck_apart(d, 380));
    d->outputs[0].logical_w = 1203;
    assert(!display_deck_apart(d, 380));
    d->outputs[0].logical_w = 1280;                                  /* a common laptop: fine */
    assert(display_deck_apart(d, 380));
    assert(!display_deck_apart(d, 420));                             /* wider items need a wider screen */

    /* The same in the other top corner; the bottom corners never share the top, narrow or not */
    d->outputs[0].logical_w = 800;
    d->anchor = EDGE_T | EDGE_L;
    assert(!display_deck_apart(d, 380));
    d->outputs[0].logical_w = 1280;
    assert(display_deck_apart(d, 380));
    d->outputs[0].logical_w = 800;
    d->anchor = EDGE_B | EDGE_R;
    assert(display_deck_apart(d, 380));
    d->anchor = EDGE_B | EDGE_L;
    assert(display_deck_apart(d, 380));

    /* A stack at the top centre is exactly where the deck would be, however wide the screen */
    d->outputs[0].logical_w = 3840;
    d->anchor = EDGE_T;
    assert(!display_deck_apart(d, 380));
    d->anchor = EDGE_B;
    assert(display_deck_apart(d, 380));

    /* The compositor tells the size, and tells again when it changes */
    d->anchor = EDGE_T | EDGE_R;
    xdg_size(&d->outputs[0], NULL, 1280, 720);
    assert(d->outputs[0].logical_w == 1280 && display_deck_apart(d, 380));
    xdg_size(&d->outputs[0], NULL, 960, 540);                        /* the scale was raised */
    assert(d->outputs[0].logical_w == 960 && !display_deck_apart(d, 380));

    /* The margin is part of the sum: a wider one needs a wider screen, a narrower one less */
    d->outputs[0].logical_w = 3 * 380 + 2 * DEFAULT_MARGIN + 2 * DECK_GAP;
    assert(display_deck_apart(d, 380));
    d->margin = 40;
    assert(!display_deck_apart(d, 380));
    d->outputs[0].logical_w = 3 * 380 + 2 * 40 + 2 * DECK_GAP;
    assert(display_deck_apart(d, 380));
    d->margin = 0;
    d->outputs[0].logical_w = 3 * 380 + 2 * DECK_GAP;
    assert(display_deck_apart(d, 380));
    free(d);
}

static void test_monitors(void) {
    Display *d = fresh();
    memset(said_key, 0, sizeof said_key);
    memset(told, 0, sizeof told);
    d->outputs[0].proxy = (struct wl_output *)d; d->outputs[0].global = 42;
    d->outputs[1].proxy = (struct wl_output *)d; d->outputs[1].global = 57;
    snprintf(d->outputs[0].name, sizeof d->outputs[0].name, "DP-1");
    publish_displays(d);
    assert(!strcmp(said("monitor.m42"), "DP-1") && !strcmp(said("monitor.m57"), "Monitor"));   /* the second has no name yet */
    d->outputs[0].logical_w = 1920; d->outputs[0].logical_h = 1080;
    snprintf(d->outputs[1].name, sizeof d->outputs[1].name, "HDMI-A-1");
    publish_displays(d);
    assert(!strcmp(said("monitor.m42"), "DP-1\tsize=1920x1080") && !strcmp(said("monitor.m57"), "HDMI-A-1"));
    d->outputs[0].proxy = NULL;                                  /* unplugged: it goes, the other stays */
    publish_displays(d);
    assert(!said("monitor.m42") && said("monitor.m57"));
    d->outputs[1].proxy = NULL;
    publish_displays(d);
    assert(!said("monitor.m57"));
    free(d);
}

int main(void) {
    test_monitors();
    Display *d = fresh();
    struct wl_surface *surface = (struct wl_surface *)d;   /* only compared with NULL below */

    /* No HUD on screen: the scale is remembered for the next one, nothing is redrawn */
    preferred_scale(d, NULL, 180);
    assert(d->scale120 == 180 && rescales == 0);
    assert(display_scale(d) == 180);

    /* A HUD on screen: it is redrawn, once per actual change */
    d->main.wl = surface;
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

    /* scale pins the scale: the compositor's preference is ignored */
    d->scale_fixed = true;
    d->scale120 = 180;
    preferred_scale(d, NULL, 240);
    assert(d->scale120 == 180 && rescales == 4);

    /* No callback registered: still fine */
    d->scale_fixed = false;
    d->rescale_cb = NULL;
    preferred_scale(d, NULL, 200);
    assert(d->scale120 == 200);

    /* The pointer over the HUD: position as it enters and moves, and the end of it as it leaves; the
     * HUD's own surface only */
    static int hovers, hx, hy, harea = -1;
    static bool hover_over;
    void on_hover_cb(int area, int x, int y, bool over) { hovers++; harea = area; hx = x; hy = y; hover_over = over; }
    d->hover_cb = on_hover_cb;
    struct wl_surface *other = (struct wl_surface *)&hovers;
    d->main.wl = surface;

    ptr_enter(d, NULL, 1, surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
    assert(hovers == 1 && hx == 10 && hy == 20 && hover_over);
    ptr_motion(d, NULL, 5, wl_fixed_from_int(30), wl_fixed_from_int(40));
    assert(hovers == 2 && hx == 30 && hy == 40 && hover_over);
    ptr_leave(d, NULL, 2, surface);
    assert(hovers == 3 && !hover_over);
    ptr_leave(d, NULL, 3, surface);                           /* a second leave is not another event */
    assert(hovers == 3);
    ptr_motion(d, NULL, 6, wl_fixed_from_int(1), wl_fixed_from_int(1));   /* motion with no pointer on us */
    assert(hovers == 3);

    ptr_enter(d, NULL, 4, other, wl_fixed_from_int(5), wl_fixed_from_int(5));   /* someone else's window */
    ptr_motion(d, NULL, 7, wl_fixed_from_int(6), wl_fixed_from_int(6));
    ptr_leave(d, NULL, 5, other);
    assert(hovers == 3);

    /* The deck is its own surface and says so */
    struct wl_surface *deck = (struct wl_surface *)&harea;
    d->deck.wl = deck;
    ptr_enter(d, NULL, 8, deck, wl_fixed_from_int(7), wl_fixed_from_int(8));
    assert(hovers == 4 && harea == DISPLAY_AREA_DECK && hx == 7 && hy == 8 && hover_over);
    ptr_leave(d, NULL, 9, deck);
    assert(hovers == 5 && harea == DISPLAY_AREA_DECK && !hover_over);
    ptr_enter(d, NULL, 10, surface, wl_fixed_from_int(1), wl_fixed_from_int(2));
    assert(harea == DISPLAY_AREA_STACK && hovers == 6);
    ptr_leave(d, NULL, 11, surface);
    d->deck.wl = NULL;
    hovers = 3;

    d->hover_cb = NULL;                                       /* nobody listening: still fine */
    ptr_enter(d, NULL, 6, surface, wl_fixed_from_int(1), wl_fixed_from_int(1));
    ptr_motion(d, NULL, 8, wl_fixed_from_int(2), wl_fixed_from_int(2));
    ptr_leave(d, NULL, 7, surface);
    assert(hovers == 3);

    free(d);
    test_margin();
    test_margin_length();
    test_deck_apart();
    puts("display: ok");
    return 0;
}
