#ifndef ICON_H
#define ICON_H

#include <stdbool.h>
#include <stddef.h>

/* Finds a PNG for a notification's icon: an absolute path (or file:// URL) as it is, or an
 * icon-theme name such as "dialog-information" looked up in the user's and the system's icon
 * directories (the GTK theme from settings.ini first, then hicolor, then Adwaita), at the
 * sizes a small card can use. SVG-only icons are not found. Returns true and fills out. */
bool icon_find(const char *spec, char *out, size_t n);

/* Finds the file of a status icon, an icon of the theme by name ("battery-low", "network-offline",
 * "audio-volume-high"), for the HUD's own messages. `names` may list several, separated by commas:
 * the first that exists wins. For each, the symbolic variant ("battery-low-symbolic", drawn in the
 * HUD's own colour) is preferred to the coloured one. SVG files are only considered when `svg` is
 * true (a renderer is built in); otherwise only PNGs. The GTK theme from settings.ini is tried
 * first, then hicolor, Adwaita, and AdwaitaLegacy (whose PNGs serve where there is no SVG renderer).
 * Returns true, fills out, and sets *symbolic to whether the file is a symbolic one. */
bool icon_find_status(const char *names, bool svg, char *out, size_t n, bool *symbolic);

#endif
