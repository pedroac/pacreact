#ifndef TEXT_H
#define TEXT_H

#include <cairo/cairo.h>
#include <stdbool.h>

/* All text goes through here, so the HUDs do not care about scripts. The font is chosen from
 * the text itself: Latin, Arabic/Hebrew (shaped and reordered), or Chinese/Japanese/Korean,
 * which also covers notification text in a language other than the UI's. Fonts load the first
 * time a script is needed. An I18N=0 build, or a missing font file, falls back to Cairo's
 * default sans-serif. */

/* Sets up the locale and translation catalogs. Call once, first. */
void text_init(void);
void text_finish(void);

/* The UI language is written right to left: layouts are mirrored. */
bool text_rtl(void);

/* Advance width of the first len bytes of utf8 (which must end on a character boundary). */
double text_width(const char *utf8, int len, double size, bool bold);
void text_extents(const char *utf8, double size, bool bold, double *ascent, double *height);
/* Draws NUL-terminated utf8 with its left edge at x and its baseline at y. */
void text_draw(cairo_t *cr, const char *utf8, double x, double baseline, double size, bool bold);

#endif
