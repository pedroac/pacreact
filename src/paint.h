#ifndef PAINT_H
#define PAINT_H

#include "hud.h"
#include "image.h"
#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include "tunables.h"

/* Drawing of the HUD's items. Each function draws one item with its top-left at the origin of cr,
 * or only measures it when cr is NULL; either way it reports the item's size in logical pixels. */

/* Sizes. Everything is designed for a 16 px font and scaled by font_size / 16, so a larger
 * font gets proportionally larger cards, padding, icons and bars (and a smaller one the opposite).
 * width sets the width of the stack's items on its own. paint_init() reads both. */
void paint_init(void);
/* font_size, width and icon_size may be lengths with a unit (mm, in, pt, %): this turns them into
 * pixels for the screen they are shown on, `px_per_mm` logical pixels to the millimetre and `whole`
 * pixels across the shorter side (what 100% is). It also holds the smallest text between text_size_min
 * and text_size_max, as large as `screen_w` (0 if not known) lets the items be. Call it when the
 * screen is known, and again when it changes. */
void paint_set_screen(double px_per_mm, double whole, int screen_w);
/* The same, from numbers: 0 for either means the default. Out of range values are brought in. */
void paint_configure(int font_px, int width_px);
void paint_set_contrast(bool high);   /* black and white with brighter accents; paint_init() reads contrast */
void paint_configure_icons(bool enabled, int size_px);   /* 0: the default size */
int paint_px(int design_px);          /* a length at the default size, as it is now */
double paint_font(double design_px);  /* a font size, likewise */

/* Everything in the notification stack is this wide, whatever it is, so the column is tidy; the
 * on-screen display is its own, narrower thing. */
int paint_item_w(void);
int paint_osd_w(void);
#define PAINT_ITEM_W paint_item_w()
#define PAINT_OSD_W paint_osd_w()

/* A toast wraps onto at most three lines (the last ending in an ellipsis if it still does not fit).
 * `icon` is a status icon name, or a comma separated list of them (the first that exists is used),
 * or NULL; one that is not found or turned off leaves it out and the text keeps the room. */
void paint_toast(cairo_t *cr, const char *text, Level level, const char *icon, int *w, int *h);
/* percent above 100 is shown but the bar stays full; dim greys it out (muted) */
void paint_slider(cairo_t *cr, const char *label, int percent, bool dim, const char *icon, int width,
                  int *w, int *h);

/* The time, large and centred, with a line of smaller text under it (the date, or a label), for a
 * glance at the clock. `detail` may be "". */
void paint_clock(cairo_t *cr, const char *time_text, const char *detail, int width, int *w, int *h);


/* A card behind the front one in the attention deck: one line of text, and the top PAINT_STRIP_TUCK
 * pixels of it lie under the card in front, so only the rest shows. */
#define PAINT_STRIP_H paint_px(32)
#define PAINT_STRIP_TUCK paint_px(10)
#define PAINT_STRIP_INSET paint_px(14)   /* each strip is this much narrower than the one in front, on both sides */
void paint_strip(cairo_t *cr, const char *text, Level level, int width, int *w, int *h);

/* What a card shows. Strings are not copied. */
typedef struct {
    const char *summary, *body;   /* the body may carry a little HTML: tags are dropped */
    bool expanded;                /* show the body up to a generous length, not just a few lines */
    Level level;
    const uint8_t *image;         /* IMAGE_BYTES of premultiplied ARGB32, or NULL for no icon */
    int nact;                     /* how many of act_label are used */
    const char *act_label[HUD_MAX_ACTIONS];
} CardPaint;

void paint_card(cairo_t *cr, const CardPaint *card, int *w, int *h);

/* A PNG file as the small raw icon a card draws (IMAGE_BYTES of premultiplied ARGB32, the same as
 * an image-data hint), or false. The path comes from the sending app, so it must be a regular file
 * of a sane size and size in pixels; see paint.c. */
bool load_icon_pixels(const char *path, uint8_t *out);

#endif
