#ifndef ICON_H
#define ICON_H

#include <stdbool.h>
#include <stddef.h>

/* Finds a PNG for a notification's icon: an absolute path (or file:// URL) as it is, or an
 * icon-theme name such as "dialog-information" looked up in the user's and the system's icon
 * directories (the GTK theme from settings.ini first, then hicolor, then Adwaita), at the
 * sizes a small card can use. SVG-only icons are not found. Returns true and fills out. */
bool icon_find(const char *spec, char *out, size_t n);

#endif
