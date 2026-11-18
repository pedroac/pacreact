#ifndef SICON_H
#define SICON_H

#include <cairo/cairo.h>
#include <stdbool.h>

/* Status icons: the icon theme's icons for the HUD's own messages (battery, network, volume, a
 * headset...), found by name, drawn at one size and kept once drawn. They are optional all the way
 * down: a theme without the icon, a build without an SVG renderer, or icons = 0 all just mean
 * the message has none. `names` is a list of icon names, separated by commas, the first that exists
 * being used (see icon_find_status()). */

/* Turns them on or off and sets the size in pixels, forgetting what was drawn at another size. */
void sicon_configure(bool enabled, int size_px);
bool sicon_enabled(void);
int sicon_size(void);
/* Draws the status icons that have one from a Nerd Font (the Material Design glyphs) instead of the
 * theme, in the HUD's colour. `font` is the path of the font file; NULL or empty looks for the Symbols
 * Nerd Font in the font directories. Names without a glyph, a font that is not found, and a build
 * without FreeType (I18N=0) fall back to the theme. Off until this is called with true. */
void sicon_set_glyphs(bool on, const char *font);
/* Whether this build can draw SVG icons (through rsvg-convert, when installed), which is most of what themes ship now */
bool sicon_svg(void);

/* Whether there is an icon for these names (and they are on) */
bool sicon_has(const char *names);
/* Draws it with its top-left corner at (x, y), sicon_size() square. A symbolic icon has no colour
 * of its own: it is drawn in rgb. Does nothing when there is no icon. */
void sicon_paint(cairo_t *cr, const char *names, double x, double y, const double rgb[3]);

#endif
