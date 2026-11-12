#ifndef SICON_H
#define SICON_H

#include <cairo/cairo.h>
#include <stdbool.h>

/* Status icons: the icon theme's icons for the HUD's own messages (battery, network, volume, a
 * headset...), found by name, drawn at one size and kept once drawn. They are optional all the way
 * down: a theme without the icon, a build without an SVG renderer, or PACHUD_ICONS=0 all just mean
 * the message has none. `names` is a list of icon names, separated by commas, the first that exists
 * being used (see icon_find_status()). */

/* Turns them on or off and sets the size in pixels, forgetting what was drawn at another size. */
void sicon_configure(bool enabled, int size_px);
bool sicon_enabled(void);
int sicon_size(void);
/* Whether this build can draw SVG icons (librsvg), which is most of what themes ship now */
bool sicon_svg(void);

/* Whether there is an icon for these names (and they are on) */
bool sicon_has(const char *names);
/* Draws it with its top-left corner at (x, y), sicon_size() square. A symbolic icon has no colour
 * of its own: it is drawn in rgb. Does nothing when there is no icon. */
void sicon_paint(cairo_t *cr, const char *names, double x, double y, const double rgb[3]);

#endif
