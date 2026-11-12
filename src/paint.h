#ifndef PAINT_H
#define PAINT_H

#include "hud.h"
#include "image.h"
#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>

/* Drawing of the HUD's items. Each function draws one item with its top-left at the origin of cr,
 * or only measures it when cr is NULL; either way it reports the item's size in logical pixels. */

void paint_toast(cairo_t *cr, const char *text, Level level, int *w, int *h);
/* percent above 100 is shown but the bar stays full; dim greys it out (muted) */
void paint_slider(cairo_t *cr, const char *label, int percent, bool dim, int *w, int *h);

/* What a card shows. Strings are not copied. */
typedef struct {
    const char *summary, *body;   /* the body may carry a little HTML: tags are dropped */
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
