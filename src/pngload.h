#ifndef PNGLOAD_H
#define PNGLOAD_H

#include <cairo/cairo.h>

/* Decodes a PNG file. The path may come from the app that sent a notification, and a file can be
 * anything: a pipe that never delivers (which would stop the whole daemon), a device, or a few
 * kilobytes of PNG that claim to be gigapixels (which would decode into gigabytes before anything
 * could object). So the path must be absolute and end in ".png", the file is opened without
 * blocking and must be a regular file of at most 4 MiB, and its header must declare a picture of at
 * most 2048 x 2048 pixels before any decoding starts. Returns an image surface the caller owns, or
 * NULL. */
cairo_surface_t *png_load(const char *path);

#endif
